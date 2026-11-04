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

#include <folly/synchronization/Hazptr.h>

#include <atomic>
#include <new>
#include <numeric>
#include <random>
#include <vector>

#include <folly/Benchmark.h>
#include <folly/SpinLock.h>
#include <folly/concurrency/AtomicSharedPtr.h>
#include <folly/concurrency/CoreCachedSharedPtr.h>
#include <folly/concurrency/memory/AtomicReadMostlyMainPtr.h>
#include <folly/concurrency/memory/ReadMostlySharedPtr.h>
#include <folly/container/Enumerate.h>
#include <folly/container/F14Set.h>
#include <folly/lang/Keep.h>
#include <folly/portability/GFlags.h>
#include <folly/synchronization/RWSpinLock.h>
#include <folly/synchronization/Rcu.h>

using namespace folly;

namespace folly {
template <typename Value, bool BaseLast = false>
struct hazptr_obj_of;

template <typename Value>
struct hazptr_obj_of<Value, false>
    : hazptr_obj_base<hazptr_obj_of<Value, false>>,
      detail::inheritable<Value> {};

template <typename Value>
struct hazptr_obj_of<Value, true>
    : detail::inheritable<Value>,
      hazptr_obj_base<hazptr_obj_of<Value, true>> {};
} // namespace folly

extern "C" FOLLY_KEEP long check_folly_hazptr_protect(
    folly::hazptr_holder<std::atomic>& holder,
    std::atomic<hazptr_obj_of<long>*>& channel) {
  auto ptr = holder.protect(channel);
  return ptr ? *ptr : 0;
}

extern "C" FOLLY_KEEP bool check_folly_hazptr_try_protect(
    folly::hazptr_holder<std::atomic>& holder,
    hazptr_obj_of<long>*& ptr,
    std::atomic<hazptr_obj_of<long>*>& channel) {
  return holder.try_protect(ptr, channel);
}

extern "C" FOLLY_KEEP void check_folly_hazptr_unprotect(
    folly::hazptr_holder<std::atomic>& holder) {
  holder.reset_protection();
}

extern "C" FOLLY_KEEP long check_folly_hazptr_protect_adjusted(
    folly::hazptr_holder<std::atomic>& holder,
    std::atomic<hazptr_obj_of<long, true>*>& channel) {
  auto ptr = holder.protect(channel);
  return ptr ? *ptr : 0;
}

extern "C" FOLLY_KEEP bool check_folly_hazptr_try_protect_adjusted(
    folly::hazptr_holder<std::atomic>& holder,
    hazptr_obj_of<long, true>*& ptr,
    std::atomic<hazptr_obj_of<long, true>*>& channel) {
  return holder.try_protect(ptr, channel);
}

extern "C" FOLLY_KEEP void check_folly_hazptr_unprotect_adjusted(
    folly::hazptr_holder<std::atomic>& holder) {
  holder.reset_protection();
}

extern "C" FOLLY_KEEP long check_folly_hazptr_local_protect_default(
    std::atomic<hazptr_obj_of<long>*>& channel) {
  hazptr_local<1> local;
  auto& holder = local[0];
  auto ptr = holder.protect(channel);
  return ptr ? *ptr : 0;
}

extern "C" FOLLY_KEEP long check_folly_hazptr_make_protect(
    folly::hazptr_domain<std::atomic>& domain,
    std::atomic<hazptr_obj_of<long>*>& channel) {
  auto holder = folly::make_hazard_pointer(domain);
  auto ptr = holder.protect(channel);
  return ptr ? *ptr : 0;
}

extern "C" FOLLY_KEEP long check_folly_hazptr_make_protect_default(
    std::atomic<hazptr_obj_of<long>*>& channel) {
  auto& domain = folly::default_hazptr_domain();
  auto holder = folly::make_hazard_pointer(domain);
  auto ptr = holder.protect(channel);
  return ptr ? *ptr : 0;
}

extern "C" FOLLY_KEEP long check_folly_hazptr_make_protect_default_implicit(
    std::atomic<hazptr_obj_of<long>*>& channel) {
  auto holder = folly::make_hazard_pointer();
  auto ptr = holder.protect(channel);
  return ptr ? *ptr : 0;
}

extern "C" FOLLY_KEEP void check_folly_hazptr_holder_destroy(
    folly::hazptr_holder<std::atomic>* holder) {
  holder->~hazptr_holder();
}

extern "C" FOLLY_KEEP void check_folly_hazptr_make_array_default(
    folly::hazptr_array<3>* out) {
  ::new (out) folly::hazptr_array<3>(folly::make_hazard_pointer_array<3>());
}

extern "C" FOLLY_KEEP void check_folly_hazptr_array_destroy_default(
    folly::hazptr_array<3>* array) {
  array->~hazptr_array();
}

extern "C" FOLLY_KEEP void check_folly_hazptr_array_move_assign_default(
    folly::hazptr_array<3>* dst, folly::hazptr_array<3>* src) {
  *dst = std::move(*src);
}

extern "C" FOLLY_KEEP void check_folly_hazptr_obj_retire(
    folly::hazptr_obj<std::atomic>& object,
    folly::hazptr_domain<std::atomic>& domain) {
  struct derived : folly::hazptr_obj_base<derived, std::atomic> {};
  static_cast<derived&>(object).retire(domain);
}

namespace {

/// simple object for testing hazptr operations
struct TestObj : public hazptr_obj_base<TestObj> {
  size_t value{0};
  explicit TestObj(size_t v) noexcept : value(v) {}
};

template <bool Mutable, size_t LinkCount>
class LinkedBenchObj
    : public hazptr_obj_base_linked<
          LinkedBenchObj<Mutable, LinkCount>,
          std::atomic,
          variadic_noop_fn> {
  std::atomic<LinkedBenchObj*> links_[LinkCount]{};

 public:
  LinkedBenchObj() { this->set_deleter(); }

  void setLink(size_t index, LinkedBenchObj* obj) {
    links_[index].store(obj, std::memory_order_relaxed);
    obj->acquire_link_safe();
  }

  template <typename F>
  void for_each_link(bool mutableLinks, F&& f) {
    if (mutableLinks != Mutable) {
      return;
    }
    for (auto& link : links_) {
      if (auto obj = link.load(std::memory_order_relaxed)) {
        f(obj);
      }
    }
  }
};

template <bool Mutable, size_t LinkCount>
std::unique_ptr<LinkedBenchObj<Mutable, LinkCount>[]> makeLinkedBenchTree(
    size_t count, hazptr_obj_cohort<>& cohort) {
  using Obj = LinkedBenchObj<Mutable, LinkCount>;
  auto objects = std::make_unique<Obj[]>(count);
  for (size_t i = 0; i < count; ++i) {
    objects[i].set_cohort_no_tag(&cohort);
    for (size_t link = 0; link < LinkCount; ++link) {
      const auto child = i * LinkCount + link + 1;
      if (child < count) {
        objects[i].setLink(link, &objects[child]);
      }
    }
  }
  return objects;
}

} // namespace

/// benchmark copying a std::shared_ptr, including copy and dtor, plus any
/// extras around it for sharing between threads and avoiding false sharing
template <typename Obj, typename Copy>
static void do_shared_ptr_copy(size_t iters, Copy copy) {
  BenchmarkSuspender braces;
  auto obj = Obj(copy_to_shared_ptr(0));

  int sum = 0;
  braces.dismissing([&] {
    while (iters--) {
      auto ptr = copy(obj);
      compiler_must_not_predict(*ptr);
      sum += *ptr;
    }
  });
  compiler_must_not_elide(sum);
}

BENCHMARK(sptr_copy, iters) {
  using Obj = std::shared_ptr<int>;
  do_shared_ptr_copy<Obj>(iters, [](auto& obj) { return obj; });
}

BENCHMARK(sptr_copy_folly_shared_mutex, iters) {
  using Obj = folly::Synchronized<std::shared_ptr<int>>;
  do_shared_ptr_copy<Obj>(iters, [](auto& obj) { return obj.copy(); });
}

BENCHMARK(sptr_copy_folly_spin_lock, iters) {
  using Obj = folly::Synchronized<std::shared_ptr<int>, folly::SpinLock>;
  do_shared_ptr_copy<Obj>(iters, [](auto& obj) { return obj.copy(); });
}

BENCHMARK(sptr_copy_folly_rw_spin_lock, iters) {
  using Obj = folly::Synchronized<std::shared_ptr<int>, folly::RWSpinLock>;
  do_shared_ptr_copy<Obj>(iters, [](auto& obj) { return obj.copy(); });
}

BENCHMARK(sptr_copy_folly_atomic_shared_ptr, iters) {
  using Obj = folly::atomic_shared_ptr<int>;
  do_shared_ptr_copy<Obj>(iters, [](auto& obj) {
    return obj.load(std::memory_order_acquire);
  });
}

BENCHMARK(sptr_copy_std_atomic_shared_ptr, iters) {
  using Obj = std::shared_ptr<int>;
  do_shared_ptr_copy<Obj>(iters, [=](auto& obj) {
    return std::atomic_load_explicit(&obj, std::memory_order_acquire);
  });
}

BENCHMARK(sptr_copy_folly_read_mostly_main_ptr, iters) {
  using Obj = folly::ReadMostlyMainPtr<int>;
  do_shared_ptr_copy<Obj>(iters, [=](auto& obj) { return obj.getShared(); });
}

BENCHMARK(sptr_copy_folly_atomic_read_mostly_main_ptr, iters) {
  using Obj = folly::AtomicReadMostlyMainPtr<int>;
  do_shared_ptr_copy<Obj>(iters, [=](auto& obj) { return obj.load(); });
}

BENCHMARK(sptr_copy_folly_core_cached_shared_ptr, iters) {
  using Obj = folly::CoreCachedSharedPtr<int>;
  do_shared_ptr_copy<Obj>(iters, [=](auto& obj) { return obj.get(); });
}

BENCHMARK(sptr_copy_folly_atomic_core_cached_shared_ptr, iters) {
  using Obj = folly::AtomicCoreCachedSharedPtr<int>;
  do_shared_ptr_copy<Obj>(iters, [=](auto& obj) { return obj.get(); });
}

BENCHMARK_DRAW_LINE();

static void do_rcu_lock_unlock(
    BenchmarkSuspender& braces, rcu_domain& domain, size_t iters) {
  auto own = std::make_unique<TestObj>(42);
  std::atomic<TestObj*> ptr{own.get()};
  folly::compiler_must_not_predict(ptr);

  int sum = 0;
  braces.dismissing([&] {
    while (iters--) {
      domain.lock();
      auto* obj = ptr.load(std::memory_order_acquire);
      folly::compiler_must_not_predict(obj->value);
      sum += obj->value;
      domain.unlock();
    }
  });
  folly::compiler_must_not_elide(sum);
}

BENCHMARK(rcu_lock_unlock, iters) {
  BenchmarkSuspender braces;
  auto&& domain = rcu_domain();
  do_rcu_lock_unlock(braces, domain, iters);
}

BENCHMARK(rcu_lock_unlock_default, iters) {
  BenchmarkSuspender braces;
  auto&& domain = rcu_default_domain();
  do_rcu_lock_unlock(braces, domain, iters);
}

BENCHMARK_DRAW_LINE();

/// benchmark hazptr-protecting a pointer, including protection and unprotection
template <bool Combine, template <typename> class Atom>
static void do_hazptr_protect(
    BenchmarkSuspender& braces, hazptr_domain<Atom>& domain, size_t iters) {
  auto own = std::make_unique<TestObj>(42);
  Atom<TestObj*> ptr{own.get()};
  folly::compiler_must_not_predict(ptr);

  int sum = 0;
  braces.dismissing([&] {
    auto h = make_hazard_pointer(domain);
    while (iters--) {
      auto* obj = h.protect(ptr);
      folly::compiler_must_not_predict(obj->value);
      sum += obj->value;
      if (!Combine) {
        h.reset_protection();
      }
    }
  });
  folly::compiler_must_not_elide(sum);
}

BENCHMARK(hazptr_protect_separate, iters) {
  BenchmarkSuspender braces;
  auto&& domain = hazptr_domain{};
  do_hazptr_protect<false>(braces, domain, iters);
}

BENCHMARK(hazptr_protect_separate_default, iters) {
  BenchmarkSuspender braces;
  auto&& domain = default_hazptr_domain();
  do_hazptr_protect<false>(braces, domain, iters);
}

BENCHMARK(hazptr_protect_combined, iters) {
  BenchmarkSuspender braces;
  auto&& domain = hazptr_domain{};
  do_hazptr_protect<true>(braces, domain, iters);
}

BENCHMARK(hazptr_protect_combined_default, iters) {
  BenchmarkSuspender braces;
  auto&& domain = default_hazptr_domain();
  do_hazptr_protect<true>(braces, domain, iters);
}

/// benchmark ending protection of a modified object while beginning protection
/// of another, isolating the release-light fence and relaxed hazptr store
BENCHMARK(hazptr_reset_retarget, iters) {
  BenchmarkSuspender braces;

  hazptr_domain<> domain;
  TestObj first{0};
  TestObj second{0};
  TestObj* objects[] = {&first, &second};

  auto holder = make_hazard_pointer(domain);
  holder.reset_protection(objects[(iters - 1) & 1]);

  braces.dismissing([&] {
    while (iters != 0) {
      --iters;
      auto const index = iters & 1;
      ++objects[index]->value;
      holder.reset_protection(objects[index ^ 1]);
    }
  });

  folly::compiler_must_not_elide(first.value);
  folly::compiler_must_not_elide(second.value);
}

BENCHMARK_DRAW_LINE();

/// benchmark creating a hazard pointer (aka a hazptr-holder) without using it,
/// including ctor and dtor
template <typename... Args>
static void do_hazptr_make(
    BenchmarkSuspender& braces, size_t iters, Args&... args) {
  braces.dismissing([&] {
    while (iters--) {
      make_hazard_pointer(args...);
    }
  });
}

BENCHMARK(hazptr_make, iters) {
  BenchmarkSuspender braces;
  auto&& domain = hazptr_domain();
  do_hazptr_make(braces, iters, domain);
}

BENCHMARK(hazptr_make_default, iters) {
  BenchmarkSuspender braces;
  auto&& domain = default_hazptr_domain();
  do_hazptr_make(braces, iters, domain);
}

/// same, but without naming the domain, which is how most callers spell it
BENCHMARK(hazptr_make_default_implicit, iters) {
  BenchmarkSuspender braces;
  do_hazptr_make(braces, iters);
}

BENCHMARK_DRAW_LINE();

/// benchmark creating a local hazard pointer (aka a hazptr-local) without using
/// it, including ctor and dtor
template <template <typename> class Atom>
static void do_hazptr_make_local_default(
    BenchmarkSuspender& braces, size_t iters) {
  braces.dismissing([&] {
    while (iters--) {
      hazptr_local<1, Atom> local;
      folly::compiler_must_not_predict(local[0]);
    }
  });
}

BENCHMARK(hazptr_make_local_default, iters) {
  BenchmarkSuspender braces;
  do_hazptr_make_local_default<std::atomic>(braces, iters);
}

BENCHMARK_DRAW_LINE();

/// benchmark creating a local hazard pointer (aka a hazptr-local) and using to
/// protect and unprotect a shared object
template <template <typename> class Atom>
static void do_hazptr_make_local_protect_default(
    BenchmarkSuspender& braces, size_t iters) {
  auto own = std::make_unique<TestObj>(42);
  std::atomic<TestObj*> ptr{own.get()};
  folly::compiler_must_not_predict(ptr);

  int sum = 0;
  braces.dismissing([&] {
    while (iters--) {
      hazptr_local<1, Atom> local;
      auto& h = local[0];
      auto* obj = h.protect(ptr);
      folly::compiler_must_not_predict(obj->value);
      sum += obj->value;
    }
  });
  folly::compiler_must_not_elide(sum);
}

BENCHMARK(hazptr_make_local_protect_default, iters) {
  BenchmarkSuspender braces;
  do_hazptr_make_local_protect_default<std::atomic>(braces, iters);
}

BENCHMARK_DRAW_LINE();

/// benchmark creating a hazard pointer array (aka a hazptr-array) without using
/// it, including ctor and dtor
template <size_t ArraySize>
void hazptr_make_array_default(size_t iters, folly::index_constant<ArraySize>) {
  // make_hazard_pointer_array assumes the default domain
  while (iters--) {
    make_hazard_pointer_array<ArraySize>();
  }
}

BENCHMARK_PARAM(hazptr_make_array_default, 1_uzic)
BENCHMARK_PARAM(hazptr_make_array_default, 2_uzic)
BENCHMARK_PARAM(hazptr_make_array_default, 3_uzic)
BENCHMARK_PARAM(hazptr_make_array_default, 4_uzic)
BENCHMARK_PARAM(hazptr_make_array_default, 8_uzic)

BENCHMARK_DRAW_LINE();

/// benchmark move-assigning a hazard pointer array over another one; the same
/// amount of hazard pointer release work as hazptr_make_array_default, so the
/// difference between the two is the cost of move assignment over destruction
template <size_t ArraySize>
void hazptr_move_assign_array_default(
    size_t iters, folly::index_constant<ArraySize>) {
  auto dst = make_hazard_pointer_array<ArraySize>();
  while (iters--) {
    dst = make_hazard_pointer_array<ArraySize>();
  }
}

BENCHMARK_PARAM(hazptr_move_assign_array_default, 1_uzic)
BENCHMARK_PARAM(hazptr_move_assign_array_default, 2_uzic)
BENCHMARK_PARAM(hazptr_move_assign_array_default, 3_uzic)
BENCHMARK_PARAM(hazptr_move_assign_array_default, 4_uzic)
BENCHMARK_PARAM(hazptr_move_assign_array_default, 8_uzic)

BENCHMARK_DRAW_LINE();

/// benchmark creating a hazard pointer and using it to protect a pointer,
/// including hazard pointer ctor/dtor and protection/unprotection
template <typename... Args>
static void do_hazptr_make_protect(
    BenchmarkSuspender& braces, size_t iters, Args&... args) {
  auto own = std::make_unique<TestObj>(42);
  std::atomic<TestObj*> ptr{own.get()};
  folly::compiler_must_not_predict(ptr);

  int sum = 0;
  braces.dismissing([&] {
    while (iters--) {
      auto h = make_hazard_pointer(args...);
      auto* obj = h.protect(ptr);
      folly::compiler_must_not_predict(obj->value);
      sum += obj->value;
    }
  });
  folly::compiler_must_not_elide(sum);
}

BENCHMARK(hazptr_make_protect, iters) {
  BenchmarkSuspender braces;
  auto&& domain = hazptr_domain();
  do_hazptr_make_protect(braces, iters, domain);
}

BENCHMARK(hazptr_make_protect_default, iters) {
  BenchmarkSuspender braces;
  auto&& domain = default_hazptr_domain();
  do_hazptr_make_protect(braces, iters, domain);
}

/// same, but without naming the domain, which is how most callers spell it
BENCHMARK(hazptr_make_protect_default_implicit, iters) {
  BenchmarkSuspender braces;
  do_hazptr_make_protect(braces, iters);
}

BENCHMARK_DRAW_LINE();

/// benchmark retiring an object to a hazptr domain
template <template <typename> class Atom>
static void do_hazptr_retire(
    BenchmarkSuspender& braces, hazptr_domain<Atom>& domain, size_t iters) {
  std::vector<std::unique_ptr<TestObj>> objs;
  objs.reserve(iters);
  for (size_t i = 0; i < iters; ++i) {
    objs.push_back(std::make_unique<TestObj>(42));
  }

  braces.dismissing([&] {
    while (iters--) {
      auto* obj = objs[iters].release();
      obj->retire(domain);
    }
  });
}

BENCHMARK(hazptr_retire, iters) {
  BenchmarkSuspender braces;
  auto&& domain = hazptr_domain();
  do_hazptr_retire(braces, domain, iters);
}

BENCHMARK(hazptr_retire_default, iters) {
  BenchmarkSuspender braces;
  auto&& domain = default_hazptr_domain();
  do_hazptr_retire(braces, domain, iters);
}

BENCHMARK_DRAW_LINE();

template <size_t LinkCount>
static void do_hazptr_obj_linked_unlink(
    size_t iters, size_t const objectCount) {
  BenchmarkSuspender braces;
  while (iters--) {
    hazptr_obj_cohort<> cohort;
    auto objects = makeLinkedBenchTree<false, LinkCount>(objectCount, cohort);
    objects[0].acquire_link_safe();

    braces.dismissing([&] { objects[0].unlink(); });

    cohort.shutdown_and_reclaim();
  }
}

static void hazptr_obj_linked_unlink_immutable_chain(
    size_t iters, size_t const objectCount) {
  do_hazptr_obj_linked_unlink<1>(iters, objectCount);
}

BENCHMARK_PARAM(hazptr_obj_linked_unlink_immutable_chain, 1)
BENCHMARK_PARAM(hazptr_obj_linked_unlink_immutable_chain, 2)
BENCHMARK_PARAM(hazptr_obj_linked_unlink_immutable_chain, 8)
BENCHMARK_PARAM(hazptr_obj_linked_unlink_immutable_chain, 16)

static void hazptr_obj_linked_unlink_immutable_tree(
    size_t iters, size_t const objectCount) {
  do_hazptr_obj_linked_unlink<2>(iters, objectCount);
}

BENCHMARK_PARAM(hazptr_obj_linked_unlink_immutable_tree, 3)
BENCHMARK_PARAM(hazptr_obj_linked_unlink_immutable_tree, 7)
BENCHMARK_PARAM(hazptr_obj_linked_unlink_immutable_tree, 15)

BENCHMARK_DRAW_LINE();

/// fragment the memory and scatter hprec allocations across cachelines with no
/// consistent stride, if hprecs are allocated separately and linked together
template <template <typename> class Atom>
FOLLY_NOINLINE static std::vector<hazptr_holder<Atom>>
grow_scattered_hprec_list(hazptr_domain<Atom>& domain, size_t hprec_count) {
  using hprec_layout = aligned_storage_for_t<hazptr_rec<std::atomic>>;

  std::vector<hazptr_holder<Atom>> holders;
  std::vector<std::unique_ptr<hprec_layout>> memory_fragmenters;
  holders.reserve(hprec_count);
  memory_fragmenters.reserve(hprec_count * 4); // bounded over-allocation
  std::mt19937_64 rng;

  /// create scattered hprec allocations by interleaving with memory
  /// fragmentation
  for (size_t i = 0; i < hprec_count; ++i) {
    std::uniform_int_distribution<size_t> dist(0, 3);
    size_t mem_fragments = dist(rng);
    for (size_t j = 0; j < mem_fragments; ++j) {
      memory_fragmenters.push_back(std::make_unique<hprec_layout>());
    }

    /// create hazard pointer, which possibly allocates its hprec
    holders.emplace_back(make_hazard_pointer(domain));
  }

  return holders;
}

/// benchmark hazptr domain cleanup with various hprec-sequence sizes
static void hazptr_cleanup_empty_with_hprec_seq(
    size_t iters, size_t hprec_count) {
  BenchmarkSuspender braces;

  hazptr_domain<> domain;
  grow_scattered_hprec_list(domain, hprec_count);

  braces.dismissing([&] {
    while (iters--) {
      auto* obj = new TestObj(iters);
      obj->retire(domain);
      domain.cleanup(); // calls load_hazptr_vals on possibly-scattered hprecs
    }
  });
}

BENCHMARK_PARAM(hazptr_cleanup_empty_with_hprec_seq, 1)
BENCHMARK_PARAM(hazptr_cleanup_empty_with_hprec_seq, 4)
BENCHMARK_PARAM(hazptr_cleanup_empty_with_hprec_seq, 16)
BENCHMARK_PARAM(hazptr_cleanup_empty_with_hprec_seq, 64)
BENCHMARK_PARAM(hazptr_cleanup_empty_with_hprec_seq, 256)
BENCHMARK_PARAM(hazptr_cleanup_empty_with_hprec_seq, 1024)
BENCHMARK_PARAM(hazptr_cleanup_empty_with_hprec_seq, 4096)
BENCHMARK_PARAM(hazptr_cleanup_empty_with_hprec_seq, 16384)
BENCHMARK_PARAM(hazptr_cleanup_empty_with_hprec_seq, 65536)
BENCHMARK_PARAM(hazptr_cleanup_empty_with_hprec_seq, 262144)
BENCHMARK_PARAM(hazptr_cleanup_empty_with_hprec_seq, 1048576)

BENCHMARK_DRAW_LINE();

/// benchmark hazptr domain cleanup with sqrt(N) protected pointers
/// selected uniformly at random from N hprecs
static void hazptr_cleanup_sqrt_with_hprec_seq(
    size_t iters, size_t hprec_count) {
  BenchmarkSuspender braces;

  size_t protected_count = static_cast<size_t>(std::sqrt(hprec_count));

  /// create sqrt(N) objects to protect
  std::vector<std::unique_ptr<TestObj>> protected_objects;
  protected_objects.reserve(protected_count);
  for (size_t i = 0; i < protected_count; ++i) {
    protected_objects.push_back(std::make_unique<TestObj>(i));
  }

  /// select sqrt(N) holders uniformly at random to protect objects
  std::mt19937_64 rng;
  std::uniform_int_distribution<size_t> dist(0, hprec_count - 1);
  folly::F14FastSet<size_t> protected_indices;
  protected_indices.reserve(protected_count);
  while (protected_indices.size() < protected_count) {
    protected_indices.insert(dist(rng));
  }

  hazptr_domain<> domain;
  auto holders = grow_scattered_hprec_list(domain, hprec_count);

  /// protect the objects using the randomly selected holders
  for (auto [objidx, hpidx] : enumerate(protected_indices)) {
    std::atomic<TestObj*> ptr{protected_objects[objidx].get()};
    holders[hpidx].protect(ptr);
  }

  braces.dismissing([&] {
    while (iters--) {
      auto* obj = new TestObj(iters);
      obj->retire(domain);
      domain.cleanup(); // calls load_hazptr_vals on possibly-scattered hprecs
    }
  });
}

BENCHMARK_PARAM(hazptr_cleanup_sqrt_with_hprec_seq, 1)
BENCHMARK_PARAM(hazptr_cleanup_sqrt_with_hprec_seq, 4)
BENCHMARK_PARAM(hazptr_cleanup_sqrt_with_hprec_seq, 16)
BENCHMARK_PARAM(hazptr_cleanup_sqrt_with_hprec_seq, 64)
BENCHMARK_PARAM(hazptr_cleanup_sqrt_with_hprec_seq, 256)
BENCHMARK_PARAM(hazptr_cleanup_sqrt_with_hprec_seq, 1024)
BENCHMARK_PARAM(hazptr_cleanup_sqrt_with_hprec_seq, 4096)
BENCHMARK_PARAM(hazptr_cleanup_sqrt_with_hprec_seq, 16384)
BENCHMARK_PARAM(hazptr_cleanup_sqrt_with_hprec_seq, 65536)
BENCHMARK_PARAM(hazptr_cleanup_sqrt_with_hprec_seq, 262144)
BENCHMARK_PARAM(hazptr_cleanup_sqrt_with_hprec_seq, 1048576)

BENCHMARK_DRAW_LINE();

static void hazptr_obj_linked_reclaim_mutable_chain(
    size_t iters, size_t const objectCount) {
  BenchmarkSuspender braces;
  while (iters--) {
    hazptr_obj_cohort<> cohort;
    auto objects = makeLinkedBenchTree<true, 1>(objectCount, cohort);
    objects[0].retire();

    braces.dismissing([&] { cohort.shutdown_and_reclaim(); });
  }
}

BENCHMARK_PARAM(hazptr_obj_linked_reclaim_mutable_chain, 1)
BENCHMARK_PARAM(hazptr_obj_linked_reclaim_mutable_chain, 2)
BENCHMARK_PARAM(hazptr_obj_linked_reclaim_mutable_chain, 8)
BENCHMARK_PARAM(hazptr_obj_linked_reclaim_mutable_chain, 16)

BENCHMARK_DRAW_LINE();

static void do_hazptr_cleanup_retired(
    size_t iters, size_t const retired_count, size_t const hazard_count) {
  using ObjDeleter = folly::variadic_noop_fn;
  struct ObjBase {
    size_t value = 0;
  };
  struct Obj : ObjBase, folly::hazptr_obj_base<Obj, std::atomic, ObjDeleter> {};

  folly::BenchmarkSuspender braces;

  std::mt19937_64 rng;

  std::vector<Obj> objects;
  objects.reserve(retired_count);

  std::vector<size_t> indices;
  indices.resize(retired_count);
  std::iota(indices.begin(), indices.end(), 0);
  std::shuffle(indices.begin(), indices.end(), rng);

  hazptr_domain<> domain;

  std::vector<folly::hazptr_holder<>> holders;
  holders.reserve(hazard_count);
  for (size_t i = 0; i < hazard_count; ++i) {
    holders.emplace_back(folly::make_hazard_pointer(domain));
  }

  std::uniform_int_distribution<size_t> protected_dist(0, retired_count - 1);

  while (iters--) {
    // create N objects quickly, without new/delete
    objects.clear();
    for (size_t i = 0; i < retired_count; ++i) {
      objects.emplace_back(ObjBase{i});
    }

    // protect objects according to hazard_count
    for (size_t i = 0; i < hazard_count; ++i) {
      std::atomic<Obj*> channel{&objects[indices[protected_dist(rng)]]};
      std::ignore = holders[i].protect(channel);
    }

    // retire objects in a randomized order to simulate typical patterns
    for (size_t idx : indices) {
      objects[idx].retire(domain);
    }

    // objects are typically retired in writer threads and reclaimed in the
    // reclaimer thread, so they will not be in cache in the reclaimer thread;
    // simulate this by evicting the entire last-level cache (aka L3C)
    bm_llc_evict(iters);

    // measure a reclamation pass
    braces.dismissing([&] { //
      domain.cleanup();
    });

    // reclaim remaining objects that survived measured reclamation pass
    for (auto& holder : holders) {
      holder.reset_protection();
    }
    domain.cleanup();
  }
}

static void hazptr_cleanup_retired_zero_hazards(
    size_t iters, size_t const retired_count) {
  do_hazptr_cleanup_retired(iters, retired_count, 0);
}

BENCHMARK_PARAM(hazptr_cleanup_retired_zero_hazards, 1)
BENCHMARK_PARAM(hazptr_cleanup_retired_zero_hazards, 4)
BENCHMARK_PARAM(hazptr_cleanup_retired_zero_hazards, 16)
BENCHMARK_PARAM(hazptr_cleanup_retired_zero_hazards, 64)
BENCHMARK_PARAM(hazptr_cleanup_retired_zero_hazards, 256)
BENCHMARK_PARAM(hazptr_cleanup_retired_zero_hazards, 1024)
BENCHMARK_PARAM(hazptr_cleanup_retired_zero_hazards, 4096)
BENCHMARK_PARAM(hazptr_cleanup_retired_zero_hazards, 16384)
BENCHMARK_PARAM(hazptr_cleanup_retired_zero_hazards, 65536)
BENCHMARK_PARAM(hazptr_cleanup_retired_zero_hazards, 262144)
BENCHMARK_PARAM(hazptr_cleanup_retired_zero_hazards, 1048576)

static void hazptr_cleanup_retired_unit_hazards(
    size_t iters, size_t const retired_count) {
  do_hazptr_cleanup_retired(iters, retired_count, 1);
}

BENCHMARK_PARAM(hazptr_cleanup_retired_unit_hazards, 1)
BENCHMARK_PARAM(hazptr_cleanup_retired_unit_hazards, 4)
BENCHMARK_PARAM(hazptr_cleanup_retired_unit_hazards, 16)
BENCHMARK_PARAM(hazptr_cleanup_retired_unit_hazards, 64)
BENCHMARK_PARAM(hazptr_cleanup_retired_unit_hazards, 256)
BENCHMARK_PARAM(hazptr_cleanup_retired_unit_hazards, 1024)
BENCHMARK_PARAM(hazptr_cleanup_retired_unit_hazards, 4096)
BENCHMARK_PARAM(hazptr_cleanup_retired_unit_hazards, 16384)
BENCHMARK_PARAM(hazptr_cleanup_retired_unit_hazards, 65536)
BENCHMARK_PARAM(hazptr_cleanup_retired_unit_hazards, 262144)
BENCHMARK_PARAM(hazptr_cleanup_retired_unit_hazards, 1048576)

static void hazptr_cleanup_retired_sqrt_hazards(
    size_t iters, size_t const retired_count) {
  do_hazptr_cleanup_retired(
      iters, retired_count, static_cast<size_t>(std::sqrt(retired_count)));
}

BENCHMARK_PARAM(hazptr_cleanup_retired_sqrt_hazards, 1)
BENCHMARK_PARAM(hazptr_cleanup_retired_sqrt_hazards, 4)
BENCHMARK_PARAM(hazptr_cleanup_retired_sqrt_hazards, 16)
BENCHMARK_PARAM(hazptr_cleanup_retired_sqrt_hazards, 64)
BENCHMARK_PARAM(hazptr_cleanup_retired_sqrt_hazards, 256)
BENCHMARK_PARAM(hazptr_cleanup_retired_sqrt_hazards, 1024)
BENCHMARK_PARAM(hazptr_cleanup_retired_sqrt_hazards, 4096)
BENCHMARK_PARAM(hazptr_cleanup_retired_sqrt_hazards, 16384)
BENCHMARK_PARAM(hazptr_cleanup_retired_sqrt_hazards, 65536)
BENCHMARK_PARAM(hazptr_cleanup_retired_sqrt_hazards, 262144)
BENCHMARK_PARAM(hazptr_cleanup_retired_sqrt_hazards, 1048576)

int main(int argc, char* argv[]) {
  folly::gflags::ParseCommandLineFlags(&argc, &argv, true);
  runBenchmarks();
  return 0;
}
