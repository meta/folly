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

// The class template has exactly the shape P0447R28 declares: two type
// parameters, no constraint. This declaration is the proof - a constrained
// hive would make the definition below a redeclaration with different
// constraints, and this file would not compile. It has to come before the
// include for that to mean anything.
namespace folly {
template <typename T, typename Allocator>
class hive;
} // namespace folly

#include <folly/container/Hive.h>

#include <algorithm>
#include <any>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <numeric>
#include <random>
#include <ranges>
#include <scoped_allocator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <folly/Conv.h>
#include <folly/ScopeGuard.h>
#include <folly/portability/GTest.h>

namespace folly {
namespace {

static_assert(std::is_same_v<hive<int>::value_type, int>);
static_assert(std::is_same_v<hive<int>::allocator_type, std::allocator<int>>);
static_assert(
    std::is_convertible_v<hive<int>::iterator, hive<int>::const_iterator>);
static_assert( //
    std::is_same_v<
        hive<int>::iterator::iterator_category,
        std::bidirectional_iterator_tag>);

// The iterators model the C++20 iterator concepts, whatever the element type
// and the priority, and a hive is a bidirectional range.
using MoveOnlyHive = hive<std::unique_ptr<int>>;
using StringHive = hive<std::string>;
static_assert(std::bidirectional_iterator<hive<int>::iterator>);
static_assert(std::bidirectional_iterator<hive<int>::const_iterator>);
static_assert(std::bidirectional_iterator<MoveOnlyHive::iterator>);
static_assert(std::bidirectional_iterator<StringHive::const_iterator>);
static_assert(std::sentinel_for<hive<int>::iterator, hive<int>::iterator>);
static_assert(
    std::sentinel_for<hive<int>::const_iterator, hive<int>::iterator>);
static_assert(std::ranges::bidirectional_range<hive<int>>);
static_assert(std::ranges::bidirectional_range<const hive<int>>);

/// A single-pass Cpp17InputIterator - the paper's `InputIterator` - so it
/// pins the constraint on the range constructors, assign() and insert(), and
/// the bulk insertion that cannot know the element count in advance.
class SinglePassIterator {
 public:
  using iterator_category = std::input_iterator_tag;
  using value_type = int;
  using difference_type = std::ptrdiff_t;
  using pointer = const int*;
  using reference = int;

  SinglePassIterator() = default;
  explicit SinglePassIterator(const int* position) : position_{position} {}

  int operator*() const { return *position_; }
  SinglePassIterator& operator++() {
    ++position_;
    return *this;
  }
  SinglePassIterator operator++(int) {
    auto copy = *this;
    ++position_;
    return copy;
  }
  bool operator==(const SinglePassIterator&) const = default;

 private:
  const int* position_ = nullptr;
};

static_assert(std::input_iterator<SinglePassIterator>);
static_assert(!std::forward_iterator<SinglePassIterator>);
static_assert(
    std::constructible_from<hive<int>, SinglePassIterator, SinglePassIterator>);
// An int models neither an iterator nor std::string, so it satisfies neither
// the range constructor nor the count-and-value one.
static_assert(!std::constructible_from<hive<std::string>, int, int>);

/// A legacy proxy input iterator: operator* hands back a prvalue proxy. It is
/// a Cpp17InputIterator, which is what the paper's constructors take, and the
/// container must accept it whether or not it also models the C++20 concept.
struct ProxyRef {
  int value;
  /* implicit */ operator int() const { return value; }
};
class LegacyProxyIterator {
 public:
  using iterator_category = std::input_iterator_tag;
  using value_type = int;
  using difference_type = std::ptrdiff_t;
  using pointer = void;
  using reference = ProxyRef;

  LegacyProxyIterator() = default;
  explicit LegacyProxyIterator(const int* position) : position_{position} {}

  ProxyRef operator*() const { return ProxyRef{*position_}; }
  LegacyProxyIterator& operator++() {
    ++position_;
    return *this;
  }
  LegacyProxyIterator operator++(int) {
    auto copy = *this;
    ++position_;
    return copy;
  }
  bool operator==(const LegacyProxyIterator&) const = default;

 private:
  const int* position_ = nullptr;
};

static_assert( //
    std::constructible_from<
        hive<int>,
        LegacyProxyIterator,
        LegacyProxyIterator>);

/// A move-only C++20 input iterator. It models std::input_iterator but is no
/// Cpp17InputIterator, so the paper's iterator-pair constructors do not take
/// it - and it has to be rejected by constraint, leaving the overload set
/// quietly, rather than by a hard error from inside the container.
class MoveOnlyIterator {
 public:
  using value_type = int;
  using difference_type = std::ptrdiff_t;

  MoveOnlyIterator() = default;
  explicit MoveOnlyIterator(const int* position) : position_{position} {}
  MoveOnlyIterator(MoveOnlyIterator&&) = default;
  MoveOnlyIterator& operator=(MoveOnlyIterator&&) = default;
  MoveOnlyIterator(const MoveOnlyIterator&) = delete;
  MoveOnlyIterator& operator=(const MoveOnlyIterator&) = delete;

  int operator*() const { return *position_; }
  MoveOnlyIterator& operator++() {
    ++position_;
    return *this;
  }
  void operator++(int) { ++position_; }
  bool operator==(std::default_sentinel_t) const { return *position_ == 0; }

 private:
  const int* position_ = nullptr;
};

static_assert(std::input_iterator<MoveOnlyIterator>);
static_assert(
    !std::constructible_from<hive<int>, MoveOnlyIterator, MoveOnlyIterator>);

/// A range over move-only iterators, ending at a sentinel. The iterator-pair
/// constructors do not take its iterator - it is no Cpp17InputIterator - but
/// insert_range does, because a range is only ever asked for begin and end.
struct MoveOnlyRange {
  const int* data;
  MoveOnlyIterator begin() const { return MoveOnlyIterator{data}; }
  std::default_sentinel_t end() const { return {}; }
};
static_assert(std::ranges::input_range<MoveOnlyRange>);

template <typename Hive>
std::vector<typename Hive::value_type> elementsOf(const Hive& values) {
  return std::vector<typename Hive::value_type>(values.begin(), values.end());
}

template <typename Hive>
std::vector<typename Hive::value_type> reversedElementsOf(const Hive& values) {
  return std::vector<typename Hive::value_type>(values.rbegin(), values.rend());
}

// A forwards walk and a backwards walk must both visit size() elements.
template <typename Hive>
void expectWalksAgreeWithSize(const Hive& values) {
  std::size_t forwards = 0;
  for (auto it = values.begin(); it != values.end(); ++it) {
    ++forwards;
  }
  std::size_t backwards = 0;
  for (auto it = values.end(); it != values.begin();) {
    --it;
    ++backwards;
  }
  EXPECT_EQ(forwards, values.size());
  EXPECT_EQ(backwards, values.size());
}

// Walking backwards from end() must visit exactly the elements that walking
// forwards from begin() does.
template <typename Hive>
void expectConsistentInBothDirections(const Hive& values) {
  expectWalksAgreeWithSize(values);
  auto forward = elementsOf(values);
  auto backward = reversedElementsOf(values);
  std::reverse(backward.begin(), backward.end());
  EXPECT_EQ(forward, backward);
  EXPECT_EQ(forward.size(), values.size());
}

TEST(Hive, BasicContainerOperations) {
  hive<int> values;
  EXPECT_TRUE(values.empty());
  EXPECT_EQ(values.begin(), values.end());

  auto first = values.insert(1);
  auto second = values.emplace(2);
  values.insert(3);

  EXPECT_EQ(*first, 1);
  EXPECT_EQ(*second, 2);
  EXPECT_EQ(values.size(), 3);
  EXPECT_GE(values.capacity(), values.size());
  EXPECT_EQ(elementsOf(values), (std::vector<int>{1, 2, 3}));

  values.erase(second);
  EXPECT_EQ(elementsOf(values), (std::vector<int>{1, 3}));

  values.clear();
  EXPECT_TRUE(values.empty());
  EXPECT_EQ(values.begin(), values.end());
}

TEST(Hive, InsertAndUnrelatedErasePreserveAddressesAndIterators) {
  hive<std::string> values(hive_limits{3, 8});
  auto first = values.emplace("first");
  auto middle = values.emplace("middle");
  auto last = values.emplace("last");

  const auto* firstAddress = std::addressof(*first);
  const auto* lastAddress = std::addressof(*last);

  for (int i = 0; i < 100; ++i) {
    values.emplace(folly::to<std::string>(i));
  }

  EXPECT_EQ(std::addressof(*first), firstAddress);
  EXPECT_EQ(std::addressof(*last), lastAddress);

  values.erase(middle);
  EXPECT_EQ(std::addressof(*first), firstAddress);
  EXPECT_EQ(std::addressof(*last), lastAddress);
  EXPECT_EQ(*first, "first");
  EXPECT_EQ(*last, "last");
}

// Every arrangement of erasures inside one block exercises a different
// combination of creating, extending and merging runs of erased slots.
TEST(Hive, EveryErasurePatternInABlockIteratesAndRefillsCorrectly) {
  constexpr int kBlockCapacity = 8;
  constexpr int kInteriorSlots = 6;

  for (int pattern = 0; pattern < (1 << kInteriorSlots); ++pattern) {
    hive<int> values(hive_limits{kBlockCapacity, kBlockCapacity});
    std::vector<hive<int>::iterator> positions;
    positions.reserve(kBlockCapacity);
    for (int i = 0; i < kBlockCapacity; ++i) {
      positions.push_back(values.insert(i));
    }

    // Erase an arbitrary subset of the interior slots, leaving the outermost
    // slots alone so that runs can merge in both directions.
    std::vector<int> expected;
    for (int slot = 1; slot <= kInteriorSlots; ++slot) {
      if ((pattern & (1 << (slot - 1))) != 0) {
        values.erase(positions[slot]);
      }
    }
    for (int slot = 0; slot < kBlockCapacity; ++slot) {
      const bool erased = slot >= 1 && slot <= kInteriorSlots &&
          (pattern & (1 << (slot - 1))) != 0;
      if (!erased) {
        expected.push_back(slot);
      }
    }

    ASSERT_EQ(elementsOf(values), expected) << "pattern " << pattern;
    expectConsistentInBothDirections(values);

    // Refilling must consume exactly the erased slots, without growing.
    const auto capacityAfterErase = values.capacity();
    const std::size_t erasedCount = kBlockCapacity - expected.size();
    for (std::size_t i = 0; i < erasedCount; ++i) {
      values.insert(1000 + static_cast<int>(i));
    }
    EXPECT_EQ(values.capacity(), capacityAfterErase) << "pattern " << pattern;
    EXPECT_EQ(values.size(), kBlockCapacity) << "pattern " << pattern;
    expectConsistentInBothDirections(values);
  }
}

TEST(Hive, ReusesErasedStorageAtTheSameAddress) {
  hive<int> values(hive_limits{3, 3});
  auto erased = values.insert(1);
  values.insert(2);
  values.insert(3);
  const auto capacity = values.capacity();
  const int* address = std::addressof(*erased);

  values.erase(erased);
  auto replacement = values.insert(4);

  EXPECT_EQ(std::addressof(*replacement), address);
  EXPECT_EQ(values.capacity(), capacity);
  EXPECT_EQ(values.size(), 3);
}

TEST(Hive, SpansManyBlocksWithoutMovingElements) {
  hive<std::string> values(hive_limits{3, 5});
  std::vector<hive<std::string>::iterator> positions;
  std::vector<const std::string*> addresses;
  positions.reserve(60);
  addresses.reserve(60);
  for (int i = 0; i < 60; ++i) {
    positions.push_back(values.emplace(folly::to<std::string>(i)));
    addresses.push_back(std::addressof(*positions.back()));
  }
  EXPECT_GE(values.capacity(), 60);

  for (int i = 0; i < 60; i += 3) {
    values.erase(positions[i]);
  }

  std::vector<std::string> expected;
  expected.reserve(40);
  for (int i = 0; i < 60; ++i) {
    if (i % 3 == 0) {
      continue;
    }
    EXPECT_EQ(std::addressof(*positions[i]), addresses[i]);
    expected.push_back(folly::to<std::string>(i));
  }
  EXPECT_EQ(elementsOf(values), expected);
  expectConsistentInBothDirections(values);
}

TEST(Hive, EraseRangeAcrossBlocks) {
  hive<int> values(hive_limits{3, 3});
  for (int i = 0; i < 12; ++i) {
    values.insert(i);
  }

  auto first = values.begin();
  std::advance(first, 2);
  auto last = first;
  std::advance(last, 7);
  auto result = values.erase(first, last);

  EXPECT_EQ(*result, 9);
  EXPECT_EQ(elementsOf(values), (std::vector<int>{0, 1, 9, 10, 11}));

  const auto afterEraseAll = values.erase(values.begin(), values.end());
  EXPECT_EQ(afterEraseAll, values.end());
  EXPECT_TRUE(values.empty());
  EXPECT_EQ(values.begin(), values.end());

  values.insert(42);
  EXPECT_EQ(elementsOf(values), (std::vector<int>{42}));
}

TEST(Hive, ReverseIterationSkipsErasedElements) {
  hive<int> values;
  for (int i = 0; i < 8; ++i) {
    values.insert(i);
  }

  auto erased = values.begin();
  std::advance(erased, 3);
  values.erase(erased);

  EXPECT_EQ(
      reversedElementsOf(values), (std::vector<int>{7, 6, 5, 4, 2, 1, 0}));
}

TEST(Hive, GetIteratorFindsLiveElementsAndRejectsEverythingElse) {
  hive<int> values(hive_limits{4, 4});
  std::vector<int*> addresses;
  addresses.reserve(20);
  for (int i = 0; i < 20; ++i) {
    addresses.push_back(std::addressof(*values.insert(i)));
  }

  for (int i = 0; i < 20; ++i) {
    auto found = values.get_iterator(addresses[i]);
    ASSERT_NE(found, values.end());
    EXPECT_EQ(*found, i);
  }

  for (int i = 0; i < 20; i += 5) {
    values.erase(values.get_iterator(addresses[i]));
  }
  for (int i = 0; i < 20; ++i) {
    auto found = values.get_iterator(addresses[i]);
    if (i % 5 == 0) {
      EXPECT_EQ(found, values.end()) << "erased element " << i;
    } else {
      ASSERT_NE(found, values.end());
      EXPECT_EQ(*found, i);
    }
  }

  const int unrelated = 7;
  EXPECT_EQ(values.get_iterator(&unrelated), values.end());

  const hive<int>& constValues = values;
  ASSERT_NE(constValues.get_iterator(addresses[1]), constValues.end());
  EXPECT_EQ(*constValues.get_iterator(addresses[1]), 1);
}

TEST(Hive, ReserveAndTrimCapacity) {
  hive<int> values(hive_limits{3, 16});
  values.reserve(100);
  EXPECT_GE(values.capacity(), 100);

  for (int i = 0; i < 10; ++i) {
    values.insert(i);
  }
  values.trim_capacity();

  EXPECT_GE(values.capacity(), values.size());
  EXPECT_LT(values.capacity(), 100);
  EXPECT_EQ(
      elementsOf(values), (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));

  values.reserve(64);
  values.trim_capacity(32);
  EXPECT_GE(values.capacity(), 32);
  EXPECT_LT(values.capacity(), 64);
}

TEST(Hive, ShrinkToFitCompactsStorage) {
  hive<int> values(hive_limits{4, 4});
  std::vector<hive<int>::iterator> positions;
  positions.reserve(40);
  for (int i = 0; i < 40; ++i) {
    positions.push_back(values.insert(i));
  }
  for (int i = 0; i < 40; i += 2) {
    values.erase(positions[i]);
  }
  const auto capacityBefore = values.capacity();

  values.shrink_to_fit();

  std::vector<int> expected;
  for (int i = 1; i < 40; i += 2) {
    expected.push_back(i);
  }
  EXPECT_LT(values.capacity(), capacityBefore);
  EXPECT_GE(values.capacity(), values.size());
  EXPECT_EQ(elementsOf(values), expected);
}

TEST(Hive, BlockLimits) {
  hive<int> values(hive_limits{4, 12});
  EXPECT_EQ(values.block_capacity_limits().min, 4);
  EXPECT_EQ(values.block_capacity_limits().max, 12);
  // One skipfield entry has to be able to hold the longest run of erased
  // slots a block can contain, which is what bounds the block capacity.
  EXPECT_EQ(hive<int>::block_capacity_hard_limits().max, 65535);
  EXPECT_EQ(hive<int>::block_capacity_hard_limits().min, 3);

  for (int i = 0; i < 30; ++i) {
    values.insert(i);
  }
  const auto before = elementsOf(values);

  values.reshape(hive_limits{16, 20});
  EXPECT_EQ(values.block_capacity_limits().min, 16);
  EXPECT_EQ(values.block_capacity_limits().max, 20);
  EXPECT_EQ(elementsOf(values), before);

  EXPECT_THROW(values.reshape(hive_limits{2, 70000}), std::length_error);
  EXPECT_THROW(hive<int>(hive_limits{8, 4}), std::length_error);
}

// The longest run one skipfield entry can describe, in the middle block of
// three, so that the jump off it lands in a block rather than at end().
TEST(Hive, HandlesTheLongestRunASkipfieldEntryCanDescribeMidChain) {
  const std::size_t blockCapacity = hive<int>::block_capacity_hard_limits().max;
  const std::size_t total = 3 * blockCapacity;

  hive<int> values(hive_limits{blockCapacity, blockCapacity});
  std::vector<hive<int>::iterator> positions;
  positions.reserve(total);
  for (std::size_t i = 0; i < total; ++i) {
    positions.push_back(values.insert(static_cast<int>(i)));
  }
  ASSERT_EQ(values.capacity(), total);

  // Erase all but the first element of the middle block: the longest run of
  // erased slots a block that still holds an element can contain.
  for (std::size_t i = blockCapacity + 1; i < 2 * blockCapacity; ++i) {
    values.erase(positions[i]);
  }
  EXPECT_EQ(values.size(), total - (blockCapacity - 1));
  EXPECT_EQ(values.capacity(), total);
  expectConsistentInBothDirections(values);
  const auto firstOfSecondBlock =
      std::next(values.begin(), static_cast<std::ptrdiff_t>(blockCapacity + 1));
  EXPECT_EQ(*firstOfSecondBlock, static_cast<int>(2 * blockCapacity));

  // Refilling consumes that run rather than allocating another block.
  for (std::size_t i = 0; i + 1 < blockCapacity; ++i) {
    values.insert(-1);
  }
  EXPECT_EQ(values.size(), total);
  EXPECT_EQ(values.capacity(), total);
  expectConsistentInBothDirections(values);
}

TEST(Hive, SortUniqueAndSplice) {
  hive<int> first{4, 1, 3, 3, 2, 2};
  first.sort();
  EXPECT_EQ(elementsOf(first), (std::vector<int>{1, 2, 2, 3, 3, 4}));
  EXPECT_EQ(first.unique(), 2);
  EXPECT_EQ(elementsOf(first), (std::vector<int>{1, 2, 3, 4}));

  hive<int> second{5, 6};
  int* address = std::addressof(*second.begin());
  first.splice(second);

  EXPECT_TRUE(second.empty());
  EXPECT_EQ(std::addressof(*std::next(first.begin(), 4)), address);
  EXPECT_EQ(elementsOf(first), (std::vector<int>{1, 2, 3, 4, 5, 6}));

  // The spliced-in blocks join the destination's storage, so the slots erased
  // by unique() are still the next ones reused.
  const auto capacity = first.capacity();
  first.insert(7);
  first.insert(8);
  EXPECT_EQ(first.capacity(), capacity);
  expectConsistentInBothDirections(first);
}

TEST(Hive, SortOrdersManyBlocksAndKeepsEveryElement) {
  hive<int> values(hive_limits{3, 9});
  std::mt19937 random{7};
  std::vector<int> expected;
  expected.reserve(400);
  for (int i = 0; i < 400; ++i) {
    const int value = static_cast<int>(random() % 100);
    values.insert(value);
    expected.push_back(value);
  }
  for (int i = 0; i < 150; ++i) {
    auto position = values.begin();
    std::advance(position, random() % values.size());
    expected.erase(std::find(expected.begin(), expected.end(), *position));
    values.erase(position);
  }

  values.sort();
  std::sort(expected.begin(), expected.end());

  EXPECT_EQ(elementsOf(values), expected);
  expectConsistentInBothDirections(values);
}

TEST(Hive, SpliceRejectsBlocksOutsideTheDestinationLimits) {
  hive<int> destination(hive_limits{100, 200});
  destination.insert(1);
  hive<int> source(hive_limits{3, 8});
  source.insert(2);

  EXPECT_THROW(destination.splice(source), std::length_error);
  EXPECT_EQ(source.size(), 1);
  EXPECT_EQ(destination.size(), 1);
}

TEST(Hive, FreeEraseAlgorithms) {
  hive<int> values{1, 2, 2, 3, 4, 4};
  EXPECT_EQ(folly::erase(values, 2), 2);
  EXPECT_EQ(
      folly::erase_if(values, [](int value) { return value % 2 == 0; }), 2);
  EXPECT_EQ(elementsOf(values), (std::vector<int>{1, 3}));
}

TEST(Hive, SupportsMoveOnlyElements) {
  hive<std::unique_ptr<int>> pointers(hive_limits{3, 3});
  std::vector<hive<std::unique_ptr<int>>::iterator> positions;
  positions.reserve(10);
  for (int i = 0; i < 10; ++i) {
    positions.push_back(pointers.emplace(std::make_unique<int>(i)));
  }
  EXPECT_EQ(**pointers.begin(), 0);

  pointers.erase(positions[4]);
  pointers.emplace(std::make_unique<int>(99));
  EXPECT_EQ(pointers.size(), 10);

  hive<std::unique_ptr<int>> moved = std::move(pointers);
  EXPECT_EQ(moved.size(), 10);
  EXPECT_EQ(**moved.begin(), 0);
}

TEST(Hive, SupportsOverAlignedAndUndersizedElements) {
  struct alignas(128) OverAligned {
    int value;
  };
  hive<OverAligned> aligned(hive_limits{3, 3});
  for (int i = 0; i < 10; ++i) {
    auto position = aligned.emplace(OverAligned{i});
    EXPECT_EQ(
        reinterpret_cast<std::uintptr_t>(std::addressof(*position)) %
            alignof(OverAligned),
        0);
  }
  EXPECT_EQ(aligned.size(), 10);

  // A char is smaller than the free-list node kept in an erased slot.
  hive<char> small(hive_limits{3, 3});
  std::vector<hive<char>::iterator> positions;
  positions.reserve(9);
  for (int i = 0; i < 9; ++i) {
    positions.push_back(small.insert(static_cast<char>('a' + i)));
  }
  small.erase(positions[3]);
  small.erase(positions[4]);
  small.insert('z');
  EXPECT_EQ(small.size(), 8);
  expectConsistentInBothDirections(small);
}

struct ThrowingValue {
  static inline int live = 0;
  static inline int constructed = 0;
  static inline int throwAt = -1;

  explicit ThrowingValue(int v) : value(v) {
    if (constructed++ == throwAt) {
      throw std::runtime_error("construction failed");
    }
    ++live;
  }
  ThrowingValue(const ThrowingValue& other) : ThrowingValue(other.value) {}
  ThrowingValue(ThrowingValue&& other) noexcept : value(other.value) { ++live; }
  ThrowingValue& operator=(const ThrowingValue&) = delete;
  ThrowingValue& operator=(ThrowingValue&&) = delete;
  ~ThrowingValue() { --live; }

  static void reset() {
    live = 0;
    constructed = 0;
    throwAt = -1;
  }

  int value;
};

TEST(Hive, FailedConstructionDoesNotConsumeErasedSlot) {
  ThrowingValue::reset();
  {
    hive<ThrowingValue> values;
    auto erased = values.emplace(1);
    values.emplace(2);
    auto* erasedAddress = std::addressof(*erased);
    values.erase(erased);

    ThrowingValue::throwAt = ThrowingValue::constructed;
    EXPECT_THROW(values.emplace(3), std::runtime_error);
    ThrowingValue::throwAt = -1;
    EXPECT_EQ(values.size(), 1);
    EXPECT_EQ(ThrowingValue::live, 1);

    auto replacement = values.emplace(4);
    EXPECT_EQ(std::addressof(*replacement), erasedAddress);
    EXPECT_EQ(values.size(), 2);

    values.clear();
    EXPECT_EQ(ThrowingValue::live, 0);
  }
  EXPECT_EQ(ThrowingValue::live, 0);
}

TEST(Hive, FailedConstructionInAFreshBlockLeavesTheHiveUsable) {
  ThrowingValue::reset();
  {
    hive<ThrowingValue> values(hive_limits{4, 4});
    for (int i = 0; i < 4; ++i) {
      values.emplace(i);
    }
    // The next insertion has to open a new block.
    ThrowingValue::throwAt = ThrowingValue::constructed;
    EXPECT_THROW(values.emplace(4), std::runtime_error);
    ThrowingValue::throwAt = -1;

    EXPECT_EQ(values.size(), 4);
    EXPECT_EQ(ThrowingValue::live, 4);
    values.emplace(5);
    EXPECT_EQ(values.size(), 5);
    EXPECT_EQ(ThrowingValue::live, 5);
  }
  EXPECT_EQ(ThrowingValue::live, 0);
}

TEST(Hive, ThrowingConstructorDestroysWhatWasAlreadyBuilt) {
  ThrowingValue::reset();
  std::vector<ThrowingValue> source;
  source.reserve(20);
  for (int i = 0; i < 20; ++i) {
    source.emplace_back(i);
  }

  ThrowingValue::throwAt = ThrowingValue::constructed + 12;
  EXPECT_THROW(
      (hive<ThrowingValue>(source.begin(), source.end(), hive_limits{4, 4})),
      std::runtime_error);
  ThrowingValue::throwAt = -1;

  EXPECT_EQ(ThrowingValue::live, static_cast<int>(source.size()));
  source.clear();
  EXPECT_EQ(ThrowingValue::live, 0);
}

struct AllocationState {
  std::size_t allocations = 0;
  std::size_t deallocations = 0;
  std::size_t liveBytes = 0;
};

template <typename T, bool Propagate>
struct TrackingAllocator {
  using value_type = T;
  using propagate_on_container_copy_assignment = std::bool_constant<Propagate>;
  using propagate_on_container_move_assignment = std::bool_constant<Propagate>;
  using propagate_on_container_swap = std::bool_constant<Propagate>;

  template <typename U>
  struct rebind {
    using other = TrackingAllocator<U, Propagate>;
  };

  explicit TrackingAllocator(AllocationState* s) : state(s) {}

  template <typename U>
  /* implicit */ TrackingAllocator(const TrackingAllocator<U, Propagate>& other)
      : state(other.state) {}

  T* allocate(std::size_t count) {
    ++state->allocations;
    state->liveBytes += count * sizeof(T);
    return std::allocator<T>{}.allocate(count);
  }

  void deallocate(T* address, std::size_t count) noexcept {
    ++state->deallocations;
    state->liveBytes -= count * sizeof(T);
    std::allocator<T>{}.deallocate(address, count);
  }

  template <typename U>
  bool operator==(const TrackingAllocator<U, Propagate>& other) const noexcept {
    return state == other.state;
  }
  template <typename U>
  bool operator!=(const TrackingAllocator<U, Propagate>& other) const noexcept {
    return state != other.state;
  }

  AllocationState* state;
};

template <bool Propagate>
using TrackedHive = hive<int, TrackingAllocator<int, Propagate>>;

TEST(Hive, UsesProvidedAllocatorAndReturnsEveryBlock) {
  AllocationState state;
  {
    TrackedHive<true> values{TrackingAllocator<int, true>{&state}};
    for (int i = 0; i < 100; ++i) {
      values.insert(i);
    }
    EXPECT_GT(state.allocations, 0);
    EXPECT_GT(state.liveBytes, 0);
    EXPECT_EQ(values.get_allocator().state, &state);
  }
  EXPECT_EQ(state.allocations, state.deallocations);
  EXPECT_EQ(state.liveBytes, 0);
}

TEST(Hive, ClearKeepsBlocksForReuseAndTrimReturnsThem) {
  AllocationState state;
  TrackedHive<true> values{TrackingAllocator<int, true>{&state}};
  for (int i = 0; i < 100; ++i) {
    values.insert(i);
  }
  const auto capacity = values.capacity();
  const auto allocations = state.allocations;

  values.clear();
  EXPECT_EQ(values.capacity(), capacity);
  for (int i = 0; i < 100; ++i) {
    values.insert(i);
  }
  EXPECT_EQ(state.allocations, allocations);
  EXPECT_EQ(values.size(), 100);

  values.clear();
  values.trim_capacity();
  EXPECT_EQ(values.capacity(), 0);
  EXPECT_EQ(state.liveBytes, 0);
}

TEST(Hive, AllocatorPropagatesOnCopyAssignment) {
  AllocationState left;
  AllocationState right;
  {
    TrackedHive<true> source{TrackingAllocator<int, true>{&left}};
    source.insert(1);
    source.insert(2);

    TrackedHive<true> target{TrackingAllocator<int, true>{&right}};
    target.insert(3);
    target = source;

    EXPECT_EQ(target.get_allocator().state, &left);
    EXPECT_EQ(right.liveBytes, 0);
    EXPECT_EQ(elementsOf(target), (std::vector<int>{1, 2}));
  }
  EXPECT_EQ(left.allocations, left.deallocations);
  EXPECT_EQ(right.allocations, right.deallocations);
  EXPECT_EQ(left.liveBytes, 0);
}

TEST(Hive, AllocatorStaysPutWhenPropagationIsDisabled) {
  AllocationState left;
  AllocationState right;
  {
    TrackedHive<false> source{TrackingAllocator<int, false>{&left}};
    source.insert(1);
    source.insert(2);

    TrackedHive<false> target{TrackingAllocator<int, false>{&right}};
    target = source;
    EXPECT_EQ(target.get_allocator().state, &right);
    EXPECT_EQ(elementsOf(target), (std::vector<int>{1, 2}));

    // Unequal allocators mean a move assignment has to move the elements
    // rather than adopt the source's memory.
    target = std::move(source);
    EXPECT_EQ(target.get_allocator().state, &right);
    EXPECT_EQ(elementsOf(target), (std::vector<int>{1, 2}));
    EXPECT_TRUE(source.empty());
  }
  EXPECT_EQ(left.allocations, left.deallocations);
  EXPECT_EQ(right.allocations, right.deallocations);
  EXPECT_EQ(left.liveBytes, 0);
  EXPECT_EQ(right.liveBytes, 0);
}

TEST(Hive, SwapExchangesAllocatorsWhenTheyPropagate) {
  AllocationState left;
  AllocationState right;
  {
    TrackedHive<true> first{TrackingAllocator<int, true>{&left}};
    first.insert(1);
    TrackedHive<true> second{TrackingAllocator<int, true>{&right}};
    second.insert(2);
    second.insert(3);

    const int* address = std::addressof(*first.begin());
    first.swap(second);

    EXPECT_EQ(first.get_allocator().state, &right);
    EXPECT_EQ(second.get_allocator().state, &left);
    EXPECT_EQ(elementsOf(first), (std::vector<int>{2, 3}));
    EXPECT_EQ(std::addressof(*second.begin()), address);
    // An iterator taken before the swap now belongs to the other hive.
    EXPECT_EQ(second.get_iterator(address), second.begin());
  }
  EXPECT_EQ(left.allocations, left.deallocations);
  EXPECT_EQ(right.allocations, right.deallocations);
}

TEST(Hive, MoveConstructionWithAnUnequalAllocatorMovesElements) {
  AllocationState left;
  AllocationState right;
  {
    TrackedHive<false> source{TrackingAllocator<int, false>{&left}};
    for (int i = 0; i < 20; ++i) {
      source.insert(i);
    }
    TrackedHive<false> target{
        std::move(source), TrackingAllocator<int, false>{&right}};

    EXPECT_EQ(target.get_allocator().state, &right);
    EXPECT_EQ(target.size(), 20);
    EXPECT_GT(right.allocations, 0);
    EXPECT_TRUE(source.empty());
  }
  EXPECT_EQ(left.allocations, left.deallocations);
  EXPECT_EQ(right.allocations, right.deallocations);
}

TEST(Hive, CopyMoveAndSwap) {
  hive<std::string> source{"a", "b", "c"};
  hive<std::string> copy = source;
  EXPECT_EQ(elementsOf(copy), elementsOf(source));

  hive<std::string> moved = std::move(copy);
  EXPECT_EQ(elementsOf(moved), (std::vector<std::string>{"a", "b", "c"}));
  EXPECT_TRUE(copy.empty());

  hive<std::string> other{"x"};
  folly::swap(moved, other);
  EXPECT_EQ(elementsOf(moved), (std::vector<std::string>{"x"}));
  EXPECT_EQ(elementsOf(other), (std::vector<std::string>{"a", "b", "c"}));
  EXPECT_NE(elementsOf(moved), elementsOf(other));

  moved = {"q", "r"};
  EXPECT_EQ(elementsOf(moved), (std::vector<std::string>{"q", "r"}));
  moved.assign(3, "z");
  EXPECT_EQ(elementsOf(moved), (std::vector<std::string>{"z", "z", "z"}));
}

TEST(Hive, CountAndRangeConstructors) {
  hive<int> counted(4, 7);
  EXPECT_EQ(elementsOf(counted), (std::vector<int>{7, 7, 7, 7}));

  hive<int> defaulted(3);
  EXPECT_EQ(elementsOf(defaulted), (std::vector<int>{0, 0, 0}));

  const std::vector<int> source{1, 2, 3};
  hive<int> fromRange(source.begin(), source.end(), hive_limits{3, 3});
  EXPECT_EQ(elementsOf(fromRange), source);
  EXPECT_EQ(fromRange.block_capacity_limits().max, 3);
}

TEST(Hive, RandomizedInsertErasePreservesLiveElementsAndAddresses) {
  struct Element {
    int id;
    int payload;
  };

  hive<Element> values(hive_limits{3, 32});
  std::map<int, hive<Element>::iterator> live;
  std::map<int, const Element*> addresses;
  std::mt19937 random{0x48495645};
  int nextId = 0;

  for (int step = 0; step < 5000; ++step) {
    if (live.empty() || random() % 3 != 0) {
      const int id = nextId++;
      auto it = values.emplace(Element{id, id * 7});
      live.emplace(id, it);
      addresses.emplace(id, std::addressof(*it));
    } else {
      auto selected = live.begin();
      std::advance(selected, random() % live.size());
      values.erase(selected->second);
      addresses.erase(selected->first);
      live.erase(selected);
    }

    ASSERT_EQ(values.size(), live.size());
    std::size_t visited = 0;
    for (const auto& element : values) {
      auto found = live.find(element.id);
      ASSERT_NE(found, live.end());
      ASSERT_EQ(element.payload, element.id * 7);
      ASSERT_EQ(std::addressof(element), addresses.at(element.id));
      ++visited;
    }
    ASSERT_EQ(visited, live.size());

    std::size_t visitedBackwards = 0;
    for (auto it = values.rbegin(); it != values.rend(); ++it) {
      ++visitedBackwards;
    }
    ASSERT_EQ(visitedBackwards, live.size());

    // The pointer-to-iterator lookup the Surreal graph relies on has to agree
    // with the addresses handed out at insertion time.
    if (!live.empty() && step % 16 == 0) {
      auto probe = live.begin();
      std::advance(probe, random() % live.size());
      auto found = values.get_iterator(addresses.at(probe->first));
      ASSERT_NE(found, values.end());
      ASSERT_EQ(found->id, probe->first);
    }
  }
}

//
// Regression coverage for defects found in review of the first implementation.
//

/// Overwrites every byte of its own storage before throwing, so a slot left
/// holding a free-list node cannot survive the attempt by luck.
struct PoisonsStorageThenThrows {
  static inline int throwAt = -1;
  static inline int constructed = 0;
  static inline int live = 0;

  explicit PoisonsStorageThenThrows(int value) {
    std::memset(payload, 0xA5, sizeof(payload));
    if (constructed++ == throwAt) {
      throw std::runtime_error("construction failed after writing storage");
    }
    std::memcpy(payload, &value, sizeof(value));
    ++live;
  }
  PoisonsStorageThenThrows(const PoisonsStorageThenThrows& other)
      : PoisonsStorageThenThrows(other.value()) {}
  PoisonsStorageThenThrows& operator=(const PoisonsStorageThenThrows&) = delete;
  ~PoisonsStorageThenThrows() { --live; }

  int value() const {
    int result = 0;
    std::memcpy(&result, payload, sizeof(result));
    return result;
  }

  static void reset() {
    throwAt = -1;
    constructed = 0;
    live = 0;
  }

  // Wide enough to cover the free-list node the hive keeps in an erased slot.
  unsigned char payload[4 * sizeof(void*)] = {};
};

// Defect 1: a constructor that throws after writing into an erased slot used
// to leave the free list pointing into its wreckage.
TEST(Hive, ThrowingConstructionRestoresTheFreeListInARunOfOne) {
  PoisonsStorageThenThrows::reset();
  {
    hive<PoisonsStorageThenThrows> values(hive_limits{4, 4});
    for (int i = 0; i < 4; ++i) {
      values.emplace(i);
    }
    auto erased = values.begin();
    std::advance(erased, 2);
    const auto* address = std::addressof(*erased);
    values.erase(erased);

    PoisonsStorageThenThrows::throwAt = PoisonsStorageThenThrows::constructed;
    EXPECT_THROW(values.emplace(99), std::runtime_error);
    PoisonsStorageThenThrows::throwAt = -1;

    auto replacement = values.emplace(7);
    EXPECT_EQ(replacement->value(), 7);
    EXPECT_EQ(std::addressof(*replacement), address);
    EXPECT_EQ(values.size(), 4);

    // Anything further has to land in fresh storage, all of it distinct.
    std::vector<const PoisonsStorageThenThrows*> addresses;
    addresses.reserve(12);
    for (int i = 0; i < 12; ++i) {
      addresses.push_back(std::addressof(*values.emplace(100 + i)));
    }
    std::sort(addresses.begin(), addresses.end());
    EXPECT_EQ(
        std::adjacent_find(addresses.begin(), addresses.end()),
        addresses.end());
    EXPECT_EQ(values.size(), 16);
    EXPECT_EQ(
        static_cast<std::size_t>(std::distance(values.begin(), values.end())),
        values.size());
    EXPECT_EQ(PoisonsStorageThenThrows::live, 16);
  }
  EXPECT_EQ(PoisonsStorageThenThrows::live, 0);
}

TEST(Hive, ThrowingConstructionRestoresTheFreeListInALongerRun) {
  PoisonsStorageThenThrows::reset();
  {
    hive<PoisonsStorageThenThrows> values(hive_limits{8, 8});
    std::vector<hive<PoisonsStorageThenThrows>::iterator> positions;
    positions.reserve(8);
    for (int i = 0; i < 8; ++i) {
      positions.push_back(values.emplace(i));
    }
    // One run of four erased slots, so the insertion has to split it.
    std::vector<const PoisonsStorageThenThrows*> freed;
    freed.reserve(4);
    for (int i = 2; i < 6; ++i) {
      freed.push_back(std::addressof(*positions[i]));
      values.erase(positions[i]);
    }
    ASSERT_EQ(values.size(), 4);

    PoisonsStorageThenThrows::throwAt = PoisonsStorageThenThrows::constructed;
    EXPECT_THROW(values.emplace(99), std::runtime_error);
    PoisonsStorageThenThrows::throwAt = -1;

    EXPECT_EQ(values.size(), 4);
    EXPECT_EQ(PoisonsStorageThenThrows::live, 4);

    // All four freed slots must still be reachable, one insertion each, and
    // the block must not have to grow to take them.
    const auto capacity = values.capacity();
    std::vector<const PoisonsStorageThenThrows*> refilled;
    refilled.reserve(4);
    for (int i = 0; i < 4; ++i) {
      refilled.push_back(std::addressof(*values.emplace(200 + i)));
    }
    EXPECT_EQ(values.capacity(), capacity);
    EXPECT_EQ(values.size(), 8);
    EXPECT_EQ(
        static_cast<std::size_t>(std::distance(values.begin(), values.end())),
        values.size());

    std::sort(freed.begin(), freed.end());
    std::sort(refilled.begin(), refilled.end());
    EXPECT_EQ(freed, refilled);

    std::vector<int> seen;
    seen.reserve(values.size());
    for (const auto& element : values) {
      seen.push_back(element.value());
    }
    std::sort(seen.begin(), seen.end());
    EXPECT_EQ(seen, (std::vector<int>{0, 1, 6, 7, 200, 201, 202, 203}));
  }
  EXPECT_EQ(PoisonsStorageThenThrows::live, 0);
}

// Defect 2: assignment took on the source's block limits but kept spare
// blocks the new limits forbid, which later surfaced as a splice failure.
// The block capacity limits belong to the hive, not to its contents, so
// assignment leaves the destination's own limits in force - and every block
// it goes on to allocate keeps honouring them, which the splice checks.
TEST(Hive, CopyAssignmentKeepsTheDestinationLimits) {
  hive<int> source(hive_limits{8, 8});
  source.insert(7);

  hive<int> target(hive_limits{3, 3});
  for (int i = 0; i < 3; ++i) {
    target.insert(i);
  }
  target = source;

  EXPECT_EQ(target.block_capacity_limits().min, 3);
  EXPECT_EQ(target.block_capacity_limits().max, 3);
  EXPECT_EQ(elementsOf(target), (std::vector<int>{7}));
  for (int i = 0; i < 20; ++i) {
    target.insert(i);
  }

  hive<int> destination(hive_limits{3, 3});
  destination.insert(1);
  EXPECT_NO_THROW(destination.splice(target));
  EXPECT_EQ(destination.size(), 22);
  expectConsistentInBothDirections(destination);
}

// Same for a move assignment that has to move the elements one at a time,
// because the allocators are unequal and do not propagate: the destination
// keeps its own blocks, so it keeps its own limits.
TEST(Hive, MoveAssignmentWithAnUnequalAllocatorKeepsTheDestinationLimits) {
  AllocationState left;
  AllocationState right;
  {
    TrackedHive<false> source{TrackingAllocator<int, false>{&left}};
    source.reshape(hive_limits{8, 8});
    source.insert(7);

    TrackedHive<false> target{TrackingAllocator<int, false>{&right}};
    target.reshape(hive_limits{3, 3});
    for (int i = 0; i < 3; ++i) {
      target.insert(i);
    }
    target = std::move(source);

    EXPECT_EQ(target.block_capacity_limits().min, 3);
    EXPECT_EQ(target.block_capacity_limits().max, 3);
    for (int i = 0; i < 20; ++i) {
      target.insert(i);
    }

    TrackedHive<false> destination{TrackingAllocator<int, false>{&right}};
    destination.reshape(hive_limits{3, 3});
    destination.insert(1);
    EXPECT_NO_THROW(destination.splice(target));
    EXPECT_EQ(destination.size(), 22);
  }
  EXPECT_EQ(left.allocations, left.deallocations);
  EXPECT_EQ(right.allocations, right.deallocations);
}

// Defect 3: assign(count, value) cleared the hive first, destroying the very
// element it was then told to copy.
struct PoisonsItselfOnDestruction {
  explicit PoisonsItselfOnDestruction(int v) : value(v) {}
  PoisonsItselfOnDestruction(const PoisonsItselfOnDestruction& other)
      : value(other.value) {}
  PoisonsItselfOnDestruction& operator=(const PoisonsItselfOnDestruction&) =
      delete;
  ~PoisonsItselfOnDestruction() { value = -999; }

  int value;
};

TEST(Hive, AssignFromOneOfItsOwnElements) {
  hive<PoisonsItselfOnDestruction> values;
  auto position = values.emplace(7);
  const PoisonsItselfOnDestruction& alias = *position;

  values.assign(3, alias);

  EXPECT_EQ(values.size(), 3);
  for (const auto& element : values) {
    EXPECT_EQ(element.value, 7);
  }
}

//
// An allocator that can be told to fail its next allocation or its next
// construction, and that counts the bytes it has handed out.
//
struct InjectionState {
  bool failAllocate = false;
  int failConstructAt = -1;
  int constructCalls = 0;
  std::size_t allocations = 0;
  std::size_t deallocations = 0;
  std::size_t liveBytes = 0;
  // How many objects of each size the hive asked for, so a test can tell
  // which of the hive's internal types came from this allocator.
  std::map<std::size_t, std::size_t> allocationsByElementSize;
};

template <typename T>
struct InjectingAllocator {
  using value_type = T;

  explicit InjectingAllocator(InjectionState* s) : state(s) {}

  template <typename U>
  /* implicit */ InjectingAllocator(const InjectingAllocator<U>& other)
      : state(other.state) {}

  T* allocate(std::size_t count) {
    if (state->failAllocate) {
      throw std::bad_alloc{};
    }
    ++state->allocations;
    ++state->allocationsByElementSize[sizeof(T)];
    state->liveBytes += count * sizeof(T);
    return std::allocator<T>{}.allocate(count);
  }

  void deallocate(T* address, std::size_t count) noexcept {
    ++state->deallocations;
    state->liveBytes -= count * sizeof(T);
    std::allocator<T>{}.deallocate(address, count);
  }

  template <typename U, typename... Args>
  void construct(U* address, Args&&... args) {
    if (state->constructCalls++ == state->failConstructAt) {
      throw std::runtime_error("allocator construct failed");
    }
    ::new (static_cast<void*>(address)) U(std::forward<Args>(args)...);
  }

  template <typename U>
  void destroy(U* address) noexcept {
    address->~U();
  }

  template <typename U>
  bool operator==(const InjectingAllocator<U>& other) const noexcept {
    return state == other.state;
  }
  template <typename U>
  bool operator!=(const InjectingAllocator<U>& other) const noexcept {
    return state != other.state;
  }

  InjectionState* state;
};

using InjectedHive = hive<int, InjectingAllocator<int>>;

// Defect 4: reshape changed the limits before doing the work that could fail.
TEST(Hive, ReshapeLeavesEverythingAloneWhenAllocationFails) {
  InjectionState state;
  {
    InjectedHive values(hive_limits{3, 3}, InjectingAllocator<int>{&state});
    for (int i = 0; i < 6; ++i) {
      values.insert(i);
    }
    const auto capacity = values.capacity();

    state.failAllocate = true;
    EXPECT_THROW(values.reshape(hive_limits{8, 8}), std::bad_alloc);
    state.failAllocate = false;

    EXPECT_EQ(values.block_capacity_limits().min, 3);
    EXPECT_EQ(values.block_capacity_limits().max, 3);
    EXPECT_EQ(values.capacity(), capacity);
    EXPECT_EQ(elementsOf(values), (std::vector<int>{0, 1, 2, 3, 4, 5}));
    EXPECT_NO_THROW(values.insert(6));
  }
  EXPECT_EQ(state.allocations, state.deallocations);
  EXPECT_EQ(state.liveBytes, 0);
}

/// Has no move constructor, so moving elements out of it copies them - and
/// its copy constructor is the one that throws.
struct CopyOnlyThrowingValue {
  static inline int live = 0;
  static inline int copies = 0;
  static inline int throwAt = -1;

  explicit CopyOnlyThrowingValue(int v) : value(v) { ++live; }
  CopyOnlyThrowingValue(const CopyOnlyThrowingValue& other)
      : value(other.value) {
    if (copies++ == throwAt) {
      throw std::runtime_error("copy failed");
    }
    ++live;
  }
  CopyOnlyThrowingValue& operator=(const CopyOnlyThrowingValue&) = delete;
  ~CopyOnlyThrowingValue() { --live; }

  static void reset() {
    live = 0;
    copies = 0;
    throwAt = -1;
  }

  int value;
};

TEST(Hive, ReshapeLeavesEverythingAloneWhenAnElementThrows) {
  CopyOnlyThrowingValue::reset();
  {
    hive<CopyOnlyThrowingValue> values(hive_limits{3, 3});
    std::vector<const CopyOnlyThrowingValue*> addresses;
    addresses.reserve(6);
    for (int i = 0; i < 6; ++i) {
      addresses.push_back(std::addressof(*values.emplace(i)));
    }
    const auto capacity = values.capacity();

    CopyOnlyThrowingValue::throwAt = CopyOnlyThrowingValue::copies + 2;
    EXPECT_THROW(values.reshape(hive_limits{8, 8}), std::runtime_error);
    CopyOnlyThrowingValue::throwAt = -1;

    EXPECT_EQ(values.block_capacity_limits().max, 3);
    EXPECT_EQ(values.capacity(), capacity);
    EXPECT_EQ(values.size(), 6);
    EXPECT_EQ(CopyOnlyThrowingValue::live, 6);

    // A failed reshape moves nothing, so the elements are where they were.
    std::size_t index = 0;
    for (const auto& element : values) {
      EXPECT_EQ(std::addressof(element), addresses[index]);
      EXPECT_EQ(element.value, static_cast<int>(index));
      ++index;
    }
    EXPECT_EQ(index, 6);
  }
  EXPECT_EQ(CopyOnlyThrowingValue::live, 0);
}

TEST(Hive, ReshapeToConformingLimitsDropsForbiddenSpares) {
  hive<int> values(hive_limits{4, 4});
  values.reserve(40);
  values.insert(1);
  const auto capacity = values.capacity();
  ASSERT_GE(capacity, 40);

  values.reshape(hive_limits{4, 4});
  EXPECT_EQ(values.capacity(), capacity);

  // The spare 4-slot blocks cannot be handed to a hive whose blocks must hold
  // at least eight, so reshape has to let them go.
  values.reshape(hive_limits{8, 8});
  EXPECT_EQ(values.size(), 1);
  for (int i = 0; i < 20; ++i) {
    values.insert(i);
  }
  hive<int> destination(hive_limits{8, 8});
  destination.insert(0);
  EXPECT_NO_THROW(destination.splice(values));
}

//
// An allocator whose bound is set per test, to put max_size() within reach.
//
template <typename T>
struct BoundedAllocator {
  using value_type = T;

  explicit BoundedAllocator(std::size_t b) : bound(b) {}
  template <typename U>
  /* implicit */ BoundedAllocator(const BoundedAllocator<U>& other)
      : bound(other.bound) {}

  T* allocate(std::size_t count) {
    if (count > max_size()) {
      throw std::bad_alloc{};
    }
    return std::allocator<T>{}.allocate(count);
  }
  void deallocate(T* address, std::size_t count) noexcept {
    std::allocator<T>{}.deallocate(address, count);
  }
  // Only the element type is bounded. A real allocator's max_size is
  // per-type, and the hive's own bookkeeping allocations - block headers,
  // skipfields - are not what these tests are about.
  std::size_t max_size() const noexcept {
    if constexpr (std::is_same_v<T, int>) {
      return bound;
    } else {
      return std::allocator_traits<std::allocator<T>>::max_size(
          std::allocator<T>{});
    }
  }

  template <typename U>
  bool operator==(const BoundedAllocator<U>& other) const noexcept {
    return bound == other.bound;
  }
  template <typename U>
  bool operator!=(const BoundedAllocator<U>& other) const noexcept {
    return bound != other.bound;
  }

  std::size_t bound;
};

using BoundedHive = hive<int, BoundedAllocator<int>>;

// max_size() is the allocator's bound. Blocks are whole, so the storage can
// run past it by less than a block, but size() never does and capacity() -
// what the hive can hold without allocating - never reports more.
TEST(Hive, NeitherSizeNorCapacityPassesMaxSize) {
  BoundedHive values(hive_limits{3, 3}, BoundedAllocator<int>{5});
  ASSERT_EQ(values.max_size(), 5);

  // Two blocks of three hold six slots, but only five elements may live here.
  values.reserve(values.max_size());
  EXPECT_EQ(values.capacity(), values.max_size());

  for (std::size_t i = 0; i < values.max_size(); ++i) {
    values.insert(static_cast<int>(i));
  }
  EXPECT_EQ(values.size(), values.max_size());
  EXPECT_EQ(values.capacity(), values.max_size());

  // A free slot is left, and it is still not usable.
  EXPECT_THROW(values.insert(99), std::length_error);
  EXPECT_EQ(values.size(), values.max_size());
  EXPECT_THROW(values.reserve(values.max_size() + 1), std::length_error);
  EXPECT_THROW(values.insert(3, 0), std::length_error);
  EXPECT_EQ(values.size(), values.max_size());
  expectConsistentInBothDirections(values);
}

// A hive at its ceiling must still be made of legal blocks: splice checks
// every source block against the destination's limits, so a hive that can be
// spliced into an empty one with the same limits has no illegal block.
void expectEveryBlockHonoursLimits(BoundedHive& values, hive_limits limits) {
  const auto size = values.size();
  BoundedHive destination(limits, values.get_allocator());
  ASSERT_NO_THROW(destination.splice(values));
  EXPECT_EQ(destination.size(), size);
  values.splice(destination);
  EXPECT_EQ(values.size(), size);
}

TEST(Hive, TinyMaxSizeWithFixedBlockCapacity) {
  const hive_limits limits{3, 3};
  BoundedHive values(limits, BoundedAllocator<int>{5});
  ASSERT_EQ(values.max_size(), 5);

  for (int i = 0; i < 5; ++i) {
    values.insert(i);
  }
  EXPECT_EQ(values.size(), 5);
  EXPECT_EQ(values.capacity(), 5);
  EXPECT_THROW(values.insert(5), std::length_error);
  EXPECT_EQ(elementsOf(values), (std::vector<int>{0, 1, 2, 3, 4}));
  expectEveryBlockHonoursLimits(values, limits);
}

TEST(Hive, TinyMaxSizeWithVariableBlockCapacity) {
  const hive_limits limits{3, 5};
  BoundedHive values(limits, BoundedAllocator<int>{7});
  ASSERT_EQ(values.max_size(), 7);

  for (int i = 0; i < 7; ++i) {
    values.insert(i);
  }
  EXPECT_EQ(values.size(), 7);
  EXPECT_EQ(values.capacity(), 7);
  EXPECT_THROW(values.insert(7), std::length_error);
  expectConsistentInBothDirections(values);
  expectEveryBlockHonoursLimits(values, limits);
}

TEST(Hive, MaxSizeIsTheAllocatorsBoundEvenWhenBlocksCannotSumToIt) {
  // Eleven is not a sum of blocks of four or five. The whole bound is still
  // usable: the third block takes the storage to twelve or more slots, and
  // capacity() reports the eleven the hive can hold.
  BoundedHive values(hive_limits{4, 5}, BoundedAllocator<int>{11});
  ASSERT_EQ(values.max_size(), 11);

  values.reserve(values.max_size());
  EXPECT_EQ(values.capacity(), 11);
  for (int i = 0; i < 11; ++i) {
    values.insert(i);
  }
  EXPECT_EQ(values.size(), 11);
  EXPECT_THROW(values.insert(11), std::length_error);
  expectEveryBlockHonoursLimits(values, hive_limits{4, 5});
}

TEST(Hive, EveryBlockHonoursTheLimitsRightUpToTheCeiling) {
  for (std::size_t bound = 3; bound <= 24; ++bound) {
    for (const hive_limits limits :
         {hive_limits{3, 3}, hive_limits{3, 5}, hive_limits{4, 5}}) {
      BoundedHive values(limits, BoundedAllocator<int>{bound});
      const auto maxSize = values.max_size();
      // No block within these limits fits under a bound below the minimum.
      ASSERT_EQ(maxSize, bound < limits.min ? 0 : bound);

      std::size_t inserted = 0;
      while (inserted < maxSize) {
        values.insert(static_cast<int>(inserted));
        ++inserted;
      }
      EXPECT_EQ(values.size(), maxSize);
      EXPECT_LE(values.capacity(), maxSize);
      EXPECT_THROW(values.insert(0), std::length_error);
      expectConsistentInBothDirections(values);
      expectEveryBlockHonoursLimits(values, limits);
    }
  }
}

// [hive.capacity]: reserve(n) throws only for n > max_size(), and must reach
// capacity() >= n otherwise - even when the room left under the allocator's
// bound is smaller than the smallest block the limits allow, and without
// moving any element to make room.
TEST(Hive, ReserveReachesEveryCountUpToMaxSize) {
  BoundedHive values(hive_limits{8, 8}, BoundedAllocator<int>{65535});
  values.reserve(65528);
  for (int i = 0; i < 65528; ++i) {
    values.insert(i);
  }
  const int* const first = std::addressof(*values.begin());
  values.reshape(hive_limits{8, 9});
  ASSERT_EQ(values.capacity(), 65528);
  ASSERT_EQ(values.max_size(), 65535);

  EXPECT_NO_THROW(values.reserve(65529));
  EXPECT_GE(values.capacity(), 65529);
  EXPECT_EQ(std::addressof(*values.begin()), first);
  EXPECT_NO_THROW(values.reserve(65535));
  EXPECT_EQ(values.capacity(), 65535);
  EXPECT_THROW(values.reserve(65536), std::length_error);
}

// [hive.operations]: the only failure splice() names is a source block
// outside the destination's limits. Spare capacity the destination holds
// must not stand in the way.
TEST(Hive, DestinationSpareCapacityDoesNotBlockSplice) {
  const BoundedAllocator<int> allocator{65535};
  BoundedHive destination(hive_limits{3, 65535}, allocator);
  destination.reserve(65535);
  BoundedHive source(hive_limits{3, 3}, allocator);
  const int* const element = std::addressof(*source.insert(7));

  EXPECT_NO_THROW(destination.splice(source));
  EXPECT_TRUE(source.empty());
  EXPECT_EQ(destination.size(), 1u);
  EXPECT_EQ(std::addressof(*destination.begin()), element);
  EXPECT_LE(destination.capacity(), destination.max_size());
  expectConsistentInBothDirections(destination);
}

TEST(Hive, ReserveRollsBackEveryBlockWhenOneAllocationFails) {
  InjectionState state;
  {
    InjectedHive values(hive_limits{3, 3}, InjectingAllocator<int>{&state});
    values.insert(1);
    const auto capacity = values.capacity();
    const auto allocations = state.allocations;

    state.failAllocate = true;
    EXPECT_THROW(values.reserve(100), std::bad_alloc);
    state.failAllocate = false;

    EXPECT_EQ(values.capacity(), capacity);
    EXPECT_EQ(state.allocations, allocations);
    EXPECT_EQ(elementsOf(values), (std::vector<int>{1}));
  }
  EXPECT_EQ(state.allocations, state.deallocations);
  EXPECT_EQ(state.liveBytes, 0);
}

TEST(Hive, FailedInsertionReleasesTheBlockItHadToAllocate) {
  ThrowingValue::reset();
  {
    hive<ThrowingValue> values(hive_limits{3, 3});
    for (int i = 0; i < 3; ++i) {
      values.emplace(i);
    }
    const auto capacity = values.capacity();

    ThrowingValue::throwAt = ThrowingValue::constructed;
    EXPECT_THROW(values.emplace(3), std::runtime_error);
    ThrowingValue::throwAt = -1;

    EXPECT_EQ(values.capacity(), capacity);
    EXPECT_EQ(values.size(), 3);

    // A block taken from the spare list is handed back instead.
    values.reserve(6);
    const auto reserved = values.capacity();
    ThrowingValue::throwAt = ThrowingValue::constructed;
    EXPECT_THROW(values.emplace(3), std::runtime_error);
    ThrowingValue::throwAt = -1;
    EXPECT_EQ(values.capacity(), reserved);
    EXPECT_NO_THROW(values.emplace(3));
    EXPECT_EQ(values.capacity(), reserved);
  }
  EXPECT_EQ(ThrowingValue::live, 0);
}

// Defect 6: splicing left the destination's last block half used and
// unreachable, while capacity() went on counting its free slots.
TEST(Hive, SpliceKeepsTheDestinationTailUsable) {
  hive<int> destination(hive_limits{10, 10});
  destination.insert(1);
  destination.insert(2);
  hive<int> source(hive_limits{10, 10});
  for (int i = 0; i < 10; ++i) {
    source.insert(100 + i);
  }

  destination.splice(source);
  const auto capacity = destination.capacity();
  const auto slack = capacity - destination.size();
  ASSERT_GT(slack, 0);

  for (std::size_t i = 0; i < slack; ++i) {
    destination.insert(200 + static_cast<int>(i));
  }
  EXPECT_EQ(destination.capacity(), capacity);
  EXPECT_EQ(destination.size(), capacity);
  EXPECT_EQ(
      static_cast<std::size_t>(
          std::distance(destination.begin(), destination.end())),
      destination.size());
  expectConsistentInBothDirections(destination);

  std::vector<int> seen = elementsOf(destination);
  std::sort(seen.begin(), seen.end());
  EXPECT_EQ(std::adjacent_find(seen.begin(), seen.end()), seen.end());
}

TEST(Hive, SpliceOntoAPartlyErasedTailKeepsEveryFreeSlot) {
  hive<int> destination(hive_limits{10, 10});
  std::vector<hive<int>::iterator> positions;
  positions.reserve(6);
  for (int i = 0; i < 6; ++i) {
    positions.push_back(destination.insert(i));
  }
  // Both kinds of free space in the block the source is appended after: an
  // erased run and the slots it has never used.
  destination.erase(positions[4]);
  destination.erase(positions[5]);

  hive<int> source(hive_limits{10, 10});
  for (int i = 0; i < 10; ++i) {
    source.insert(100 + i);
  }
  destination.splice(source);

  const auto capacity = destination.capacity();
  const auto slack = capacity - destination.size();
  for (std::size_t i = 0; i < slack; ++i) {
    destination.insert(200 + static_cast<int>(i));
  }
  EXPECT_EQ(destination.capacity(), capacity);
  EXPECT_EQ(destination.size(), capacity);
  expectConsistentInBothDirections(destination);
}

// The same has to hold of the source's own last block, which is left in the
// middle of the chain once its blocks are appended.
TEST(Hive, SpliceKeepsTheSourceTailUsable) {
  hive<int> destination(hive_limits{10, 10});
  for (int i = 0; i < 10; ++i) {
    destination.insert(i);
  }
  hive<int> source(hive_limits{10, 10});
  source.insert(100);
  source.insert(101);

  destination.splice(source);
  EXPECT_TRUE(source.empty());

  const auto capacity = destination.capacity();
  const auto slack = capacity - destination.size();
  ASSERT_EQ(slack, 8);
  for (std::size_t i = 0; i < slack; ++i) {
    destination.insert(200 + static_cast<int>(i));
  }
  EXPECT_EQ(destination.capacity(), capacity);
  EXPECT_EQ(destination.size(), capacity);
  expectConsistentInBothDirections(destination);

  std::vector<int> seen = elementsOf(destination);
  std::sort(seen.begin(), seen.end());
  EXPECT_EQ(std::adjacent_find(seen.begin(), seen.end()), seen.end());
}

// Free space in a block that is no longer the last one is reached through
// the hive's list of blocks with room, whether it was erased or never used.
TEST(Hive, InsertionReusesFreeSpaceInAnEarlierBlock) {
  hive<int> values(hive_limits{4, 4});
  std::vector<hive<int>::iterator> positions;
  positions.reserve(12);
  for (int i = 0; i < 12; ++i) {
    positions.push_back(values.insert(i));
  }
  // Erase one slot in the first block and one in the last, so a block that
  // is not the last one holds an erased slot on the list of blocks with
  // room.
  values.erase(positions[1]);
  values.erase(positions[9]);
  const auto capacity = values.capacity();

  auto first = values.insert(100);
  auto second = values.insert(101);
  EXPECT_EQ(values.capacity(), capacity);
  EXPECT_EQ(values.size(), 12);

  // Both erased slots are reused; which one goes first is not specified.
  std::vector<const int*> reused{
      std::addressof(*first),
      std::addressof(*second),
  };
  std::vector<const int*> freed{
      std::addressof(*positions[1]),
      std::addressof(*positions[9]),
  };
  std::sort(reused.begin(), reused.end());
  std::sort(freed.begin(), freed.end());
  EXPECT_EQ(reused, freed);
  expectConsistentInBothDirections(values);
}

// Defect 7: an element whose construction fails in a newly allocated block
// must not take the block's storage with it. (The block header itself is
// built directly, never through the allocator, so it cannot be what fails.)
TEST(Hive, ElementConstructionFailureInANewBlockLeaksNothing) {
  InjectionState state;
  {
    InjectedHive values{InjectingAllocator<int>{&state}};
    state.failConstructAt = state.constructCalls;
    EXPECT_THROW(values.insert(1), std::runtime_error);
    state.failConstructAt = -1;

    EXPECT_TRUE(values.empty());
    EXPECT_EQ(values.capacity(), 0);
    EXPECT_NO_THROW(values.insert(1));
    EXPECT_EQ(elementsOf(values), (std::vector<int>{1}));
  }
  EXPECT_EQ(state.allocations, state.deallocations);
  EXPECT_EQ(state.liveBytes, 0);
}

// Bulk insertion is transactional: what it inserted is erased again, and any
// block it had to allocate is released.
TEST(Hive, BulkInsertionRollsBackWhenAnElementThrows) {
  ThrowingValue::reset();
  {
    hive<ThrowingValue> values(hive_limits{4, 4});
    std::vector<const ThrowingValue*> addresses;
    addresses.reserve(3);
    for (int i = 0; i < 3; ++i) {
      addresses.push_back(std::addressof(*values.emplace(i)));
    }

    const ThrowingValue value{9};
    ThrowingValue::throwAt = ThrowingValue::constructed + 2;
    EXPECT_THROW(values.insert(5, value), std::runtime_error);
    ThrowingValue::throwAt = -1;

    EXPECT_EQ(values.size(), 3);
    EXPECT_EQ(ThrowingValue::live, 4); // three in the hive, plus `value`
    std::size_t index = 0;
    for (const auto& element : values) {
      EXPECT_EQ(std::addressof(element), addresses[index]);
      EXPECT_EQ(element.value, static_cast<int>(index));
      ++index;
    }
    EXPECT_EQ(index, 3);
    expectWalksAgreeWithSize(values);
  }
  EXPECT_EQ(ThrowingValue::live, 0);
}

TEST(Hive, BulkRangeInsertionRollsBackWhenAnElementThrows) {
  ThrowingValue::reset();
  {
    std::vector<ThrowingValue> source;
    source.reserve(6);
    for (int i = 0; i < 6; ++i) {
      source.emplace_back(i);
    }

    hive<ThrowingValue> values(hive_limits{4, 4});
    values.emplace(100);
    const auto capacityBefore = values.capacity();

    ThrowingValue::throwAt = ThrowingValue::constructed + 3;
    EXPECT_THROW(
        values.insert(source.begin(), source.end()), std::runtime_error);
    ThrowingValue::throwAt = -1;

    EXPECT_EQ(values.size(), 1);
    EXPECT_EQ(values.begin()->value, 100);
    EXPECT_EQ(values.capacity(), capacityBefore);
    EXPECT_EQ(ThrowingValue::live, 7); // six in the vector, one in the hive
  }
  EXPECT_EQ(ThrowingValue::live, 0);
}

// A failed bulk insertion also puts capacity() back exactly where it was,
// blocks the attempt had to allocate included.
TEST(Hive, FailedBulkInsertionRestoresCapacityExactly) {
  ThrowingValue::reset();
  {
    hive<ThrowingValue> values(hive_limits{4, 4});
    values.emplace(0);
    const auto capacityBefore = values.capacity();
    ASSERT_EQ(capacityBefore, 4);

    // Enough elements to need several new blocks before the throw.
    ThrowingValue::throwAt = ThrowingValue::constructed + 10;
    const ThrowingValue value{7};
    EXPECT_THROW(values.insert(40, value), std::runtime_error);
    ThrowingValue::throwAt = -1;

    EXPECT_EQ(values.size(), 1);
    EXPECT_EQ(values.capacity(), capacityBefore);
    EXPECT_EQ(values.begin()->value, 0);
    expectWalksAgreeWithSize(values);
  }
  EXPECT_EQ(ThrowingValue::live, 0);
}

TEST(Hive, FailedBulkInsertionRestoresCapacityWithSparesAlreadyHeld) {
  ThrowingValue::reset();
  {
    hive<ThrowingValue> values(hive_limits{4, 4});
    values.emplace(0);
    values.reserve(20);
    const auto capacityBefore = values.capacity();
    ASSERT_GE(capacityBefore, 20);

    ThrowingValue::throwAt = ThrowingValue::constructed + 25;
    const ThrowingValue value{7};
    EXPECT_THROW(values.insert(60, value), std::runtime_error);
    ThrowingValue::throwAt = -1;

    // The spare blocks reserved beforehand are still there, and the ones the
    // attempt added are not.
    EXPECT_EQ(values.capacity(), capacityBefore);
    EXPECT_EQ(values.size(), 1);
  }
  EXPECT_EQ(ThrowingValue::live, 0);
}

// The positions a bulk insertion remembers are this container's bookkeeping,
// so they come from this container's allocator. They are needed when the
// hive already holds elements and an element's construction can throw -
// this allocator's construct() can.
TEST(Hive, BulkInsertionBookkeepingUsesTheHivesAllocator) {
  InjectionState state;
  {
    InjectedHive values(hive_limits{4, 4}, InjectingAllocator<int>{&state});
    values.insert(0);
    values.insert(10, 1);
    EXPECT_EQ(values.size(), 11);
    EXPECT_GT(
        state.allocationsByElementSize[sizeof(InjectedHive::iterator)], 0);
  }
  EXPECT_EQ(state.allocations, state.deallocations);
  EXPECT_EQ(state.liveBytes, 0);
}

TEST(Hive, BuildsFromASinglePassRange) {
  const std::vector<int> source{1, 2, 3, 4, 5, 6, 7};
  const auto first = [&] { return SinglePassIterator{source.data()}; };
  const auto last = [&](std::size_t count) {
    return SinglePassIterator{source.data() + count};
  };

  // std::input_iterator asks for a default-initializable, post-incrementable
  // type, and both matter to the hive: the constructors take the iterator by
  // value and step it.
  EXPECT_EQ(SinglePassIterator{}, SinglePassIterator{});
  auto stepped = first();
  stepped++;
  EXPECT_EQ(*stepped, 2);

  hive<int> values(first(), last(source.size()), hive_limits{3, 3});
  EXPECT_EQ(elementsOf(values), source);

  values.insert(first(), last(3));
  EXPECT_EQ(values.size(), 10);
  expectConsistentInBothDirections(values);

  values.assign(first(), last(2));
  EXPECT_EQ(elementsOf(values), (std::vector<int>{1, 2}));
}

// A run of erased slots holds its length at both of its ends, so a step
// across one is a single read and add whatever the run's length: this erases
// every interior slot of the largest block a 16-bit skipfield can describe,
// and the step from the first element to the last is one increment.
TEST(Hive, OneStepCrossesTheLongestRunASkipfieldCanDescribe) {
  constexpr std::size_t kBlockCapacity = 65535;
  hive<int> values(hive_limits{kBlockCapacity, kBlockCapacity});
  std::vector<hive<int>::iterator> positions;
  positions.reserve(kBlockCapacity);
  for (std::size_t i = 0; i < kBlockCapacity; ++i) {
    positions.push_back(values.insert(static_cast<int>(i)));
  }
  const int* firstAddress = std::addressof(*positions.front());
  const int* lastAddress = std::addressof(*positions.back());

  for (std::size_t i = 1; i + 1 < kBlockCapacity; ++i) {
    values.erase(positions[i]);
  }
  ASSERT_EQ(values.size(), 2);

  auto forwards = values.begin();
  EXPECT_EQ(std::addressof(*forwards), firstAddress);
  ++forwards;
  EXPECT_EQ(std::addressof(*forwards), lastAddress);
  ++forwards;
  EXPECT_EQ(forwards, values.end());

  auto backwards = values.end();
  --backwards;
  EXPECT_EQ(std::addressof(*backwards), lastAddress);
  --backwards;
  EXPECT_EQ(std::addressof(*backwards), firstAddress);
  EXPECT_EQ(backwards, values.begin());

  // Refilling consumes the run from its front, and leaves the block with
  // nothing erased in it at all.
  const auto capacity = values.capacity();
  for (std::size_t i = 2; i < kBlockCapacity; ++i) {
    values.insert(0);
  }
  EXPECT_EQ(values.size(), kBlockCapacity);
  EXPECT_EQ(values.capacity(), capacity);
  expectWalksAgreeWithSize(values);
}

// Emptying a block is the uncommon erasure: the block leaves the chain, so
// the iterator returned has to come from the block that follows - or from
// end(), which that same erasure has just moved.
TEST(Hive, ErasingTheLastElementOfABlockKeepsIterationRight) {
  hive<int> values(hive_limits{3, 3});
  std::vector<hive<int>::iterator> positions;
  positions.reserve(9);
  for (int i = 0; i < 9; ++i) {
    positions.push_back(values.insert(i));
  }
  const auto capacity = values.capacity();

  values.erase(positions[3]);
  values.erase(positions[4]);
  auto next = values.erase(positions[5]);
  EXPECT_EQ(*next, 6);
  EXPECT_EQ(elementsOf(values), (std::vector<int>{0, 1, 2, 6, 7, 8}));
  expectConsistentInBothDirections(values);

  values.erase(positions[6]);
  values.erase(positions[7]);
  const auto afterEraseBlock = values.erase(positions[8]);
  EXPECT_EQ(afterEraseBlock, values.end());
  EXPECT_EQ(elementsOf(values), (std::vector<int>{0, 1, 2}));
  expectConsistentInBothDirections(values);

  // Both emptied blocks were kept, and are used again before anything new is
  // allocated.
  EXPECT_EQ(values.capacity(), capacity);
  for (int i = 0; i < 6; ++i) {
    values.insert(100 + i);
  }
  EXPECT_EQ(values.size(), 9);
  EXPECT_EQ(values.capacity(), capacity);
  expectConsistentInBothDirections(values);
}

// erase_if and unique walk the whole hive while erasing, and an erasure that
// empties the last block moves end(). Both stop on a count taken before the
// walk rather than on an iterator that the walk itself can invalidate.
TEST(Hive, EraseIfAndUniqueHandleFreeingTheLastBlock) {
  hive<int> values(hive_limits{3, 3});
  for (int i = 0; i < 9; ++i) {
    values.insert(i);
  }

  EXPECT_EQ(folly::erase_if(values, [](int value) { return value >= 6; }), 3);
  EXPECT_EQ(elementsOf(values), (std::vector<int>{0, 1, 2, 3, 4, 5}));
  expectConsistentInBothDirections(values);

  EXPECT_EQ(folly::erase_if(values, [](int) { return true; }), 6);
  EXPECT_TRUE(values.empty());
  EXPECT_EQ(values.begin(), values.end());
  EXPECT_EQ(folly::erase_if(values, [](int) { return true; }), 0);

  hive<int> repeated(hive_limits{3, 3});
  for (int value : {1, 2, 3, 4, 5, 6, 6, 6, 6}) {
    repeated.insert(value);
  }
  EXPECT_EQ(repeated.unique(), 3);
  EXPECT_EQ(elementsOf(repeated), (std::vector<int>{1, 2, 3, 4, 5, 6}));
  expectConsistentInBothDirections(repeated);
}

// shrink_to_fit releases the blocks holding no elements before it does
// anything that can fail, so a failure keeps every element where it was and
// leaves capacity() no higher than it found it - and never below size().
TEST(Hive, FailedShrinkToFitKeepsEveryElement) {
  InjectionState state;
  {
    InjectedHive values(hive_limits{3, 3}, InjectingAllocator<int>{&state});
    for (int i = 0; i < 7; ++i) {
      values.insert(i);
    }
    values.reserve(20);
    const auto before = elementsOf(values);
    const auto capacityBefore = values.capacity();

    state.failAllocate = true;
    EXPECT_THROW(values.shrink_to_fit(), std::bad_alloc);
    state.failAllocate = false;

    EXPECT_EQ(elementsOf(values), before);
    EXPECT_LE(values.capacity(), capacityBefore);
    EXPECT_GE(values.capacity(), values.size());
    EXPECT_NO_THROW(values.insert(99));
    expectConsistentInBothDirections(values);
  }
  EXPECT_EQ(state.allocations, state.deallocations);
  EXPECT_EQ(state.liveBytes, 0);
}

// M1. shrink_to_fit compacts into blocks sized by the limits in force, and
// those limits can have been widened since the current blocks were
// allocated, so the compacted hive is not always the smaller one. Here a
// single 12-slot block holding 10 elements would be rebuilt as two 8-slot
// blocks: committing that would take capacity from 12 up to 16.
TEST(Hive, ShrinkToFitNeverRaisesCapacity) {
  hive<int> values(hive_limits{12, 12});
  for (int i = 0; i < 10; ++i) {
    values.insert(i);
  }
  ASSERT_EQ(values.capacity(), 12);

  values.reshape(hive_limits{8, 65535});
  ASSERT_EQ(values.capacity(), 12) << "reshape kept the conforming block";

  const auto before = elementsOf(values);
  values.shrink_to_fit();
  EXPECT_LE(values.capacity(), 12);
  EXPECT_GE(values.capacity(), values.size());
  EXPECT_EQ(elementsOf(values), before);
  expectConsistentInBothDirections(values);

  // And it still shrinks when there is something to gain.
  hive<int> roomy(hive_limits{3, 3});
  for (int i = 0; i < 10; ++i) {
    roomy.insert(i);
  }
  roomy.reserve(60);
  const auto inflated = roomy.capacity();
  roomy.shrink_to_fit();
  EXPECT_LT(roomy.capacity(), inflated);
  EXPECT_GE(roomy.capacity(), roomy.size());
}

// An allocator that reports no bound at all still leaves max_size() within
// difference_type, which is what a distance between iterators can express:
// the container requirements define max_size() through that distance.
template <typename T>
struct UnboundedAllocator {
  using value_type = T;
  using size_type = std::size_t;

  UnboundedAllocator() = default;
  template <typename U>
  /* implicit */ UnboundedAllocator(const UnboundedAllocator<U>&) noexcept {}

  T* allocate(std::size_t count) { return std::allocator<T>{}.allocate(count); }
  void deallocate(T* address, std::size_t count) noexcept {
    std::allocator<T>{}.deallocate(address, count);
  }
  std::size_t max_size() const noexcept {
    return std::numeric_limits<std::size_t>::max();
  }
  template <typename U>
  bool operator==(const UnboundedAllocator<U>&) const noexcept {
    return true;
  }
  template <typename U>
  bool operator!=(const UnboundedAllocator<U>&) const noexcept {
    return false;
  }
};

TEST(Hive, MaxSizeStaysWithinDifferenceType) {
  using Unbounded = hive<int, UnboundedAllocator<int>>;
  constexpr auto kDistanceCeiling = static_cast<std::size_t>(
      std::numeric_limits<Unbounded::difference_type>::max());

  Unbounded values;
  EXPECT_EQ(values.max_size(), kDistanceCeiling);
  Unbounded fixed(hive_limits{1000, 1000});
  EXPECT_EQ(fixed.max_size(), kDistanceCeiling);

  values.insert(1);
  EXPECT_EQ(values.size(), 1);
  EXPECT_LE(values.capacity(), values.max_size());
  fixed.insert(1);
  EXPECT_LE(fixed.capacity(), fixed.max_size());

  // std::allocator<char> is bounded only by the address space.
  EXPECT_LE(
      hive<char>{}.max_size(),
      static_cast<std::size_t>(
          std::numeric_limits<hive<char>::difference_type>::max()));
}

// Iterators into one hive are totally ordered, and the order is the order
// iteration visits them - across blocks, across erased runs, and after a
// splice has moved blocks between hives.
static_assert(std::three_way_comparable<hive<int>::iterator>);
static_assert(std::three_way_comparable<hive<int>::const_iterator>);
static_assert( //
    std::same_as<
        std::compare_three_way_result_t<hive<int>::iterator>,
        std::strong_ordering>);

TEST(Hive, IteratorsAreTotallyOrderedInIterationOrder) {
  const auto expectOrderMatchesTraversal = [](hive<int>& values) {
    std::vector<hive<int>::iterator> visited;
    for (auto it = values.begin(); it != values.end(); ++it) {
      visited.push_back(it);
    }
    for (std::size_t i = 0; i < visited.size(); ++i) {
      EXPECT_EQ(visited[i] <=> visited[i], std::strong_ordering::equal);
      for (std::size_t j = i + 1; j < visited.size(); ++j) {
        EXPECT_LT(visited[i], visited[j]) << i << " vs " << j;
        EXPECT_GT(visited[j], visited[i]) << j << " vs " << i;
      }
      // end() is past every element.
      EXPECT_LT(visited[i], values.end());
    }
  };

  // Several blocks, so the order has to come from the blocks and not just
  // from the slot addresses within one.
  hive<int> values(hive_limits{3, 3});
  std::vector<hive<int>::iterator> positions;
  positions.reserve(12);
  for (int i = 0; i < 12; ++i) {
    positions.push_back(values.insert(i));
  }
  expectOrderMatchesTraversal(values);

  // Erasing leaves runs behind; the survivors keep their order.
  for (int i : {1, 2, 5, 9}) {
    values.erase(positions[i]);
  }
  expectOrderMatchesTraversal(values);

  // A splice appends another hive's blocks, which have to be renumbered into
  // this hive's order rather than keeping the keys they had.
  hive<int> other(hive_limits{3, 3});
  for (int i = 100; i < 109; ++i) {
    other.insert(i);
  }
  const auto firstOther = other.begin();
  values.splice(other);
  expectOrderMatchesTraversal(values);
  // The spliced elements sort after everything that was already here.
  EXPECT_LT(values.begin(), firstOther);

  // Refilling erased slots puts elements back in the middle of the order.
  for (int i = 0; i < 4; ++i) {
    values.insert(500 + i);
  }
  expectOrderMatchesTraversal(values);

  // Cross-constness comparison goes through the usual conversion.
  hive<int>::const_iterator constFirst = values.begin();
  EXPECT_EQ(constFirst <=> values.begin(), std::strong_ordering::equal);
  EXPECT_LT(constFirst, values.end());

  // A default-constructed iterator is equivalent only to another one, and
  // orders before every iterator into a hive - including those into the
  // first block - so that <=> never disagrees with ==.
  EXPECT_EQ(
      hive<int>::iterator{} <=> hive<int>::iterator{},
      std::strong_ordering::equal);
  hive<int> single;
  single.insert(1);
  EXPECT_EQ(
      hive<int>::iterator{} <=> single.begin(), std::strong_ordering::less);
  EXPECT_EQ(
      single.begin() <=> hive<int>::iterator{}, std::strong_ordering::greater);
  EXPECT_NE(hive<int>::iterator{}, single.begin());
}

TEST(Hive, RangeMembersAndHintedForms) {
  const std::vector<int> source{1, 2, 3, 4, 5};

  hive<int> values(hive_limits{3, 3});
  values.insert_range(source);
  EXPECT_EQ(elementsOf(values), source);

  // A view, not just a container: the range members take any input range
  // whose reference the element type can be built from.
  values.insert_range(
      source | std::views::transform([](int v) { return v * 10; }));
  EXPECT_EQ(values.size(), 10);
  expectConsistentInBothDirections(values);

  values.assign_range(source);
  EXPECT_EQ(elementsOf(values), source);

  // The hint cannot place an element in a hive, but the hinted forms exist
  // so that code written against the sequence containers compiles.
  const auto hint = values.begin();
  auto placed = values.emplace_hint(hint, 6);
  EXPECT_EQ(*placed, 6);
  EXPECT_EQ(*values.insert(hint, 7), 7);
  const int eight = 8;
  EXPECT_EQ(*values.insert(hint, eight), 8);
  EXPECT_EQ(values.size(), 8);
  expectConsistentInBothDirections(values);
}

TEST(Hive, DeductionGuides) {
  const std::vector<std::string> source{"a", "b"};

  hive fromRange(source.begin(), source.end());
  static_assert(std::same_as<decltype(fromRange), hive<std::string>>);
  EXPECT_EQ(fromRange.size(), 2);

  hive fromRangeWithLimits(source.begin(), source.end(), hive_limits{3, 4});
  static_assert(std::same_as<decltype(fromRangeWithLimits), hive<std::string>>);
  EXPECT_EQ(fromRangeWithLimits.block_capacity_limits().max, 4);

  hive fromList{1, 2, 3};
  static_assert(std::same_as<decltype(fromList), hive<int>>);
  EXPECT_EQ(fromList.size(), 3);

  hive fromListWithLimits({1, 2, 3}, hive_limits{3, 3});
  static_assert(std::same_as<decltype(fromListWithLimits), hive<int>>);
  EXPECT_EQ(fromListWithLimits.block_capacity_limits().min, 3);
}

// An element type the allocator cannot move still has a working move
// constructor and move assignment, because those hand the storage over
// rather than touching an element.
struct Immobile {
  explicit Immobile(int v) : value{v} {}
  Immobile(const Immobile&) = delete;
  Immobile(Immobile&&) = delete;
  Immobile& operator=(const Immobile&) = delete;
  Immobile& operator=(Immobile&&) = delete;
  int value;
};
static_assert(!std::move_constructible<Immobile>);

TEST(Hive, SupportsAnImmobileElementType) {
  hive<Immobile> values(hive_limits{3, 3});
  for (int i = 0; i < 7; ++i) {
    values.emplace(i);
  }
  const Immobile* first = std::addressof(*values.begin());

  hive<Immobile> moved = std::move(values);
  EXPECT_EQ(moved.size(), 7);
  EXPECT_EQ(std::addressof(*moved.begin()), first)
      << "the storage changed hands, so no element moved";

  hive<Immobile> target(hive_limits{3, 3});
  target.emplace(99);
  target = std::move(moved);
  EXPECT_EQ(target.size(), 7);
  EXPECT_EQ(std::addressof(*target.begin()), first);

  // An allocator-extended move with an equal allocator adopts as well.
  hive<Immobile> extended{std::move(target), std::allocator<Immobile>{}};
  EXPECT_EQ(extended.size(), 7);
  EXPECT_EQ(std::addressof(*extended.begin()), first);

  extended.erase(extended.begin());
  EXPECT_EQ(extended.size(), 6);
}

// M5. A Block is this container's own bookkeeping, not an element, so the
// allocator's construct()/destroy() must never be run on one: an allocator
// that does something there - scoped_allocator_adaptor handing itself down,
// or any instrumented allocator - has no business being handed a Block.
// Counting the calls pins it: one per element, and nothing else.
struct ConstructCounts {
  std::size_t constructs = 0;
  std::size_t destroys = 0;
};

template <typename T>
struct ConstructCountingAllocator {
  using value_type = T;

  explicit ConstructCountingAllocator(ConstructCounts* c) : counts(c) {}
  template <typename U>
  /* implicit */ ConstructCountingAllocator(
      const ConstructCountingAllocator<U>& other)
      : counts(other.counts) {}

  T* allocate(std::size_t count) { return std::allocator<T>{}.allocate(count); }
  void deallocate(T* address, std::size_t count) noexcept {
    std::allocator<T>{}.deallocate(address, count);
  }

  template <typename U, typename... Args>
  void construct(U* address, Args&&... args) {
    ++counts->constructs;
    ::new (static_cast<void*>(address)) U(std::forward<Args>(args)...);
  }
  template <typename U>
  void destroy(U* address) noexcept {
    ++counts->destroys;
    address->~U();
  }

  template <typename U>
  bool operator==(const ConstructCountingAllocator<U>& other) const noexcept {
    return counts == other.counts;
  }
  template <typename U>
  bool operator!=(const ConstructCountingAllocator<U>& other) const noexcept {
    return counts != other.counts;
  }

  ConstructCounts* counts;
};

TEST(Hive, AllocatorConstructIsOnlyEverRunOnElements) {
  ConstructCounts counts;
  {
    // Small blocks, so that plenty of them are allocated: each one would be
    // an extra construct() call if blocks went through the allocator.
    hive<int, ConstructCountingAllocator<int>> values(
        hive_limits{3, 3}, ConstructCountingAllocator<int>{&counts});
    for (int i = 0; i < 30; ++i) {
      values.insert(i);
    }
    EXPECT_EQ(counts.constructs, 30);

    values.reserve(60);
    EXPECT_EQ(counts.constructs, 30) << "reserving builds no element";

    for (int i = 0; i < 10; ++i) {
      values.erase(values.begin());
    }
    EXPECT_EQ(counts.destroys, 10);
    EXPECT_EQ(counts.constructs, 30);

    values.clear();
    EXPECT_EQ(counts.destroys, 30);
  }
  EXPECT_EQ(counts.constructs, 30);
  EXPECT_EQ(counts.destroys, 30);
}

//
// P0447R28 conformance
//

/// A pointer that is not T*: distinct const flavour, its own rebind, no
/// conversion from a raw pointer, and a to_address hook. An allocator's
/// pointer must also be a Cpp17RandomAccessIterator, which is what lets the
/// container's own bookkeeping vectors be built on it.
template <typename T>
class FancyPtr {
 public:
  using element_type = T;
  using value_type = std::remove_cv_t<T>;
  using difference_type = std::ptrdiff_t;
  using pointer = T*;
  using reference = T&;
  using iterator_category = std::random_access_iterator_tag;

  FancyPtr() = default;
  /* implicit */ FancyPtr(std::nullptr_t) noexcept {}
  explicit FancyPtr(T* raw) noexcept : raw_{raw} {}

  template <typename U>
    requires std::convertible_to<U*, T*>
  /* implicit */ FancyPtr(const FancyPtr<U>& other) noexcept
      : raw_{other.raw()} {}

  // The allocator requirements give pointer_to to the allocator's pointer
  // only, so the const_pointer this allocator hands out has none: anything
  // the container needs from a const_pointer it has to get by conversion.
  static FancyPtr pointer_to(T& value) noexcept
    requires(!std::is_const_v<T>)
  {
    return FancyPtr{std::addressof(value)};
  }

  T* operator->() const noexcept { return raw_; }
  T& operator*() const noexcept { return *raw_; }
  T& operator[](std::ptrdiff_t i) const noexcept { return raw_[i]; }
  explicit operator bool() const noexcept { return raw_ != nullptr; }

  FancyPtr& operator++() noexcept {
    ++raw_;
    return *this;
  }
  FancyPtr operator++(int) noexcept {
    auto copy = *this;
    ++raw_;
    return copy;
  }
  FancyPtr& operator--() noexcept {
    --raw_;
    return *this;
  }
  FancyPtr operator--(int) noexcept {
    auto copy = *this;
    --raw_;
    return copy;
  }
  FancyPtr& operator+=(std::ptrdiff_t n) noexcept {
    raw_ += n;
    return *this;
  }
  FancyPtr& operator-=(std::ptrdiff_t n) noexcept {
    raw_ -= n;
    return *this;
  }
  FancyPtr operator+(std::ptrdiff_t n) const noexcept {
    return FancyPtr{raw_ + n};
  }
  friend FancyPtr operator+(std::ptrdiff_t n, FancyPtr p) noexcept {
    return FancyPtr{p.raw_ + n};
  }
  FancyPtr operator-(std::ptrdiff_t n) const noexcept {
    return FancyPtr{raw_ - n};
  }
  std::ptrdiff_t operator-(const FancyPtr& other) const noexcept {
    return raw_ - other.raw_;
  }

  bool operator==(const FancyPtr&) const = default;
  auto operator<=>(const FancyPtr&) const = default;

  T* raw() const noexcept { return raw_; }

 private:
  T* raw_ = nullptr;
};

struct FancyCounts {
  std::size_t allocations = 0;
  std::size_t deallocations = 0;
  std::size_t constructs = 0;
  std::size_t destroys = 0;
  std::map<const void*, std::size_t> live;
};

/// Hands out FancyPtr, checks that every deallocation is given back exactly
/// the pointer and the count it was allocated with, and counts construction
/// through its own construct(), which allocator_traits must route to.
template <typename T>
struct FancyAllocator {
  using value_type = T;
  using pointer = FancyPtr<T>;
  using const_pointer = FancyPtr<const T>;
  using void_pointer = FancyPtr<void>;
  using const_void_pointer = FancyPtr<const void>;
  using propagate_on_container_copy_assignment = std::false_type;
  using propagate_on_container_move_assignment = std::true_type;
  using propagate_on_container_swap = std::true_type;

  template <typename U>
  struct rebind {
    using other = FancyAllocator<U>;
  };

  explicit FancyAllocator(FancyCounts* c) : counts{c} {}
  template <typename U>
  /* implicit */ FancyAllocator(const FancyAllocator<U>& other) noexcept
      : counts{other.counts} {}

  pointer allocate(std::size_t n) {
    ++counts->allocations;
    T* raw = static_cast<T*>(::operator new(n * sizeof(T)));
    counts->live[raw] = n * sizeof(T);
    return pointer{raw};
  }
  void deallocate(pointer p, std::size_t n) noexcept {
    auto found = counts->live.find(p.raw());
    EXPECT_NE(found, counts->live.end()) << "deallocating an unknown pointer";
    if (found != counts->live.end()) {
      EXPECT_EQ(found->second, n * sizeof(T)) << "deallocating a wrong count";
      counts->live.erase(found);
    }
    ++counts->deallocations;
    ::operator delete(static_cast<void*>(p.raw()));
  }

  template <typename U, typename... Args>
  void construct(U* address, Args&&... args) {
    ++counts->constructs;
    ::new (static_cast<void*>(address)) U(std::forward<Args>(args)...);
  }
  template <typename U>
  void destroy(U* address) noexcept {
    ++counts->destroys;
    address->~U();
  }

  template <typename U>
  bool operator==(const FancyAllocator<U>& other) const noexcept {
    return counts == other.counts;
  }
  template <typename U>
  bool operator!=(const FancyAllocator<U>& other) const noexcept {
    return counts != other.counts;
  }

  FancyCounts* counts;
};

using FancyHive = hive<int, FancyAllocator<int>>;

// The public pointer types are exactly the allocator's, and so are the
// iterators'.
static_assert(std::same_as<FancyHive::pointer, FancyPtr<int>>);
static_assert(std::same_as<FancyHive::const_pointer, FancyPtr<const int>>);
static_assert(std::same_as<FancyHive::iterator::pointer, FancyPtr<int>>);
static_assert(
    std::same_as<FancyHive::const_iterator::pointer, FancyPtr<const int>>);
static_assert(std::bidirectional_iterator<FancyHive::iterator>);
static_assert(std::bidirectional_iterator<FancyHive::const_iterator>);

struct Boxed {
  int value;
};

TEST(Hive, FancyPointerAllocatorDrivesEveryAllocation) {
  FancyCounts counts;
  {
    FancyHive values(hive_limits{3, 5}, FancyAllocator<int>{&counts});
    std::vector<FancyHive::iterator> positions;
    positions.reserve(40);
    for (int i = 0; i < 40; ++i) {
      positions.push_back(values.insert(i));
    }
    EXPECT_EQ(values.size(), 40);
    EXPECT_EQ(counts.constructs, 40) << "one construct per element";
    EXPECT_GT(counts.allocations, 3u) << "blocks came from the allocator";

    // get_iterator takes the allocator's const_pointer, not a raw one.
    const FancyPtr<const int> address{std::addressof(*positions[11])};
    EXPECT_EQ(values.get_iterator(address), positions[11]);
    const int unrelated = 0;
    EXPECT_EQ(
        values.get_iterator(FancyPtr<const int>{std::addressof(unrelated)}),
        values.end());

    // Erase, refill, splice, reshape and shrink all move allocations around.
    for (int i = 0; i < 40; i += 2) {
      values.erase(positions[i]);
    }
    EXPECT_EQ(values.size(), 20);
    for (int i = 0; i < 20; ++i) {
      values.insert(100 + i);
    }
    FancyHive other(hive_limits{3, 5}, FancyAllocator<int>{&counts});
    for (int i = 0; i < 12; ++i) {
      other.insert(i);
    }
    values.splice(other);
    EXPECT_EQ(values.size(), 52);
    values.reshape(hive_limits{4, 8});
    values.shrink_to_fit();
    expectConsistentInBothDirections(values);

    FancyHive copy = values;
    EXPECT_EQ(copy.size(), values.size());
    FancyHive moved = std::move(copy);
    EXPECT_EQ(moved.size(), values.size());
    moved.clear();
    moved.trim_capacity();
  }
  EXPECT_EQ(counts.allocations, counts.deallocations);
  EXPECT_TRUE(counts.live.empty()) << "an allocation leaked";
  EXPECT_EQ(counts.constructs, counts.destroys);
}

TEST(Hive, FancyPointerIsARandomAccessIterator) {
  // An allocator's pointer type has to be a Cpp17RandomAccessIterator; the
  // container's own bookkeeping is built on that.
  int values[] = {1, 2, 3, 4};
  FancyPtr<int> first{values};
  auto walked = first;
  EXPECT_EQ(*walked++, 1);
  EXPECT_EQ(*walked, 2);
  EXPECT_EQ(*walked--, 2);
  EXPECT_EQ(*walked, 1);
  walked += 3;
  EXPECT_EQ(*walked, 4);
  walked -= 2;
  EXPECT_EQ(*walked, 2);
  EXPECT_EQ(walked - first, 1);
  EXPECT_EQ(*(first + 2), 3);
  EXPECT_EQ(*(2 + first), 3);
  EXPECT_EQ(*(walked - 1), 1);
  EXPECT_EQ(first[3], 4);
  EXPECT_TRUE(static_cast<bool>(first));
  EXPECT_FALSE(static_cast<bool>(FancyPtr<int>{}));
  EXPECT_LT(first, walked);
  EXPECT_EQ(std::to_address(first), values);
}

TEST(Hive, FancyPointerOperatorArrowHandsBackTheAllocatorsPointer) {
  FancyCounts counts;
  hive<Boxed, FancyAllocator<Boxed>> values(
      hive_limits{3, 3}, FancyAllocator<Boxed>{&counts});
  auto position = values.insert(Boxed{7});
  static_assert(std::same_as<decltype(position.operator->()), FancyPtr<Boxed>>);
  EXPECT_EQ(position->value, 7);
  auto constPosition = values.cbegin();
  static_assert( //
      std::same_as< //
          decltype(constPosition.operator->()),
          FancyPtr<const Boxed>>);
  EXPECT_EQ(constPosition->value, 7);
}

/// A type nothing but its allocator can build: no public constructor of its
/// own, and copy and move deleted. This is the shape scoped_allocator_adaptor
/// produces when it passes itself down, and the paper's requirements are
/// written to accept it - Cpp17MoveInsertable is about what the ALLOCATOR
/// can construct, not about std::move_constructible.
struct OnlyAllocatorBuildable {
  OnlyAllocatorBuildable(const OnlyAllocatorBuildable&) = delete;
  OnlyAllocatorBuildable(OnlyAllocatorBuildable&&) = delete;
  OnlyAllocatorBuildable& operator=(const OnlyAllocatorBuildable&) = delete;
  OnlyAllocatorBuildable& operator=(OnlyAllocatorBuildable&&) = delete;

  int value;

 private:
  template <typename>
  friend struct BuilderAllocator;
  explicit OnlyAllocatorBuildable(int v) : value{v} {}
};

template <typename T>
struct BuilderAllocator {
  using value_type = T;

  template <typename U>
  struct rebind {
    using other = BuilderAllocator<U>;
  };

  BuilderAllocator() = default;
  template <typename U>
  /* implicit */ BuilderAllocator(const BuilderAllocator<U>&) noexcept {}

  T* allocate(std::size_t n) { return std::allocator<T>{}.allocate(n); }
  void deallocate(T* p, std::size_t n) noexcept {
    std::allocator<T>{}.deallocate(p, n);
  }

  // Everything the container builds for itself goes through the ordinary
  // path...
  template <typename U, typename... Args>
    requires std::is_constructible_v<U, Args...>
  void construct(U* address, Args&&... args) {
    ::new (static_cast<void*>(address)) U(std::forward<Args>(args)...);
  }

  // ... and the element type, which has no ordinary path, goes through here.
  void construct(OnlyAllocatorBuildable* address, int value) {
    ::new (static_cast<void*>(address)) OnlyAllocatorBuildable(value);
  }
  void construct(
      OnlyAllocatorBuildable* address, const OnlyAllocatorBuildable& other) {
    ::new (static_cast<void*>(address)) OnlyAllocatorBuildable(other.value);
  }
  void construct(
      OnlyAllocatorBuildable* address, OnlyAllocatorBuildable&& other) {
    ::new (static_cast<void*>(address)) OnlyAllocatorBuildable(other.value);
  }

  template <typename U>
  bool operator==(const BuilderAllocator<U>&) const noexcept {
    return true;
  }
  template <typename U>
  bool operator!=(const BuilderAllocator<U>&) const noexcept {
    return false;
  }
};

static_assert(!std::move_constructible<OnlyAllocatorBuildable>);
static_assert(!std::copy_constructible<OnlyAllocatorBuildable>);

TEST(Hive, ElementRequirementsAreAllocatorAware) {
  hive<OnlyAllocatorBuildable, BuilderAllocator<OnlyAllocatorBuildable>> values(
      hive_limits{3, 3});
  for (int i = 0; i < 10; ++i) {
    values.emplace(i);
  }
  EXPECT_EQ(values.size(), 10);
  // reshape moves elements, which only the allocator knows how to do.
  values.reshape(hive_limits{5, 5});
  EXPECT_EQ(values.size(), 10);
  values.shrink_to_fit();
  EXPECT_EQ(values.size(), 10);
  std::size_t walked = 0;
  for (const auto& element : values) {
    EXPECT_GE(element.value, 0);
    ++walked;
  }
  EXPECT_EQ(walked, 10u);
}

TEST(Hive, LimitsRespectABoundedAllocator) {
  // block_capacity_hard_limits() is static, so it reports what the
  // representation allows and cannot see a stateful allocator's bound. The
  // per-instance bound shows up in max_size() instead: limits this allocator
  // can never satisfy leave a usable empty hive whose max_size() is zero and
  // whose first insertion throws.
  BoundedHive unreachable(hive_limits{100, 200}, BoundedAllocator<int>{10});
  EXPECT_EQ(unreachable.max_size(), 0u);
  EXPECT_TRUE(unreachable.empty());
  EXPECT_EQ(unreachable.begin(), unreachable.end());
  EXPECT_THROW(unreachable.insert(1), std::length_error);

  BoundedHive values(hive_limits{3, 4}, BoundedAllocator<int>{10});
  EXPECT_LE(values.max_size(), 10u);
  while (values.size() < values.max_size()) {
    values.insert(static_cast<int>(values.size()));
  }
  EXPECT_LE(values.capacity(), values.max_size());
  EXPECT_THROW(values.insert(0), std::length_error);
  expectConsistentInBothDirections(values);
}

TEST(Hive, SpliceChecksTheCombinedSizeBeforeItMovesAnything) {
  BoundedHive destination(hive_limits{3, 3}, BoundedAllocator<int>{6});
  for (int i = 0; i < 6; ++i) {
    destination.insert(i);
  }
  const auto before = elementsOf(destination);
  const auto capacityBefore = destination.capacity();

  BoundedHive source(hive_limits{3, 3}, BoundedAllocator<int>{6});
  for (int i = 0; i < 6; ++i) {
    source.insert(100 + i);
  }

  // The combined size passes what the allocator allows, so the splice is
  // refused - and refused before it has moved anything.
  EXPECT_THROW(destination.splice(source), std::length_error);
  EXPECT_EQ(elementsOf(destination), before);
  EXPECT_EQ(destination.capacity(), capacityBefore);
  EXPECT_EQ(source.size(), 6u);
  expectConsistentInBothDirections(destination);
  expectConsistentInBothDirections(source);
}

TEST(Hive, BuildsFromNonCommonRanges) {
  // A counted view: iterator and sentinel are different types.
  const std::vector<int> source{1, 2, 3, 4, 5};
  auto counted = std::views::counted(source.begin(), 4);
  hive<int> values(hive_limits{3, 3});
  values.insert_range(counted);
  EXPECT_EQ(values.size(), 4u);

  // take_while, whose sentinel is not the iterator type either.
  auto taken = std::views::iota(1, 100) |
      std::views::take_while([](int value) { return value < 7; });
  static_assert(!std::ranges::common_range<decltype(taken)>);
  values.insert_range(taken);
  EXPECT_EQ(values.size(), 10u);
  expectConsistentInBothDirections(values);

  values.assign_range(taken);
  EXPECT_EQ(elementsOf(values), (std::vector<int>{1, 2, 3, 4, 5, 6}));

  // A single-pass range with no size at all.
  std::istringstream input{"10 20 30"};
  auto streamed = std::ranges::istream_view<int>(input);
  static_assert(!std::ranges::common_range<decltype(streamed)>);
  hive<int> fromStream(hive_limits{3, 3});
  fromStream.insert_range(streamed);
  EXPECT_EQ(elementsOf(fromStream), (std::vector<int>{10, 20, 30}));
}

TEST(Hive, BuildsFromARangeOfMoveOnlyIterators) {
  // Zero-terminated, because that is what the sentinel compares against.
  const int data[] = {4, 5, 6, 0};
  hive<int> values(hive_limits{3, 3});
  values.insert_range(MoveOnlyRange{data});
  EXPECT_EQ(elementsOf(values), (std::vector<int>{4, 5, 6}));
}

TEST(Hive, CustomIteratorsSupportPostIncrement) {
  // Cpp17InputIterator requires *it++, which is the form the container is
  // entitled to use and which the iterator-pair constructors accept.
  const int data[] = {1, 2, 3};
  SinglePassIterator single{data};
  EXPECT_EQ(*single++, 1);
  EXPECT_EQ(*single, 2);

  LegacyProxyIterator proxy{data};
  EXPECT_EQ(static_cast<int>(*proxy++), 1);
  EXPECT_EQ(static_cast<int>(*proxy), 2);

  MoveOnlyIterator moveOnly{data};
  moveOnly++;
  EXPECT_EQ(*moveOnly, 2);

  // The operations the concepts require of these iterators, which is why
  // they are declared: default construction, and movability.
  LegacyProxyIterator defaultProxy;
  EXPECT_EQ(defaultProxy, LegacyProxyIterator{});
  MoveOnlyIterator defaultMoveOnly;
  defaultMoveOnly = std::move(moveOnly);
  EXPECT_EQ(*defaultMoveOnly, 2);
  MoveOnlyIterator adopted{std::move(defaultMoveOnly)};
  EXPECT_EQ(*adopted, 2);
}

TEST(Hive, BuildsFromLegacyProxyIterators) {
  const int source[] = {1, 2, 3, 4, 5};
  hive<int> values(
      LegacyProxyIterator{source},
      LegacyProxyIterator{source + 5},
      hive_limits{3, 3});
  EXPECT_EQ(elementsOf(values), (std::vector<int>{1, 2, 3, 4, 5}));
  values.insert(LegacyProxyIterator{source}, LegacyProxyIterator{source + 2});
  EXPECT_EQ(values.size(), 7u);
  values.assign(LegacyProxyIterator{source}, LegacyProxyIterator{source + 3});
  EXPECT_EQ(elementsOf(values), (std::vector<int>{1, 2, 3}));
}

TEST(Hive, IteratorArithmeticThroughTheStandardAlgorithms) {
  hive<int> values(hive_limits{3, 3});
  std::vector<hive<int>::iterator> positions;
  positions.reserve(12);
  for (int i = 0; i < 12; ++i) {
    positions.push_back(values.insert(i));
  }

  auto it = values.begin();
  std::advance(it, 7);
  EXPECT_EQ(*it, 7);
  std::advance(it, -3);
  EXPECT_EQ(*it, 4);
  EXPECT_EQ(std::distance(values.begin(), values.end()), 12);
  EXPECT_EQ(std::distance(values.begin(), std::next(values.begin(), 5)), 5);
  EXPECT_EQ(*std::prev(values.end()), 11);
  EXPECT_EQ(std::ranges::distance(values), 12);

  // Still right once the blocks are full of holes.
  for (int i : {1, 2, 5, 9}) {
    values.erase(positions[i]);
  }
  EXPECT_EQ(std::distance(values.begin(), values.end()), 8);
  auto walked = values.begin();
  std::ranges::advance(walked, 3);
  EXPECT_EQ(*walked, 6);
}

// Every operation that does no work on an empty hive has to be usable during
// constant evaluation, one by one across the synopsis. Allocation into the
// hive's own blocks cannot constant-evaluate - the slots are a union whose
// storage is reused by hand - so these are the zero-work paths.
constexpr bool constexprDefaultConstruct() {
  hive<int> values;
  return values.empty() && values.size() == 0 && values.capacity() == 0;
}
constexpr bool constexprLimits() {
  hive<int> values{hive_limits{3, 9}};
  const hive_limits limits = values.block_capacity_limits();
  return limits.min == 3 && limits.max == 9;
}
constexpr bool constexprCopyConstruct() {
  hive<int> source{hive_limits{3, 9}};
  hive<int> copy{source};
  hive<int> withAllocator{source, std::allocator<int>{}};
  copy.reserve(0);
  withAllocator.trim_capacity();
  // A copy takes on the source's block capacity limits.
  return copy.empty() && withAllocator.empty() &&
      copy.block_capacity_limits().min == 3 &&
      withAllocator.block_capacity_limits().max == 9;
}
constexpr bool constexprMoveConstruct() {
  hive<int> source;
  hive<int> moved{std::move(source)};
  hive<int> withAllocator{std::move(moved), std::allocator<int>{}};
  return withAllocator.empty();
}
constexpr bool constexprCopyAssign() {
  hive<int> source;
  hive<int> target;
  target = source;
  return target.empty();
}
constexpr bool constexprMoveAssign() {
  hive<int> source;
  hive<int> target;
  target = std::move(source);
  return target.empty();
}
constexpr bool constexprSwap() {
  hive<int> left{hive_limits{3, 5}};
  hive<int> right{hive_limits{4, 6}};
  left.swap(right);
  folly::swap(left, right);
  return left.block_capacity_limits().min == 3 &&
      right.block_capacity_limits().min == 4;
}
constexpr bool constexprIterators() {
  hive<int> values;
  const hive<int>& constValues = values;
  return values.begin() == values.end() &&
      constValues.begin() == constValues.end() &&
      values.cbegin() == values.cend() && values.rbegin() == values.rend() &&
      values.crbegin() == values.crend();
}
constexpr bool constexprCapacityAndTrim() {
  hive<int> values;
  values.reserve(0);
  values.trim_capacity();
  values.trim_capacity(0);
  values.shrink_to_fit();
  return values.capacity() == 0 && values.max_size() > 0;
}
constexpr bool constexprEmptyCountOperations() {
  hive<int> defaults(std::size_t{0});
  hive<int> copies(std::size_t{0}, 7);
  defaults.assign(std::size_t{0}, 9);
  return defaults.empty() && copies.empty();
}
constexpr bool constexprClearAndErase() {
  hive<int> values;
  values.clear();
  const bool erasedRange =
      values.erase(values.begin(), values.end()) == values.end();
  return erasedRange && values.empty();
}
constexpr bool constexprSpliceSortUnique() {
  hive<int> values;
  hive<int> source;
  values.splice(source);
  values.sort();
  const bool none = values.unique() == 0;
  return none && values.empty();
}
constexpr bool constexprFreeAlgorithms() {
  hive<int> values;
  const bool noneErased = folly::erase(values, 1) == 0 &&
      folly::erase_if(values, [](int) { return true; }) == 0;
  return noneErased;
}
constexpr bool constexprAllocatorAndReshape() {
  hive<int> values;
  values.reshape(hive_limits{5, 7});
  return values.get_allocator() == std::allocator<int>{} &&
      values.block_capacity_limits().max == 7;
}
static_assert(constexprDefaultConstruct());
static_assert(constexprLimits());
static_assert(constexprCopyConstruct());
static_assert(constexprMoveConstruct());
static_assert(constexprCopyAssign());
static_assert(constexprMoveAssign());
static_assert(constexprSwap());
static_assert(constexprIterators());
static_assert(constexprCapacityAndTrim());
static_assert(constexprEmptyCountOperations());
static_assert(constexprClearAndErase());
static_assert(constexprSpliceSortUnique());
static_assert(constexprFreeAlgorithms());
static_assert(constexprAllocatorAndReshape());

// Constant destruction of a namespace-scope hive, which is what makes it a
// literal type rather than merely a type with constexpr members.
constexpr hive<int> kConstantEmptyHive{};
static_assert(kConstantEmptyHive.empty());

static_assert(hive<int>::block_capacity_hard_limits().min == 3);
static_assert(hive<int>::block_capacity_hard_limits().max == 65535);
static_assert(hive<int>::block_capacity_default_limits().min == 8);

// The one-argument limits constructor is explicit, as the paper declares it.
static_assert(!std::is_convertible_v<hive_limits, hive<int>>);
static_assert(std::is_constructible_v<hive<int>, hive_limits>);
// The two-argument one is not, so copy-list-initialization can use it.
constexpr bool limitsAndAllocatorCopyListInitialize() {
  hive<int> values = {hive_limits{3, 4}, std::allocator<int>{}};
  return values.block_capacity_limits().max == 4;
}
static_assert(limitsAndAllocatorCopyListInitialize());
// The allocator constructor is explicit, and the count one too.
static_assert(!std::is_convertible_v<std::allocator<int>, hive<int>>);
static_assert(!std::is_convertible_v<std::size_t, hive<int>>);
static_assert(std::is_nothrow_move_constructible_v<hive<int>>);
static_assert(std::is_nothrow_constructible_v<hive<int>, std::allocator<int>&>);

TEST(Hive, FreeEraseBindsTheElementItself) {
  // erase() binds the element by auto&, as the paper writes it, so the
  // comparison sees a non-const lvalue. Binding by const T& instead would
  // not compile against a comparison operator that is not const - which is
  // legal for an element type, however unusual.
  struct Tagged {
    int value;
    bool operator==(int other) { return value == other; }
  };
  hive<Tagged> values(hive_limits{3, 3});
  for (int i = 0; i < 9; ++i) {
    values.insert(Tagged{i % 3});
  }
  EXPECT_EQ(folly::erase(values, 1), 3u);
  EXPECT_EQ(values.size(), 6u);
  EXPECT_EQ(
      folly::erase_if(values, [](const Tagged& t) { return t.value == 2; }),
      3u);
  EXPECT_EQ(values.size(), 3u);
}

// The from-range constructors and their deduction guides, which take
// folly::from_range_t and so exist in a C++20 build too.
TEST(Hive, FromRangeConstructors) {
  const std::vector<int> source{1, 2, 3, 4, 5};

  hive<int> values(from_range, source);
  EXPECT_EQ(elementsOf(values), source);

  hive<int> withLimits(from_range, source, hive_limits{3, 4});
  EXPECT_EQ(withLimits.block_capacity_limits().max, 4u);
  EXPECT_EQ(elementsOf(withLimits), source);

  hive<int> withAllocator(from_range, source, std::allocator<int>{});
  EXPECT_EQ(withAllocator.size(), 5u);

  // A non-common range, whose sentinel is not its iterator type.
  auto taken = std::views::iota(1, 100) |
      std::views::take_while([](int value) { return value < 4; });
  hive<int> fromView(from_range, taken);
  EXPECT_EQ(elementsOf(fromView), (std::vector<int>{1, 2, 3}));

  // And the deduction guides that go with them.
  hive deduced(from_range, source);
  static_assert(std::same_as<decltype(deduced), hive<int>>);
  EXPECT_EQ(deduced.size(), 5u);

  hive deducedWithLimits(from_range, source, hive_limits{3, 4});
  static_assert(std::same_as<decltype(deducedWithLimits), hive<int>>);
  EXPECT_EQ(deducedWithLimits.block_capacity_limits().max, 4u);

#if defined(__cpp_lib_ranges_to_container) && \
    __cpp_lib_ranges_to_container >= 202202L
  // Where the standard library has the tag, folly's is the same type.
  hive<int> standardTag(std::from_range, source);
  EXPECT_EQ(elementsOf(standardTag), source);
  hive deducedFromStandardTag(std::from_range, source);
  static_assert(std::same_as<decltype(deducedFromStandardTag), hive<int>>);
#endif
}

/// Records the arguments each call receives, so the order can be checked.
struct OrderRecordingPredicate {
  std::vector<std::pair<int, int>>* calls;
  bool operator()(int left, int right) const {
    calls->emplace_back(left, right);
    return false;
  }
};

// [list.ops] invokes unique()'s predicate as pred(*i, *(i - 1)): the
// candidate for erasure first, the element being kept second. A symmetric
// predicate cannot tell the difference, which is why every other test here
// misses it; an asymmetric one can.
TEST(Hive, UniqueCallsThePredicateWithTheCandidateFirst) {
  hive<int> values(hive_limits{3, 3});
  values.insert(1);
  values.insert(2);

  std::vector<std::pair<int, int>> calls;
  EXPECT_EQ(values.unique(OrderRecordingPredicate{&calls}), 0u);
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].first, 2) << "the candidate for erasure comes first";
  EXPECT_EQ(calls[0].second, 1) << "the element being kept comes second";

  // And the order is what an asymmetric predicate acts on: keep an element
  // only while the next one is greater, so a descending run collapses.
  hive<int> sequence(hive_limits{3, 3});
  for (int value : {1, 5, 4, 3, 9, 2}) {
    sequence.insert(value);
  }
  EXPECT_EQ(
      sequence.unique([](int candidate, int kept) { return candidate < kept; }),
      3u);
  EXPECT_EQ(elementsOf(sequence), (std::vector<int>{1, 5, 9}));
}

/// Counts every allocation, and separately every allocation that did not
/// come through it - so scratch taken from the global heap is visible.
struct ScratchCounts {
  std::size_t allocations = 0;
  std::size_t bytes = 0;
};

template <typename T>
struct ScratchCountingAllocator {
  using value_type = T;

  explicit ScratchCountingAllocator(ScratchCounts* c) : counts{c} {}
  template <typename U>
  /* implicit */ ScratchCountingAllocator(
      const ScratchCountingAllocator<U>& other) noexcept
      : counts{other.counts} {}

  T* allocate(std::size_t n) {
    ++counts->allocations;
    counts->bytes += n * sizeof(T);
    return std::allocator<T>{}.allocate(n);
  }
  void deallocate(T* address, std::size_t n) noexcept {
    std::allocator<T>{}.deallocate(address, n);
  }
  template <typename U>
  bool operator==(const ScratchCountingAllocator<U>& other) const noexcept {
    return counts == other.counts;
  }
  template <typename U>
  bool operator!=(const ScratchCountingAllocator<U>& other) const noexcept {
    return counts != other.counts;
  }

  ScratchCounts* counts;
};

// sort() permutes values between fixed addresses, and the bookkeeping it
// needs for that is this container's, so it comes from this container's
// allocator. A hive over an arena must not step outside the arena to sort.
TEST(Hive, SortTakesItsScratchFromTheContainersAllocator) {
  ScratchCounts counts;
  {
    hive<int, ScratchCountingAllocator<int>> values(
        hive_limits{4, 8}, ScratchCountingAllocator<int>{&counts});
    for (int i = 40; i > 0; --i) {
      values.insert(i);
    }
    const auto afterFill = counts;
    ASSERT_GT(afterFill.allocations, 0u);

    values.sort();

    EXPECT_GT(counts.allocations, afterFill.allocations)
        << "sort's scratch has to come from somewhere, and it must be here";
    EXPECT_GT(counts.bytes, afterFill.bytes);
    std::vector<int> expected(40);
    std::iota(expected.begin(), expected.end(), 1);
    EXPECT_EQ(elementsOf(values), expected);
  }
}

#if FOLLY_HAS_MEMORY_RESOURCE

/// Counts what escapes to the global heap while it is installed.
class CountingUpstream : public std::pmr::memory_resource {
 public:
  std::size_t allocations = 0;

 private:
  void* do_allocate(std::size_t bytes, std::size_t align) override {
    ++allocations;
    return std::pmr::new_delete_resource()->allocate(bytes, align);
  }
  void do_deallocate(void* p, std::size_t bytes, std::size_t align) override {
    std::pmr::new_delete_resource()->deallocate(p, bytes, align);
  }
  bool do_is_equal(
      const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }
};

// The same property through pmr, which is where it matters most: everything
// sort() needs comes out of the arena, and the global heap is not touched.
TEST(Hive, SortOverPmrStaysInsideTheArena) {
  CountingUpstream global;
  // Restored on every exit, before global goes away.
  std::pmr::memory_resource* const previous =
      std::pmr::set_default_resource(&global);
  SCOPE_EXIT {
    std::pmr::set_default_resource(previous);
  };

  std::array<std::byte, 64 * 1024> buffer{};
  std::pmr::monotonic_buffer_resource arena{
      buffer.data(), buffer.size(), std::pmr::null_memory_resource()};
  {
    folly::pmr::hive<int> values(hive_limits{4, 8}, &arena);
    for (int i = 40; i > 0; --i) {
      values.insert(i);
    }
    const auto before = global.allocations;
    values.sort();
    EXPECT_EQ(global.allocations, before)
        << "sort() escaped the arena to the global heap";
    std::vector<int> expected(40);
    std::iota(expected.begin(), expected.end(), 1);
    EXPECT_EQ(elementsOf(values), expected);
  }
}

#endif

/// Copy-insertable only through the allocator: the copy constructor is
/// deleted, so `T copy = value;` does not compile, but the allocator can
/// still make a copy.
struct AllocatorOnlyCopy {
  AllocatorOnlyCopy(const AllocatorOnlyCopy&) = delete;
  AllocatorOnlyCopy& operator=(const AllocatorOnlyCopy&) = delete;

  int value;

 private:
  template <typename>
  friend struct CopyBuilderAllocator;
  explicit AllocatorOnlyCopy(int v) : value{v} {}
};

template <typename T>
struct CopyBuilderAllocator {
  using value_type = T;

  template <typename U>
  struct rebind {
    using other = CopyBuilderAllocator<U>;
  };

  CopyBuilderAllocator() = default;
  template <typename U>
  /* implicit */ CopyBuilderAllocator(const CopyBuilderAllocator<U>&) noexcept {
  }

  T* allocate(std::size_t n) { return std::allocator<T>{}.allocate(n); }
  void deallocate(T* p, std::size_t n) noexcept {
    std::allocator<T>{}.deallocate(p, n);
  }

  template <typename U, typename... Args>
    requires std::is_constructible_v<U, Args...>
  void construct(U* address, Args&&... args) {
    ::new (static_cast<void*>(address)) U(std::forward<Args>(args)...);
  }
  void construct(AllocatorOnlyCopy* address, int value) {
    ::new (static_cast<void*>(address)) AllocatorOnlyCopy(value);
  }
  void construct(AllocatorOnlyCopy* address, const AllocatorOnlyCopy& other) {
    ::new (static_cast<void*>(address)) AllocatorOnlyCopy(other.value);
  }

  template <typename U>
  bool operator==(const CopyBuilderAllocator<U>&) const noexcept {
    return true;
  }
  template <typename U>
  bool operator!=(const CopyBuilderAllocator<U>&) const noexcept {
    return false;
  }
};

static_assert(!std::copy_constructible<AllocatorOnlyCopy>);

// Cpp17CopyInsertable says the ALLOCATOR can copy an element, which is not
// the same as the element having a copy constructor. assign(n, value) has to
// make a copy that outlives the clear(), and it has to make it the same way.
TEST(Hive, AssignCountValueUsesTheAllocatorToCopy) {
  hive<AllocatorOnlyCopy, CopyBuilderAllocator<AllocatorOnlyCopy>> values(
      hive_limits{3, 3});
  for (int i = 0; i < 5; ++i) {
    values.emplace(i);
  }

  values.assign(4, *values.begin());
  EXPECT_EQ(values.size(), 4u);
  for (const auto& element : values) {
    EXPECT_EQ(element.value, 0);
  }

  // The aliasing case, which is the reason the copy is made up front: value
  // names an element that clear() is about to destroy.
  hive<std::string> aliased(hive_limits{3, 3});
  for (int i = 0; i < 6; ++i) {
    aliased.insert("value-" + folly::to<std::string>(i));
  }
  aliased.assign(3, *std::next(aliased.begin(), 4));
  EXPECT_EQ(
      elementsOf(aliased),
      (std::vector<std::string>{"value-4", "value-4", "value-4"}));
}

// The class template has the shape P0447R28 declares - two type parameters
// and no constraint - so it can be forward declared, specialized, and passed
// where an unconstrained template-template parameter is expected.
template <template <typename, typename> class Container>
struct TakesAnUnconstrainedTemplate {
  using type = Container<int, std::allocator<int>>;
};
static_assert( //
    std::same_as<
        TakesAnUnconstrainedTemplate<hive>::type,
        hive<int, std::allocator<int>>>);

// CTAD picks the iterator-pair-with-limits guide even though a hive_limits
// sits where an allocator could: the guides are constrained, and the
// implicit candidate that would take hive_limits for an allocator is merely
// non-viable rather than a hard error, which is what lets the class
// template itself stay unconstrained.
static_assert(requires {
  hive{
      std::declval<const std::vector<int>&>().begin(),
      std::declval<const std::vector<int>&>().end(),
      hive_limits{3, 4}};
});
static_assert(!detail::hive_allocator_like<hive_limits>);
static_assert(detail::hive_allocator_like<std::allocator<int>>);

struct TaggedError : std::runtime_error {
  explicit TaggedError(int t) : std::runtime_error("tagged"), tag{t} {}
  int tag;
};

struct TaggedThrowingValue {
  static inline int live = 0;
  static inline int constructed = 0;
  static inline int throwAt = -1;

  explicit TaggedThrowingValue(int v) : value{v} {
    if (constructed++ == throwAt) {
      throw TaggedError{value};
    }
    ++live;
  }
  TaggedThrowingValue(const TaggedThrowingValue& other)
      : TaggedThrowingValue(other.value) {}
  TaggedThrowingValue& operator=(const TaggedThrowingValue&) = delete;
  ~TaggedThrowingValue() { --live; }

  static void armAtNextConstruction() { throwAt = constructed; }
  static void reset() {
    live = 0;
    constructed = 0;
    throwAt = -1;
  }

  int value;
};

// Every rollback is a dismissible scope guard rather than a catch and a bare
// rethrow, so what leaves each of these paths is the exception the element
// threw, on its original way out.
TEST(Hive, RollbackKeepsTheOriginalExceptionOnEveryPath) {
  const auto expectTag = [](auto&& fn, int tag) {
    try {
      fn();
      ADD_FAILURE() << "expected TaggedError " << tag;
    } catch (const TaggedError& error) {
      EXPECT_EQ(error.tag, tag);
    }
  };

  TaggedThrowingValue::reset();
  {
    // A fresh block, then a spare block, then an erased slot.
    hive<TaggedThrowingValue> values(hive_limits{3, 3});
    TaggedThrowingValue::armAtNextConstruction();
    expectTag([&] { values.emplace(11); }, 11);

    values.emplace(1);
    values.reserve(6);
    auto erased = values.emplace(2);
    values.erase(erased);
    TaggedThrowingValue::armAtNextConstruction();
    expectTag([&] { values.emplace(22); }, 22);

    EXPECT_EQ(values.size(), 1);
    EXPECT_EQ(TaggedThrowingValue::live, 1);

    // Bulk insertion: the copies made before the throw are erased again.
    const TaggedThrowingValue prototype{33};
    TaggedThrowingValue::throwAt = TaggedThrowingValue::constructed + 2;
    expectTag([&] { values.insert(5, prototype); }, 33);
    TaggedThrowingValue::throwAt = -1;
    EXPECT_EQ(values.size(), 1);
    EXPECT_EQ(TaggedThrowingValue::live, 2);
  }
  EXPECT_EQ(TaggedThrowingValue::live, 0);

  TaggedThrowingValue::reset();
  {
    // A constructor that throws leaves no hive behind to destroy.
    std::vector<TaggedThrowingValue> source;
    source.reserve(8);
    for (int i = 0; i < 8; ++i) {
      source.emplace_back(i);
    }
    TaggedThrowingValue::throwAt = TaggedThrowingValue::constructed + 4;
    expectTag(
        [&] {
          hive<TaggedThrowingValue> values(
              source.begin(), source.end(), hive_limits{3, 3});
        },
        4);
    TaggedThrowingValue::throwAt = -1;
    EXPECT_EQ(TaggedThrowingValue::live, 8);
  }
  EXPECT_EQ(TaggedThrowingValue::live, 0);
}

//
// Constructors keep their meaning for an element type that can be built from
// anything. Built with braces, a delegation or a temporary hive would pick the
// initializer_list constructor instead: a "default" hive holding one element,
// a copy that recursed, a reshape or shrink_to_fit that added elements.
//
TEST(Hive, AnElementTypeBuildableFromAnythingKeepsEveryConstructorsMeaning) {
  hive<std::any> defaulted;
  EXPECT_TRUE(defaulted.empty());
  hive<std::any> withAllocator(std::allocator<std::any>{});
  EXPECT_TRUE(withAllocator.empty());
  hive<std::any> limited(hive_limits{3, 3});
  EXPECT_TRUE(limited.empty());
  EXPECT_EQ(limited.block_capacity_limits().max, 3u);
  hive<std::any> listed{std::any(1), std::any(2)};
  EXPECT_EQ(listed.size(), 2u);

  hive<std::any> values(hive_limits{3, 3});
  for (int i = 0; i < 7; ++i) {
    values.insert(std::any(i));
  }
  hive<std::any> copy(values);
  EXPECT_EQ(copy.size(), 7u);
  hive<std::any> copyWithAllocator(values, std::allocator<std::any>{});
  EXPECT_EQ(copyWithAllocator.size(), 7u);

  values.erase(values.begin());
  values.reshape(hive_limits{4, 4});
  EXPECT_EQ(values.size(), 6u);
  values.reserve(40);
  values.shrink_to_fit();
  EXPECT_EQ(values.size(), 6u);
  int sum = 0;
  for (const std::any& element : values) {
    sum += std::any_cast<int>(element);
  }
  EXPECT_EQ(sum, 1 + 2 + 3 + 4 + 5 + 6);
}

//
// An allocator whose skipfield rebind counts in 16 bits. A block of 65,535
// slots needs 65,536 skipfield entries, which that count cannot express: the
// hard limit has to keep blocks one short of it, rather than let the count
// wrap to zero.
//
struct NarrowSkipfieldCounts {
  std::size_t smallest = std::numeric_limits<std::size_t>::max();
  std::size_t largestSkipfield = 0;
};

template <typename T>
struct NarrowSkipfieldAllocator {
  using value_type = T;
  using size_type = std::conditional_t<
      std::is_same_v<T, std::uint16_t>,
      std::uint16_t,
      std::size_t>;

  constexpr NarrowSkipfieldAllocator() = default;
  constexpr explicit NarrowSkipfieldAllocator(NarrowSkipfieldCounts* c) noexcept
      : counts{c} {}
  template <typename U>
  constexpr /* implicit */ NarrowSkipfieldAllocator(
      const NarrowSkipfieldAllocator<U>& other) noexcept
      : counts{other.counts} {}

  T* allocate(size_type count) {
    counts->smallest = std::min<std::size_t>(counts->smallest, count);
    if constexpr (std::is_same_v<T, std::uint16_t>) {
      counts->largestSkipfield =
          std::max<std::size_t>(counts->largestSkipfield, count);
    }
    return std::allocator<T>{}.allocate(count);
  }
  void deallocate(T* address, size_type count) noexcept {
    std::allocator<T>{}.deallocate(address, count);
  }
  constexpr size_type max_size() const noexcept {
    return std::numeric_limits<size_type>::max();
  }
  template <typename U>
  bool operator==(const NarrowSkipfieldAllocator<U>& other) const noexcept {
    return counts == other.counts;
  }

  NarrowSkipfieldCounts* counts = nullptr;
};

TEST(Hive, ANarrowSkipfieldAllocatorNeverReceivesATruncatedCount) {
  using NarrowHive = hive<int, NarrowSkipfieldAllocator<int>>;
  constexpr std::size_t kLargest = 65534;
  static_assert(NarrowHive::block_capacity_hard_limits().max == kLargest);

  NarrowSkipfieldCounts counts;
  NarrowHive values(
      hive_limits{kLargest, kLargest}, NarrowSkipfieldAllocator<int>{&counts});
  for (int i = 0; i < 70000; ++i) {
    values.insert(i);
  }
  EXPECT_EQ(values.size(), 70000u);
  expectWalksAgreeWithSize(values);
  EXPECT_GT(counts.smallest, 0u);
  EXPECT_EQ(counts.largestSkipfield, kLargest + 1);
}

//
// sort() asks of T what the paper does - Cpp17MoveInsertable,
// Cpp17MoveAssignable and Cpp17Swappable - and nothing more: not an implicit
// move constructor, and not an accessible one when the allocator can build
// the element without it.
//
struct ExplicitlyMovable {
  explicit ExplicitlyMovable(int v) : value{v} {}
  explicit ExplicitlyMovable(ExplicitlyMovable&& other) noexcept
      : value{other.value} {}
  ExplicitlyMovable& operator=(ExplicitlyMovable&&) noexcept = default;
  bool operator<(const ExplicitlyMovable& other) const {
    return value < other.value;
  }
  int value;
};

class MovedOnlyByItsAllocator {
 public:
  MovedOnlyByItsAllocator& operator=(MovedOnlyByItsAllocator&&) = default;
  bool operator<(const MovedOnlyByItsAllocator& other) const {
    return value < other.value;
  }
  int value;

 private:
  template <typename>
  friend struct MovingAllocator;
  explicit MovedOnlyByItsAllocator(int v) : value{v} {}
  MovedOnlyByItsAllocator(MovedOnlyByItsAllocator&&) = default;
};

template <typename T>
struct MovingAllocator {
  using value_type = T;
  MovingAllocator() = default;
  template <typename U>
  /* implicit */ MovingAllocator(const MovingAllocator<U>&) noexcept {}
  T* allocate(std::size_t n) { return std::allocator<T>{}.allocate(n); }
  void deallocate(T* p, std::size_t n) noexcept {
    std::allocator<T>{}.deallocate(p, n);
  }
  template <typename U, typename... Args>
  void construct(U* address, Args&&... args) {
    ::new (static_cast<void*>(address)) U(std::forward<Args>(args)...);
  }
  template <typename U>
  bool operator==(const MovingAllocator<U>&) const noexcept {
    return true;
  }
};

static_assert(
    !std::is_constructible_v< //
        MovedOnlyByItsAllocator,
        MovedOnlyByItsAllocator&&>);

TEST(Hive, SortNeedsOnlyWhatThePaperAsksOfTheElementType) {
  hive<ExplicitlyMovable> explicitly(hive_limits{3, 3});
  for (int v : {5, 3, 9, 1, 7, 2}) {
    explicitly.emplace(v);
  }
  explicitly.sort();
  std::vector<int> sorted;
  for (const auto& element : explicitly) {
    sorted.push_back(element.value);
  }
  EXPECT_EQ(sorted, (std::vector<int>{1, 2, 3, 5, 7, 9}));

  hive<MovedOnlyByItsAllocator, MovingAllocator<MovedOnlyByItsAllocator>>
      byAllocator(hive_limits{3, 3});
  for (int v : {4, 8, 6, 2, 0}) {
    byAllocator.emplace(v);
  }
  byAllocator.sort();
  sorted.clear();
  for (const auto& element : byAllocator) {
    sorted.push_back(element.value);
  }
  EXPECT_EQ(sorted, (std::vector<int>{0, 2, 4, 6, 8}));
}

//
// Moving elements between blocks moves them through the allocator, as the
// paper asks, instead of choosing between move and copy by the core-language
// traits. Those traits would pick the copy constructor of a type whose move
// can throw - and std::deque's can - even when, as for a deque of unique_ptr,
// that copy constructor cannot be instantiated.
//
TEST(Hive, CompactionMovesAnElementWhoseMoveMayThrow) {
  using Queue = std::deque<std::unique_ptr<int>>;
  hive<Queue> values(hive_limits{3, 3});
  for (int i = 0; i < 7; ++i) {
    Queue queue;
    queue.push_back(std::make_unique<int>(i));
    values.insert(std::move(queue));
  }
  values.erase(values.begin());
  values.reserve(30);
  values.shrink_to_fit();
  values.reshape(hive_limits{4, 4});
  int sum = 0;
  for (const Queue& queue : values) {
    sum += *queue.front();
  }
  EXPECT_EQ(sum, 1 + 2 + 3 + 4 + 5 + 6);
}

struct MoveCopyCounts {
  int moves = 0;
  int copies = 0;
};

struct CountsItsMovesAndCopies {
  explicit CountsItsMovesAndCopies(MoveCopyCounts* c) : counts{c} {}
  CountsItsMovesAndCopies(const CountsItsMovesAndCopies& other)
      : counts{other.counts} {
    ++counts->copies;
  }
  // May throw, which is what used to divert relocation to the copy.
  CountsItsMovesAndCopies(CountsItsMovesAndCopies&& other) noexcept(false)
      : counts{other.counts} {
    ++counts->moves;
  }
  // Only construction is counted; the container never assigns these.
  CountsItsMovesAndCopies& operator=(const CountsItsMovesAndCopies&) = delete;
  CountsItsMovesAndCopies& operator=(CountsItsMovesAndCopies&&) = delete;
  MoveCopyCounts* counts;
};

TEST(Hive, MovingBetweenUnequalAllocatorsMovesEveryElement) {
  std::pmr::monotonic_buffer_resource left;
  std::pmr::monotonic_buffer_resource right;
  MoveCopyCounts counts;

  folly::pmr::hive<CountsItsMovesAndCopies> source(&left);
  for (int i = 0; i < 3; ++i) {
    source.emplace(&counts);
  }
  counts = MoveCopyCounts{};
  folly::pmr::hive<CountsItsMovesAndCopies> constructed(
      std::move(source), &right);
  EXPECT_EQ(constructed.size(), 3u);
  EXPECT_EQ(counts.moves, 3);
  EXPECT_EQ(counts.copies, 0);

  folly::pmr::hive<CountsItsMovesAndCopies> assigned(&left);
  counts = MoveCopyCounts{};
  assigned = std::move(constructed);
  EXPECT_EQ(assigned.size(), 3u);
  EXPECT_EQ(counts.moves, 3);
  EXPECT_EQ(counts.copies, 0);
}

//
// When the allocator type guarantees that storage can change hands, moving a
// hive asks nothing of the element type: the elementwise path must not even
// be instantiated. scoped_allocator_adaptor is the sharp case, because its
// construct() accepts any arguments and so cannot be asked in advance.
//
TEST(Hive, StorageOnlyMovesDoNotInstantiateElementMoves) {
  using Scoped = std::scoped_allocator_adaptor<std::allocator<Immobile>>;
  static_assert(std::allocator_traits<Scoped>::is_always_equal::value);
  hive<Immobile, Scoped> source(hive_limits{3, 3});
  for (int i = 0; i < 5; ++i) {
    source.emplace(i);
  }
  const Immobile* const first = std::addressof(*source.begin());

  hive<Immobile, Scoped> constructed(std::move(source), Scoped{});
  EXPECT_EQ(constructed.size(), 5u);
  EXPECT_EQ(std::addressof(*constructed.begin()), first);

  hive<Immobile, Scoped> assigned;
  assigned = std::move(constructed);
  EXPECT_EQ(assigned.size(), 5u);
  EXPECT_EQ(std::addressof(*assigned.begin()), first);
  EXPECT_EQ(assigned.block_capacity_limits().max, 3u);
}

//
// An allocator may need a complete value_type - one that picks a size class
// from sizeof(T) when it is instantiated, say. The hive's block header must
// not instantiate the allocator for itself while it is still incomplete.
//
template <typename T>
struct SizeClassAllocator {
  using value_type = T;
  static constexpr std::size_t kSizeClass = sizeof(T) <= 8 ? 8 : sizeof(T);

  SizeClassAllocator() = default;
  template <typename U>
  /* implicit */ SizeClassAllocator(const SizeClassAllocator<U>&) noexcept {}
  T* allocate(std::size_t n) { return std::allocator<T>{}.allocate(n); }
  void deallocate(T* p, std::size_t n) noexcept {
    std::allocator<T>{}.deallocate(p, n);
  }
  template <typename U>
  bool operator==(const SizeClassAllocator<U>&) const noexcept {
    return true;
  }
};

TEST(Hive, AnAllocatorMayRequireACompleteValueType) {
  hive<int, SizeClassAllocator<int>> values(hive_limits{3, 3});
  std::vector<hive<int, SizeClassAllocator<int>>::iterator> positions;
  positions.reserve(10);
  for (int i = 0; i < 10; ++i) {
    positions.push_back(values.insert(i));
  }
  values.erase(positions[4]);
  values.reserve(20);
  values.trim_capacity();
  EXPECT_EQ(values.size(), 9u);
  expectConsistentInBothDirections(values);
}

//
// When the allocators differ, a moved-into hive allocates its own blocks and
// so uses the default limits, as any constructor not given limits does. Only
// adopting the source's storage brings the source's limits with it.
//
TEST(Hive, MoveConstructionAcrossUnequalAllocatorsUsesTheDefaultLimits) {
  std::pmr::monotonic_buffer_resource left;
  std::pmr::monotonic_buffer_resource right;
  using PmrHive = folly::pmr::hive<int>;

  PmrHive source(hive_limits{3, 3}, &left);
  source.insert({1, 2, 3, 4});
  PmrHive moved(std::move(source), &right);
  EXPECT_EQ(elementsOf(moved).size(), 4u);
  EXPECT_EQ(
      moved.block_capacity_limits().min,
      PmrHive::block_capacity_default_limits().min);
  EXPECT_EQ(
      moved.block_capacity_limits().max,
      PmrHive::block_capacity_default_limits().max);

  PmrHive sameResource(hive_limits{3, 3}, &left);
  sameResource.insert({1, 2});
  PmrHive adopted(std::move(sameResource), &left);
  EXPECT_EQ(adopted.block_capacity_limits().max, 3u);
}

//
// [hive.overview]/5.3: the maximum hard limit is no larger than the
// allocator's max_size(). That bound is visible to the static
// block_capacity_hard_limits() when it is a property of the allocator type.
//
template <typename T>
struct TwelveElementAllocator {
  using value_type = T;
  constexpr TwelveElementAllocator() = default;
  template <typename U>
  constexpr /* implicit */ TwelveElementAllocator(
      const TwelveElementAllocator<U>&) noexcept {}
  T* allocate(std::size_t n) { return std::allocator<T>{}.allocate(n); }
  void deallocate(T* p, std::size_t n) noexcept {
    std::allocator<T>{}.deallocate(p, n);
  }
  constexpr std::size_t max_size() const noexcept { return 12; }
  template <typename U>
  bool operator==(const TwelveElementAllocator<U>&) const noexcept {
    return true;
  }
};

TEST(Hive, HardLimitsHonourAFixedAllocatorBound) {
  using Twelve = hive<int, TwelveElementAllocator<int>>;
  // Every rebind of this allocator reports the same bound, and a block's
  // skipfield takes one entry more than the block has slots.
  static_assert(Twelve::block_capacity_hard_limits().max == 11);
  static_assert(Twelve::block_capacity_default_limits().max == 11);
  static_assert(Twelve::block_capacity_default_limits().min == 8);

  Twelve values;
  EXPECT_EQ(values.max_size(), 12u);
  for (int i = 0; i < 12; ++i) {
    values.insert(i);
  }
  EXPECT_THROW(values.insert(12), std::length_error);
  EXPECT_THROW(Twelve(hive_limits{8, 12}), std::length_error);
}

//
// trim_capacity(n) never takes capacity() below n, and costs the blocks it
// releases rather than every spare block it holds.
//
TEST(Hive, TrimCapacityToACountReleasesWholeSpareBlocks) {
  hive<int> values(hive_limits{4, 4});
  values.insert({1, 2, 3});
  values.reserve(400);
  ASSERT_EQ(values.capacity(), 400u);

  values.trim_capacity(values.capacity());
  EXPECT_EQ(values.capacity(), 400u);
  values.trim_capacity(401);
  EXPECT_EQ(values.capacity(), 400u);

  values.trim_capacity(201);
  EXPECT_EQ(values.capacity(), 204u);
  values.trim_capacity(4);
  EXPECT_EQ(values.capacity(), 4u);
  EXPECT_EQ(elementsOf(values), (std::vector<int>{1, 2, 3}));
}

//
// A failed bulk insertion releases exactly the blocks it allocated: the hive
// is left with the same capacity made of the same blocks, not merely with
// spare blocks that happen to add up to it.
//
class ThrowingCountdown {
 public:
  using value_type = int;
  using difference_type = std::ptrdiff_t;

  ThrowingCountdown() = default;
  explicit ThrowingCountdown(int throwAt) : throwAt_{throwAt} {}

  int operator*() const {
    if (index_ == throwAt_) {
      throw std::runtime_error("countdown");
    }
    return index_;
  }
  ThrowingCountdown& operator++() {
    ++index_;
    return *this;
  }
  void operator++(int) { ++index_; }
  friend bool operator==(const ThrowingCountdown&, std::default_sentinel_t) {
    return false;
  }

 private:
  int index_ = 0;
  int throwAt_ = 0;
};
static_assert(std::input_iterator<ThrowingCountdown>);

TEST(Hive, FailedBulkInsertionReleasesExactlyTheBlocksItAllocated) {
  FancyCounts counts;
  {
    hive<int, FancyAllocator<int>> values(
        hive_limits{4, 4}, FancyAllocator<int>{&counts});
    values.reserve(4);
    values.reshape(hive_limits{3, 7});
    values.reserve(7);
    ASSERT_EQ(values.capacity(), 7u);
    const auto liveBefore = counts.live;

    EXPECT_THROW(
        values.insert_range(
            std::ranges::subrange(
                ThrowingCountdown{12}, std::default_sentinel)),
        std::runtime_error);
    EXPECT_TRUE(values.empty());
    EXPECT_EQ(values.capacity(), 7u);
    EXPECT_EQ(counts.live, liveBefore);
  }
  EXPECT_TRUE(counts.live.empty());

  // The operations std::input_iterator requires of the countdown, which the
  // container itself never needs.
  ThrowingCountdown idle;
  idle++;
  EXPECT_EQ(*idle, 1);
}

//
// Bulk insertion remembers positions only when an element-by-element undo is
// the only way back. Building a hive, copying one, compacting one, or
// inserting elements whose construction cannot throw into one that already
// holds elements needs no such list - and on a large hive that list would be
// three pointers per element.
//
using AllocationsByElementSize = std::map<std::size_t, std::size_t>;

template <typename T>
struct SizeTallyingAllocator {
  using value_type = T;
  explicit SizeTallyingAllocator(AllocationsByElementSize* t) noexcept
      : tally{t} {}
  template <typename U>
  /* implicit */ SizeTallyingAllocator(
      const SizeTallyingAllocator<U>& other) noexcept
      : tally{other.tally} {}
  T* allocate(std::size_t n) {
    ++(*tally)[sizeof(T)];
    return std::allocator<T>{}.allocate(n);
  }
  void deallocate(T* p, std::size_t n) noexcept {
    std::allocator<T>{}.deallocate(p, n);
  }
  template <typename U>
  bool operator==(const SizeTallyingAllocator<U>& other) const noexcept {
    return tally == other.tally;
  }

  AllocationsByElementSize* tally;
};

// Whether an element's construction can throw is the allocator's question
// when it has a construct() of its own, and the constructor's otherwise -
// never noexcept(allocator_traits::construct(...)), whose specification
// differs between standard libraries (libc++ gives it none).
static_assert( //
    detail::hive_nothrow_alloc_construct< //
        std::allocator<int>,
        int,
        const int&>());
static_assert(
    detail::hive_nothrow_alloc_construct<SizeTallyingAllocator<int>, int>());
static_assert( //
    !detail::hive_nothrow_alloc_construct<
        std::allocator<ThrowingValue>,
        ThrowingValue,
        int>());
static_assert(
    !detail::hive_nothrow_alloc_construct<InjectingAllocator<int>, int, int>());

TEST(Hive, BulkInsertionRemembersPositionsOnlyWhenItMustUndoThem) {
  using Tallied = hive<int, SizeTallyingAllocator<int>>;
  constexpr std::size_t kIteratorSize = sizeof(Tallied::iterator);
  AllocationsByElementSize tally;

  Tallied built(1000, 7, SizeTallyingAllocator<int>{&tally});
  Tallied copy(built);
  built.insert(1000, 8);
  built.reserve(10000);
  built.shrink_to_fit();
  built.reshape(hive_limits{16, 16});
  EXPECT_EQ(built.size(), 2000u);
  EXPECT_EQ(copy.size(), 1000u);
  EXPECT_EQ(tally[kIteratorSize], 0u);
}

//
// A run of short batches grows the hive geometrically, as insertion one
// element at a time does, instead of allocating a block for each batch.
//
TEST(Hive, ShortBatchesStillGrowGeometrically) {
  using Tallied = hive<int, SizeTallyingAllocator<int>>;
  AllocationsByElementSize tally;
  Tallied values(SizeTallyingAllocator<int>{&tally});
  const std::vector<int> batch(64, 1);
  for (int i = 0; i < 1024; ++i) {
    values.insert(batch.begin(), batch.end());
  }
  EXPECT_EQ(values.size(), 65536u);
  // One skipfield allocation per block; doubling reaches 65536 in about a
  // dozen blocks, where a block per batch would take 1024.
  EXPECT_LT(tally[sizeof(std::uint16_t)], 40u);
}

//
// end() moves on an insertion into the last block's never-used slots, not
// only when a new block is linked.
//
TEST(Hive, InsertingIntoTheLastBlocksUnusedSlotsMovesEnd) {
  hive<int> values(hive_limits{8, 8});
  values.insert(1);
  const auto stale = values.end();
  values.insert(2);
  EXPECT_NE(stale, values.end());
  EXPECT_EQ(std::next(stale), values.end());
}

//
// reshape() of a move-only element whose move throws keeps every element
// and the hive's limits; the elements already moved are left moved-from,
// which is all the paper and this container's documentation promise.
//
struct ThrowingMoveOnly {
  static inline int moves = 0;
  static inline int throwAt = -1;

  explicit ThrowingMoveOnly(int v) : value{v} {}
  ThrowingMoveOnly(ThrowingMoveOnly&& other) noexcept(false)
      : value{other.value} {
    if (moves++ == throwAt) {
      throw std::runtime_error("move");
    }
    other.value = -1;
  }
  // reshape() moves by construction alone.
  ThrowingMoveOnly& operator=(ThrowingMoveOnly&&) = delete;
  int value;
};

TEST(Hive, ReshapeOfAThrowingMoveOnlyTypeKeepsEveryElement) {
  hive<ThrowingMoveOnly> values(hive_limits{3, 3});
  for (int i = 0; i < 6; ++i) {
    values.emplace(i);
  }
  ThrowingMoveOnly::moves = 0;
  ThrowingMoveOnly::throwAt = 2;
  EXPECT_THROW(values.reshape(hive_limits{8, 8}), std::runtime_error);
  ThrowingMoveOnly::throwAt = -1;

  EXPECT_EQ(values.size(), 6u);
  EXPECT_EQ(values.block_capacity_limits().max, 3u);
  std::vector<int> seen;
  for (const auto& element : values) {
    seen.push_back(element.value);
  }
  EXPECT_EQ(seen, (std::vector<int>{-1, -1, 2, 3, 4, 5}));
}

} // namespace
} // namespace folly
