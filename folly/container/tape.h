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

#include <folly/Portability.h>
#include <folly/Range.h>
#include <folly/container/Iterator.h>
#include <folly/container/detail/tape_detail.h>
#include <folly/lang/Ordering.h>
#include <folly/memory/UninitializedMemoryHacks.h>

#include <algorithm>
#include <cassert>
#include <compare>
#include <initializer_list>
#include <iterator>
#include <numeric>
#include <string_view>
#include <type_traits>
#include <vector>

#include <ranges>

namespace folly {

/* # Tape
 *
 * A container adapter, that builds a version of `vector<vector>` on top of a
 * random access underlying container.
 *
 * Instead of having a container of containers it's more efficient to have
 * a single container and store where the separators are.
 *
 * [string second string third string]
 *  ^      ^             ^
 *
 * One subrange of internal elements we call a `record`.
 *
 * You can `push` a new `record` or pop one from the back.
 * We also support an `erase` like `std::vector` but `insert` only for one
 * element. (there is no reason for limitation, except it's not implemented).
 *
 * NOTE: for when you don't have the `record` ready, you can use a
 * `record_builder` interface.
 *
 * Existing `records` can be accessed by index.
 * Existing `records` cannot be mutated, except for the last record (see record
 * builder).
 *
 * ## tape<tape>
 *
 * tape<tape> is supported, though not all of the APIs.
 * More apis can be implemented if/when needed.
 * Use `record_builder`.
 *
 * ## PERFORMANCE CHARACTERISTICS (folly/container/test/tape_bench):
 *
 * Reading (cache miss):
 * Container performs much better for access than vector<vector>/vector<string>
 * for cases where the data is out of cache.
 * If the data is in cache, reading is roughly the same.
 *
 * Construction
 * If you know for a fact that all the elements are fitting into SSO buffer,
 * and you always have complete records (not building) then `tape` does not help
 * you, or can even be a slight regression.
 *
 * Otherwise tape can give you good speedups, especially if you need to
 * `push_back` on individual records.
 *
 * Potential future perf improvements.
 * * it is possible to do a tape with one allocation for both metadata and
 *   data (in special cases).
 * * when converting indexes to pointers, compiler has to shift.
 *   For contigious containers we can store offsets in bytes.
 *
 * ## Exception safety
 * We provide only basic exception safety: the object is destructible or
 * assignable.
 * `std::bad_alloc` is assumed to never happen (a function that uses malloc can
 * be marked noexcept).
 *
 * ## NAME TAPE
 *
 * Name tape is taken from a lecture by Alexander Stepanov but we are not 100%
 * sure if this is the container he had in mind.
 */
template <std::ranges::random_access_range Container>
class tape;

// string_tape - a common usecase.
using string_tape = tape<std::vector<char>>;

template <std::ranges::random_access_range Container>
class tape {
  using ref_traits = detail::tape_reference_traits<Container>;

 public:
  using container_type = Container;

  using const_reference = typename ref_traits::reference;
  using reference = const_reference;

  // value_type for tape does not make much sense.
  // The best we found is to make reference type to be value type.
  // This does not quite make sense but works well enough.
  using value_type = const_reference;
  using scalar_value_type = detail::maybe_range_value_t<container_type>;

  using size_type = typename Container::size_type;
  using difference_type = typename Container::difference_type;

  using iterator = folly::index_iterator<const tape>;
  using const_iterator = iterator;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

 private:
  using scalar_cmp_fn = std::compare_three_way;
  template <typename T = scalar_value_type>
  using scalar_cmp_result_t = std::compare_three_way_result_t<T>;

  static constexpr bool byte_ordered =
      std::is_pointer_v<typename const_reference::iterator> &&
      (std::is_same_v<scalar_value_type, char> ||
       std::is_same_v<scalar_value_type, signed char> ||
       std::is_same_v<scalar_value_type, unsigned char>);

  template <typename T = scalar_value_type>
  using elem_cmp_result_t = std::
      conditional_t<byte_ordered, std::strong_ordering, scalar_cmp_result_t<T>>;

  struct elem_cmp_fn {
    template <typename T = scalar_value_type>
    elem_cmp_result_t<T> operator()(
        const const_reference& x, const const_reference& y) const {
      if constexpr (byte_ordered) {
        return x.compare(y) <=> 0;
      } else {
        return detail::lexicographical_compare_three_way(
            x.begin(), x.end(), y.begin(), y.end(), scalar_cmp_fn{});
      }
    }
  };

 public:
  // concepts (defined as statics because no local concepts) ------

  template <typename I>
  static constexpr bool iterator_of_scalars =
      std::is_convertible_v<iterator_value_type_t<I>, scalar_value_type>;

  template <typename I>
  static constexpr bool range_of_scalars =
      iterator_of_scalars<detail::maybe_range_const_iterator_t<I>>;

  template <typename I>
  static constexpr bool iterator_of_records =
      range_of_scalars<iterator_value_type_t<I>>;

  template <typename I>
  static constexpr bool range_of_records =
      iterator_of_records<detail::maybe_range_const_iterator_t<I>>;

  // rule of 5
  tape(const tape&) = default;
  tape& operator=(const tape&) = default;
  ~tape() = default;
  tape(tape&&) noexcept;
  tape& operator=(tape&&) noexcept;

  // constructors -----
  tape() noexcept = default;

  template <typename I, std::sentinel_for<I> S>
  explicit tape(I f, S l)
    requires iterator_of_records<I>
  {
    range_constructor(f, l);
  }

  template <typename R>
    requires(
        !std::is_same_v<std::remove_cvref_t<R>, tape> &&
        (std::is_convertible_v<R, const_reference> || // const char*
         range_of_records<R>))
  explicit tape(std::initializer_list<R> il) {
    range_constructor(il.begin(), il.end());
  }

  // access ------

  [[nodiscard]] const_reference operator[](size_type i) const noexcept {
    return ref_traits::make(
        data_.begin() + markers_[i], data_.begin() + markers_[i + 1]);
  }

  [[nodiscard]] const_reference at(size_type i) const {
    if (i >= size()) [[unlikely]] {
      // libc++ doesn't provide index. This helps optimizations.
      throw std::out_of_range("tape");
    }
    return operator[](i);
  }

  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
  [[nodiscard]] size_type size() const noexcept { return markers_.size() - 1; }
  [[nodiscard]] size_type size_flat() const noexcept { return data_.size(); }

  [[nodiscard]] const_reference front() const noexcept { return operator[](0); }
  [[nodiscard]] const_reference back() const noexcept {
    return operator[](size() - 1);
  }

  // iterators ----

  [[nodiscard]] const_iterator begin() const noexcept { return {*this, 0}; }
  [[nodiscard]] const_iterator cbegin() const noexcept { return begin(); }

  [[nodiscard]] const_iterator end() const noexcept { return {*this, size()}; }
  [[nodiscard]] const_iterator cend() const noexcept { return end(); }

  [[nodiscard]] auto rbegin() const noexcept {
    return const_reverse_iterator{end()};
  }
  [[nodiscard]] auto crbegin() const noexcept { return rbegin(); }

  [[nodiscard]] auto rend() const noexcept {
    return const_reverse_iterator{begin()};
  }
  [[nodiscard]] auto crend() const noexcept { return rend(); }

  // push / emplace_back --------

  template <typename I, std::sentinel_for<I> S>
  void push_back(I f, S l)
    requires iterator_of_scalars<I>
  {
    data_.insert(data_.end(), f, l);
    markers_.push_back(static_cast<difference_type>(data_.size()));
  }

  template <typename R>
  void push_back(R&& r)
    requires(
        range_of_scalars<R> &&
        !std::is_convertible_v<R, const_reference>) // handle \0 separately
  {
    push_back(std::begin(r), std::end(r));
  }

  void push_back(const_reference r) { push_back(r.begin(), r.end()); }

  void push_back(std::initializer_list<scalar_value_type> r) {
    push_back(r.begin(), r.end());
  }

  void emplace_back() { push_back({}); }

  template <typename... Args>
  void emplace_back(Args&&... args) {
    push_back(std::forward<Args>(args)...);
  }

  // push_back_unsafe --------
  // like push_back but requires you to have enough capacity for added range.
  // happened to give a 2x performance improvements on certain benchmarks.

  // requires to have enough capacity
  template <typename I, std::sentinel_for<I> S>
  void push_back_unsafe(I f, S l)
    requires iterator_of_scalars<I>
  {
    // basic exception guarantee is preserved here.
    detail::append_range_unsafe(data_, f, l);
    markers_.push_back(static_cast<difference_type>(data_.size()));
  }

  template <typename R>
  void push_back_unsafe(R&& r)
    requires(
        range_of_scalars<R> &&
        !std::is_convertible_v<R, const_reference>) // handle \0 separately
  {
    push_back_unsafe(std::begin(r), std::end(r));
  }

  void push_back_unsafe(const_reference r) {
    push_back_unsafe(r.begin(), r.end());
  }

  // record builder (constructing last record) -------

  class record_builder;

  // get a record builder.
  // new_record_builder starts a builder for a new record.
  // last_record_builder allows you to append/mutate the last record.
  [[nodiscard]] record_builder new_record_builder();
  [[nodiscard]] record_builder last_record_builder();

  // insert one record ----------

  template <typename I, std::sentinel_for<I> S>
  iterator insert(const_iterator pos, I f, S l)
    requires iterator_of_scalars<I>;

  template <typename R>
  iterator insert(const_iterator pos, R&& r)
    requires(range_of_scalars<R> && !std::is_convertible_v<R, const_reference>)
  {
    return insert(pos, std::begin(r), std::end(r));
  }

  iterator insert(
      const_iterator pos, std::initializer_list<scalar_value_type> r) {
    return insert(pos, r.begin(), r.end());
  }

  iterator insert(const_iterator pos, const_reference r) {
    return insert(pos, r.begin(), r.end());
  }

  // capacity ------
  void reserve(size_type records, size_type elements) {
    markers_.reserve(records + 1);
    data_.reserve(elements);
  }

  // assumes that 1 element per record. This is likely to help a bit.
  void reserve(size_type records) {
    markers_.reserve(records + 1);
    data_.reserve(records);
  }

  void shrink_to_fit() {
    markers_.shrink_to_fit();
    data_.shrink_to_fit();
  }

  // resize/clear -------

  // same args as for push_back/emplace back are accepted
  template <typename... Args>
  void resize(size_type new_size, const Args&... args);

  void clear() noexcept {
    markers_.resize(1);
    data_.clear();
  }

  // erase -------

  void pop_back() noexcept {
    assert(!empty());
    data_.resize(data_.size() - back().size());
    markers_.pop_back();
  }

  // note: same behaviour as for std::vector, erasing end() is UB
  iterator erase(const_iterator pos) {
    assert(pos != end());
    return erase(pos, pos + 1);
  }

  iterator erase(const_iterator f, const_iterator l);

  // ordering --------

  friend bool operator==(const tape& x, const tape& y) {
    return x.markers_ == y.markers_ && x.data_ == y.data_;
  }

  friend auto operator<=>(const tape& a, const tape& b)
    requires std::three_way_comparable<scalar_value_type>
  {
    return detail::lexicographical_compare_three_way(
        a.begin(), a.end(), b.begin(), b.end(), elem_cmp_fn{});
  }

  folly::Range<const difference_type*> markers() const { return markers_; }
  reference scalars() const {
    return ref_traits::make(data_.begin(), data_.end());
  }

 private:
  template <typename I, typename S>
  void range_constructor(I f, S l);

  // NOTE: using container difference_type might be too much here but,
  // on the other hand, there should be reasonably few items on the tape and
  // this makes interface simpler.
  std::vector<difference_type> markers_ = {0};
  container_type data_;
};

// Provides a way to construct a last record similar
// to how you would `std::vector`.
// Typical workflow is you `push_back` a bunch of individual elements and then
// `commit()`.
template <std::ranges::random_access_range Container>
class tape<Container>::record_builder {
 public:
  record_builder(const record_builder&) = delete;
  record_builder(record_builder&&) = delete;
  record_builder& operator=(const record_builder&) = delete;
  record_builder& operator=(record_builder&&) = delete;

  using iterator = typename container_type::iterator;
  using const_iterator = typename container_type::const_iterator;
  using reference = typename std::iterator_traits<iterator>::reference;
  using const_reference =
      typename std::iterator_traits<const_iterator>::reference;
  using size_type = typename container_type::size_type;
  using difference_type =
      typename std::iterator_traits<iterator>::difference_type;

  // mutators ---

  void push_back(scalar_value_type x) { self_->data_.push_back(std::move(x)); }

  template <typename... Args>
  reference emplace_back(Args&&... args) {
    self_->data_.emplace_back(std::forward<Args>(args)...);
    // cannot rely on the container doing the right thing here.
    return self_->data_.back();
  }

  // constructed record is added to the tape.
  void commit() { self_->markers_.push_back(self_->data_.size()); }

  // discards elements of the constructed record. (automatic on destruction)
  void abort() { self_->data_.resize(self_->markers_.back()); }

  // iterators -----

  [[nodiscard]] iterator begin() noexcept {
    return self_->data_.begin() + self_->markers_.back();
  }
  [[nodiscard]] const_iterator begin() const noexcept {
    return self_->data_.cbegin() + self_->markers_.back();
  }
  [[nodiscard]] const_iterator cbegin() const noexcept { return begin(); }

  [[nodiscard]] iterator end() noexcept { return self_->data_.end(); }
  [[nodiscard]] const_iterator end() const noexcept {
    return self_->data_.cend();
  }
  [[nodiscard]] const_iterator cend() const noexcept { return end(); }

  // sometimes functions (like fmt) optimize for vector back inserter.
  // so better expose that.
  [[nodiscard]] auto back_inserter() noexcept {
    return std::back_inserter(self_->data_);
  }

  // access ---

  [[nodiscard]] bool empty() const noexcept { return begin() == end(); }

  [[nodiscard]] size_type size() const noexcept {
    return static_cast<size_type>(end() - begin());
  }

  [[nodiscard]] reference operator[](size_type i) noexcept {
    return begin()[static_cast<difference_type>(i)];
  }

  [[nodiscard]] const_reference operator[](size_type i) const noexcept {
    return begin()[static_cast<difference_type>(i)];
  }

  [[nodiscard]] reference at(size_type i) {
    if (i >= size()) [[unlikely]] {
      // libc++ doesn't provide index. This helps optimizations.
      throw std::out_of_range("tape::scoped_record_builder");
    }
    return operator[](i);
  }

  [[nodiscard]] const_reference at(size_type i) const {
    if (i >= size()) [[unlikely]] {
      // libc++ doesn't provide index. This helps optimizations.
      throw std::out_of_range("tape::scoped_record_builder");
    }
    return operator[](i);
  }

  [[nodiscard]] reference back() { return self_->data_.back(); }
  [[nodiscard]] const_reference back() const { return self_->data_.back(); }

  ~record_builder() noexcept { abort(); }

 private:
  friend class tape;

  explicit record_builder(tape& self) : self_(&self) {}

  tape* self_;
};

template <std::ranges::random_access_range Container>
auto tape<Container>::new_record_builder() -> record_builder {
  return record_builder{*this};
}

template <std::ranges::random_access_range Container>
auto tape<Container>::last_record_builder() -> record_builder {
  assert(!empty());
  markers_.pop_back();
  return new_record_builder();
}

// tape methods -----

template <std::ranges::random_access_range Container>
tape<Container>::tape(tape&& x) noexcept
    : markers_(std::move(x.markers_)), data_(std::move(x.data_)) {
  // we assume that allocations never fail
  x.markers_ = {0};
  x.data_.clear();
}

template <std::ranges::random_access_range Container>
tape<Container>& tape<Container>::operator=(tape&& x) noexcept {
  if (this != &x) {
    markers_ = std::move(x.markers_);
    data_ = std::move(x.data_);
  }
  // we assume that allocations never fail
  x.markers_ = {0};
  x.data_.clear();
  return *this;
}

template <std::ranges::random_access_range Container>
template <typename I, typename S>
void tape<Container>::range_constructor(I f, S l) {
  if constexpr (auto maybe = detail::compute_total_tape_len_if_possible(f, l);
                std::is_same_v<decltype(maybe), detail::fake_type>) {
    while (f != l) {
      push_back(*f);
      ++f;
    }
  } else {
    auto [nrecords, total_len] = maybe;
    reserve(nrecords, total_len);

    while (f != l) {
      push_back_unsafe(*f);
      ++f;
    }
  }
}

template <std::ranges::random_access_range Container>
template <typename... Args>
void tape<Container>::resize(size_type new_size, const Args&... args) {
  if (new_size >= size()) {
    new_size -= size();
    while (new_size--) {
      emplace_back(args...);
    }
    return;
  }

  data_.resize(markers_[new_size]);
  markers_.resize(new_size + 1);
}

template <std::ranges::random_access_range Container>
template <typename I, std::sentinel_for<I> S>
auto tape<Container>::insert(const_iterator pos, I f, S l) -> iterator
  requires iterator_of_scalars<I>
{
  auto data_pos = data_.begin() + markers_[pos.get_index()];
  size_type old_size = data_.size();
  data_.insert(data_pos, f, l);

  auto inserted_len = static_cast<difference_type>(data_.size() - old_size);

  difference_type start = markers_[pos.get_index()];

  auto markers_tail =
      markers_.insert(markers_.begin() + pos.get_index(), start);
  ++markers_tail;

  std::transform(
      markers_tail, markers_.end(), markers_tail, [&](difference_type m) {
        return m + inserted_len;
      });

  // both tape* and index stayed the same
  return pos;
}

template <std::ranges::random_access_range Container>
auto tape<Container>::erase(const_iterator f, const_iterator l) -> iterator {
  difference_type from = f.get_index();
  difference_type to = l.get_index();

  auto markers_f = markers_.begin() + from;
  auto markers_l = markers_.begin() + to;
  auto data_f = data_.begin() + *markers_f;
  auto data_l = data_.begin() + *markers_l;

  std::ptrdiff_t removed_length = data_l - data_f;
  std::transform(markers_l, markers_.end(), markers_l, [&](difference_type m) {
    return m - removed_length;
  });

  markers_.erase(markers_f, markers_l);
  data_.erase(data_f, data_l);

  // both tape* and index stayed the same
  return f;
}

} // namespace folly
