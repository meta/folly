/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * Improved thread local storage for non-trivial types (similar speed as
 * pthread_getspecific but only consumes a single pthread_key_t, and 4x faster
 * than boost::thread_specific_ptr).
 *
 * ThreadLocal objects can be grouped together logically under a tag. Within
 * a tag, each object has a unique id. The combination of tag and id is used to
 * locate the managed object corresponding to the current thread.
 *
 * Also includes an accessor interface to iterate all of the managed
 * objects owned by a ThreadLocal object, each corresponding to a
 * separate thread.  accessAllThreads() initializes an accessor
 * which holds
 * a lock *that blocks all creation and destruction of managed
 * objects managed by the ThreadLocal. The accessor can be used
 * as an iterable container. Note: for now, the accessor also happens to hold
 * other per tag global locks and hence calls to accessAllThreads() are
 * serialized at tag level.
 *
 * accessAllThreads() can race with destruction of thread-local elements. We
 * provide a strict mode which is dangerous because it requires the access lock
 * to be held while destroying thread-local elements which could cause
 * deadlocks. We gate this mode behind the AccessModeStrict template parameter.
 *
 * Intended use is for frequent write, infrequent read data access patterns such
 * as counters.
 *
 * There are two classes here - ThreadLocal and ThreadLocalPtr.  ThreadLocalPtr
 * has semantics similar to boost::thread_specific_ptr. ThreadLocal is a thin
 * wrapper around ThreadLocalPtr that manages allocation automatically.
 */

#pragma once

#include <iterator>
#include <thread>
#include <type_traits>
#include <utility>

#include <folly/Likely.h>
#include <folly/Portability.h>
#include <folly/ScopeGuard.h>
#include <folly/SharedMutex.h>
#include <folly/detail/ThreadLocalDetail.h>

namespace folly {

template <class T, class Tag, class AccessMode>
class ThreadLocalPtr;

template <class T, class Tag = void, class AccessMode = void>
class ThreadLocal {
 public:
  constexpr ThreadLocal() noexcept : constructor_([]() { return T(); }) {}

  template <typename F>
    requires std::is_invocable_r_v<T, F>
  explicit ThreadLocal(F&& constructor)
      : constructor_(std::forward<F>(constructor)) {}

  ThreadLocal(ThreadLocal&& that) noexcept
      : tlp_{std::move(that.tlp_)},
        constructor_{std::exchange(that.constructor_, {})} {}

  ThreadLocal& operator=(ThreadLocal&& that) noexcept {
    assert(this != &that);
    tlp_ = std::exchange(that.tlp_, {});
    constructor_ = std::exchange(that.constructor_, {});
    return *this;
  }

  FOLLY_ERASE T* get() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    auto const ptr = tlp_.get();
    return FOLLY_LIKELY(!!ptr) ? ptr : makeTlp();
  }

  // may return null
  FOLLY_ERASE T* get_existing() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return tlp_.get();
  }

  T* operator->() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] { return get(); }

  T& operator*() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] { return *get(); }

  void reset(T* newPtr = nullptr) { tlp_.reset(newPtr); }

  using Accessor = typename ThreadLocalPtr<T, Tag, AccessMode>::Accessor;
  Accessor accessAllThreads() const { return tlp_.accessAllThreads(); }

 private:
  // non-copyable
  ThreadLocal(const ThreadLocal&) = delete;
  ThreadLocal& operator=(const ThreadLocal&) = delete;

  FOLLY_NOINLINE T* makeTlp() const {
    auto const ptr = new T(constructor_());
    tlp_.reset(ptr);
    return ptr;
  }

  mutable ThreadLocalPtr<T, Tag, AccessMode> tlp_;
  std::function<T()> constructor_;
};

/*
 * The idea here is that __thread is faster than pthread_getspecific, so we
 * keep a __thread array of pointers to objects (ThreadEntry::elements) where
 * each array has an index for each unique instance of the ThreadLocalPtr
 * object.  Each ThreadLocalPtr object has a unique id that is an index into
 * these arrays so we can fetch the correct object from thread local storage
 * very efficiently.
 *
 * In order to prevent unbounded growth of the id space and thus huge
 * ThreadEntry::elements, arrays, for example due to continuous creation and
 * destruction of ThreadLocalPtr objects, we keep a set of all active
 * instances.  When an instance is destroyed we remove it from the active
 * set and insert the id into freeIds_ for reuse.  These operations require a
 * global mutex, but only happen at construction and destruction time.
 *
 * We use a single global pthread_key_t per Tag to manage object destruction and
 * memory cleanup upon thread exit because there is a finite number of
 * pthread_key_t's available per machine.
 *
 * NOTE: Apple platforms don't support the same semantics for __thread that
 *       Linux does (and it's only supported at all on i386). For these, use
 *       pthread_setspecific()/pthread_getspecific() for the per-thread
 *       storage.  Windows (MSVC and GCC) does support the same semantics
 *       with __declspec(thread)
 */

template <class T, class Tag = void, class AccessMode = void>
class ThreadLocalPtr {
 private:
  using StaticMeta = threadlocal_detail::StaticMeta<Tag, AccessMode>;

  using AccessAllThreadsEnabled = std::negation<std::is_same<Tag, void>>;

 public:
  constexpr ThreadLocalPtr() noexcept : id_() {}

  ThreadLocalPtr(ThreadLocalPtr&& other) noexcept : id_(std::move(other.id_)) {}

  ThreadLocalPtr& operator=(ThreadLocalPtr&& other) noexcept {
    assert(this != &other);
    destroy(); // user-provided dtors invoked within here must not throw
    id_ = std::move(other.id_);
    return *this;
  }

  ~ThreadLocalPtr() { destroy(); }

  T* get() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    threadlocal_detail::ElementWrapper& w = StaticMeta::get(&id_);
    return static_cast<T*>(w.ptr);
  }

  T* operator->() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] { return get(); }

  T& operator*() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] { return *get(); }

  T* release() {
    auto rlocked = getForkGuard();
    threadlocal_detail::ThreadEntry* te = StaticMeta::getThreadEntry(&id_);
    auto id = id_.getOrInvalid();
    // Only valid index into the elements array
    DCHECK_NE(id, threadlocal_detail::kEntryIDInvalid);
    return static_cast<T*>(te->releaseElement(id));
  }

  void reset(T* newPtr = nullptr) {
    auto rlocked = getForkGuard();
    auto guard = makeGuard([&] { delete newPtr; });
    threadlocal_detail::ThreadEntry* te = StaticMeta::getThreadEntry(&id_);
    uint32_t id = id_.getOrInvalid();
    // Only valid index into the elements array
    DCHECK_NE(id, threadlocal_detail::kEntryIDInvalid);
    te->resetElement(newPtr, id);
    guard.dismiss();
  }

  explicit operator bool() const { return get() != nullptr; }

  /**
   * reset() that transfers ownership from a smart pointer
   */
  template <
      typename SourceT,
      typename Deleter,
      typename = typename std::enable_if<
          std::is_convertible<SourceT*, T*>::value>::type>
  void reset(std::unique_ptr<SourceT, Deleter> source) {
    auto deleter =
        [delegate = source.get_deleter()](T* ptr, TLPDestructionMode) {
          delegate(ptr);
        };
    reset(source.release(), deleter);
  }

  /**
   * reset() that transfers ownership from a smart pointer with the default
   * deleter
   */
  template <
      typename SourceT,
      typename = typename std::enable_if<
          std::is_convertible<SourceT*, T*>::value>::type>
  void reset(std::unique_ptr<SourceT> source) {
    reset(source.release());
  }

  /**
   * reset() with a custom deleter:
   * deleter(T* ptr, TLPDestructionMode mode)
   * "mode" is ALL_THREADS if we're destructing this ThreadLocalPtr (and thus
   * deleting pointers for all threads), and THIS_THREAD if we're only deleting
   * the member for one thread (because of thread exit or reset()).
   * Invoking the deleter must not throw.
   */
  template <class Deleter>
  void reset(T* newPtr, const Deleter& deleter) {
    auto guard = makeGuard([&] {
      if (newPtr) {
        deleter(newPtr, TLPDestructionMode::THIS_THREAD);
      }
    });

    auto rlocked = getForkGuard();
    threadlocal_detail::ThreadEntry* te = StaticMeta::getThreadEntry(&id_);
    uint32_t id = id_.getOrInvalid();
    // Only valid index into the elements array
    DCHECK_NE(id, threadlocal_detail::kEntryIDInvalid);
    te->resetElement(newPtr, deleter, id);
    guard.dismiss();
  }

  void reset(const std::shared_ptr<T>& newPtr) {
    reset(newPtr.get(), threadlocal_detail::SharedPtrDeleter{newPtr});
  }

  // Holds a global lock for iteration through all thread local child objects.
  // Can be used as an iterable container.
  // Use accessAllThreads() to obtain one.
  class Accessor {
    friend class ThreadLocalPtr<T, Tag, AccessMode>;

    threadlocal_detail::StaticMetaBase& meta_ =
        threadlocal_detail::StaticMeta<Tag, AccessMode>::instance();
    std::unique_lock<SharedMutex> accessAllThreadsLock_;
    std::shared_lock<SharedMutex> forkHandlerLock_;
    uint32_t id_ = 0;

    // Prevent the entry set from changing while we are iterating over it.
    // reset() calls to populate will acquire shared lock on the id's set.
    threadlocal_detail::StaticMetaBase::SynchronizedThreadEntrySet::WLockedPtr
        wlockedThreadEntrySet_;

   public:
    class Iterator;
    friend class Iterator;

    // The iterators obtained from Accessor are bidirectional iterators.
    class Iterator {
      friend class Accessor;
      const Accessor* accessor_{nullptr};
      using InnerVector = threadlocal_detail::ThreadEntrySet::ElementVector;
      using InnerIterator = InnerVector::iterator;

      InnerVector& vec_;
      InnerIterator iter_;

      void increment() {
        if (iter_ != vec_.end()) {
          ++iter_;
          incrementToValid();
        }
      }

      void decrement() {
        if (iter_ != vec_.begin()) {
          --iter_;
          decrementToValid();
        }
      }

      const T& dereference() const {
        return *static_cast<T*>(iter_->wrapper.ptr);
      }

      T& dereference() { return *static_cast<T*>(iter_->wrapper.ptr); }

      bool equal(const Iterator& other) const {
        return (accessor_->id_ == other.accessor_->id_ && iter_ == other.iter_);
      }

      void setToEnd() { iter_ = vec_.end(); }

      explicit Iterator(const Accessor* accessor, bool toEnd = false)
          : accessor_(accessor),
            vec_(accessor_->wlockedThreadEntrySet_->threadElements),
            iter_(vec_.begin()) {
        if (toEnd) {
          setToEnd();
        } else {
          incrementToValid();
        }
      }

      // we just need to check the ptr since it can be set to nullptr
      // even if the entry is part of the list
      bool valid() const { return (iter_ != vec_.end() && iter_->wrapper.ptr); }

      void incrementToValid() {
        for (; iter_ != vec_.end() && !valid(); ++iter_) {
        }
      }

      void decrementToValid() {
        for (; iter_ != vec_.begin() && !valid(); --iter_) {
        }
      }

     public:
      using difference_type = ssize_t;
      using value_type = T;
      using reference = T const&;
      using pointer = T const*;
      using iterator_category = std::bidirectional_iterator_tag;

      Iterator() = default;

      Iterator& operator++() {
        increment();
        return *this;
      }

      Iterator operator++(int) {
        Iterator copy(*this);
        increment();
        return copy;
      }

      Iterator& operator--() {
        decrement();
        return *this;
      }

      Iterator operator--(int) {
        Iterator copy(*this);
        decrement();
        return copy;
      }

      T& operator*() { return dereference(); }

      T const& operator*() const { return dereference(); }

      T* operator->() { return &dereference(); }

      T const* operator->() const { return &dereference(); }

      bool operator==(Iterator const& rhs) const { return equal(rhs); }

      std::thread::id getThreadId() const { return iter_->threadEntry->tid(); }

      uint64_t getOSThreadId() const { return iter_->threadEntry->tid_os; }
    };

    ~Accessor() { release(); }

    Iterator begin() const { return Iterator(this); }

    Iterator end() const { return Iterator(this, true); }

    Accessor(const Accessor&) = delete;
    Accessor& operator=(const Accessor&) = delete;

    Accessor(Accessor&& other) noexcept
        : meta_(other.meta_),
          accessAllThreadsLock_(std::move(other.accessAllThreadsLock_)),
          forkHandlerLock_(std::move(other.forkHandlerLock_)),
          id_(std::exchange(other.id_, 0)) {
      wlockedThreadEntrySet_ = std::move(other.wlockedThreadEntrySet_);
    }

    Accessor& operator=(Accessor&& other) noexcept {
      // Each Tag has its own unique meta, and accessors with different Tags
      // have different types.  So either *this is empty, or this and other
      // have the same tag.  But if they have the same tag, they have the same
      // meta (and lock), so they'd both hold the lock at the same time,
      // which is impossible, which leaves only one possible scenario --
      // *this is empty.  Assert it.
      assert(&meta_ == &other.meta_);
      using std::swap;
      swap(accessAllThreadsLock_, other.accessAllThreadsLock_);
      swap(forkHandlerLock_, other.forkHandlerLock_);
      swap(id_, other.id_);
      wlockedThreadEntrySet_.unlock();
      swap(wlockedThreadEntrySet_, other.wlockedThreadEntrySet_);
      return *this;
    }

    Accessor() = default;

   private:
    explicit Accessor(uint32_t id)
        : accessAllThreadsLock_(meta_.accessAllThreadsLock_, std::defer_lock),
          forkHandlerLock_(meta_.forkHandlerLock_, std::defer_lock),
          id_(id) {
      forkHandlerLock_.lock();
      accessAllThreadsLock_.lock();
      wlockedThreadEntrySet_ = meta_.allId2ThreadEntrySets_[id_].wlock();
    }

    void release() {
      if (accessAllThreadsLock_) {
        wlockedThreadEntrySet_.unlock();
        accessAllThreadsLock_.unlock();
        DCHECK(forkHandlerLock_);
        forkHandlerLock_.unlock();
        id_ = 0;
      }
    }
  };

  // accessor allows a client to iterate through all thread local child
  // elements of this ThreadLocal instance.  Holds a global lock for each <Tag>
  Accessor accessAllThreads() const {
    static_assert(
        AccessAllThreadsEnabled::value,
        "Must use a unique Tag to use the accessAllThreads feature");
    return Accessor(id_.getOrAllocate(StaticMeta::instance()));
  }

 private:
  void destroy() noexcept {
    auto const val = id_.value.load(std::memory_order_relaxed);
    if (val == threadlocal_detail::kEntryIDInvalid) {
      return;
    }
    StaticMeta::instance().destroy(&id_);
    // User provided destructors should not cause the TL to have its id
    // reallocated.
    DCHECK(
        id_.value.load(std::memory_order_relaxed) ==
        threadlocal_detail::kEntryIDInvalid);
  }

  // non-copyable
  ThreadLocalPtr(const ThreadLocalPtr&) = delete;
  ThreadLocalPtr& operator=(const ThreadLocalPtr&) = delete;

  static auto getForkGuard() {
    auto& mutex = StaticMeta::instance().forkHandlerLock_;
    return std::shared_lock{mutex};
  }

  mutable typename StaticMeta::EntryID id_;
};

} // namespace folly
