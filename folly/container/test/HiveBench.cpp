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

/***
 *  folly::hive measured against the alternatives for keeping element
 *  addresses stable, over a whole lifecycle rather than one warm scan. Run
 *  it: it prints a legend describing the containers, units and fixture
 *  states, and closes with allocation counts and notes on reading the table.
 *
 *  Why the numbers can be trusted: a row is one unit of work rather than one
 *  pass, because the harness subtracts its own per-iteration overhead;
 *  fixtures are deterministic and built outside any timed region; every
 *  fixture of a payload holds the same live count, so states differ only in
 *  the memory layout underneath and not in how much work a row does; kLive
 *  elements is 1MiB of payload at 8B and 8MiB at 64B, against 32KiB L1d and
 *  1MiB L2 per core here; and allocation is counted in a separate untimed
 *  pass, with each container's promised stability verified rather than
 *  assumed. The aged state is the honest one for std::list, whose nodes come
 *  out contiguous when freshly built - measuring only a fresh build measures
 *  an array.
 */

#include <folly/container/Hive.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <folly/Benchmark.h>
#include <folly/portability/GFlags.h>

namespace {

struct Small {
  std::uint64_t key;
};
struct Large {
  std::uint64_t key;
  char pad[56];
};
static_assert(sizeof(Small) == 8);
static_assert(sizeof(Large) == 64);

template <typename T>
T make(std::uint64_t key) {
  T value{};
  value.key = key;
  return value;
}

// allocation accounting

struct AllocStats {
  std::size_t calls = 0;
  std::size_t requested = 0; // cumulative
  std::size_t retained = 0; // outstanding right now
};

AllocStats& stats() {
  static AllocStats value;
  return value;
}

/// Reports every allocation to stats(). Used only by the untimed accounting
/// pass; the timed benchmarks all run on std::allocator.
template <typename T>
struct CountingAlloc {
  using value_type = T;

  CountingAlloc() = default;
  template <typename U>
  /* implicit */ CountingAlloc(const CountingAlloc<U>&) noexcept {}

  T* allocate(std::size_t count) {
    ++stats().calls;
    stats().requested += count * sizeof(T);
    stats().retained += count * sizeof(T);
    return std::allocator<T>{}.allocate(count);
  }
  void deallocate(T* address, std::size_t count) noexcept {
    stats().retained -= count * sizeof(T);
    std::allocator<T>{}.deallocate(address, count);
  }
  template <typename U>
  bool operator==(const CountingAlloc<U>&) const noexcept {
    return true;
  }
  template <typename U>
  bool operator!=(const CountingAlloc<U>&) const noexcept {
    return false;
  }
};

// the containers, behind one interface

template <typename T, typename A>
auto nodeInsert(folly::hive<T, A>& c, std::uint64_t key) {
  return c.emplace(make<T>(key));
}
template <typename T, typename A>
auto nodeInsert(std::list<T, A>& c, std::uint64_t key) {
  return c.insert(c.end(), make<T>(key));
}
template <typename T, typename A>
void nodeReserve(folly::hive<T, A>& c, std::size_t count) {
  c.reserve(count);
}
template <typename T, typename A>
void nodeReserve(std::list<T, A>&, std::size_t) {}

/// hive and list: a handle is an iterator, and erasing one leaves the rest
/// alone.
template <typename Container>
struct NodeBox {
  using Handle = typename Container::iterator;
  static constexpr bool kStable = true;

  void reserve(std::size_t count) { nodeReserve(c_, count); }
  Handle insert(std::uint64_t key) { return nodeInsert(c_, key); }
  void erase(Handle handle) {
    c_.erase(handle);
    primed_ = false;
  }
  static const void* address(Handle handle) { return std::addressof(*handle); }
  static std::uint64_t key(Handle handle) { return handle->key; }
  std::size_t size() const { return c_.size(); }
  /// The handle churn keeps after replacing an element: the new one's.
  static Handle kept(Handle /* erased */, Handle inserted) { return inserted; }

  std::uint64_t scan(unsigned steps) {
    std::uint64_t sum = 0;
    while (steps-- > 0) {
      if (!primed_ || cursor_ == c_.end()) {
        cursor_ = c_.begin();
        primed_ = true;
      }
      sum += cursor_->key;
      ++cursor_;
    }
    return sum;
  }

 private:
  Container c_;
  Handle cursor_{};
  bool primed_ = false;
};

template <typename T, typename A>
struct AllocDeleter {
  void operator()(T* address) const noexcept {
    A alloc;
    std::allocator_traits<A>::destroy(alloc, address);
    std::allocator_traits<A>::deallocate(alloc, address, 1);
  }
};
template <typename T, typename A>
using AllocPtr = std::unique_ptr<T, AllocDeleter<T, A>>;

template <typename T, typename A>
void slotSet(AllocPtr<T, A>& slot, std::uint64_t key) {
  A alloc;
  T* address = std::allocator_traits<A>::allocate(alloc, 1);
  std::allocator_traits<A>::construct(alloc, address, make<T>(key));
  slot.reset(address);
}
template <typename T>
void slotSet(std::optional<T>& slot, std::uint64_t key) {
  slot = make<T>(key);
}

/// The hand-rolled answers: an erased slot keeps its place, so a live
/// element never moves and iteration walks over the holes.
template <typename Slot, typename SlotAlloc, typename HandleAlloc>
struct SlotBox {
  using Handle = std::uint32_t;
  static constexpr bool kStable = true;

  void reserve(std::size_t count) { slots_.reserve(count); }

  Handle insert(std::uint64_t key) {
    ++live_;
    if (!free_.empty()) {
      const Handle handle = free_.back();
      free_.pop_back();
      slotSet(slots_[handle], key);
      return handle;
    }
    slots_.emplace_back();
    slotSet(slots_.back(), key);
    return static_cast<Handle>(slots_.size() - 1);
  }

  void erase(Handle handle) {
    slots_[handle].reset();
    free_.push_back(handle);
    --live_;
  }

  const void* address(Handle handle) const {
    return std::addressof(*slots_[handle]);
  }
  std::uint64_t key(Handle handle) const { return slots_[handle]->key; }
  std::size_t size() const { return live_; }
  static Handle kept(Handle /* erased */, Handle inserted) { return inserted; }

  std::uint64_t scan(unsigned steps) {
    std::uint64_t sum = 0;
    while (steps > 0) {
      if (cursor_ >= slots_.size()) {
        cursor_ = 0;
      }
      if (slots_[cursor_]) {
        sum += slots_[cursor_]->key;
        --steps;
      }
      ++cursor_;
    }
    return sum;
  }

 private:
  std::vector<Slot, SlotAlloc> slots_;
  std::vector<Handle, HandleAlloc> free_;
  std::size_t live_ = 0;
  std::size_t cursor_ = 0;
};

/// No stability: erasing moves the last element into the hole, so an
/// untouched handle now names a different element.
template <typename T, typename A>
struct PlainVecBox {
  using Handle = std::uint32_t;
  static constexpr bool kStable = false;

  void reserve(std::size_t count) { v_.reserve(count); }
  Handle insert(std::uint64_t key) {
    v_.push_back(make<T>(key));
    return static_cast<Handle>(v_.size() - 1);
  }
  void erase(Handle handle) {
    v_[handle] = std::move(v_.back());
    v_.pop_back();
  }
  const void* address(Handle handle) const {
    return std::addressof(v_[handle]);
  }
  std::uint64_t key(Handle handle) const { return v_[handle].key; }
  std::size_t size() const { return v_.size(); }
  /// Keeps the erased index rather than the inserted one. Every insertion
  /// lands at the back, so keeping its index would point every churned
  /// handle at the tail, and once the trace repeated the churn would erase
  /// nothing but the last element. The erased index now holds what was the
  /// tail element, and choosing it again erases from a random position, as
  /// the other containers do.
  static Handle kept(Handle erased, Handle /* inserted */) { return erased; }

  std::uint64_t scan(unsigned steps) {
    std::uint64_t sum = 0;
    while (steps-- > 0) {
      if (cursor_ >= v_.size()) {
        cursor_ = 0;
      }
      sum += v_[cursor_++].key;
    }
    return sum;
  }

 private:
  std::vector<T, A> v_;
  std::size_t cursor_ = 0;
};

template <typename T, typename A>
using HiveBox = NodeBox<folly::hive<T, A>>;
template <typename T, typename A>
using ListBox = NodeBox<std::list<T, A>>;
template <typename T, typename A>
using PtrBox = SlotBox<
    AllocPtr<T, A>,
    typename std::allocator_traits<A>::template rebind_alloc<AllocPtr<T, A>>,
    typename std::allocator_traits<A>::template rebind_alloc<std::uint32_t>>;
template <typename T, typename A>
using TombBox = SlotBox<
    std::optional<T>,
    typename std::allocator_traits<A>::template rebind_alloc<std::optional<T>>,
    typename std::allocator_traits<A>::template rebind_alloc<std::uint32_t>>;

// hive leads every group, so the relative column reads as the fraction of
// hive's throughput each alternative reaches.
#define FOLLY_HIVE_BENCHMARK_FOR_EACH_BOX(X)    \
  X(HiveBox, "hive", "yes")                     \
  X(ListBox,                                    \
    "list (stable, node allocated)",            \
    "yes, one allocation per element")          \
  X(PtrBox,                                     \
    "vector<unique_ptr> (stable pointees)",     \
    "yes, one allocation per element")          \
  X(TombBox,                                    \
    "tombstone vector (stable within reserve)", \
    "only until it outgrows its reserve")       \
  X(PlainVecBox,                                \
    "vector (not stable)",                      \
    "no - erase swaps the last element down")

// fixtures, all built before any timing starts

constexpr unsigned kLive = 1u << 17;
constexpr unsigned kAgeMutations = kLive * 2;

template <typename Box>
struct Fixture {
  Box box;
  std::vector<typename Box::Handle> handles;
};

/// The order in which churn visits handles: a shuffled permutation of
/// 0..kLive-1. Over a fixture of kLive handles, as the timed churn rows and
/// the aged fixture use, each pass of kLive mutations replaces every element
/// exactly once. The untimed checks reduce it modulo their smaller fixtures
/// and get no such guarantee.
const std::vector<std::uint32_t>& trace() {
  static const std::vector<std::uint32_t> values = [] {
    std::vector<std::uint32_t> out(kLive);
    std::iota(out.begin(), out.end(), std::uint32_t{0});
    std::shuffle(out.begin(), out.end(), std::mt19937{20260916});
    return out;
  }();
  return values;
}

// Real churn runs alongside other allocation, so a freed node does not come
// straight back. Sizes are derived from the element size, so every container
// that allocates per element sees the same disturbance; noise of one fixed
// size would land in one container's size class and miss another's.
void heapNoise(
    std::vector<std::unique_ptr<char[]>>& ring,
    std::size_t step,
    std::size_t elementSize) {
  static constexpr std::size_t kOffsets[] = {0, 8, 16, 24};
  ring[step % ring.size()] =
      std::make_unique<char[]>(elementSize + kOffsets[step % 4]);
}

template <typename Box>
void fill(Fixture<Box>& fx, std::size_t count, bool reserve) {
  if (reserve) {
    fx.box.reserve(count);
  }
  fx.handles.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    fx.handles.push_back(fx.box.insert(i));
  }
}

/// Replaces elements at deterministic positions, holding the live count
/// constant, with unrelated allocations interleaved.
template <typename Box>
void churn(
    Fixture<Box>& fx,
    unsigned mutations,
    std::size_t& position,
    std::size_t elementSize) {
  std::vector<std::unique_ptr<char[]>> ring(64);
  const auto& tr = trace();
  for (unsigned i = 0; i < mutations; ++i) {
    const std::size_t slot = tr[position % tr.size()] % fx.handles.size();
    ++position;
    const auto erased = fx.handles[slot];
    fx.box.erase(erased);
    fx.handles[slot] = Box::kept(erased, fx.box.insert(slot));
    if ((i & 15) == 0) {
      heapNoise(ring, i >> 4, elementSize);
    }
  }
}

/// Leaves kLive live elements with (k-1)/k of the slots erased, by filling
/// to k * kLive and erasing all but every kth. The stride keeps any block
/// from emptying, so hive holds the holes rather than freeing the block, and
/// list's survivors end up scattered over a region k times larger.
template <typename Box>
void punch(Fixture<Box>& fx, unsigned keepEveryKth) {
  const std::size_t total = std::size_t{kLive} * keepEveryKth;
  fill(fx, total, true);
  if constexpr (Box::kStable) {
    std::vector<typename Box::Handle> kept;
    kept.reserve(kLive);
    for (std::size_t i = 0; i < fx.handles.size(); ++i) {
      if (i % keepEveryKth == 0) {
        kept.push_back(fx.handles[i]);
      } else {
        fx.box.erase(fx.handles[i]);
      }
    }
    fx.handles = std::move(kept);
  } else {
    // Swap-and-pop cannot leave a hole, so this just drains back down to the
    // live count; the survivors stay packed, which is the point of it.
    while (fx.handles.size() > kLive) {
      fx.box.erase(fx.handles.back());
      fx.handles.pop_back();
    }
  }
}

enum class Shape { Dense, Aged, Holes2x, Holes10x };

template <typename Box>
std::unique_ptr<Fixture<Box>> buildFixture(
    Shape shape, std::size_t elementSize) {
  auto fx = std::make_unique<Fixture<Box>>();
  std::size_t position = 0;
  switch (shape) {
    case Shape::Dense:
      fill(*fx, kLive, true);
      break;
    case Shape::Aged:
      fill(*fx, kLive, true);
      churn(*fx, kAgeMutations, position, elementSize);
      break;
    case Shape::Holes2x:
      punch(*fx, 2);
      break;
    case Shape::Holes10x:
      punch(*fx, 10);
      break;
  }
  return fx;
}

// the timed bodies

template <typename Box>
unsigned scanBody(Fixture<Box>& fx, unsigned steps) {
  folly::doNotOptimizeAway(fx.box.scan(steps));
  return steps;
}

template <typename Box>
unsigned churnBody(
    Fixture<Box>& fx, std::size_t& position, unsigned mutations) {
  const auto& tr = trace();
  for (unsigned i = 0; i < mutations; ++i) {
    const std::size_t slot = tr[position % tr.size()] % fx.handles.size();
    ++position;
    const auto erased = fx.handles[slot];
    fx.box.erase(erased);
    fx.handles[slot] = Box::kept(erased, fx.box.insert(slot));
  }
  return mutations;
}

/// Built and destroyed without reserving, so the figure covers however the
/// container chooses to grow.
template <typename Box>
unsigned buildBody(unsigned elements) {
  constexpr unsigned kBatch = 8192;
  unsigned done = 0;
  while (done < elements) {
    const unsigned count = std::min(kBatch, elements - done);
    Fixture<Box> fx;
    fill(fx, count, false);
    folly::doNotOptimizeAway(fx.box.size());
    done += count;
  }
  return done;
}

// registration

/// A churn row's fixture and its place in the trace, owned together by the
/// registered benchmark.
template <typename Box>
struct ChurnState {
  explicit ChurnState(std::unique_ptr<Fixture<Box>> built)
      : fixture{std::move(built)} {}
  std::unique_ptr<Fixture<Box>> fixture;
  std::size_t position = 0;
};

void text(const std::string& line) {
  folly::addBenchmark(__FILE__, fmt::format("\"{}\"", line), []() -> unsigned {
    return 0;
  });
}
void rule() {
  folly::addBenchmark(__FILE__, "-", []() -> unsigned { return 0; });
}

template <typename Payload>
void addRows(const char* payload) {
  using A = std::allocator<Payload>;
  bool first = true;
  const auto name = [&first](const char* label) {
    const char* const relative = first ? "" : "%";
    first = false;
    return fmt::format("{}{}", relative, label);
  };

  // Each registered benchmark owns its fixture, so the fixtures live exactly
  // as long as the registry that runs them and nothing is leaked at exit.
#define FOLLY_HIVE_BENCHMARK_ADD_SCAN(Tmpl, Label, Promise)           \
  {                                                                   \
    std::shared_ptr<Fixture<Tmpl<Payload, A>>> fx =                   \
        buildFixture<Tmpl<Payload, A>>(shape, sizeof(Payload));       \
    folly::addBenchmark(__FILE__, name(Label), [fx](unsigned steps) { \
      return scanBody(*fx, steps);                                    \
    });                                                               \
  }

  const std::pair<Shape, const char*> shapes[] = {
      {Shape::Dense, "scan / dense"},
      {Shape::Aged, "scan / aged"},
      {Shape::Holes2x, "scan / 50% holes"},
      {Shape::Holes10x, "scan / 90% holes"},
  };
  for (const auto& entry : shapes) {
    const Shape shape = entry.first;
    text(fmt::format("{} / {}   (ns per live element)", payload, entry.second));
    first = true;
    FOLLY_HIVE_BENCHMARK_FOR_EACH_BOX(FOLLY_HIVE_BENCHMARK_ADD_SCAN)
    rule();
  }
#undef FOLLY_HIVE_BENCHMARK_ADD_SCAN

#define FOLLY_HIVE_BENCHMARK_ADD_CHURN(Tmpl, Label, Promise)                 \
  {                                                                          \
    auto state = std::make_shared<ChurnState<Tmpl<Payload, A>>>(             \
        buildFixture<Tmpl<Payload, A>>(Shape::Dense, sizeof(Payload)));      \
    folly::addBenchmark(__FILE__, name(Label), [state](unsigned mutations) { \
      return churnBody(*state->fixture, state->position, mutations);         \
    });                                                                      \
  }

  text(fmt::format("{} / churn   (ns per erase+insert pair)", payload));
  first = true;
  FOLLY_HIVE_BENCHMARK_FOR_EACH_BOX(FOLLY_HIVE_BENCHMARK_ADD_CHURN)
  rule();
#undef FOLLY_HIVE_BENCHMARK_ADD_CHURN

#define FOLLY_HIVE_BENCHMARK_ADD_BUILD(Tmpl, Label, Promise)         \
  folly::addBenchmark(__FILE__, name(Label), [](unsigned elements) { \
    return buildBody<Tmpl<Payload, A>>(elements);                    \
  });

  text(
      fmt::format(
          "{} / build + destroy, no reserve   (ns per element)", payload));
  first = true;
  FOLLY_HIVE_BENCHMARK_FOR_EACH_BOX(FOLLY_HIVE_BENCHMARK_ADD_BUILD)
  rule();
#undef FOLLY_HIVE_BENCHMARK_ADD_BUILD
}

// untimed: is the promise actually kept, and what does it allocate

/// Churns the first half and checks the untouched second half is still where
/// it was and still itself: address alone would not settle it, since a
/// vector whose buffer never reallocates keeps the addresses and moves
/// different elements into them.
template <typename Box>
bool checkStability(
    const char* label,
    const char* promise,
    std::size_t elementSize,
    bool print) {
  constexpr unsigned kCount = 4096;
  Fixture<Box> fx;
  fill(fx, kCount, true);

  std::vector<std::pair<const void*, std::uint64_t>> before;
  for (std::size_t i = kCount / 2; i < kCount; ++i) {
    before.emplace_back(
        fx.box.address(fx.handles[i]), fx.box.key(fx.handles[i]));
  }

  std::vector<std::unique_ptr<char[]>> ring(64);
  for (unsigned i = 0; i < kCount * 8; ++i) {
    const std::size_t slot = trace()[i % trace().size()] % (kCount / 2);
    fx.box.erase(fx.handles[slot]);
    fx.handles[slot] = fx.box.insert(slot);
    if ((i & 15) == 0) {
      heapNoise(ring, i >> 4, elementSize);
    }
  }

  bool held = true;
  for (std::size_t i = kCount / 2; i < kCount; ++i) {
    const auto& was = before[i - kCount / 2];
    held = held && fx.box.address(fx.handles[i]) == was.first &&
        fx.box.key(fx.handles[i]) == was.second;
  }
  const bool ok = held == Box::kStable;
  if (print || !ok) {
    fmt::print(
        "  {:<44} {:<39} {}\n",
        label,
        promise,
        ok ? "confirmed" : "MISMATCH <--");
  }
  return ok;
}

template <typename Box>
void account(const char* payload, const char* label, std::size_t elementSize) {
  constexpr unsigned kCount = 1u << 14;
  stats() = AllocStats{};
  Fixture<Box> fx;
  fill(fx, kCount, false);
  const AllocStats built = stats();
  std::size_t position = 0;
  churn(fx, kCount, position, elementSize);
  const AllocStats churned = stats();
  fmt::print(
      "  {:<4} {:<44} {:10.3f} {:12.1f} {:12.1f} {:11.3f}\n",
      payload,
      label,
      double(built.calls) / kCount,
      double(built.requested) / kCount,
      double(built.retained) / kCount,
      double(churned.calls - built.calls) / kCount);
}

/// Returns how many containers did not behave as they promise. Both payload
/// sizes are checked; the table prints once, because the guarantee is a
/// property of the container rather than of the element size.
template <typename Payload>
int checkAll(bool print) {
  using A = std::allocator<Payload>;
  int bad = 0;
#define FOLLY_HIVE_BENCHMARK_CHECK_ONE(Tmpl, Label, Promise)                   \
  bad +=                                                                       \
      checkStability<Tmpl<Payload, A>>(Label, Promise, sizeof(Payload), print) \
      ? 0                                                                      \
      : 1;
  FOLLY_HIVE_BENCHMARK_FOR_EACH_BOX(FOLLY_HIVE_BENCHMARK_CHECK_ONE)
#undef FOLLY_HIVE_BENCHMARK_CHECK_ONE
  return bad;
}

template <typename Payload>
void accountAll(const char* payload) {
  using A = CountingAlloc<Payload>;
#define FOLLY_HIVE_BENCHMARK_ACCOUNT_ONE(Tmpl, Label, Promise) \
  account<Tmpl<Payload, A>>(payload, Label, sizeof(Payload));
  FOLLY_HIVE_BENCHMARK_FOR_EACH_BOX(FOLLY_HIVE_BENCHMARK_ACCOUNT_ONE)
#undef FOLLY_HIVE_BENCHMARK_ACCOUNT_ONE
}

/// Who is being compared, what the rows mean, and what the fixtures are.
void printLegend() {
  fmt::print(
      "{}",
      R"(folly::hive against the alternatives for keeping element addresses stable.

  Rows are ns per unit of work, not per pass: one live element visited, one
  erase paired with the insert that reuses its slot, or one element built and
  later destroyed. Lower time is better. The relative column compares each
  row against the hive row above it: 100% is the same throughput as hive,
  above 100% is faster than hive, below 100% is slower.

  Fixture states, all holding the same live count so that only the memory
  layout underneath differs:
    dense      filled once, nothing erased
    aged       same count after 2x its size in erase/insert churn
    50% holes  half the slots erased, live elements over 2x the span
    90% holes  nine slots in ten erased, live elements over 10x the span

  Does an element keep its address when others are erased? Each container
  says what it promises, and the check below confirms it:
)");
}

/// Notes that follow from how these containers are built, not from the
/// timings: this program cannot see the medians, and guessing at them in
/// prose would be worse than saying nothing.
void printInterpretation() {
  fmt::print(
      "{}",
      R"(
Reading the table, from how these containers work:

  - Compare each container across the four scan states before comparing
    containers to each other: only the layout changes between them, so that
    difference is what a container costs once it has been used.
  - hive reads one skipfield entry per step and jumps a whole run of erased
    slots at once, so its scan cost should barely move from dense to 90%
    holes. The tombstoned containers walk every slot, so theirs rises with
    the hole count, and neither can compact without moving elements.
  - list keeps its nodes wherever the allocator put them, so its aged row is
    the one that matters; its dense row measures a fresh allocation sequence
    that no long-lived list has.
  - list and vector<unique_ptr> allocate per element and again per mutation;
    hive and the flat containers allocate per block - the churn column.
  - In the churn rows every erase is followed at once by an insert, so the
    freed storage is reused while it is still in cache: hive takes the slot
    it just freed, and list usually gets back from the allocator the node it
    just freed. How much the per-element containers gain from that depends
    on the allocator, so their churn rows can move by several ns under a
    different one.
  - vector (not stable) is the floor, not a competitor: it is what the work
    costs when nobody needs the guarantee.
  - The build rows include each container's own handle bookkeeping, and a
    hive iterator is wider than the index the slot vectors hand out, so
    those rows lean slightly against hive.
)");
}

} // namespace

int main(int argc, char** argv) {
  folly::gflags::ParseCommandLineFlags(&argc, &argv, true);

  static_assert(
      sizeof(AllocPtr<Large, std::allocator<Large>>) == sizeof(Large*),
      "the owning pointer must not be fatter than a raw one");

  printLegend();

  const int mismatches = checkAll<Small>(true) + checkAll<Large>(false);
  if (mismatches != 0) {
    fmt::print("{} stability mismatches\n", mismatches);
    return 1;
  }
  fmt::print("\n");

  addRows<Small>("8B");
  addRows<Large>("64B");
  folly::runBenchmarks();

  fmt::print(
      "\nallocation, counted in an untimed pass so the counters cannot "
      "distort a timing:\n");
  fmt::print(
      "  {:<4} {:<44} {:>10} {:>12} {:>12} {:>11}\n",
      "",
      "",
      "allocs/elem",
      "requested B",
      "retained B",
      "allocs/mut");
  accountAll<Small>("8B");
  accountAll<Large>("64B");

  printInterpretation();
  return 0;
}
