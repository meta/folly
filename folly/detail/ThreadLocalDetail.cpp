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

#include <folly/detail/ThreadLocalDetail.h>

#include <algorithm>
#include <mutex>
#include <random>

#include <folly/ConstexprMath.h>
#include <folly/Utility.h>
#include <folly/container/irange.h>
#include <folly/detail/thread_local_globals.h>
#include <folly/hash/MurmurHash.h>
#include <folly/lang/Hint.h>
#include <folly/memory/SanitizeLeak.h>
#include <folly/random/hash.h>
#include <folly/synchronization/CallOnce.h>

constexpr auto kSmallGrowthFactor = 1.1;
constexpr auto kBigGrowthFactor = 1.7;

namespace folly {
namespace threadlocal_detail {

namespace {

struct murmur_hash_fn {
  std::uint64_t operator()(std::uint64_t const val) const noexcept {
    auto const ptr = reinterpret_cast<const char*>(&val);
    return folly::hash::murmurHash64(ptr, sizeof(val), 0u);
  }
};

} // namespace

SharedPtrDeleter::SharedPtrDeleter(std::shared_ptr<void> const& ts) noexcept
    : ts_{ts} {}
SharedPtrDeleter::SharedPtrDeleter(SharedPtrDeleter const& that) noexcept
    : ts_{that.ts_} {}
SharedPtrDeleter::~SharedPtrDeleter() = default;
void SharedPtrDeleter::operator()(
    void* /* ptr */, folly::TLPDestructionMode) const {
  ts_.reset();
}

uintptr_t ElementWrapper::castForgetAlign(DeleterFunType* f) noexcept {
  auto const p = reinterpret_cast<char const*>(f);
  auto const q = std::launder(p);
  return reinterpret_cast<uintptr_t>(q);
}

bool ThreadEntrySet::basicSanity() const {
  if constexpr (!kIsDebug) {
    return true;
  }
  if (threadElements.empty() && entryToVectorSlot.empty()) {
    return true;
  }
  if (threadElements.size() != entryToVectorSlot.size()) {
    return false;
  }
  auto const size = threadElements.size();
  // NOLINTNEXTLINE
  static std::atomic<std::uint64_t> rng_seed_{to_unsigned(std::rand())};
  using engine = hash_counter_engine<murmur_hash_fn, std::uint64_t>;
  thread_local engine rng{rng_seed_.fetch_add(1, std::memory_order_relaxed)};
  std::uniform_int_distribution<size_t> dist{0, size - 1};
  if (!(dist(rng) < constexpr_log2(size))) {
    return true;
  }
  for (auto const& kvp : entryToVectorSlot) {
    if (!(kvp.second < threadElements.size() &&
          threadElements[kvp.second].threadEntry == kvp.first)) {
      return false;
    }
  }
  return true;
}

void ThreadEntrySet::clear() {
  DCHECK(basicSanity());
  entryToVectorSlot.clear();
  threadElements.clear();
}

int64_t ThreadEntrySet::getIndexFor(ThreadEntry* entry) const {
  auto iter = entryToVectorSlot.find(entry);
  if (iter != entryToVectorSlot.end()) {
    return static_cast<int64_t>(iter->second);
  }
  return -1;
}

void* ThreadEntrySet::getPtrForThread(ThreadEntry* entry) const {
  auto index = getIndexFor(entry);
  if (index < 0) {
    return nullptr;
  }
  return threadElements[static_cast<size_t>(index)].wrapper.ptr;
}

bool ThreadEntrySet::contains(ThreadEntry* entry) const {
  DCHECK(basicSanity());
  return entryToVectorSlot.find(entry) != entryToVectorSlot.end();
}

bool ThreadEntrySet::insert(ThreadEntry* entry) {
  DCHECK(basicSanity());
  auto iter = entryToVectorSlot.find(entry);
  if (iter != entryToVectorSlot.end()) {
    // Entry already present. Sanity check and exit.
    DCHECK_EQ(entry, threadElements[iter->second].threadEntry);
    return false;
  }
  threadElements.emplace_back(entry);
  auto idx = threadElements.size() - 1;
  entryToVectorSlot[entry] = idx;
  return true;
}

bool ThreadEntrySet::insert(const Element& element) {
  DCHECK(basicSanity());
  auto iter = entryToVectorSlot.find(element.threadEntry);
  if (iter != entryToVectorSlot.end()) {
    // Entry already present. Skip copying over element. Caller
    // responsible for handling acceptability of this behavior.
    DCHECK_EQ(element.threadEntry, threadElements[iter->second].threadEntry);
    return false;
  }
  threadElements.push_back(element);
  auto idx = threadElements.size() - 1;
  entryToVectorSlot[element.threadEntry] = idx;
  return true;
}

ThreadEntrySet::Element ThreadEntrySet::erase(ThreadEntry* entry) {
  DCHECK(basicSanity());
  auto iter = entryToVectorSlot.find(entry);
  if (iter == entryToVectorSlot.end()) {
    // Entry not present.
    return Element{nullptr};
  }
  auto idx = iter->second;
  DCHECK_LT(idx, threadElements.size());
  entryToVectorSlot.erase(iter);
  Element last = threadElements.back();
  Element current = threadElements[idx];
  if (idx != threadElements.size() - 1) {
    threadElements[idx] = last;
    entryToVectorSlot[last.threadEntry] = idx;
  }
  threadElements.pop_back();
  DCHECK(basicSanity());
  if (compressible()) {
    compress();
  }
  DCHECK(basicSanity());
  return current;
}

bool ThreadEntrySet::compressible() const {
  // We choose a sufficiently-large multiplier so that there is no risk of a
  // following insert growing the vector and then a following erase shrinking
  // the vector, since that way lies non-amortized-O(N)-complexity costs for
  // both insert and erase ops.
  constexpr size_t const mult = 4;
  auto& vec = threadElements;
  return std::max(size_t(1), vec.size()) * mult <= vec.capacity();
}

bool ThreadEntry::cachedInSetMatchesElementsArray(uint32_t id) {
  if constexpr (!kIsDebug) {
    return true;
  }

  if (removed_) {
    // Pointer in entry set and elements array need not match anymore.
    return true;
  }

  auto rlock = meta->allId2ThreadEntrySets_[id].tryRLock();
  if (!rlock) {
    // Try lock failed. Skip checking in this case. Avoids
    // getting stuck in case this validation is called when
    // already holding the entry set lock.
    return true;
  }

  return elements[id].ptr == rlock->getPtrForThread(this);
}

void ThreadEntrySet::compress() {
  assert(compressible());
  // compress the vector
  threadElements.shrink_to_fit();
  // compress the index
  entryToVectorSlot.reserve(0);
}

/**
 * We want to disable onThreadExit call at the end of shutdown, we don't care
 * about leaking memory at that point.
 *
 * Otherwise if ThreadLocal is used in a shared library, onThreadExit may be
 * called after the library's static destructors have run, accessing destroyed
 * data structures.
 *
 * Rather than deleting pthread keys (which races with threads still calling
 * pthread_setspecific), we set an atomic flag that causes onThreadExit
 * callbacks to no-op. The keys and TLS values are intentionally leaked — the
 * process is shutting down and the kernel will reclaim everything.
 *
 * A shared library that is about to be unloaded by dlclose deletes the keys
 * instead, by calling detail::thread_local_delete_pthread_keys.
 */
// NOLINTNEXTLINE
std::atomic<bool> threadLocalShuttingDown{false};

// NOLINTNEXTLINE
struct ThreadLocalShutdownGuard {
  ThreadLocalShutdownGuard() = default;
  ThreadLocalShutdownGuard(const ThreadLocalShutdownGuard&) = delete;
  ThreadLocalShutdownGuard& operator=(const ThreadLocalShutdownGuard&) = delete;
  ThreadLocalShutdownGuard(ThreadLocalShutdownGuard&&) = delete;
  ThreadLocalShutdownGuard& operator=(ThreadLocalShutdownGuard&&) = delete;
  ~ThreadLocalShutdownGuard() {
#if !defined(__APPLE__) && !defined(_MSC_VER)
    threadLocalShuttingDown.store(true, std::memory_order_release);
#endif
  }
};

StaticMetaBase::StaticMetaBase(ThreadEntry* (*threadEntry)(), bool strict)
    : nextId_(1), threadEntry_(threadEntry), strict_(strict) {
  int ret = pthread_key_create(&pthreadKey_, &onThreadExit);
  checkPosixError(ret, "pthread_key_create failed");
  detail::thread_local_register_pthread_key(pthreadKey_);
}

ThreadEntryList* StaticMetaBase::getThreadEntryList() {
  class PthreadKey {
   public:
    static void onThreadExit(void* ptr) {
      if (threadLocalShuttingDown.load(std::memory_order_acquire)) {
        return;
      }
      ThreadEntryList* list = static_cast<ThreadEntryList*>(ptr);
      StaticMetaBase::cleanupThreadEntriesAndList(list);
    }

    PthreadKey() {
      int ret = pthread_key_create(&pthreadKey_, &onThreadExit);
      checkPosixError(ret, "pthread_key_create failed");
      detail::thread_local_register_pthread_key(pthreadKey_);
    }

    FOLLY_ALWAYS_INLINE pthread_key_t get() const { return pthreadKey_; }

   private:
    pthread_key_t pthreadKey_;
  };

  auto& instance = detail::createGlobal<PthreadKey, void>();

  ThreadEntryList* threadEntryList =
      static_cast<ThreadEntryList*>(pthread_getspecific(instance.get()));

  if (!threadEntryList) [[unlikely]] {
    auto uptr = std::make_unique<ThreadEntryList>();
    int ret = pthread_setspecific(instance.get(), uptr.get());
    checkPosixError(ret, "pthread_setspecific failed");
    threadEntryList = uptr.release();
    threadEntryList->count = 1; // Pin once for own onThreadExit callback.
    lsan_ignore_object(threadEntryList);
  }

  return threadEntryList;
}

ThreadEntry* StaticMetaBase::allocateNewThreadEntry() {
  // If a thread is exiting, various cleanup handlers could
  // end up triggering use of a ThreadLocal and (re)create a
  // ThreadEntry. This is problematic since we don't know if the
  // destructor specified in the pthreadKey_ has already been run
  // for this StaticMeta or will run again. Such a newly created
  // ThreadEntry could leak. The count incremented on threadEntryList
  // could also cause all the list and other threadEntry objects used
  // by this thread to leak.
  // For now, debug assert that the thread is not exiting to keep the
  // legacy behavior. The handling of this case needs to be improved
  // as well.
  DCHECK(!dying());
  ThreadEntryList* threadEntryList = getThreadEntryList();
  ThreadEntry* threadEntry = new ThreadEntry();

  threadEntry->list = threadEntryList;
  threadEntry->listNext = threadEntryList->head;
  threadEntryList->head = threadEntry;

  threadEntry->tid() = std::this_thread::get_id();
  threadEntry->tid_os = folly::getOSThreadID();

  // if we're adding a thread entry
  // we need to increment the list count
  // even if the entry is reused
  threadEntryList->count++;

  threadEntry->meta = this;
  return threadEntry;
}

bool StaticMetaBase::dying() {
  return folly::detail::thread_is_dying();
}

/*
 * Note on safe lifecycle management of TL objects:
 * Any instance of a TL object created has to be disposed of safely. The
 * non-trivial cases are a) when a thread exits and we have to dispose of all
 * instances that belong to the thread across different TL objects; b) when a TL
 * object is destroyed and we have to dispose of all instances of that TL object
 * across all the threads that may have instantiated it. These 2 paths can also
 * race and are resolved, using the ThreadEntrySet locks, as follows:
 * - The exiting thread goes over all ThreadEntrySets and removes itself from
 * it. After this, the thread is responsible for disposing of any elements that
 * are still set in its 'elements' array. By setting the removed_ flag any new
 * elements created when disposing elements will not be made discoverable via
 * ThreadEntrySets and hence remain the responsibility of the exiting thread to
 * dispose.
 * - The thread calling destroy goes over all ThreadEntry in the ThreadEntrySet
 * of the TL object being destroyed (in popThreadEntrySetAndClearElementPtrs).
 * The ElementWrapper for the ThreadEntry found there are copied out and cleared
 * from the 'elements' array. This is done under the ThreadEntrySet lock and the
 * 'destroy' call is responsible for disposing the elements it thus claimed. The
 * actual dispose happens after releasing the locks and since the TL object is
 * being destroyed, no new elements of it should be getting created. The destroy
 * code should not access any ThreadEntry outside the safety of
 * popThreadEntrySetAndClearElementPtrs as their owner threads could exit and
 * release them.
 */

void StaticMetaBase::onThreadExit(void* ptr) {
  if (threadLocalShuttingDown.load(std::memory_order_acquire)) {
    return;
  }
  folly::detail::thread_is_dying_mark();
  auto threadEntry = static_cast<ThreadEntry*>(ptr);

  {
    auto& meta = *threadEntry->meta;

    // Make sure this ThreadEntry is available if ThreadLocal A is accessed in
    // ThreadLocal B destructor.
    pthread_setspecific(meta.pthreadKey_, threadEntry);

    std::shared_lock forkRlock(meta.forkHandlerLock_);
    std::shared_lock rlock(meta.accessAllThreadsLock_, std::defer_lock);
    if (meta.strict_) {
      rlock.lock();
    }
    meta.removeThreadEntryFromAllInMap(threadEntry);
    forkRlock.unlock();
    {
      std::lock_guard g(meta.lock_);
      // mark it as removed. Once this is set, any new TL object created in this
      // thread as part of invoking the destructor on another TL object will not
      // record this threadEntry in that TL object's ThreadEntrySet. This thread
      // itself will be responsible for cleaning up the new object(s) so
      // created.
      threadEntry->removed_ = true;
      auto elementsCapacity = threadEntry->getElementsCapacity();
      auto beforeCount = meta.totalElementWrappers_.fetch_sub(elementsCapacity);
      DCHECK_GE(beforeCount, elementsCapacity);
      // No need to hold the lock any longer; the ThreadEntry is private to this
      // thread now that it's been removed from meta.
    }
    // NOTE: User-provided deleter / object dtor itself may be using ThreadLocal
    // with the same Tag, so dispose() calls below may (re)create some of the
    // elements or even increase elementsCapacity, thus multiple cleanup rounds
    // may be required.
    for (bool shouldRun = true; shouldRun;) {
      shouldRun = false;
      auto elementsCapacity = threadEntry->getElementsCapacity();
      for (const auto i : folly::irange(elementsCapacity)) {
        if (threadEntry->elements[i].dispose(TLPDestructionMode::THIS_THREAD)) {
          threadEntry->elements[i].cleanup();
          shouldRun = true;
        }
      }
      DCHECK(meta.isThreadEntryRemovedFromAllInMap(threadEntry, !meta.strict_));
    }
    pthread_setspecific(meta.pthreadKey_, nullptr);
  }

  auto threadEntryList = threadEntry->list;
  DCHECK_GT(threadEntryList->count, 0u);

  cleanupThreadEntriesAndList(threadEntryList);
}

/* static */
void StaticMetaBase::cleanupThreadEntriesAndList(
    ThreadEntryList* threadEntryList) {
  --threadEntryList->count;
  if (threadEntryList->count) {
    return;
  }

  // dispose all the elements
  for (bool shouldRunOuter = true; shouldRunOuter;) {
    shouldRunOuter = false;
    auto tmp = threadEntryList->head;
    while (tmp) {
      auto& meta = *tmp->meta;
      pthread_setspecific(meta.pthreadKey_, tmp);
      std::shared_lock rlock(meta.accessAllThreadsLock_, std::defer_lock);
      if (meta.strict_) {
        rlock.lock();
      }
      // ThreadEntry 'tmp' was removed from all sets in its own cleanup. Since
      // we set removed_ flag, no subsequent element being added (during destroy
      // of other TL elements) should have added the ThreadEntry back to the
      // tracking set.
      DCHECK(meta.isThreadEntryRemovedFromAllInMap(tmp, !meta.strict_));

      for (bool shouldRunInner = true; shouldRunInner;) {
        shouldRunInner = false;
        auto elementsCapacity = tmp->getElementsCapacity();
        for (const auto i : folly::irange(elementsCapacity)) {
          if (tmp->elements[i].dispose(TLPDestructionMode::THIS_THREAD)) {
            tmp->elements[i].cleanup();
            shouldRunInner = true;
            shouldRunOuter = true;
          }
        }
      }
      pthread_setspecific(meta.pthreadKey_, nullptr);
      tmp = tmp->listNext;
    }
  }

  // free the entry list
  auto head = threadEntryList->head;
  threadEntryList->head = nullptr;
  while (head) {
    auto tmp = head;
    head = head->listNext;
    if (tmp->elements) {
      free(tmp->elements);
      tmp->elements = nullptr;
      tmp->setElementsCapacity(0);
    }

    // Fail safe check to make sure that the ThreadEntry is not present
    // before issuing a delete.
    DCHECK(tmp->meta->isThreadEntryRemovedFromAllInMap(tmp, true));

    delete tmp;
  }

  delete threadEntryList;
}

uint32_t StaticMetaBase::elementsCapacity() const {
  ThreadEntry* threadEntry = (*threadEntry_)();

  return FOLLY_LIKELY(!!threadEntry) ? threadEntry->getElementsCapacity() : 0;
}

uint32_t StaticMetaBase::allocate(EntryID* ent) {
  uint32_t id;
  auto& meta = *this;
  std::lock_guard g(meta.lock_);

  id = ent->value.load(std::memory_order_relaxed);

  if (id == kEntryIDInvalid) {
    if (!meta.freeIds_.empty()) {
      id = meta.freeIds_.back();
      meta.freeIds_.pop_back();
    } else {
      id = meta.nextId_++;
    }
    uint32_t old_id = ent->value.exchange(id, std::memory_order_release);
    DCHECK_EQ(old_id, kEntryIDInvalid);
  }
  return id;
}

ThreadEntrySet StaticMetaBase::popThreadEntrySetAndClearElementPtrs(
    uint32_t id) {
  // Lock the ThreadEntrySet for id so that no other thread can update
  // its local ptr or alter the its elements array or ThreadEntry object
  // itself, before this function is done updating them. This is called
  // by the `destroy` function to release a TL object. There should be
  // no racing accessAllThreads() call with it.
  auto wlocked = allId2ThreadEntrySets_[id].wlock();
  ThreadEntrySet tmp;
  std::swap(*wlocked, tmp);
  std::lock_guard g(lock_);
  for (auto& e : tmp.threadElements) {
    auto elementsCapacity = e.threadEntry->getElementsCapacity();
    if (id < elementsCapacity) {
      /*
       * Writing another thread's ThreadEntry from here is fine;
       * The TL object is being destroyed, so get(id), or reset()
       * or accessAllThreads calls on it are illegal. Only other
       * racing accesses would be from the owner thread itself
       * either a) reallocating the elements array (guarded by
       * lock_, so safe) or b) exiting and trying to clear the
       * elements array or free the elements and ThreadEntry itself. The
       * ThreadEntrySet lock synchronizes this part as the exiting thread will
       * acquire it to remove itself from the set.
       */
      DCHECK_EQ(e.threadEntry->elements[id].ptr, e.wrapper.ptr);
      e.wrapper.deleter = e.threadEntry->elements[id].deleter;
      e.threadEntry->elements[id].ptr = nullptr;
      e.threadEntry->elements[id].deleter = 0;
    }
    // Destroy should not access thread entry after this call as racing
    // exit call can make it invalid.
    e.threadEntry = nullptr;
  }
  return tmp;
}

void StaticMetaBase::destroy(EntryID* ent) {
  try {
    auto& meta = *this;

    // Elements in other threads that use this id.
    ThreadEntrySet tmpEntrySet;

    {
      std::shared_lock forkRlock(meta.forkHandlerLock_);
      std::unique_lock wlock(meta.accessAllThreadsLock_, std::defer_lock);
      if (meta.strict_) {
        /*
         * In strict mode, the logic guarantees per-thread instances are
         * destroyed by the moment ThreadLocal<> dtor returns.
         * In order to achieve that, we should wait until concurrent
         * onThreadExit() calls (that might acquire ownership over per-thread
         * instances in order to destroy them) are finished.
         */
        wlock.lock();
      }

      uint32_t id =
          ent->value.exchange(kEntryIDInvalid, std::memory_order_acquire);
      if (id == kEntryIDInvalid) {
        return;
      }
      tmpEntrySet = meta.popThreadEntrySetAndClearElementPtrs(id);
      forkRlock.unlock();

      {
        // Release the id to be reused by another TL variable. Some
        // other TL object may acquire and re-use it before the rest
        // of the cleanup work is done. That is ok. The destructor callbacks
        // for the objects should not access the same TL variable again. If
        // we want to tolerate that pattern, due to any legacy behavior, this
        // block should be after the loop to dispose of all collected elements
        // below.
        std::lock_guard g(meta.lock_);
        meta.freeIds_.push_back(id);
      }
    }
    // Delete elements outside the locks.
    for (auto& e : tmpEntrySet.threadElements) {
      if (e.wrapper.dispose(TLPDestructionMode::ALL_THREADS)) {
        e.wrapper.cleanup();
      }
    }
  } catch (...) { // Just in case we get a lock error or something anyway...
    LOG(WARNING) << "Destructor discarding an exception that was thrown.";
  }
}

ElementWrapper* StaticMetaBase::reallocate(
    ThreadEntry* threadEntry, uint32_t idval, size_t& newCapacity) {
  size_t prevCapacity = threadEntry->getElementsCapacity();

  // Growth factor < 2, see folly/docs/FBVector.md; + 5 to prevent
  // very slow start.
  auto smallCapacity = static_cast<size_t>((idval + 5) * kSmallGrowthFactor);
  auto bigCapacity = static_cast<size_t>((idval + 5) * kBigGrowthFactor);

  newCapacity =
      (threadEntry->meta && (bigCapacity <= threadEntry->meta->nextId_))
      ? bigCapacity
      : smallCapacity;

  assert(newCapacity > prevCapacity);
  ElementWrapper* reallocated = nullptr;

  // Need to grow. Note that we can't call realloc, as elements is
  // still linked in meta, so another thread might access invalid memory
  // after realloc succeeds. We'll copy by hand and update our ThreadEntry
  // under the lock.
  if (usingJEMalloc()) {
    bool success = false;
    size_t newByteSize = nallocx(newCapacity * sizeof(ElementWrapper), 0);

    // Try to grow in place.
    //
    // Note that xallocx(MALLOCX_ZERO) will only zero newly allocated memory,
    // even if a previous allocation allocated more than we requested.
    // This is fine; we always use MALLOCX_ZERO with jemalloc and we
    // always expand our allocation to the real size.
    if (prevCapacity * sizeof(ElementWrapper) >= jemallocMinInPlaceExpandable) {
      success =
          (xallocx(threadEntry->elements, newByteSize, 0, MALLOCX_ZERO) ==
           newByteSize);
    }

    // In-place growth failed.
    if (!success) {
      success =
          ((reallocated = static_cast<ElementWrapper*>(
                mallocx(newByteSize, MALLOCX_ZERO))) != nullptr);
    }

    if (success) {
      // Expand to real size
      assert(newByteSize / sizeof(ElementWrapper) >= newCapacity);
      newCapacity = newByteSize / sizeof(ElementWrapper);
    } else {
      throw_exception<std::bad_alloc>();
    }
  } else { // no jemalloc
    // calloc() is simpler than malloc() followed by memset(), and
    // potentially faster when dealing with a lot of memory, as it can get
    // already-zeroed pages from the kernel.
    reallocated = static_cast<ElementWrapper*>(
        calloc(newCapacity, sizeof(ElementWrapper)));
    if (!reallocated) {
      throw_exception<std::bad_alloc>();
    }

    // When the main thread exits, it will call functions registered with
    // 'atexit' and then call 'exit()'. However, It will NOT call any functions
    // registered via the 'TLS' feature of pthread_key_create.
    // Reference:
    // https://pubs.opengroup.org/onlinepubs/9699919799/functions/pthread_create.html
    folly::lsan_ignore_object(reallocated);
  }
  return reallocated;
}

/**
 * Reserve enough space in the ThreadEntry::elements for the item
 * @id to fit in.
 */

void StaticMetaBase::reserve(EntryID* id) {
  auto& meta = *this;
  ThreadEntry* threadEntry = (*threadEntry_)();
  size_t prevCapacity = threadEntry->getElementsCapacity();

  uint32_t idval = id->getOrAllocate(meta);
  if (prevCapacity > idval) {
    return;
  }

  size_t newCapacity;
  ElementWrapper* reallocated = reallocate(threadEntry, idval, newCapacity);

  // Success, update the entry
  {
    std::lock_guard g(meta.lock_);

    if (reallocated) {
      /*
       * Note: we need to hold the meta lock when copying data out of
       * the old vector, because some other thread might be
       * destructing a ThreadLocal and writing to the elements vector
       * of this thread.
       */
      if (prevCapacity != 0) {
        memcpy(
            reallocated,
            threadEntry->elements,
            sizeof(*reallocated) * prevCapacity);
      }
      std::swap(reallocated, threadEntry->elements);
    }

    threadEntry->setElementsCapacity(newCapacity);
  }

  meta.totalElementWrappers_ += (newCapacity - prevCapacity);
  free(reallocated);
}

FOLLY_NOINLINE void StaticMetaBase::ensureThreadEntryIsInSet(
    ThreadEntry* te,
    SynchronizedThreadEntrySet& set,
    SynchronizedThreadEntrySet::RLockedPtr& rlock) {
  rlock.unlock();
  auto wlock = set.wlock();
  wlock->insert(te);
  rlock = wlock.moveFromWriteToRead();
}

/*
 * release the element @id.
 */
void* ThreadEntry::releaseElement(uint32_t id) {
  auto rlocked = meta->allId2ThreadEntrySets_[id].rlock();
  auto capacity = getElementsCapacity();
  void* ptrToReturn = (capacity > id) ? elements[id].release() : nullptr;
  auto slot = rlocked->getIndexFor(this);
  if (slot < 0) {
    DCHECK(removed_ || ptrToReturn == nullptr);
    return ptrToReturn;
  }
  auto& element = rlocked.asNonConstUnsafe().threadElements[slot];
  DCHECK_EQ(ptrToReturn, element.wrapper.ptr);
  element.wrapper = {};
  return ptrToReturn;
}

/*
 * Cleanup the element. Caller is holding rlock on the ThreadEntrySet
 * corresponding to the id. Running destructors of user objects isn't ideal
 * under lock but this is the historical behavior. It should be possible to
 * restructure this if a need for it arises.
 */
void ThreadEntry::cleanupElement(uint32_t id) {
  elements[id].dispose(TLPDestructionMode::THIS_THREAD);
  // Cleanup
  elements[id].cleanup();
}

void ThreadEntry::resetElementImplAfterSet(
    const ElementWrapper& element, uint32_t id) {
  auto& set = meta->allId2ThreadEntrySets_[id];
  auto rlock = set.rlock();
  cleanupElement(id);
  elements[id] = element;
  if (removed_) {
    // Elements no longer being mirrored in the ThreadEntrySet.
    // Thread must have cleared itself from the set when it started exiting.
    DCHECK(!rlock->contains(this));
    return;
  }
  if (element.ptr != nullptr && !rlock->contains(this)) {
    meta->ensureThreadEntryIsInSet(this, set, rlock);
  }
  auto slot = rlock->getIndexFor(this);
  if (slot < 0) {
    // Not present in ThreadEntrySet implies the value was never set to be
    // non-null and new value in element.ptr is nullptr as well.
    DCHECK(!element.ptr);
    DCHECK(!elements[id].ptr);
    return;
  }
  size_t uslot = static_cast<size_t>(slot);
  rlock.asNonConstUnsafe().threadElements[uslot].wrapper = element;
}

FOLLY_STATIC_CTOR_PRIORITY_MAX
ThreadLocalShutdownGuard threadLocalShutdownGuard; // NOLINT
#if defined(__GLIBC__)
// Invoking thread_local dtor register early to fix issue
// https://github.com/facebook/folly/issues/1252
struct GlibcThreadLocalInit {
  struct GlibcThreadLocalInitHelper {
    FOLLY_NOINLINE ~GlibcThreadLocalInitHelper() {
      compiler_must_not_elide(this);
    }
  };
  GlibcThreadLocalInit() {
    static thread_local GlibcThreadLocalInitHelper glibcThreadLocalInit;
    compiler_must_not_elide(glibcThreadLocalInit);
  }
};
__attribute__((
    __init_priority__(101))) GlibcThreadLocalInit glibcThreadLocalInit;
#endif
} // namespace threadlocal_detail
} // namespace folly
