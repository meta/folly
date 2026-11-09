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

#pragma once

#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include <folly/ScopeGuard.h>
#include <folly/ThreadLocal.h>
#include <folly/detail/Iterators.h>
#include <folly/detail/Singleton.h>
#include <folly/detail/UniqueInstance.h>
#include <folly/functional/Invoke.h>
#include <folly/lang/Hint.h>

namespace folly {

namespace detail {

struct SingletonThreadLocalState {
  struct LocalCache {
    void* object; // type-erased pointer to the object field of wrapper, below
  };
  static_assert( // pod avoids tls-init guard var and tls-fini ub use-after-dtor
      std::is_standard_layout<LocalCache>::value &&
          std::is_trivial<LocalCache>::value,
      "non-pod");

  struct LocalLifetime;

  struct Tracking {
    using LocalCacheSet = std::unordered_set<LocalCache*>;

    // per-cache refcounts, the number of lifetimes tracking that cache
    std::unordered_map<LocalCache*, size_t> caches;

    // per-lifetime cache tracking; 1-M lifetimes may track 1-N caches
    std::unordered_map<LocalLifetime*, LocalCacheSet> lifetimes;

    Tracking() noexcept;
    ~Tracking();
  };

  struct LocalLifetime {
    void destroy(Tracking& tracking) noexcept;
    void track(LocalCache& cache, Tracking& tracking, void* object) noexcept;
  };
};

} // namespace detail

/// SingletonThreadLocal
///
/// Useful for a per-thread leaky-singleton model in libraries and applications.
///
/// By "leaky" it is meant that the T instances held by the instantiation
/// SingletonThreadLocal<T> will survive until their owning thread exits.
/// Therefore, they can safely be used before main() begins and after main()
/// ends, and they can also safely be used in an application that spawns many
/// temporary threads throughout its life.
///
/// Example:
///
///   struct UsefulButHasExpensiveCtor {
///     UsefulButHasExpensiveCtor(); // this is expensive
///     Result operator()(Arg arg);
///   };
///
///   Result useful(Arg arg) {
///     using Useful = UsefulButHasExpensiveCtor;
///     auto& useful = folly::SingletonThreadLocal<Useful>::get();
///     return useful(arg);
///   }
///
/// As an example use-case, the random generators in <random> are expensive to
/// construct. And their constructors are deterministic, but many cases require
/// that they be randomly seeded. So folly::Random makes good canonical uses of
/// folly::SingletonThreadLocal so that a seed is computed from the secure
/// random device once per thread, and the random generator is constructed with
/// the seed once per thread.
///
/// Keywords to help people find this class in search:
/// Thread Local Singleton ThreadLocalSingleton
template <
    typename T,
    typename Tag = detail::DefaultTag,
    typename Make = void,
    typename TLTag = std::
        conditional_t<std::is_same<Tag, detail::DefaultTag>::value, void, Tag>>
class SingletonThreadLocal {
 private:
  static detail::UniqueInstance unique;

  using State = detail::SingletonThreadLocalState;
  using LocalCache = State::LocalCache;

  using MakeFn =
      std::conditional_t<std::is_void_v<Make>, detail::DefaultMake<T>, Make>;
  using Object = invoke_result_t<MakeFn>;
  static_assert(std::is_convertible<Object&, T&>::value, "inconvertible");

  struct ObjectWrapper {
    // keep as first field in first base, to save 1 instr in the fast path
    Object object{MakeFn{}()};
  };
  struct Wrapper : ObjectWrapper, State::Tracking {
    /* implicit */ operator T&() { return ObjectWrapper::object; }
  };

  using WrapperTL = ThreadLocal<Wrapper, TLTag>;

  struct LocalLifetime : State::LocalLifetime {
    ~LocalLifetime() { destroy(getWrapper()); }
  };

  SingletonThreadLocal() = delete;

  FOLLY_ALWAYS_INLINE static WrapperTL& getWrapperTL() {
    (void)unique; // force the object not to be thrown out as unused
    return detail::createGlobal<WrapperTL, Tag>();
  }

  FOLLY_NOINLINE static Wrapper& getWrapper() { return *getWrapperTL(); }

  FOLLY_NOINLINE static Wrapper& getSlow(LocalCache& cache) {
    auto& wrapper = getWrapper();
    if (threadlocal_detail::StaticMetaBase::dying()) {
      return wrapper;
    }
    static thread_local LocalLifetime lifetime;
    lifetime.track(cache, wrapper, &wrapper.object); // idempotent
    return wrapper;
  }

  FOLLY_EXPORT FOLLY_ALWAYS_INLINE static LocalCache& getLocalCache() {
    static thread_local LocalCache cache;
    return cache;
  }

 public:
  FOLLY_ALWAYS_INLINE static T& get() {
    if (kIsMobile) {
      return getWrapper();
    }
    auto& cache = getLocalCache();
    auto* object = static_cast<Object*>(cache.object);
    return FOLLY_LIKELY(!!object) ? *object : getSlow(cache).object;
  }

  static T* try_get() {
    if (!kIsMobile) {
      auto& cache = getLocalCache();
      if (auto* object = static_cast<Object*>(cache.object)) {
        return object;
      }
    }
    if (threadlocal_detail::StaticMetaBase::dying()) {
      return nullptr;
    }
    auto* wrapper = getWrapperTL().get_existing();
    return wrapper ? &static_cast<T&>(*wrapper) : nullptr;
  }

  class Accessor {
   private:
    using Inner = typename WrapperTL::Accessor;
    using IteratorBase = typename Inner::Iterator;
    using IteratorTag = typename IteratorBase::iterator_category;

    Inner inner_;

    explicit Accessor(Inner inner) noexcept : inner_(std::move(inner)) {}

   public:
    friend class SingletonThreadLocal<T, Tag, Make, TLTag>;

    class Iterator
        : public detail::
              IteratorAdaptor<Iterator, IteratorBase, T, IteratorTag> {
     private:
      using Super =
          detail::IteratorAdaptor<Iterator, IteratorBase, T, IteratorTag>;
      using Super::Super;

     public:
      friend class Accessor;

      T& dereference() const {
        return const_cast<Iterator*>(this)->base()->object;
      }

      std::thread::id getThreadId() const { return this->base().getThreadId(); }

      uint64_t getOSThreadId() const { return this->base().getOSThreadId(); }
    };

    Accessor(const Accessor&) = delete;
    Accessor& operator=(const Accessor&) = delete;
    Accessor(Accessor&&) = default;
    Accessor& operator=(Accessor&&) = default;

    Iterator begin() const { return Iterator(inner_.begin()); }

    Iterator end() const { return Iterator(inner_.end()); }
  };

  // Must use a unique Tag, takes a lock that is one per Tag
  static Accessor accessAllThreads() {
    return Accessor(getWrapperTL().accessAllThreads());
  }
};

FOLLY_PUSH_WARNING
FOLLY_CLANG_DISABLE_WARNING("-Wglobal-constructors")
template <typename T, typename Tag, typename Make, typename TLTag>
detail::UniqueInstance SingletonThreadLocal<T, Tag, Make, TLTag>::unique{
    tag<SingletonThreadLocal>, tag<T, Tag>, tag<Make, TLTag>};
FOLLY_POP_WARNING

} // namespace folly

/// FOLLY_DECLARE_REUSED
///
/// Useful for local variables of container types, where it is desired to avoid
/// the overhead associated with the local variable entering and leaving scope.
/// Rather, where it is desired that the memory be reused between invocations
/// of the same scope in the same thread rather than deallocated and reallocated
/// between invocations of the same scope in the same thread. Note that the
/// container will always be cleared between invocations; it is only the backing
/// memory allocation which is reused.
///
/// Example:
///
///   void traverse_perform(int root);
///   template <typename F>
///   void traverse_each_child_r(int root, F const&);
///   void traverse_depthwise(int root) {
///     // preserves some of the memory backing these per-thread data structures
///     FOLLY_DECLARE_REUSED(seen, std::unordered_set<int>);
///     FOLLY_DECLARE_REUSED(work, std::vector<int>);
///     // example algorithm that uses these per-thread data structures
///     work.push_back(root);
///     while (!work.empty()) {
///       root = work.back();
///       work.pop_back();
///       seen.insert(root);
///       traverse_perform(root);
///       traverse_each_child_r(root, [&](int item) {
///         if (!seen.count(item)) {
///           work.push_back(item);
///         }
///       });
///     }
///   }
#define FOLLY_DECLARE_REUSED(name, ...)                                        \
  struct __folly_reused_type_##name {                                          \
    __VA_ARGS__ object;                                                        \
  };                                                                           \
  [[maybe_unused]] ::folly::unsafe_for_async_usage                             \
      __folly_reused_g_prevent_async_##name;                                   \
  auto& name =                                                                 \
      ::folly::SingletonThreadLocal<__folly_reused_type_##name>::get().object; \
  auto __folly_reused_g_##name = ::folly::makeGuard([&] { name.clear(); })
