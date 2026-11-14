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

#include <algorithm>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include <glog/logging.h>

#include <folly/Benchmark.h>
#include <folly/Likely.h>
#include <folly/Optional.h>
#include <folly/compression/Instructions.h>
#include <folly/portability/GTest.h>

namespace folly {
namespace compression {

template <class URNG>
std::vector<uint64_t> generateRandomList(
    size_t n, uint64_t maxId, URNG&& g, bool withDuplicates = false) {
  CHECK_LT(n, 2 * maxId);
  std::uniform_int_distribution<uint64_t> uid(1, maxId);
  std::unordered_set<uint64_t> dataset;
  while (dataset.size() < n) {
    uint64_t value = uid(g);
    if (dataset.count(value) == 0) {
      dataset.insert(value);
    }
  }

  std::vector<uint64_t> ids(dataset.begin(), dataset.end());
  if (withDuplicates && n > 0) {
    // Ensure 20% of the list has at least 1 duplicate, 10% has at least 2, and
    // 5% is a run of the same value.
    std::copy(ids.begin() + ids.size() * 8 / 10, ids.end(), ids.begin());
    std::copy(ids.begin() + ids.size() * 9 / 10, ids.end(), ids.begin());
    std::fill(ids.begin(), ids.begin() + ids.size() / 20, ids[0]);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

inline std::vector<uint64_t> generateRandomList(
    size_t n, uint64_t maxId, bool withDuplicates = false) {
  std::mt19937 gen;
  return generateRandomList(n, maxId, gen, withDuplicates);
}

inline std::vector<uint64_t> generateSeqList(
    uint64_t minId, uint64_t maxId, uint64_t step = 1) {
  CHECK_LE(minId, maxId);
  CHECK_GT(step, 0);
  std::vector<uint64_t> ids;
  ids.reserve((maxId - minId) / step + 1);
  for (uint64_t i = minId; i <= maxId; i += step) {
    ids.push_back(i);
  }
  return ids;
}

inline std::vector<uint64_t> loadList(const std::string& filename) {
  std::ifstream fin(filename);
  std::vector<uint64_t> result;
  uint64_t id;
  while (fin >> id) {
    result.push_back(id);
  }
  return result;
}

// Test previousValue only if Reader has it.
template <class... Args>
void maybeTestPreviousValue(Args&&...) {}

// Make all the arguments template because if the types are not exact,
// the above overload will be picked (for example i could be size_t or
// ssize_t).
template <class Vector, class Reader, class Index>
auto maybeTestPreviousValue(const Vector& data, Reader& reader, Index i)
    -> decltype(reader.previousValue(), void()) {
  if (i != 0) {
    EXPECT_EQ(reader.previousValue(), data[i - 1]);
  }
}

// Test previous only if Reader has it.
template <class... Args>
void maybeTestPrevious(Args&&...) {}

// Make all the arguments template because if the types are not exact,
// the above overload will be picked (for example i could be size_t or
// ssize_t).
template <class Vector, class Reader, class Index>
auto maybeTestPrevious(const Vector& data, Reader& reader, Index i)
    -> decltype(reader.previous(), void()) {
  auto r = reader.previous();
  if (i != 0) {
    EXPECT_TRUE(r);
    EXPECT_EQ(reader.value(), data[i - 1]);
  } else {
    EXPECT_FALSE(r);
  }
  reader.next();
  EXPECT_EQ(reader.value(), data[i]);
}

template <class Reader, class List>
void testNext(const std::vector<uint64_t>& data, const List& list) {
  Reader reader(list);
  EXPECT_FALSE(reader.valid());

  for (size_t i = 0; i < data.size(); ++i) {
    EXPECT_TRUE(reader.next()) << i << " " << data.size();
    EXPECT_TRUE(reader.valid()) << i << " " << data.size();
    EXPECT_EQ(reader.value(), data[i]) << i << " " << data.size();
    EXPECT_EQ(reader.position(), i) << i << " " << data.size();
    maybeTestPreviousValue(data, reader, i);
    maybeTestPrevious(data, reader, i);
  }
  EXPECT_FALSE(reader.next()) << data.size();
  EXPECT_FALSE(reader.valid()) << data.size();
  EXPECT_EQ(reader.position(), reader.size());
}

template <class Reader, class List>
void testSkip(
    const std::vector<uint64_t>& data, const List& list, size_t skipStep) {
  CHECK_GT(skipStep, 0);
  Reader reader(list);

  for (size_t i = skipStep - 1; i < data.size(); i += skipStep) {
    // Destination must be representable.
    if (i + skipStep > std::numeric_limits<typename Reader::SizeType>::max()) {
      return;
    }

    // Also test that skip(0) stays in place.
    for (auto step : {skipStep, size_t(0)}) {
      EXPECT_TRUE(reader.skip(step));
      EXPECT_TRUE(reader.valid());
      EXPECT_EQ(reader.value(), data[i]);
      EXPECT_EQ(reader.position(), i);
    }

    maybeTestPreviousValue(data, reader, i);
    maybeTestPrevious(data, reader, i);
  }
  EXPECT_FALSE(reader.skip(skipStep));
  EXPECT_FALSE(reader.valid());
  EXPECT_EQ(reader.position(), reader.size());
  EXPECT_FALSE(reader.next());
}

template <class Reader, class List>
void testSkip(const std::vector<uint64_t>& data, const List& list) {
  for (size_t skipStep = 1; skipStep < 25; ++skipStep) {
    testSkip<Reader, List>(data, list, skipStep);
  }
  for (size_t skipStep = 25; skipStep <= 500; skipStep += 25) {
    testSkip<Reader, List>(data, list, skipStep);
  }
}

template <class Reader, class List>
void testSkipTo(
    const std::vector<uint64_t>& data, const List& list, size_t skipToStep) {
  using ValueType = typename Reader::ValueType;

  CHECK_GT(skipToStep, 0);
  Reader reader(list);

  const uint64_t delta = std::max<uint64_t>(1, data.back() / skipToStep);
  ValueType target = delta;
  auto it = data.begin();
  while (true) {
    it = std::lower_bound(it, data.end(), target);
    if (it == data.end()) {
      EXPECT_FALSE(reader.skipTo(target));
      break;
    }

    EXPECT_TRUE(reader.skipTo(target));
    // Test the whole group of equal values.
    for (auto it2 = it; it2 != data.end() && *it2 == *it;
         ++it2, reader.next()) {
      ASSERT_TRUE(reader.valid());
      EXPECT_EQ(reader.value(), *it2);
      EXPECT_EQ(reader.position(), std::distance(data.begin(), it2));

      // The reader should stay in place even if we're in the middle of a group.
      EXPECT_TRUE(reader.skipTo(*it2));
      EXPECT_EQ(reader.value(), *it2);
      EXPECT_EQ(reader.position(), std::distance(data.begin(), it2));

      maybeTestPreviousValue(data, reader, std::distance(data.begin(), it2));
      maybeTestPrevious(data, reader, std::distance(data.begin(), it2));
    }

    // Reader is now past the group.
    if (!reader.valid()) {
      break;
    }

    target += delta;
    if (target < reader.value()) {
      // Value following the group is already greater than target, or delta is
      // so large that target wrapped around.
      target = reader.value();
    }
  }
  EXPECT_FALSE(reader.valid());
  EXPECT_EQ(reader.position(), reader.size());
  EXPECT_FALSE(reader.next());
}

template <class Reader, class List>
void testSkipTo(const std::vector<uint64_t>& data, const List& list) {
  for (size_t steps = 10; steps < 100; steps += 10) {
    testSkipTo<Reader, List>(data, list, steps);
  }
  for (size_t steps = 100; steps <= 1000; steps += 100) {
    testSkipTo<Reader, List>(data, list, steps);
  }
  testSkipTo<Reader, List>(data, list, std::numeric_limits<size_t>::max());
  {
    // Skip to the first element.
    Reader reader(list);
    EXPECT_TRUE(reader.skipTo(data[0]));
    EXPECT_EQ(reader.value(), data[0]);
    EXPECT_EQ(reader.position(), 0);
  }

  // Skip past the last element, when possible. Make sure to probe values far
  // from the last element, as the reader implementation may keep an internal
  // upper bound larger than that, and we need to make sure we exercise skipping
  // both before and after that.
  using ValueType = typename Reader::ValueType;
  std::vector<ValueType> valuesPastTheEnd;
  const auto lastValue = data.back();
  const auto kMaxValue = std::numeric_limits<ValueType>::max();
  // Keep doubling the distance from the last value until we overflow.
  for (ValueType value = lastValue + 1; value > lastValue;
       value += value - lastValue) {
    valuesPastTheEnd.push_back(value);
  }
  if (kMaxValue != lastValue) {
    valuesPastTheEnd.push_back(kMaxValue);
  }

  for (auto value : valuesPastTheEnd) {
    Reader reader(list);
    EXPECT_FALSE(reader.skipTo(value)) << value << " " << lastValue;
    EXPECT_FALSE(reader.valid()) << value << " " << lastValue;
    EXPECT_EQ(reader.position(), reader.size()) << value << " " << lastValue;
    EXPECT_FALSE(reader.next()) << value << " " << lastValue;
  }
}

template <class Reader, class List>
void testJump(const std::vector<uint64_t>& data, const List& list) {
  std::mt19937 gen;
  std::vector<size_t> is(data.size());
  for (size_t i = 0; i < data.size(); ++i) {
    is[i] = i;
  }
  std::shuffle(is.begin(), is.end(), gen);
  if (Reader::EncoderType::forwardQuantum == 0) {
    is.resize(std::min<size_t>(is.size(), 100));
  }

  Reader reader(list);
  for (auto i : is) {
    // Also test idempotency.
    for (size_t round = 0; round < 2; ++round) {
      EXPECT_TRUE(reader.jump(i)) << i << " " << data.size();
      EXPECT_EQ(reader.value(), data[i]) << i << " " << data.size();
      EXPECT_EQ(reader.position(), i) << i << " " << data.size();
    }
    maybeTestPreviousValue(data, reader, i);
    maybeTestPrevious(data, reader, i);
  }
  EXPECT_FALSE(reader.jump(data.size()));
  EXPECT_FALSE(reader.valid());
  EXPECT_EQ(reader.position(), reader.size());
}

template <class Reader, class List>
void testJumpTo(const std::vector<uint64_t>& data, const List& list) {
  using ValueType = typename Reader::ValueType;

  CHECK(!data.empty());
  Reader reader(list);

  std::mt19937 gen;
  std::uniform_int_distribution<ValueType> targets(0, data.back());
  const size_t iters = Reader::EncoderType::skipQuantum == 0 ? 100 : 10000;
  for (size_t i = 0; i < iters; ++i) {
    uint64_t target;
    // Force boundary targets interleaved with random targets.
    if (i == 10) {
      target = data.back();
    } else if (i == 20) {
      target = 0;
    } else {
      target = targets(gen);
    }

    auto it = std::lower_bound(data.begin(), data.end(), target);
    CHECK(it != data.end());
    EXPECT_TRUE(reader.jumpTo(target));
    // Test the whole group of equal values.
    for (auto it2 = it; it2 != data.end() && *it2 == *it;
         ++it2, reader.next()) {
      EXPECT_EQ(reader.value(), *it2);
      EXPECT_EQ(reader.position(), std::distance(data.begin(), it2));
    }
    // Calling jumpTo() on the current value should reposition on the beginning
    // of the group.
    EXPECT_TRUE(reader.jumpTo(*it));
    EXPECT_EQ(reader.position(), std::distance(data.begin(), it));
  }

  if (data.back() != std::numeric_limits<ValueType>::max()) {
    EXPECT_FALSE(reader.jumpTo(data.back() + 1));
    EXPECT_FALSE(reader.valid());
    EXPECT_EQ(reader.position(), reader.size());
  }
}

template <class Reader, class Encoder>
void testEmpty() {
  const typename Encoder::ValueType* const data = nullptr;
  auto list = Encoder::encode(data, data);
  {
    Reader reader(list);
    EXPECT_FALSE(reader.next());
    EXPECT_EQ(reader.size(), 0);
  }
  {
    Reader reader(list);
    EXPECT_FALSE(reader.skip(1));
    EXPECT_FALSE(reader.skip(10));
    EXPECT_FALSE(reader.jump(0));
    EXPECT_FALSE(reader.jump(10));
  }
  {
    Reader reader(list);
    EXPECT_FALSE(reader.skipTo(1));
    EXPECT_FALSE(reader.jumpTo(1));
  }
}

// `upperBoundExtension` is required to inject additional 0-blocks
// at the end of the list. This allows us to test lists with a large gap between
// last element and universe upper bound, to exercise bounds-checking when
// skipping past the last element
template <class Reader, class Encoder>
void testAll(
    const std::vector<uint64_t>& data, uint64_t upperBoundExtension = 0) {
  SCOPED_TRACE(__PRETTY_FUNCTION__);

  Encoder encoder(data.size(), data.back() + upperBoundExtension);
  for (const auto value : data) {
    encoder.add(value);
  }
  auto list = encoder.finish();
  testNext<Reader>(data, list);
  testSkip<Reader>(data, list);
  testSkipTo<Reader>(data, list);
  testJump<Reader>(data, list);
  testJumpTo<Reader>(data, list);
  list.free();
}

template <class Reader, class List>
void bmNext(const List& list, const std::vector<uint64_t>& data, size_t iters) {
  if (data.empty()) {
    return;
  }

  Reader reader(list);
  for (size_t i = 0; i < iters; ++i) {
    if (reader.next()) [[likely]] {
      folly::doNotOptimizeAway(reader.value());
    } else {
      reader.reset();
    }
  }
}

template <class Reader, class List>
void bmSkip(
    const List& list,
    const std::vector<uint64_t>& /* data */,
    size_t logAvgSkip,
    size_t iters) {
  size_t avg = (size_t(1) << logAvgSkip);
  size_t base = avg - (avg >> 2);
  size_t mask = (avg > 1) ? (avg >> 1) - 1 : 0;

  Reader reader(list);
  for (size_t i = 0; i < iters; ++i) {
    size_t skip = base + (i & mask);
    if (reader.skip(skip)) [[likely]] {
      folly::doNotOptimizeAway(reader.value());
    } else {
      reader.reset();
    }
  }
}

template <class Reader, class List>
void bmSkipTo(
    const List& list,
    const std::vector<uint64_t>& data,
    size_t logAvgSkip,
    size_t iters) {
  size_t avg = (size_t(1) << logAvgSkip);
  size_t base = avg - (avg >> 2);
  size_t mask = (avg > 1) ? (avg >> 1) - 1 : 0;

  Reader reader(list);
  for (size_t i = 0, j = -1; i < iters; ++i) {
    size_t skip = base + (i & mask);
    j += skip;
    if (j >= data.size()) {
      reader.reset();
      j = -1;
    }

    reader.skipTo(data[j]);
    folly::doNotOptimizeAway(reader.value());
  }
}

template <class Reader, class List>
void bmJump(
    const List& list,
    const std::vector<uint64_t>& data,
    const std::vector<size_t>& order,
    size_t iters) {
  CHECK(!data.empty());
  CHECK_EQ(data.size(), order.size());

  Reader reader(list);
  for (size_t i = 0, j = 0; i < iters; ++i, ++j) {
    if (j == order.size()) {
      j = 0;
    }
    reader.jump(order[j]);
    folly::doNotOptimizeAway(reader.value());
  }
}

template <class Reader, class List>
void bmJumpTo(
    const List& list,
    const std::vector<uint64_t>& data,
    const std::vector<size_t>& order,
    size_t iters) {
  CHECK(!data.empty());
  CHECK_EQ(data.size(), order.size());

  Reader reader(list);
  for (size_t i = 0, j = 0; i < iters; ++i, ++j) {
    if (j == order.size()) {
      j = 0;
    }
    reader.jumpTo(data[order[j]]);
    folly::doNotOptimizeAway(reader.value());
  }
}

folly::Optional<instructions::Type> instructionsOverride();

template <class F>
auto dispatchInstructions(F&& f)
    -> decltype(f(std::declval<instructions::Default>())) {
  if (auto type = instructionsOverride()) {
    return instructions::dispatch(*type, std::forward<F>(f));
  } else {
    return instructions::dispatch(std::forward<F>(f));
  }
}

} // namespace compression
} // namespace folly
