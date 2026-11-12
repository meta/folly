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

#include <folly/concurrency/AtomicSharedPtr.h>

#include <array>
#include <atomic>
#include <barrier>
#include <memory>
#include <thread>

#include <folly/Portability.h>
#include <folly/concurrency/test/AtomicSharedPtrCounted.h>
#include <folly/portability/Config.h>
#include <folly/portability/GFlags.h>
#include <folly/portability/GTest.h>
#include <folly/synchronization/detail/AtomicUtils.h>
#include <folly/test/DeterministicSchedule.h>

using namespace folly;
using namespace folly::test;
using namespace std;
static int c_count{0};
static int d_count{0};

using DSched = DeterministicSchedule;

DEFINE_int64(seed, 0, "Seed for random number generators");
DEFINE_int32(num_threads, 32, "Number of threads");

struct foo {
  foo() { c_count++; }
  ~foo() { d_count++; }
};

TEST(AtomicSharedPtr, operators) {
  atomic_shared_ptr<int> fooptr;
#if FOLLY_HAS_ATOMIC_SHARED_PTR_HOOKED
  EXPECT_TRUE(fooptr.is_lock_free());
#endif
  auto i = new int(5);
  std::shared_ptr<int> s(i);
  fooptr.store(s);
  shared_ptr<int> bar(fooptr);
  EXPECT_TRUE(fooptr.compare_exchange_strong(s, nullptr));
  s.reset();
  bar.reset();
}

TEST(AtomicSharedPtr, exchange) {
  atomic_shared_ptr<int> fooptr;
  auto a = make_shared<int>(1);
  fooptr.store(std::move(a));
  auto b = fooptr.exchange(make_shared<int>());
  EXPECT_EQ(*b, 1);
}

TEST(AtomicSharedPtr, PointerBlockVarieties) {
  // shared_ptr(new T) control blocks must be read directly rather than
  // wrapped: wrapping breaks owner comparison, so a compare_exchange
  // against the original owner only succeeds when the block is read.
  atomic_shared_ptr<int> asp;
  auto check = [&](std::shared_ptr<int> s, int expect, bool direct) {
    asp.store(s);
    auto loaded = asp.load();
    EXPECT_EQ(loaded.get(), s.get());
    EXPECT_EQ(*loaded, expect);
    auto desired = std::make_shared<int>(expect + 1);
    if (direct) {
      EXPECT_TRUE(asp.compare_exchange_strong(s, desired));
    } else {
      asp.store(desired);
    }
    EXPECT_EQ(asp.load().get(), desired.get());
  };
  // NOLINTNEXTLINE(facebook-hte-SharedPtrFromNew)
  check(std::shared_ptr<int>(new int(1)), 1, true);
  check(std::shared_ptr<int>(new int(2), std::default_delete<int>()), 2, true);
  // Whether a block with a stateful deleter is read directly or wrapped
  // depends on the library's block layout; either way the value and the
  // deleter must survive.
  int tag = 0;
  auto deleter = [&tag](int* p) {
    ++tag;
    delete p;
  };
  check(std::shared_ptr<int>(new int(3), deleter), 3, false);
  EXPECT_EQ(tag, 1);
}

struct IncompleteType; // Deliberately never defined in this TU.

TEST(AtomicSharedPtr, IncompleteType) {
  atomic_shared_ptr<IncompleteType> asp;
  asp.store(std::shared_ptr<IncompleteType>{});
  EXPECT_EQ(asp.load(), nullptr);
}

#if FOLLY_HAS_ATOMIC_SHARED_PTR_HOOKED
TEST(AtomicSharedPtr, BlockDecodingIgnoresT) {
  // One atomic_shared_ptr<T> can be stored from a TU where T is complete and
  // loaded from one where it is only forward-declared, so a control block
  // must decode to the same object whatever T it is read as.
  struct Polymorphic {
    virtual ~Polymorphic() = default;
  };
  using internals = folly::detail::shared_ptr_internals;
  auto check = [](const std::shared_ptr<Polymorphic>& s) {
    auto* base = internals::get_counted_base(s);
    EXPECT_EQ(internals::get_shared_ptr<Polymorphic>(base), s.get());
    EXPECT_EQ(
        static_cast<const void*>(
            internals::get_shared_ptr<IncompleteType>(base)),
        s.get());
  };
  check(std::make_shared<Polymorphic>());
  // NOLINTNEXTLINE(facebook-hte-SharedPtrFromNew)
  check(std::shared_ptr<Polymorphic>(new Polymorphic()));
}
#endif

TEST(AtomicSharedPtr, OverAlignedMakeShared) {
  // An over-aligned make_shared object does not sit right after the control
  // block base; the store path must notice and wrap it.
  struct alignas(64) OverAligned {
    int value = 7;
  };
  atomic_shared_ptr<OverAligned> asp;
  auto s = std::make_shared<OverAligned>();
  asp.store(s);
  EXPECT_EQ(asp.load().get(), s.get());
  EXPECT_EQ(asp.load()->value, 7);
}

TEST(AtomicSharedPtr, AbstractType) {
  struct Base {
    virtual ~Base() = default;
    virtual int f() const = 0;
  };
  struct Derived : Base {
    int f() const override { return 7; }
  };
  // Abstract T can never name a make_shared block; stores must still work
  // and loads must unwrap. No compare_exchange here: wrapped values never
  // compare equal (pre-existing, both STLs).
  atomic_shared_ptr<Base> asp;
  auto s = std::shared_ptr<Base>(std::make_shared<Derived>());
  asp.store(s);
  EXPECT_EQ(asp.load()->f(), 7);
  // NOLINTNEXTLINE(facebook-hte-SharedPtrFromNew)
  auto s2 = std::shared_ptr<Base>(new Derived());
  auto prev = asp.exchange(s2);
  EXPECT_EQ(prev->f(), 7);
  EXPECT_EQ(asp.load()->f(), 7);
}

TEST(AtomicSharedPtr, foo) {
  c_count = 0;
  d_count = 0;
  {
    atomic_shared_ptr<foo> fooptr;
    fooptr.store(make_shared<foo>());
    EXPECT_EQ(1, c_count);
    EXPECT_EQ(0, d_count);
    {
      auto res = fooptr.load();
      EXPECT_EQ(1, c_count);
      EXPECT_EQ(0, d_count);
    }
    EXPECT_EQ(1, c_count);
    EXPECT_EQ(0, d_count);
  }
  EXPECT_EQ(1, c_count);
  EXPECT_EQ(1, d_count);
}

#if FOLLY_HAS_ATOMIC_SHARED_PTR_HOOKED

TEST(AtomicSharedPtr, counted) {
  c_count = 0;
  d_count = 0;
  {
    atomic_shared_ptr<foo, std::atomic, counted_ptr_internals<std::atomic>>
        fooptr;
    fooptr.store(make_counted<std::atomic, foo>());
    EXPECT_EQ(1, c_count);
    EXPECT_EQ(0, d_count);
    {
      auto res = fooptr.load();
      EXPECT_EQ(1, c_count);
      EXPECT_EQ(0, d_count);
    }
    EXPECT_EQ(1, c_count);
    EXPECT_EQ(0, d_count);
  }
  EXPECT_EQ(1, c_count);
  EXPECT_EQ(1, d_count);
}

TEST(AtomicSharedPtr, counted2) {
  auto foo = make_counted<std::atomic, bool>();
  atomic_shared_ptr<bool, std::atomic, counted_ptr_internals<std::atomic>>
      fooptr(foo);
  fooptr.store(foo);
  fooptr.load();
}

#endif

TEST(AtomicSharedPtr, ConstTest) {
  const auto a(std::make_shared<foo>());
  atomic_shared_ptr<foo> atom;
  atom.store(a);

  atomic_shared_ptr<const foo> catom;
}

// make_shared<T> blocks stored in atomic_shared_ptr<const T> must not be
// treated as aliased: compare_exchange has to match the stored block.
TEST(AtomicSharedPtr, ConstOfMakeSharedCompareExchange) {
  atomic_shared_ptr<const int> atom(std::make_shared<int>(0));
  for (int i = 0; i < 100; ++i) {
    auto expected = atom.load();
    EXPECT_TRUE(atom.compare_exchange_strong(
        expected, std::make_shared<int>(*expected + 1)));
  }
  EXPECT_EQ(*atom.load(), 100);
}

// Values stored through the aliased-wrapper path (here: an aliasing
// shared_ptr) must still compare equal to themselves in compare_exchange, and
// a failed comparison must not drop a reference.
TEST(AtomicSharedPtr, AliasedCompareExchange) {
  auto owner = std::make_shared<std::pair<int, int>>(0, 0);
  atomic_shared_ptr<int> atom(std::shared_ptr<int>(owner, &owner->second));
  for (int i = 0; i < 100; ++i) {
    auto expected = atom.load();
    auto next = std::make_shared<std::pair<int, int>>(0, *expected + 1);
    EXPECT_TRUE(atom.compare_exchange_strong(
        expected, std::shared_ptr<int>(next, &next->second)));
  }
  EXPECT_EQ(*atom.load(), 100);
}

TEST(AtomicSharedPtr, AliasingConstructorTest) {
  c_count = 0;
  d_count = 0;
  auto a = std::make_shared<foo>();
  auto b = new foo;
  auto alias = std::shared_ptr<foo>(a, b);

  atomic_shared_ptr<foo> asp;
  asp.store(alias);
  a.reset();
  alias.reset();
  auto res1 = asp.load();
  auto res2 = asp.exchange(nullptr);
  EXPECT_EQ(b, res1.get());
  EXPECT_EQ(b, res2.get());
  EXPECT_EQ(2, c_count);
  EXPECT_EQ(0, d_count);
  res1.reset();
  res2.reset();
  EXPECT_EQ(2, c_count);
  EXPECT_EQ(1, d_count);
  delete b;
  EXPECT_EQ(2, c_count);
  EXPECT_EQ(2, d_count);
}

TEST(AtomicSharedPtr, AliasingWithNoControlBlockConstructorTest) {
  long value = 0;
  atomic_shared_ptr<long> ptr{shared_ptr<long>{shared_ptr<long>{}, &value}};
  EXPECT_EQ(ptr.load().get(), &value);
}

TEST(AtomicSharedPtr, AliasingWithNullptrConstructorTest) {
  c_count = 0;
  d_count = 0;
  atomic_shared_ptr<foo> ptr{shared_ptr<foo>{std::make_shared<foo>(), nullptr}};
  EXPECT_EQ(ptr.load().get(), nullptr);
  // Verify that atomic_shared_ptr is holding the underlying object.
  EXPECT_EQ(d_count, 0);
  ptr.store({});
  EXPECT_EQ(d_count, 1);
}

#if FOLLY_HAS_ATOMIC_SHARED_PTR_HOOKED

TEST(AtomicSharedPtr, MaxPtrs) {
  shared_ptr<long> p(new long);
  constexpr size_t max_atomic_shared_ptrs = 262144;
  atomic_shared_ptr<long> ptrs[max_atomic_shared_ptrs];
  for (size_t i = 0; i < max_atomic_shared_ptrs - 1; i++) {
    ptrs[i].store(p);
  }
  atomic_shared_ptr<long> fail;
  EXPECT_DEATH(fail.store(p), "");
}

TEST(AtomicSharedPtr, DeterministicTest) {
  DSched sched(DSched::uniform(FLAGS_seed));

  auto foo = make_counted<DeterministicAtomic, bool>();
  atomic_shared_ptr<
      bool,
      DeterministicAtomic,
      counted_ptr_internals<DeterministicAtomic>>
      fooptr(foo);
  std::vector<std::thread> threads(FLAGS_num_threads);
  for (int tid = 0; tid < FLAGS_num_threads; ++tid) {
    threads[tid] = DSched::thread([&]() {
      for (int i = 0; i < 1000; i++) {
        auto l = fooptr.load();
        EXPECT_TRUE(l.get() != nullptr);
        fooptr.compare_exchange_strong(l, l);
        fooptr.store(make_counted<DeterministicAtomic, bool>());
        EXPECT_FALSE(fooptr.compare_exchange_strong(
            l, make_counted<DeterministicAtomic, bool>()));
      }
    });
  }
  for (auto& t : threads) {
    DSched::join(t);
  }
}

#endif

TEST(AtomicSharedPtr, StressTest) {
  constexpr size_t kExternalOffset = 0x2000;

  atomic_shared_ptr<bool> ptr;
  std::atomic<size_t> num_loads = 0;
  std::atomic<size_t> num_stores = 0;

  // DeterministicSchedule is too slow for the number of iterations required
  // here, and we need a test that exercises the native atomics.
  std::vector<std::thread> threads;
  for (int tid = 0; tid < FLAGS_num_threads; ++tid) {
    threads.emplace_back([&] {
      shared_ptr<bool> v;
      for (size_t i = 0; i < 16 * kExternalOffset; ++i) {
        // Each time we've gone through a few local -> global batches, replace
        // the pointer to contend with other load()s. This also does the first
        // initialization, and a few threads may see nullptr for a while.
        if (num_loads++ % (kExternalOffset * 2) == 0) {
          auto newv = std::make_shared<bool>();
          // Alternate between store and CAS.
          if (num_stores++ % 2 == 0) {
            ptr.store(std::move(newv), std::memory_order_acq_rel);
          } else {
            v = ptr.load(std::memory_order_relaxed);
            ptr.compare_exchange_strong(
                v,
                std::move(newv),
                std::memory_order_acq_rel,
                std::memory_order_relaxed);
          }
        }
        // Increments the local count and decrements the external one,
        // eventually forcing a batch transfer.
        v = ptr.load(std::memory_order_acquire);
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
}

#if FOLLY_HAS_ATOMIC_SHARED_PTR_HOOKED

namespace {
std::atomic<unsigned>& atomic_accesses() {
  static std::atomic<unsigned> count{0};
  return count;
}

template <typename T>
class access_counting_atomic : public std::atomic<T> {
  using Base = std::atomic<T>;

 public:
  using Base::Base;
  T load(std::memory_order order) const noexcept {
    ++atomic_accesses();
    return Base::load(order);
  }
  void store(T value, std::memory_order order = std::memory_order_seq_cst) {
    ++atomic_accesses();
    Base::store(value, order);
  }
  T exchange(T value, std::memory_order order) {
    ++atomic_accesses();
    return Base::exchange(value, order);
  }
  bool compare_exchange_weak(
      T& expected,
      T desired,
      std::memory_order success,
      std::memory_order failure) {
    ++atomic_accesses();
    return Base::compare_exchange_weak(expected, desired, success, failure);
  }
  bool compare_exchange_weak(T& expected, T desired, std::memory_order order) {
    const auto failure = folly::detail::default_failure_memory_order(order);
    return compare_exchange_weak(expected, desired, order, failure);
  }
};
} // namespace

TEST(AtomicSharedPtr, InjectedAtomicObservesAllLoads) {
  atomic_shared_ptr<int, access_counting_atomic> ptr;
  atomic_accesses().store(0);
  EXPECT_EQ(ptr.load().get(), nullptr);
  EXPECT_GT(atomic_accesses().load(), 0);
  ptr.store(std::make_shared<int>(7));
  atomic_accesses().store(0);
  EXPECT_EQ(*ptr.load(), 7);
  EXPECT_GT(atomic_accesses().load(), 1);
}

TEST(AtomicSharedPtr, DeterministicSharedPtrAliases) {
  DSched sched(DSched::uniform(FLAGS_seed));
  auto owner = std::make_shared<std::pair<int, int>>(7, 11);
  int value = 13;
  const std::array values{
      std::shared_ptr<int>(owner, &owner->first),
      std::shared_ptr<int>(owner, &owner->second),
      std::shared_ptr<int>(owner, nullptr),
      std::shared_ptr<int>(std::shared_ptr<int>{}, &value),
      std::shared_ptr<int>{},
  };
  atomic_shared_ptr<int, DeterministicAtomic> ptr(values[0]);
  std::vector<std::thread> threads;
  for (unsigned tid = 0; tid < 4; ++tid) {
    threads.emplace_back(DSched::thread([&, tid] {
      for (unsigned i = 0; i < 100; ++i) {
        auto loaded = ptr.load();
        if (loaded.get()) {
          EXPECT_TRUE(*loaded == 7 || *loaded == 11 || *loaded == 13);
        }
        if (loaded.get() == &value) {
          EXPECT_EQ(loaded.use_count(), 0);
        }
        ptr.compare_exchange_strong(loaded, values[(i + tid) % values.size()]);
      }
    }));
  }
  for (auto& thread : threads) {
    DSched::join(thread);
  }
  ptr.store({});
  EXPECT_EQ(owner.use_count(), 4);
}

#endif

TEST(AtomicSharedPtr, SameOwnerAliasBatchAccounting) {
  auto owner = std::make_shared<std::pair<int, int>>(7, 11);
  const std::array values{
      std::shared_ptr<int>(owner, &owner->first),
      std::shared_ptr<int>(owner, &owner->second),
  };
  {
    atomic_shared_ptr<int> ptr(values[0]);
    std::barrier start(3);
    std::atomic<unsigned> loads{0};
    auto reader = [&] {
      start.arrive_and_wait();
      for (unsigned i = 0; i < 65536; ++i) {
        loads.fetch_add(1, std::memory_order_relaxed);
        auto loaded = ptr.load(std::memory_order_acquire);
        EXPECT_TRUE(
            loaded.get() == &owner->first || loaded.get() == &owner->second);
        EXPECT_TRUE(*loaded == 7 || *loaded == 11);
      }
    };
    std::thread first(reader);
    std::thread second(reader);
    start.arrive_and_wait();
    for (unsigned i = 1; i < 32; ++i) {
      while (loads.load(std::memory_order_relaxed) < i * 4097) {
        std::this_thread::yield();
      }
      ptr.store(values[i & 1], std::memory_order_release);
    }
    first.join();
    second.join();
    ptr.store({});
  }
  EXPECT_EQ(owner.use_count(), 3);
}

TEST(AtomicSharedPtr, MixedRepresentationStress) {
  int ownerless_value = 13;
  std::atomic<unsigned> alive{0};
  std::atomic<unsigned> done{0};
  std::barrier start(6);
  atomic_shared_ptr<int> ptr;
  std::vector<std::thread> threads;
  auto make_owner = [&] {
    ++alive;
    return std::shared_ptr<std::pair<int, int>>(
        new std::pair<int, int>(7, 11), // NOLINT
        [&](auto* p) {
          delete p;
          --alive;
        });
  };
  for (unsigned writer = 0; writer < 2; ++writer) {
    threads.emplace_back([&, writer] {
      start.arrive_and_wait();
      for (unsigned i = 0; i < 16384; ++i) {
        switch ((i + writer) % 5) {
          case 0:
            ptr.store({});
            break;
          case 1:
            ptr.store(
                std::shared_ptr<int>(std::shared_ptr<int>{}, &ownerless_value));
            break;
          case 2: {
            auto owner = make_owner();
            ptr.store(std::shared_ptr<int>(owner, &owner->first));
            break;
          }
          case 3: {
            auto owner = make_owner();
            ptr.store(std::shared_ptr<int>(owner, &owner->second));
            break;
          }
          case 4:
            ptr.store(std::shared_ptr<int>(make_owner(), nullptr));
            break;
        }
      }
      ++done;
    });
  }
  for (unsigned reader = 0; reader < 4; ++reader) {
    threads.emplace_back([&] {
      start.arrive_and_wait();
      do {
        auto loaded = ptr.load(std::memory_order_acquire);
        if (loaded.get() == &ownerless_value) {
          EXPECT_EQ(loaded.use_count(), 0);
          EXPECT_EQ(*loaded, 13);
        } else if (loaded.get()) {
          EXPECT_GE(loaded.use_count(), 1);
          EXPECT_TRUE(*loaded == 7 || *loaded == 11);
        }
      } while (done.load() != 2);
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  ptr.store({});
  EXPECT_EQ(alive.load(), 0);
}

TEST(AtomicSharedPtr, Leak) {
  static auto& ptr = *new atomic_shared_ptr<int>();
  ptr.store(std::make_shared<int>(3), std::memory_order_relaxed);
  EXPECT_EQ(3, *ptr.load(std::memory_order_relaxed));
}
