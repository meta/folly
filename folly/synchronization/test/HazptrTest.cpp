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
#include <bit>
#include <cstdint>
#include <thread>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#include <iomanip>
#include <folly/Singleton.h>
#include <folly/portability/GFlags.h>
#include <folly/portability/GTest.h>
#include <folly/synchronization/HazptrThreadPoolExecutor.h>
#include <folly/synchronization/example/HazptrLockFreeLIFO.h>
#include <folly/synchronization/example/HazptrSWMRSet.h>
#include <folly/synchronization/example/HazptrWideCAS.h>
#include <folly/synchronization/test/Barrier.h>
#include <folly/test/DeterministicSchedule.h>

DEFINE_bool(bench, false, "run benchmark");
DEFINE_int64(num_reps, 10, "Number of test reps");
DEFINE_int32(num_threads, 6, "Number of threads");
DEFINE_int64(num_ops, 1003, "Number of ops or pairs of ops per rep");

using folly::default_hazptr_domain;
using folly::hazard_pointer;
using folly::hazard_pointer_clean_up;
using folly::hazard_pointer_default_domain;
using folly::hazard_pointer_obj_base;
using folly::hazptr_array;
using folly::hazptr_cleanup;
using folly::hazptr_domain;
using folly::hazptr_holder;
using folly::hazptr_local;
using folly::hazptr_obj_base;
using folly::hazptr_obj_base_linked;
using folly::hazptr_obj_cohort;
using folly::hazptr_retire;
using folly::hazptr_root;
using folly::hazptr_tc;
using folly::hazptr_tc_evict;
using folly::HazptrLockFreeLIFO;
using folly::HazptrSWMRSet;
using folly::HazptrWideCAS;
using folly::make_hazard_pointer;
using folly::make_hazard_pointer_array;
using folly::test::Barrier;
using folly::test::DeterministicAtomic;

using DSched = folly::test::DeterministicSchedule;

// Structures

/** Count */
class Count {
  std::atomic<int> ctors_{0};
  std::atomic<int> dtors_{0};
  std::atomic<int> retires_{0};

 public:
  void clear() noexcept {
    ctors_.store(0);
    dtors_.store(0);
    retires_.store(0);
  }

  int ctors() const noexcept { return ctors_.load(); }

  int dtors() const noexcept { return dtors_.load(); }

  int retires() const noexcept { return retires_.load(); }

  void inc_ctors() noexcept { ctors_.fetch_add(1); }

  void inc_dtors() noexcept { dtors_.fetch_add(1); }

  void inc_retires() noexcept { retires_.fetch_add(1); }
}; // Count

static Count c_;

/** Node */
template <template <typename> class Atom = std::atomic>
class Node : public hazptr_obj_base<Node<Atom>, Atom> {
  int val_;
  Atom<Node<Atom>*> next_;

 public:
  explicit Node(int v = 0, Node* n = nullptr, bool = false) noexcept
      : val_(v), next_(n) {
    c_.inc_ctors();
  }

  ~Node() { c_.inc_dtors(); }

  int value() const noexcept { return val_; }

  Node<Atom>* next() const noexcept {
    return next_.load(std::memory_order_acquire);
  }

  Atom<Node<Atom>*>* ptr_next() noexcept { return &next_; }
}; // Node

template <typename T, typename Func>
concept SupportsHazptrFilter = requires(
    hazptr_holder<std::atomic>& holder, T*& ptr, std::atomic<T*>& src, Func f) {
  holder.try_protect(ptr, src, f);
  holder.protect(src, f);
};

template <typename Src>
concept SupportsHazptrSource = requires(
    hazptr_holder<std::atomic>& holder, Node<>*& ptr, Src&& src) {
  holder.try_protect(ptr, static_cast<Src&&>(src));
};

struct NothrowPointerSource {
  Node<>* operator()() noexcept;
};

struct ThrowingPointerSource {
  Node<>* operator()();
};

struct MoveOnlyPointerSource {
  MoveOnlyPointerSource() = default;
  MoveOnlyPointerSource(const MoveOnlyPointerSource&) = delete;
  MoveOnlyPointerSource& operator=(const MoveOnlyPointerSource&) = delete;
  MoveOnlyPointerSource(MoveOnlyPointerSource&&) = default;
  MoveOnlyPointerSource& operator=(MoveOnlyPointerSource&&) = default;

  Node<>* operator()() noexcept;
};

struct NothrowPointerFilter {
  Node<>* operator()(Node<>*) const noexcept;
};

struct NothrowRvaluePointerFilter {
  Node<>* operator()(Node<>*&&) const noexcept;
};

struct NothrowLvaluePointerFilter {
  Node<>* operator()(Node<>*&) const noexcept;
};

struct ThrowingPointerFilter {
  Node<>* operator()(Node<>*) const;
};

struct MemberPointerNode : hazptr_obj_base<MemberPointerNode> {
  MemberPointerNode* link{nullptr};
};

static_assert(SupportsHazptrFilter<Node<>, NothrowPointerFilter>);
static_assert(SupportsHazptrFilter<Node<>, NothrowRvaluePointerFilter>);
static_assert(!SupportsHazptrFilter<Node<>, NothrowLvaluePointerFilter>);
static_assert(!SupportsHazptrFilter<Node<>, ThrowingPointerFilter>);
static_assert(!SupportsHazptrFilter< //
    MemberPointerNode,
    MemberPointerNode * MemberPointerNode::*>);
static_assert(SupportsHazptrSource<NothrowPointerSource>);
static_assert(!SupportsHazptrSource<ThrowingPointerSource>);
static_assert(SupportsHazptrSource<MoveOnlyPointerSource>);
static_assert(!SupportsHazptrSource<MoveOnlyPointerSource&>);

/** NodeRC */
template <bool Mutable, template <typename> class Atom = std::atomic>
class NodeRC : public hazptr_obj_base_linked<NodeRC<Mutable, Atom>, Atom> {
  Atom<NodeRC<Mutable, Atom>*> next_;
  int val_;

 public:
  explicit NodeRC(int v = 0, NodeRC* n = nullptr, bool acq = false) noexcept
      : next_(n), val_(v) {
    this->set_deleter();
    c_.inc_ctors();
    if (acq) {
      if (Mutable) {
        this->acquire_link_safe();
      } else {
        this->acquire_ref_safe();
      }
    }
  }

  ~NodeRC() { c_.inc_dtors(); }

  int value() const noexcept { return val_; }

  NodeRC<Mutable, Atom>* next() const noexcept {
    return next_.load(std::memory_order_acquire);
  }

  template <typename F>
  void for_each_link(bool m, F&& f) {
    if (Mutable == m) {
      f(next());
    }
  }
}; // NodeRC

/** List */
template <typename T, template <typename> class Atom = std::atomic>
struct List {
  Atom<T*> head_{nullptr};

 public:
  explicit List(int size) {
    auto p = head_.load(std::memory_order_relaxed);
    for (int i = 0; i < size - 1; ++i) {
      p = new T(i + 10000, p, true);
    }
    p = new T(size + 9999, p);
    head_.store(p, std::memory_order_relaxed);
  }

  ~List() {
    auto curr = head_.load(std::memory_order_relaxed);
    while (curr) {
      auto next = curr->next();
      curr->retire();
      curr = next;
    }
  }

  bool hand_over_hand(
      int val, hazptr_holder<Atom>* hptr_prev, hazptr_holder<Atom>* hptr_curr) {
    while (true) {
      auto prev = &head_;
      auto curr = prev->load(std::memory_order_acquire);
      while (true) {
        if (!curr) {
          return false;
        }
        if (!hptr_curr->try_protect(curr, *prev)) {
          break;
        }
        auto next = curr->next();
        if (prev->load(std::memory_order_acquire) != curr) {
          break;
        }
        if (curr->value() == val) {
          return true;
        }
        prev = curr->ptr_next();
        curr = next;
        std::swap(hptr_curr, hptr_prev);
      }
    }
  }

  bool hand_over_hand(int val) {
    hazptr_local<2, Atom> hptr;
    return hand_over_hand(val, &hptr[0], &hptr[1]);
  }

  bool protect_all(int val, hazptr_holder<Atom>& hptr) {
    auto curr = hptr.protect(head_);
    while (curr) {
      auto next = curr->next();
      if (curr->value() == val) {
        return true;
      }
      curr = next;
    }
    return false;
  }

  bool protect_all(int val) {
    hazptr_local<1, Atom> hptr;
    return protect_all(val, hptr[0]);
  }
}; // List

/** NodeAuto */
template <template <typename> class Atom = std::atomic>
class NodeAuto : public hazptr_obj_base_linked<NodeAuto<Atom>, Atom> {
  Atom<NodeAuto<Atom>*> link_[2];

 public:
  explicit NodeAuto(NodeAuto* l1 = nullptr, NodeAuto* l2 = nullptr) noexcept {
    this->set_deleter();
    link_[0].store(l1, std::memory_order_relaxed);
    link_[1].store(l2, std::memory_order_relaxed);
    c_.inc_ctors();
  }

  ~NodeAuto() { c_.inc_dtors(); }

  NodeAuto<Atom>* link(size_t i) {
    return link_[i].load(std::memory_order_acquire);
  }

  template <typename F>
  void for_each_link(bool m, F&& f) {
    if (m == false) { // Immutable
      for (int i = 0; i < 2; ++i) {
        auto p = link(i);
        if (p) {
          f(p);
        }
      }
    }
  }
}; // NodeAuto

// Test Functions

template <template <typename> class Atom = std::atomic>
void basic_objects_test() {
  c_.clear();
  int num = 0;
  {
    ++num;
    auto obj = new Node<Atom>;
    obj->retire();
  }
  {
    ++num;
    auto obj = new NodeRC<false, Atom>(0, nullptr);
    obj->retire();
  }
  {
    ++num;
    auto obj = new NodeRC<false, Atom>(0, nullptr);
    obj->acquire_link_safe();
    obj->unlink();
  }
  {
    ++num;
    auto obj = new NodeRC<false, Atom>(0, nullptr);
    obj->acquire_link_safe();
    obj->unlink_and_reclaim_unchecked();
  }
  {
    ++num;
    auto obj = new NodeRC<false, Atom>(0, nullptr);
    obj->acquire_link_safe();
    hazptr_root<NodeRC<false, Atom>> root(obj);
  }
  ASSERT_EQ(c_.ctors(), num);
  hazptr_cleanup<Atom>();
  ASSERT_EQ(c_.dtors(), num);
}

template <template <typename> class Atom = std::atomic>
void copy_and_move_test() {
  struct Obj : hazptr_obj_base<Obj, Atom> {
    int a;
  };

  auto p1 = new Obj();
  auto p2 = new Obj(*p1);
  p1->retire();
  p2->retire();

  p1 = new Obj();
  p2 = new Obj(std::move(*p1));
  p1->retire();
  p2->retire();

  p1 = new Obj();
  p2 = new Obj();
  *p2 = *p1;
  p1->retire();
  p2->retire();

  p1 = new Obj();
  p2 = new Obj();
  *p2 = std::move(*p1);
  p1->retire();
  p2->retire();
  hazptr_cleanup<Atom>();
}

template <template <typename> class Atom = std::atomic>
void basic_holders_test() {
  {
    hazptr_holder<Atom> h = make_hazard_pointer<Atom>();
  }
  {
    hazptr_array<2, Atom> h = make_hazard_pointer_array<2, Atom>();
  }
  {
    hazptr_local<2, Atom> h;
  }
}

template <template <typename> class Atom = std::atomic>
void basic_protection_test() {
  c_.clear();
  auto obj = new Node<Atom>;
  hazptr_holder<Atom> h = make_hazard_pointer<Atom>();
  h.reset_protection(obj);
  obj->retire();
  ASSERT_EQ(c_.ctors(), 1);
  hazptr_cleanup<Atom>();
  ASSERT_EQ(c_.dtors(), 0);
  h.reset_protection();
  hazptr_cleanup<Atom>();
  ASSERT_EQ(c_.dtors(), 1);
}

template <template <typename> class Atom = std::atomic>
void callable_source_try_protect_test() {
  c_.clear();
  auto first = new Node<Atom>(1);
  auto second = new Node<Atom>(2);
  auto* src = first;
  auto* ptr = first;

  hazptr_holder<Atom> h = make_hazard_pointer<Atom>();
  ASSERT_TRUE(h.try_protect(ptr, [&]() noexcept { return src; }));
  EXPECT_EQ(ptr, first);

  src = second;
  ASSERT_FALSE(h.try_protect(ptr, [&]() noexcept { return src; }));
  EXPECT_EQ(ptr, second);

  first->retire();
  hazptr_cleanup<Atom>();
  EXPECT_EQ(c_.dtors(), 1);

  ASSERT_TRUE(h.try_protect(ptr, [&]() noexcept { return src; }));
  second->retire();
  hazptr_cleanup<Atom>();
  EXPECT_EQ(c_.dtors(), 1);

  h.reset_protection();
  hazptr_cleanup<Atom>();
  EXPECT_EQ(c_.dtors(), 2);
}

void callable_source_by_value_test() {
  struct Source {
    Node<>* ptr;
    int calls{0};

    Node<>* operator()() noexcept {
      ++calls;
      return ptr;
    }
  };

  c_.clear();
  auto* obj = new Node<>;
  auto* ptr = obj;
  Source src{obj};

  hazptr_holder<std::atomic> h = make_hazard_pointer<std::atomic>();
  ASSERT_TRUE(h.try_protect(ptr, src));
  EXPECT_EQ(src.calls, 0);

  obj->retire();
  hazptr_cleanup();
  EXPECT_EQ(c_.dtors(), 0);
  h.reset_protection();
  hazptr_cleanup();
  EXPECT_EQ(c_.dtors(), 1);
}

template <template <typename> class Atom = std::atomic>
void tagged_pointer_try_protect_test() {
  static_assert(alignof(Node<Atom>) >= 2);
  c_.clear();
  auto* obj = new Node<Atom>;
  auto* const tagged = std::bit_cast<Node<Atom>*>(
      std::bit_cast<std::uintptr_t>(obj) | std::uintptr_t{1});
  auto untag = [](Node<Atom>* ptr) noexcept {
    return std::bit_cast<Node<Atom>*>(
        std::bit_cast<std::uintptr_t>(ptr) & ~std::uintptr_t{1});
  };
  Atom<Node<Atom>*> src{obj};
  auto* ptr = obj;

  hazptr_holder<Atom> h = make_hazard_pointer<Atom>();
  ASSERT_TRUE(h.try_protect(ptr, src, untag));

  src.store(tagged, std::memory_order_relaxed);
  ASSERT_FALSE(h.try_protect(ptr, src, untag));
  EXPECT_EQ(ptr, tagged);
  ASSERT_TRUE(h.try_protect(ptr, src, untag));

  obj->retire();
  hazptr_cleanup<Atom>();
  EXPECT_EQ(c_.dtors(), 0);
  h.reset_protection();
  hazptr_cleanup<Atom>();
  EXPECT_EQ(c_.dtors(), 1);
}

template <template <typename> class Atom = std::atomic>
void virtual_test() {
  struct Thing : public hazptr_obj_base<Thing, Atom> {
    virtual ~Thing() {}
    int a;
  };
  for (int i = 0; i < 100; i++) {
    auto bar = new Thing;
    bar->a = i;

    hazptr_holder<Atom> hptr = make_hazard_pointer<Atom>();
    hptr.reset_protection(bar);
    bar->retire();
    ASSERT_EQ(bar->a, i);
  }
  hazptr_cleanup<Atom>();
}

template <template <typename> class Atom = std::atomic>
void destruction_test(hazptr_domain<Atom>& domain) {
  struct Thing : public hazptr_obj_base<Thing, Atom> {
    Thing* next;
    hazptr_domain<Atom>* domain;
    int val;
    Thing(int v, Thing* n, hazptr_domain<Atom>* d)
        : next(n), domain(d), val(v) {}
    ~Thing() {
      if (next) {
        next->retire(*domain);
      }
    }
  };
  Thing* last{nullptr};
  for (int i = 0; i < 2000; i++) {
    last = new Thing(i, last, &domain);
  }
  last->retire(domain);
  hazptr_cleanup<Atom>(domain);
}

template <template <typename> class Atom = std::atomic>
void destruction_protected_test(hazptr_domain<Atom>& domain) {
  struct Rec;

  struct RecState {
    hazptr_domain<Atom>& domain;
    Atom<Rec*> cell{};
  };

  struct Rec : hazptr_obj_base<Rec, Atom> {
    int rem_;
    RecState& state_;

    Rec(int rem, RecState& state) : rem_{rem}, state_{state} {}
    ~Rec() { go(rem_, state_); }

    static void go(int rem, RecState& state) {
      if (rem) {
        auto p = new Rec(rem - 1, state);
        state.cell.store(p, std::memory_order_relaxed);
        auto h = make_hazard_pointer(state.domain);
        h.protect(state.cell);
        state.cell.store(nullptr, std::memory_order_relaxed);
        p->retire(state.domain);
      }
    }
  };

  RecState state{domain};
  Rec::go(2000, state);

  hazptr_cleanup<Atom>(domain);
}

template <template <typename> class Atom = std::atomic>
void move_test() {
  for (int i = 0; i < 100; ++i) {
    auto x = new Node<Atom>(i);
    hazptr_holder<Atom> hptr0 = make_hazard_pointer<Atom>();
    // Protect object
    hptr0.reset_protection(x);
    // Retire object
    x->retire();
    // Move constructor - still protected
    hazptr_holder<Atom> hptr1(std::move(hptr0));
    // Self move is no-op - still protected
    auto phptr1 = &hptr1;
    ASSERT_EQ(phptr1, &hptr1);
    hptr1 = std::move(*phptr1);
    // Empty constructor
    hazptr_holder<Atom> hptr2;
    // Move assignment - still protected
    hptr2 = std::move(hptr1);
    // Access object
    ASSERT_EQ(x->value(), i);
    // Unprotect object - hptr2 is nonempty
    hptr2.reset_protection();
  }
  hazptr_cleanup<Atom>();
}

template <template <typename> class Atom = std::atomic>
void array_test() {
  for (int i = 0; i < 100; ++i) {
    auto x = new Node<Atom>(i);
    hazptr_array<3, Atom> hptr = make_hazard_pointer_array<3, Atom>();
    // Protect object
    hptr[2].reset_protection(x);
    // Empty array
    hazptr_array<3, Atom> h;
    // Move assignment
    h = std::move(hptr);
    // Retire object
    x->retire();
    ASSERT_EQ(x->value(), i);
    // Unprotect object - hptr2 is nonempty
    h[2].reset_protection();
  }
  hazptr_cleanup<Atom>();
}

template <template <typename> class Atom = std::atomic>
void array_self_move_test() {
  c_.clear();
  {
    hazptr_array<3, Atom> h = make_hazard_pointer_array<3, Atom>();
    for (uint8_t i = 0; i < 3; ++i) {
      auto x = new Node<Atom>();
      h[i].reset_protection(x);
      x->retire();
    }
    // Self-move must leave h owning its hazard pointers. Going through a
    // pointer avoids -Wself-move.
    auto p = &h;
    h = std::move(*p);
    // The retired nodes are still protected, so cleanup must not reclaim them.
    hazptr_cleanup<Atom>();
    ASSERT_EQ(c_.dtors(), 0);
  }
  // h dtor unprotected the nodes, so cleanup can now reclaim them.
  hazptr_cleanup<Atom>();
  ASSERT_EQ(c_.dtors(), 3);
}

template <template <typename> class Atom = std::atomic>
void array_move_assign_nonempty_test() {
  c_.clear();
  {
    hazptr_array<3, Atom> x = make_hazard_pointer_array<3, Atom>();
    auto p = new Node<Atom>();
    x[0].reset_protection(p);
    p->retire();
    hazptr_array<3, Atom> y = make_hazard_pointer_array<3, Atom>();
    // x is nonempty, so move assignment must release x's hazard pointers,
    // which unprotects p, before taking y's.
    x = std::move(y);
    hazptr_cleanup<Atom>();
    ASSERT_EQ(c_.dtors(), 1);
  }
  hazptr_cleanup<Atom>();
}

template <template <typename> class Atom = std::atomic>
void array_move_assign_full_tc_test() {
#if FOLLY_HAZPTR_THR_LOCAL
  const uint8_t C = hazptr_tc<Atom>::min_capacity();
#endif
  hazptr_array<3, Atom> x = make_hazard_pointer_array<3, Atom>();
#if FOLLY_HAZPTR_THR_LOCAL
  {
    // v dtor fills the thread cache; then w dtor grows it past its base
    // capacity, so that the release below has to evict back to the domain.
    hazptr_holder<Atom> w = make_hazard_pointer<Atom>();
    hazptr_array<C, Atom> v = make_hazard_pointer_array<C, Atom>();
  }
#endif
  // x is nonempty, so move assignment takes the release path.
  x = make_hazard_pointer_array<3, Atom>();
}

template <template <typename> class Atom = std::atomic>
void array_dtor_full_tc_test() {
#if FOLLY_HAZPTR_THR_LOCAL
  const uint8_t M = hazptr_tc<Atom>::min_capacity();
#else
  const uint8_t M = 3;
#endif
  {
    // Fill the thread cache
    hazptr_array<M, Atom> w = make_hazard_pointer_array<M, Atom>();
  }
  {
    // Empty array x
    hazptr_array<M, Atom> x;
    {
      // y ctor gets elements from the thread cache filled by w dtor.
      hazptr_array<M, Atom> y = make_hazard_pointer_array<M, Atom>();
      // z ctor gets elements from the default domain.
      hazptr_array<M, Atom> z = make_hazard_pointer_array<M, Atom>();
      // Elements of y are moved to x.
      x = std::move(y);
      // z dtor fills the thread cache.
    }
    // x dtor finds the thread cache full. It has to call
    // ~hazptr_holder() for each of its elements, which were
    // previously taken from the thread cache by y ctor.
  }
}

template <template <typename> class Atom = std::atomic>
void local_test() {
  for (int i = 0; i < 100; ++i) {
    auto x = new Node<Atom>(i);
    hazptr_local<3, Atom> hptr;
    // Protect object
    hptr[2].reset_protection(x);
    // Retire object
    x->retire();
    // Unprotect object - hptr2 is nonempty
    hptr[2].reset_protection();
  }
  hazptr_cleanup<Atom>();
}

template <bool Mutable, template <typename> class Atom = std::atomic>
void linked_test() {
  c_.clear();
  NodeRC<Mutable, Atom>* p = nullptr;
  int num = 193;
  for (int i = 0; i < num - 1; ++i) {
    p = new NodeRC<Mutable, Atom>(i, p, true);
  }
  p = new NodeRC<Mutable, Atom>(num - 1, p, Mutable);
  hazptr_holder<Atom> hptr = make_hazard_pointer<Atom>();
  hptr.reset_protection(p);
  if (!Mutable) {
    for (auto q = p->next(); q; q = q->next()) {
      q->retire();
    }
  }
  int v = num;
  for (auto q = p; q; q = q->next()) {
    ASSERT_GT(v, 0);
    --v;
    ASSERT_EQ(q->value(), v);
  }

  hazptr_cleanup<Atom>();
  ASSERT_EQ(c_.ctors(), num);
  ASSERT_EQ(c_.dtors(), 0);

  if (Mutable) {
    hazptr_root<NodeRC<Mutable, Atom>, Atom> root(p);
  } else {
    p->retire();
  }
  hazptr_cleanup<Atom>();
  ASSERT_EQ(c_.dtors(), 0);

  hptr.reset_protection();
  hazptr_cleanup<Atom>();
  ASSERT_EQ(c_.dtors(), num);
}

template <bool Mutable, template <typename> class Atom = std::atomic>
void mt_linked_test() {
  c_.clear();

  Atom<bool> ready(false);
  Atom<bool> done(false);
  Atom<int> setHazptrs(0);
  hazptr_root<NodeRC<Mutable, Atom>, Atom> head;

  int num = FLAGS_num_ops;
  int nthr = FLAGS_num_threads;
  ASSERT_GT(FLAGS_num_threads, 0);
  std::vector<std::thread> thr(nthr);
  for (int i = 0; i < nthr; ++i) {
    thr[i] = DSched::thread([&] {
      while (!ready.load()) {
        /* spin */
      }
      hazptr_holder<Atom> hptr = make_hazard_pointer<Atom>();
      auto p = hptr.protect(head());
      ++setHazptrs;
      /* Concurrent with removal */
      int v = num;
      for (auto q = p; q; q = q->next()) {
        ASSERT_GT(v, 0);
        --v;
        ASSERT_EQ(q->value(), v);
      }
      ASSERT_EQ(v, 0);
      while (!done.load()) {
        /* spin */
      }
    });
  }

  NodeRC<Mutable, Atom>* p = nullptr;
  for (int i = 0; i < num - 1; ++i) {
    p = new NodeRC<Mutable, Atom>(i, p, true);
  }
  p = new NodeRC<Mutable, Atom>(num - 1, p, Mutable);
  ASSERT_EQ(c_.ctors(), num);
  head().store(p);
  ready.store(true);
  while (setHazptrs.load() < nthr) {
    /* spin */
  }

  /* this is concurrent with traversal by readers */
  head().store(nullptr);
  if (Mutable) {
    p->unlink();
  } else {
    for (auto q = p; q; q = q->next()) {
      q->retire();
    }
  }
  ASSERT_EQ(c_.dtors(), 0);
  done.store(true);

  for (auto& t : thr) {
    DSched::join(t);
  }

  hazptr_cleanup<Atom>();
  ASSERT_EQ(c_.dtors(), num);
}

template <template <typename> class Atom = std::atomic>
void auto_retire_test() {
  c_.clear();
  auto d = new NodeAuto<Atom>;
  d->acquire_link_safe();
  auto c = new NodeAuto<Atom>(d);
  d->acquire_link_safe();
  auto b = new NodeAuto<Atom>(d);
  c->acquire_link_safe();
  b->acquire_link_safe();
  auto a = new NodeAuto<Atom>(b, c);
  hazptr_holder<Atom> h = make_hazard_pointer<Atom>();
  {
    hazptr_root<NodeAuto<Atom>> root;
    a->acquire_link_safe();
    root().store(a);
    ASSERT_EQ(c_.ctors(), 4);
    /* So far the links and link counts are:
           root-->a  a-->b  a-->c  b-->d  c-->d
           a(1,0) b(1,0) c(1,0) d(2,0)
    */
    h.reset_protection(c); /* h protects c */
    hazptr_cleanup<Atom>();
    ASSERT_EQ(c_.dtors(), 0);
    /* Nothing is retired or reclaimed yet */
  }
  /* root dtor calls a->unlink, which calls a->release_link, which
     changes a's link counts from (1,0) to (0,0), which triggers calls
     to c->downgrade_link, b->downgrade_link, and a->retire.

     c->downgrade_link changes c's link counts from (1,0) to (0,1),
     which triggers calls to d->downgrade_link and c->retire.

     d->downgrade_link changes d's link counts from (2,0) to (1,1).

     b->downgrade_link changes b's link counts from (1,0) to (0,1),
     which triggers calls to d->downgrade_link and b->retire.

     d->downgrade_link changes d's link counts from (1,1) to (0,2),
     which triggers a call to d->retire.

     So far (assuming retire-s did not trigger bulk_reclaim):
           a-->b  a-->c  b-->d  c-->d
           a(0,0) b(0,1) c(0,1) d(0,2)
           Retired: a b c d
           Protected: c
  */
  hazptr_cleanup<Atom>();
  /* hazptr_cleanup calls bulk_reclaim which finds a, b, and d
     unprotected, which triggers calls to a->release_ref,
     b->release_ref, and d->release_ref (not necessarily in that
     order).

     a->release_ref finds a's link counts to be (0,0), which triggers
     calls to c->release_ref, b->release_ref and delete a.

     The call to c->release_ref changes its link counts from (0,1) to
     (0,0).

     The first call to b->release_ref changes b's link counts to
     (0,0). The second call finds the link counts to be (0,0), which
     triggers a call to d->release_ref and delete b.

     The first call to d->release_ref changes its link counts to
     (0,1), and the second call changes them to (0,0);

     So far:
           c-->d
           a(deleted) b(deleted) c(0,0) d(0,0)
           Retired and protected: c
           bulk_reclamed-ed (i.e, found not protected): d
  */
  ASSERT_EQ(c_.dtors(), 2);
  h.reset_protection(); /* c is now no longer protected */
  hazptr_cleanup<Atom>();
  /* hazptr_cleanup calls bulk_reclaim which finds c unprotected,
     which triggers a call to c->release_ref.

     c->release_ref finds c's link counts to be (0,0), which
     triggers calls to d->release_ref and delete c.

     d->release_ref finds d's link counts to be (0,0), which triggers
     a call to delete d.

     Finally:
           a(deleted) b(deleted) c(deleted) d(deleted)
  */
  ASSERT_EQ(c_.dtors(), 4);
}

template <template <typename> class Atom = std::atomic>
void free_function_retire_test() {
  auto foo = new int;
  hazptr_retire<Atom>(foo);
  auto foo2 = new int;
  hazptr_retire<Atom>(foo2, [](int* obj) { delete obj; });

  bool retired = false;
  {
    hazptr_domain<Atom> myDomain0;
    struct delret {
      bool* retired_;
      explicit delret(bool* retire) : retired_(retire) {}
      ~delret() { *retired_ = true; }
    };
    auto foo3 = new delret(&retired);
    myDomain0.retire(foo3);
  }
  ASSERT_TRUE(retired);
}

template <template <typename> class Atom = std::atomic>
void cleanup_test() {
  int threadOps = 1007;
  int mainOps = 19;
  c_.clear();
  Atom<int> threadsDone{0};
  Atom<bool> mainDone{false};
  std::vector<std::thread> threads(FLAGS_num_threads);
  for (int tid = 0; tid < FLAGS_num_threads; ++tid) {
    threads[tid] = DSched::thread([&, tid]() {
      for (int j = tid; j < threadOps; j += FLAGS_num_threads) {
        auto p = new Node<Atom>;
        p->retire();
      }
      threadsDone.fetch_add(1);
      while (!mainDone.load()) {
        /* spin */;
      }
    });
  }
  { // include the main thread in the test
    for (int i = 0; i < mainOps; ++i) {
      auto p = new Node<Atom>;
      p->retire();
    }
  }
  while (threadsDone.load() < FLAGS_num_threads) {
    /* spin */;
  }
  ASSERT_EQ(c_.ctors(), threadOps + mainOps);
  hazptr_cleanup<Atom>();
  ASSERT_EQ(c_.dtors(), threadOps + mainOps);
  mainDone.store(true);
  for (auto& t : threads) {
    DSched::join(t);
  }
  { // Cleanup after using array
    c_.clear();
    {
      hazptr_array<2, Atom> h = make_hazard_pointer_array<2, Atom>();
    }
    {
      hazptr_array<2, Atom> h = make_hazard_pointer_array<2, Atom>();
      auto p0 = new Node<Atom>;
      auto p1 = new Node<Atom>;
      h[0].reset_protection(p0);
      h[1].reset_protection(p1);
      p0->retire();
      p1->retire();
    }
    ASSERT_EQ(c_.ctors(), 2);
    hazptr_cleanup<Atom>();
    ASSERT_EQ(c_.dtors(), 2);
  }
  { // Cleanup after using local
    c_.clear();
    {
      hazptr_local<2, Atom> h;
    }
    {
      hazptr_local<2, Atom> h;
      auto p0 = new Node<Atom>;
      auto p1 = new Node<Atom>;
      h[0].reset_protection(p0);
      h[1].reset_protection(p1);
      p0->retire();
      p1->retire();
    }
    ASSERT_EQ(c_.ctors(), 2);
    hazptr_cleanup<Atom>();
    ASSERT_EQ(c_.dtors(), 2);
  }
}

template <template <typename> class Atom = std::atomic>
void priv_dtor_test() {
  c_.clear();
  using NodeT = NodeRC<true, Atom>;
  auto y = new NodeT;
  y->acquire_link_safe();
  struct Foo : hazptr_obj_base<Foo, Atom> {
    hazptr_root<NodeT, Atom> r_;
  };
  auto x = new Foo;
  x->r_().store(y);
  /* Thread retires x. Dtor of TLS priv list pushes x to domain, which
     triggers bulk reclaim due to timed cleanup (when the test is run
     by itself). Reclamation of x unlinks and retires y. y should
     not be pushed into the thread's priv list. It should be pushed to
     domain instead. */
  auto thr = DSched::thread([&]() { x->retire(); });
  DSched::join(thr);
  ASSERT_EQ(c_.ctors(), 1);
  hazptr_cleanup<Atom>();
  ASSERT_EQ(c_.dtors(), 1);
}

template <template <typename> class Atom = std::atomic>
void cohort_test() {
  int num = 10001;
  using NodeT = Node<Atom>;
  c_.clear();
  {
    hazptr_obj_cohort<Atom> cohort;
    auto thr = DSched::thread([&]() {
      for (int i = 0; i < num; ++i) {
        auto p = new NodeT;
        p->set_cohort_no_tag(&cohort);
        p->retire();
      }
    });
    DSched::join(thr);
  }
  ASSERT_EQ(c_.ctors(), num);
  //  ASSERT_GT(c_.dtors(), 0);
  hazptr_cleanup<Atom>();
  c_.clear();
  {
    hazptr_obj_cohort<Atom> cohort;
    auto thr = DSched::thread([&]() {
      for (int i = 0; i < num; ++i) {
        auto p = new NodeT;
        p->set_cohort_tag(&cohort);
        p->retire();
      }
    });
    DSched::join(thr);
  }
  ASSERT_EQ(c_.ctors(), num);
  ASSERT_GT(c_.dtors(), 0);
  hazptr_cleanup<Atom>();
}

template <template <typename> class Atom = std::atomic>
void cohort_recursive_destruction_test() {
  struct Foo : public hazptr_obj_base<Foo, Atom> {
    hazptr_obj_cohort<Atom> cohort_;
    Foo* foo_{nullptr};
    explicit Foo(hazptr_obj_cohort<Atom>* b) {
      this->set_cohort_tag(b);
      c_.inc_ctors();
    }
    ~Foo() {
      set(nullptr);
      c_.inc_dtors();
    }
    void set(Foo* foo) {
      if (foo_) {
        foo_->retire();
      }
      foo_ = foo;
    }
    hazptr_obj_cohort<Atom>* cohort() { return &cohort_; }
  };

  int num1 = 101;
  int num2 = 42;
  int nthr = FLAGS_num_threads;
  c_.clear();
  std::vector<std::thread> threads(nthr);
  for (int tid = 0; tid < nthr; ++tid) {
    threads[tid] = DSched::thread([&, tid]() {
      hazptr_obj_cohort<Atom> b0;
      Foo* foo0 = new Foo(&b0);
      for (int i = tid; i < num1; i += nthr) {
        Foo* foo1 = new Foo(foo0->cohort());
        foo0->set(foo1);
        for (int j = 0; j < num2; ++j) {
          foo1->set(new Foo(foo1->cohort()));
        }
      }
      foo0->retire();
    });
  }
  for (auto& t : threads) {
    DSched::join(t);
  }
  int total = nthr + num1 + num1 * num2;
  ASSERT_EQ(c_.ctors(), total);
  ASSERT_EQ(c_.dtors(), total);
}

// Used in cohort_safe_list_children_test
struct NodeA : hazptr_obj_base_linked<NodeA> {
  std::atomic<NodeA*> next_{nullptr};
  int& sum_;
  int val_;

  NodeA(hazptr_obj_cohort<>& coh, int& sum, int v = 0) : sum_(sum), val_(v) {
    this->set_cohort_tag(&coh);
  }
  ~NodeA() { sum_ += val_; }
  void set_next(NodeA* ptr) { next_.store(ptr); }
  template <typename F>
  void for_each_link(bool m, F&& f) {
    if (m) {
      auto p = next_.load();
      if (p) {
        f(p);
      }
    }
  }
};

void cohort_safe_list_children_test() {
  int sum = 0;
  hazptr_obj_cohort<> cohort;
  NodeA* p1 = new NodeA(cohort, sum, 1000);
  NodeA* p2 = new NodeA(cohort, sum, 2000);
  p2->acquire_link_safe();
  p1->set_next(p2); // p2 is p1's child
  hazard_pointer<> h = make_hazard_pointer<>();
  h.reset_protection(p2);
  /* When p1 is retired, it is inserted into cohort, then pushed into
     the domain's tagged list, then when p1 is found unprotected by
     hazard pointers it will be pushed into cohort's safe list. When
     p1 is reclaimed, p2 (p1's child) will be automatically retired to
     the domain's tagged list. */
  p1->retire();
  /* Retire enough nodes to invoke asynchronous reclamation until p1
     and/or p2 are reclaimed. */
  while (sum == 0) {
    NodeA* p = new NodeA(cohort, sum);
    p->retire();
  }
  /* At this point p1 must be already reclaimed but not p2 */
  DCHECK_EQ(sum, 1000);
  NodeA* p3 = new NodeA(cohort, sum, 3000);
  p3->retire();
  /* Retire more nodes to invoke asynchronous reclamation until p3
     and/or p2 are reclaimed. */
  while (sum == 1000) {
    NodeA* p = new NodeA(cohort, sum);
    p->retire();
  }
  /* At this point p3 must be already reclaimed but not p2 */
  DCHECK_EQ(sum, 4000);
}

/* Gives the default domain a blank slate, and runs passes inline
   rather than in its executor. hazptr_cleanup skips the tagged shards
   that a concurrent pass holds locked, but waits for every pass
   scheduled to the executor, which runs nothing else. So the first
   call drains the executor and the second finds no concurrent pass. */
struct InlineReclamationGuard {
  InlineReclamationGuard() {
    default_hazptr_domain().clear_executor();
    hazptr_cleanup();
    hazptr_cleanup();
  }
  ~InlineReclamationGuard() { folly::enable_hazptr_thread_pool_executor(); }
};

void cohort_reregister_test() {
  InlineReclamationGuard guard;
  c_.clear();
  hazard_pointer<> h = make_hazard_pointer<>();
  {
    hazptr_obj_cohort<> cohort;
    auto retire = [&](Node<>* p) {
      p->set_cohort_tag(&cohort);
      p->retire();
    };
    /* The cohort pushes its tagged objects to the domain in batches
       of 20. The pass leaves the cohort with no tagged objects in the
       domain. The next batch includes a protected object, which the
       next pass leaves in the domain. */
    for (int i = 0; i < 20; ++i) {
      retire(new Node<>);
    }
    hazptr_cleanup();
    auto p = new Node<>;
    h.reset_protection(p);
    retire(p); // reclaims the safe list
    ASSERT_EQ(c_.dtors(), 20);
    for (int i = 1; i < 20; ++i) {
      retire(new Node<>);
    }
    hazptr_cleanup();
    retire(new Node<>); // reclaims the safe list
    ASSERT_EQ(c_.dtors(), 39);
    h.reset_protection();
  }
  ASSERT_EQ(c_.dtors(), 41);
}

void cohort_many_lists_test() {
  /* More cohorts than domain shards, with different numbers of
     tagged objects in the domain, half with a protected object. */
  int num = 12;
  InlineReclamationGuard guard;
  c_.clear();
  std::vector<hazard_pointer<>> hs;
  {
    std::vector<hazptr_obj_cohort<>> cohorts(num);
    int retired = 0;
    for (int i = 0; i < num; ++i) {
      int n = 20 * (1 + i % 4); // whole batches, all pushed to the domain
      for (int j = 0; j < n; ++j) {
        auto p = new Node<>;
        if (i % 2 == 0 && j == n / 2) {
          hs.push_back(make_hazard_pointer<>());
          hs.back().reset_protection(p);
        }
        p->set_cohort_tag(&cohorts[i]);
        p->retire();
      }
      retired += n;
    }
    hazptr_cleanup();
    for (auto& cohort : cohorts) {
      auto p = new Node<>;
      p->set_cohort_tag(&cohort);
      p->retire(); // reclaims the safe list
    }
    ASSERT_EQ(c_.dtors(), retired - num / 2);
    for (auto& h : hs) {
      h.reset_protection();
    }
  }
  ASSERT_EQ(c_.dtors(), c_.ctors());
}

void cohort_concurrent_lifecycle_test() {
  /* Cohorts are destroyed while other threads run passes, which may
     find their objects protected and push them back. */
  InlineReclamationGuard guard;
  c_.clear();
  std::atomic<bool> stop{false};
  std::thread cleaner([&] {
    while (!stop.load()) {
      hazptr_cleanup();
    }
  });
  std::vector<std::thread> threads(FLAGS_num_threads);
  for (int tid = 0; tid < FLAGS_num_threads; ++tid) {
    threads[tid] = std::thread([&, tid] {
      hazard_pointer<> h = make_hazard_pointer<>();
      for (int i = tid; i < FLAGS_num_ops; i += FLAGS_num_threads) {
        hazptr_obj_cohort<> cohort;
        int n = 1 + i % 61; // up to three batches of 20
        for (int j = 0; j < n; ++j) {
          auto p = new Node<>;
          if (j == n / 2) {
            h.reset_protection(p);
          }
          p->set_cohort_tag(&cohort);
          p->retire();
        }
        h.reset_protection();
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  stop.store(true);
  cleaner.join();
  ASSERT_EQ(c_.dtors(), c_.ctors());
}

#ifndef _WIN32

void fork_test() {
  folly::enable_hazptr_thread_pool_executor();
  auto trigger_reclamation = [] {
    hazptr_obj_cohort b;
    for (int i = 0; i < 2001; ++i) {
      auto p = new Node;
      p->set_cohort_no_tag(&b);
      p->retire();
    }
  };
  std::thread t1(trigger_reclamation);
  t1.join();
  folly::SingletonVault::singleton()->destroyInstances();
  auto pid = fork();
  folly::SingletonVault::singleton()->reenableInstances();
  if (pid > 0) {
    // parent
    int status = -1;
    auto pid2 = waitpid(pid, &status, 0);
    EXPECT_EQ(status, 0);
    EXPECT_EQ(pid, pid2);
    trigger_reclamation();
  } else if (pid == 0) {
    // child
    c_.clear();
    std::thread t2(trigger_reclamation);
    t2.join();
    exit(0); // Do not print gtest results
  } else {
    PLOG(FATAL) << "Failed to fork()";
  }
}

#endif

template <template <typename> class Atom = std::atomic>
void lifo_test() {
  for (int i = 0; i < FLAGS_num_reps; ++i) {
    Atom<int> sum{0};
    HazptrLockFreeLIFO<int, Atom> s;
    std::vector<std::thread> threads(FLAGS_num_threads);
    for (int tid = 0; tid < FLAGS_num_threads; ++tid) {
      threads[tid] = DSched::thread([&, tid]() {
        int local = 0;
        for (int j = tid; j < FLAGS_num_ops; j += FLAGS_num_threads) {
          s.push(j);
          int v;
          ASSERT_TRUE(s.pop(v));
          local += v;
        }
        sum.fetch_add(local);
      });
    }
    for (auto& t : threads) {
      DSched::join(t);
    }
    hazptr_cleanup<Atom>();
    int expected = FLAGS_num_ops * (FLAGS_num_ops - 1) / 2;
    ASSERT_EQ(sum.load(), expected);
  }
}

template <template <typename> class Atom = std::atomic>
void swmr_test() {
  using T = uint64_t;
  for (int i = 0; i < FLAGS_num_reps; ++i) {
    HazptrSWMRSet<T, Atom> s;
    std::vector<std::thread> threads(FLAGS_num_threads);
    for (int tid = 0; tid < FLAGS_num_threads; ++tid) {
      threads[tid] = DSched::thread([&s, tid]() {
        for (int j = tid; j < FLAGS_num_ops; j += FLAGS_num_threads) {
          s.contains(j);
        }
      });
    }
    for (int j = 0; j < 10; ++j) {
      s.add(j);
    }
    for (int j = 0; j < 10; ++j) {
      s.remove(j);
    }
    for (auto& t : threads) {
      DSched::join(t);
    }
    hazptr_cleanup<Atom>();
  }
}

template <template <typename> class Atom = std::atomic>
void wide_cas_test() {
  HazptrWideCAS<std::string, Atom> s;
  std::string u;
  std::string v = "11112222";
  auto ret = s.cas(u, v);
  ASSERT_TRUE(ret);
  u = "";
  v = "11112222";
  ret = s.cas(u, v);
  ASSERT_FALSE(ret);
  u = "11112222";
  v = "22223333";
  ret = s.cas(u, v);
  ASSERT_TRUE(ret);
  u = "22223333";
  v = "333344445555";
  ret = s.cas(u, v);
  ASSERT_TRUE(ret);
  hazptr_cleanup<Atom>();
}

class HazptrPreInitTest : public testing::Test {
 private:
  // pre-init to avoid deadlock when using DeterministicAtomic
  hazptr_domain<DeterministicAtomic>& defaultDomainHelper_{
      folly::default_hazptr_domain<DeterministicAtomic>()};
};

// Tests

TEST(HazptrTest, basic_objects) {
  basic_objects_test();
}

TEST_F(HazptrPreInitTest, dsched_basic_objects) {
  DSched sched(DSched::uniform(0));
  basic_objects_test<DeterministicAtomic>();
}

TEST(HazptrTest, copy_and_move) {
  copy_and_move_test();
}

TEST_F(HazptrPreInitTest, dsched_copy_and_move) {
  DSched sched(DSched::uniform(0));
  copy_and_move_test<DeterministicAtomic>();
}

TEST(HazptrTest, basic_holders) {
  basic_holders_test();
}

TEST_F(HazptrPreInitTest, dsched_basic_holders) {
  DSched sched(DSched::uniform(0));
  basic_holders_test<DeterministicAtomic>();
}

TEST(HazptrTest, basic_protection) {
  basic_protection_test();
}

TEST(HazptrTest, callable_source_try_protect) {
  callable_source_try_protect_test();
}

TEST(HazptrTest, callable_source_by_value) {
  callable_source_by_value_test();
}

TEST(HazptrTest, tagged_pointer_try_protect) {
  tagged_pointer_try_protect_test();
}

TEST_F(HazptrPreInitTest, dsched_basic_protection) {
  DSched sched(DSched::uniform(0));
  basic_protection_test<DeterministicAtomic>();
}

TEST_F(HazptrPreInitTest, dsched_callable_source_try_protect) {
  DSched sched(DSched::uniform(0));
  callable_source_try_protect_test<DeterministicAtomic>();
}

TEST_F(HazptrPreInitTest, dsched_tagged_pointer_try_protect) {
  DSched sched(DSched::uniform(0));
  tagged_pointer_try_protect_test<DeterministicAtomic>();
}

TEST(HazptrTest, virtual) {
  virtual_test();
}

TEST_F(HazptrPreInitTest, dsched_virtual) {
  DSched sched(DSched::uniform(0));
  virtual_test<DeterministicAtomic>();
}

TEST(HazptrTest, destruction) {
  {
    hazptr_domain<> myDomain0;
    destruction_test(myDomain0);
  }
  destruction_test(default_hazptr_domain<std::atomic>());
}

TEST_F(HazptrPreInitTest, dsched_destruction) {
  DSched sched(DSched::uniform(0));
  {
    hazptr_domain<DeterministicAtomic> myDomain0;
    destruction_test<DeterministicAtomic>(myDomain0);
  }
  destruction_test<DeterministicAtomic>(
      default_hazptr_domain<DeterministicAtomic>());
}

TEST(HazptrTest, destruction_protected) {
  {
    hazptr_domain<> myDomain0;
    destruction_protected_test(myDomain0);
  }
  destruction_protected_test(default_hazptr_domain<std::atomic>());
}

TEST_F(HazptrPreInitTest, dsched_destruction_protected) {
  DSched sched(DSched::uniform(0));
  {
    hazptr_domain<DeterministicAtomic> myDomain0;
    destruction_protected_test<DeterministicAtomic>(myDomain0);
  }
  destruction_protected_test<DeterministicAtomic>(
      default_hazptr_domain<DeterministicAtomic>());
}

TEST(HazptrTest, move) {
  move_test();
}

TEST_F(HazptrPreInitTest, dsched_move) {
  DSched sched(DSched::uniform(0));
  move_test<DeterministicAtomic>();
}

TEST(HazptrTest, array) {
  array_test();
}

TEST_F(HazptrPreInitTest, dsched_array) {
  DSched sched(DSched::uniform(0));
  array_test<DeterministicAtomic>();
}

TEST(HazptrTest, array_self_move) {
  array_self_move_test();
}

TEST_F(HazptrPreInitTest, dsched_array_self_move) {
  DSched sched(DSched::uniform(0));
  array_self_move_test<DeterministicAtomic>();
}

TEST(HazptrTest, array_move_assign_nonempty) {
  array_move_assign_nonempty_test();
}

TEST_F(HazptrPreInitTest, dsched_array_move_assign_nonempty) {
  DSched sched(DSched::uniform(0));
  array_move_assign_nonempty_test<DeterministicAtomic>();
}

TEST(HazptrTest, array_move_assign_full_tc) {
  array_move_assign_full_tc_test();
}

TEST(HazptrTest, array_dtor_full_tc) {
  array_dtor_full_tc_test();
}

TEST_F(HazptrPreInitTest, dsched_array_dtor_full_tc) {
  DSched sched(DSched::uniform(0));
  array_dtor_full_tc_test<DeterministicAtomic>();
}

TEST(HazptrTest, local) {
  local_test();
}

TEST_F(HazptrPreInitTest, dsched_local) {
  DSched sched(DSched::uniform(0));
  local_test<DeterministicAtomic>();
}

TEST(HazptrTest, linked_mutable) {
  linked_test<true>();
}

TEST_F(HazptrPreInitTest, dsched_linked_mutable) {
  DSched sched(DSched::uniform(0));
  linked_test<true, DeterministicAtomic>();
}

TEST(HazptrTest, linked_immutable) {
  linked_test<false>();
}

TEST_F(HazptrPreInitTest, dsched_linked_immutable) {
  DSched sched(DSched::uniform(0));
  linked_test<false, DeterministicAtomic>();
}

TEST(HazptrTest, mt_linked_mutable) {
  mt_linked_test<true>();
}

TEST_F(HazptrPreInitTest, dsched_mt_linked_mutable) {
  DSched sched(DSched::uniform(0));
  mt_linked_test<true, DeterministicAtomic>();
}

TEST(HazptrTest, mt_linked_immutable) {
  mt_linked_test<false>();
}

TEST_F(HazptrPreInitTest, dsched_mt_linked_immutable) {
  DSched sched(DSched::uniform(0));
  mt_linked_test<false, DeterministicAtomic>();
}

TEST(HazptrTest, auto_retire) {
  auto_retire_test();
}

TEST_F(HazptrPreInitTest, dsched_auto_retire) {
  DSched sched(DSched::uniform(0));
  auto_retire_test<DeterministicAtomic>();
}

TEST(HazptrTest, free_function_retire) {
  free_function_retire_test();
}

TEST_F(HazptrPreInitTest, dsched_free_function_retire) {
  DSched sched(DSched::uniform(0));
  free_function_retire_test<DeterministicAtomic>();
}

TEST(HazptrTest, cleanup) {
  cleanup_test();
}

TEST_F(HazptrPreInitTest, dsched_cleanup) {
  DSched sched(DSched::uniform(0));
  cleanup_test<DeterministicAtomic>();
}

TEST(HazptrTest, priv_dtor) {
  priv_dtor_test();
}

TEST_F(HazptrPreInitTest, dsched_priv_dtor) {
  DSched sched(DSched::uniform(0));
  priv_dtor_test<DeterministicAtomic>();
}

TEST(HazptrTest, cohort) {
  cohort_test();
}

TEST(HazptrTest, dsched_cohort) {
  DSched sched(DSched::uniform(0));
  cohort_test<DeterministicAtomic>();
}

TEST(HazptrTest, cohort_recursive_destruction) {
  cohort_recursive_destruction_test();
}

TEST(HazptrTest, dsched_cohort_recursive_destruction) {
  cohort_recursive_destruction_test<DeterministicAtomic>();
}

TEST(HazptrTest, cohort_safe_list_children) {
  cohort_safe_list_children_test();
}

TEST(HazptrTest, cohort_reregister) {
  cohort_reregister_test();
}

TEST(HazptrTest, cohort_many_lists) {
  cohort_many_lists_test();
}

TEST(HazptrTest, cohort_concurrent_lifecycle) {
  cohort_concurrent_lifecycle_test();
}

#ifndef _WIN32

TEST(HazptrTest, fork) {
  fork_test();
}

#endif

TEST(HazptrTest, lifo) {
  lifo_test();
}

TEST_F(HazptrPreInitTest, dsched_lifo) {
  DSched sched(DSched::uniform(0));
  lifo_test<DeterministicAtomic>();
}

TEST(HazptrTest, swmr) {
  swmr_test();
}

TEST_F(HazptrPreInitTest, dsched_swmr) {
  DSched sched(DSched::uniform(0));
  swmr_test<DeterministicAtomic>();
}

TEST(HazptrTest, wide_cas) {
  wide_cas_test();
}

TEST_F(HazptrPreInitTest, dsched_wide_cas) {
  DSched sched(DSched::uniform(0));
  wide_cas_test<DeterministicAtomic>();
}

TEST(HazptrTest, reclamation_without_calling_cleanup) {
  c_.clear();
  int nthr = 5;
  int objs = folly::detail::hazptr_domain_rcount_threshold();
  std::vector<std::thread> thr(nthr);
  for (int tid = 0; tid < nthr; ++tid) {
    thr[tid] = std::thread([&, tid] {
      for (int i = tid; i < objs; i += nthr) {
        auto p = new Node<>;
        p->retire();
      }
    });
  }
  for (auto& t : thr) {
    t.join();
  }
  while (c_.dtors() == 0) {
    /* Wait for asynchronous reclamation. */;
  }
  ASSERT_GT(c_.dtors(), 0);
}

TEST(HazptrTest, standard_names) {
  struct Foo : hazard_pointer_obj_base<Foo> {};
  DCHECK_EQ(&hazard_pointer_default_domain<>(), &default_hazptr_domain<>());
  hazard_pointer<> h = make_hazard_pointer();
  hazard_pointer_clean_up<>();
}

// Benchmark drivers

template <typename InitFunc, typename Func, typename EndFunc>
uint64_t run_once(
    int nthreads, const InitFunc& init, const Func& fn, const EndFunc& endFn) {
  Barrier b(nthreads + 1);
  init();
  std::vector<std::thread> threads(nthreads);
  for (int tid = 0; tid < nthreads; ++tid) {
    threads[tid] = std::thread([&, tid] {
      b.wait();
      b.wait();
      fn(tid);
    });
  }
  b.wait();
  // begin time measurement
  auto tbegin = std::chrono::steady_clock::now();
  b.wait();
  for (auto& t : threads) {
    t.join();
  }
  hazptr_cleanup();
  // end time measurement
  auto tend = std::chrono::steady_clock::now();
  endFn();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(tend - tbegin)
      .count();
}

template <typename RepFunc>
uint64_t bench(std::string name, int ops, const RepFunc& repFn) {
  int reps = 10;
  uint64_t min = UINTMAX_MAX;
  uint64_t max = 0;
  uint64_t sum = 0;

  repFn(); // sometimes first run is outlier
  for (int r = 0; r < reps; ++r) {
    uint64_t dur = repFn();
    sum += dur;
    min = std::min(min, dur);
    max = std::max(max, dur);
  }

  const std::string unit = " ns";
  uint64_t avg = sum / reps;
  uint64_t res = min;
  std::cout << name;
  std::cout << "   " << std::setw(4) << max / ops << unit;
  std::cout << "   " << std::setw(4) << avg / ops << unit;
  std::cout << "   " << std::setw(4) << res / ops << unit;
  std::cout << std::endl;
  return res;
}

//
// Benchmarks
//
const int ops = 1000000;

inline uint64_t holder_bench(std::string name, int nthreads) {
  auto repFn = [&] {
    auto init = [] {};
    auto fn = [&](int tid) {
      for (int j = tid; j < 10 * ops; j += nthreads) {
        hazptr_holder<> h = make_hazard_pointer<>();
      }
    };
    auto endFn = [] {};
    return run_once(nthreads, init, fn, endFn);
  };
  return bench(name, ops, repFn);
}

template <size_t M>
inline uint64_t array_bench(std::string name, int nthreads) {
  auto repFn = [&] {
    auto init = [] {};
    auto fn = [&](int tid) {
      for (int j = tid; j < 10 * ops; j += nthreads) {
        hazptr_array<M> a = make_hazard_pointer_array<M>();
      }
    };
    auto endFn = [] {};
    return run_once(nthreads, init, fn, endFn);
  };
  return bench(name, ops, repFn);
}

template <size_t M>
inline uint64_t local_bench(std::string name, int nthreads) {
  auto repFn = [&] {
    auto init = [] {};
    auto fn = [&](int tid) {
      for (int j = tid; j < 10 * ops; j += nthreads) {
        hazptr_local<M> a;
      }
    };
    auto endFn = [] {};
    return run_once(nthreads, init, fn, endFn);
  };
  return bench(name, ops, repFn);
}

inline uint64_t obj_bench(std::string name, int nthreads) {
  struct Foo : public hazptr_obj_base<Foo> {};
  auto repFn = [&] {
    auto init = [] {};
    auto fn = [&](int tid) {
      for (int j = tid; j < ops; j += nthreads) {
        auto p = new Foo;
        p->retire();
      }
    };
    auto endFn = [] {};
    return run_once(nthreads, init, fn, endFn);
  };
  return bench(name, ops, repFn);
}

uint64_t list_hoh_bench(
    std::string name, int nthreads, int size, bool provided = false) {
  auto repFn = [&] {
    List<Node<>> l(size);
    auto init = [&] {};
    auto fn = [&](int tid) {
      if (provided) {
        hazptr_local<2> hptr;
        for (int j = tid; j < ops; j += nthreads) {
          l.hand_over_hand(size, &hptr[0], &hptr[1]);
        }
      } else {
        for (int j = tid; j < ops; j += nthreads) {
          l.hand_over_hand(size);
        }
      }
    };
    auto endFn = [] {};
    return run_once(nthreads, init, fn, endFn);
  };
  return bench(name, ops, repFn);
}

uint64_t list_protect_all_bench(
    std::string name, int nthreads, int size, bool provided = false) {
  auto repFn = [&] {
    List<NodeRC<false>> l(size);
    auto init = [] {};
    auto fn = [&](int tid) {
      if (provided) {
        hazptr_local<1> hptr;
        for (int j = tid; j < ops; j += nthreads) {
          l.protect_all(size, hptr[0]);
        }
      } else {
        for (int j = tid; j < ops; j += nthreads) {
          l.protect_all(size);
        }
      }
    };
    auto endFn = [] {};
    return run_once(nthreads, init, fn, endFn);
  };
  return bench(name, ops, repFn);
}

uint64_t cleanup_bench(std::string name, int nthreads) {
  auto repFn = [&] {
    auto init = [] {};
    auto fn = [&](int) {
      hazptr_holder<> hptr = make_hazard_pointer<>();
      for (int i = 0; i < ops / 1000; i++) {
        hazptr_cleanup();
      }
    };
    auto endFn = [] {};
    return run_once(nthreads, init, fn, endFn);
  };
  return bench(name, ops, repFn);
}

uint64_t cohort_bench(std::string name, int nthreads) {
  struct Foo : public hazptr_obj_base<Foo> {};
  // Push unrelated objects into the domain tagged list
  hazptr_obj_cohort cohort;
  for (int i = 0; i < 999; ++i) {
    auto p = new Foo;
    p->set_cohort_tag(&cohort);
    p->retire();
  }
  auto repFn = [&] {
    auto init = [] {};
    auto fn = [&](int tid) {
      for (int j = tid; j < ops; j += nthreads) {
        hazptr_obj_cohort b;
      }
    };
    auto endFn = [] {};
    return run_once(nthreads, init, fn, endFn);
  };
  return bench(name, ops, repFn);
}

#if FOLLY_HAZPTR_THR_LOCAL
uint64_t tc_miss_bench(std::string name, int nthreads) {
  hazptr_tc_evict();
  hazard_pointer_default_domain<>().delete_hazard_pointers();
  // Thread cache capacity
  constexpr int C = hazptr_tc<>::min_capacity();
  // Number of unavailable hazard pointers that will be at the head of
  // the main list of hazard pointers before reaching available ones.
  constexpr int N = 10000;
  // Max number of threads
  constexpr int P = 100;
  hazard_pointer<> aa[N + 2 * C * P];
  // The following creates N+2*C*P hazard pointers
  for (int i = 0; i < N + 2 * C * P; ++i) {
    aa[i] = make_hazard_pointer<>();
  }
  // Make the last 2*C*P in the domain's hazard pointer list available
  for (int i = 0; i < 2 * C * P; ++i) {
    aa[i] = hazard_pointer<>();
  }
  hazptr_tc_evict();
  // The domain now has N unavailable hazard pointers at the head of
  // the list following by C*P available ones.
  auto repFn = [&] {
    auto init = [] {};
    auto fn = [&](int tid) {
      for (int j = tid; j < ops; j += nthreads) {
        // By using twice the TC capacity, each iteration does one
        // filling and one eviction of the TC.
        hazptr_array<C> a1 = make_hazard_pointer_array<C>();
        hazptr_array<C> a2 = make_hazard_pointer_array<C>();
      }
    };
    auto endFn = [] {};
    return run_once(nthreads, init, fn, endFn);
  };
  return bench(name, ops, repFn);
}
#endif // FOLLY_HAZPTR_THR_LOCAL

const int nthr[] = {1, 10};
const int sizes[] = {10, 20};

void benches() {
  for (int i : nthr) {
    std::cout << "================================ " << std::setw(2) << i
              << " threads " << "================================" << std::endl;
    std::cout << "10x construct/destruct hazptr_holder          ";
    holder_bench("", i);
    std::cout << "10x construct/destruct hazptr_array<1>        ";
    array_bench<1>("", i);
    std::cout << "10x construct/destruct hazptr_array<2>        ";
    array_bench<2>("", i);
    std::cout << "10x construct/destruct hazptr_array<3>        ";
    array_bench<3>("", i);
    std::cout << "10x construct/destruct hazptr_local<1>        ";
    local_bench<1>("", i);
    std::cout << "10x construct/destruct hazptr_local<2>        ";
    local_bench<2>("", i);
    std::cout << "10x construct/destruct hazptr_local<3>        ";
    local_bench<3>("", i);
    std::cout << "10x construct/destruct hazptr_array<9>        ";
    array_bench<9>("", i);
#if FOLLY_HAZPTR_THR_LOCAL
    std::cout << "TC hit + miss & overflow                      ";
    tc_miss_bench("", i);
#endif // FOLLY_HAZPTR_THR_LOCAL
    std::cout << "allocate/retire/reclaim object                ";
    obj_bench("", i);
    for (int j : sizes) {
      std::cout << j << "-item list hand-over-hand - own hazptrs     ";
      list_hoh_bench("", i, j, true);
      std::cout << j << "-item list hand-over-hand                   ";
      list_hoh_bench("", i, j);
      std::cout << j << "-item list protect all - own hazptr         ";
      list_protect_all_bench("", i, j, true);
      std::cout << j << "-item list protect all                      ";
      list_protect_all_bench("", i, j);
    }
    std::cout << "1/1000 hazptr_cleanup                         ";
    cleanup_bench("", i);
    std::cout << "Life cycle of unused tagged obj cohort        ";
    cohort_bench("", i);
  }
}

TEST(HazptrTest, bench) {
  if (FLAGS_bench) {
    benches();
  }
}

/*
$ numactl -N 1 ./buck-out/gen/folly/synchronization/test/hazptr_test --bench

================================  1 threads ================================
10x construct/destruct hazptr_holder               51 ns     51 ns     50 ns
10x construct/destruct hazptr_array<1>             54 ns     52 ns     52 ns
10x construct/destruct hazptr_array<2>             60 ns     59 ns     58 ns
10x construct/destruct hazptr_array<3>            141 ns     88 ns     82 ns
10x construct/destruct hazptr_local<1>             13 ns     12 ns     12 ns
10x construct/destruct hazptr_local<2>             15 ns     15 ns     15 ns
10x construct/destruct hazptr_local<3>             39 ns     39 ns     38 ns
allocate/retire/reclaim object                     70 ns     68 ns     67 ns
10-item list hand-over-hand - own hazptrs          22 ns     20 ns     18 ns
10-item list hand-over-hand                        28 ns     25 ns     22 ns
10-item list protect all - own hazptr              12 ns     11 ns     11 ns
10-item list protect all                           22 ns     13 ns     12 ns
20-item list hand-over-hand - own hazptrs          42 ns     40 ns     38 ns
20-item list hand-over-hand                        48 ns     43 ns     41 ns
20-item list protect all - own hazptr              28 ns     28 ns     28 ns
20-item list protect all                           31 ns     29 ns     29 ns
1/1000 hazptr_cleanup                               2 ns      1 ns      1 ns
Life cycle of unused tagged obj cohort              1 ns      1 ns      1 ns
================================ 10 threads ================================
10x construct/destruct hazptr_holder               11 ns      8 ns      8 ns
10x construct/destruct hazptr_array<1>              8 ns      7 ns      7 ns
10x construct/destruct hazptr_array<2>              9 ns      9 ns      9 ns
10x construct/destruct hazptr_array<3>             19 ns     17 ns     14 ns
10x construct/destruct hazptr_local<1>              8 ns      8 ns      8 ns
10x construct/destruct hazptr_local<2>              8 ns      8 ns      7 ns
10x construct/destruct hazptr_local<3>             11 ns     11 ns     10 ns
allocate/retire/reclaim object                     20 ns     17 ns     16 ns
10-item list hand-over-hand - own hazptrs           3 ns      3 ns      3 ns
10-item list hand-over-hand                         3 ns      3 ns      3 ns
10-item list protect all - own hazptr               2 ns      2 ns      2 ns
10-item list protect all                            2 ns      2 ns      2 ns
20-item list hand-over-hand - own hazptrs           6 ns      6 ns      6 ns
20-item list hand-over-hand                         6 ns      6 ns      6 ns
20-item list protect all - own hazptr               4 ns      4 ns      4 ns
20-item list protect all                            5 ns      4 ns      4 ns
1/1000 hazptr_cleanup                             119 ns    113 ns     97 ns
Life cycle of unused tagged obj cohort              0 ns      0 ns      0 ns
*/
