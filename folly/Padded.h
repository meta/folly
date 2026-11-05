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
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <type_traits>

#include <folly/Portability.h>
#include <folly/Traits.h>
#include <folly/detail/Iterators.h>
#include <folly/functional/Invoke.h>
#include <folly/portability/SysTypes.h>

/**
 * Code that aids in storing data aligned on block (possibly cache-line)
 * boundaries, perhaps with padding.
 *
 * Class Node represents one block.  Given an iterator to a container of
 * Node, class Iterator encapsulates an iterator to the underlying elements.
 * Adaptor converts a sequence of Node into a sequence of underlying elements
 * (not fully compatible with STL container requirements, see comments
 * near the Node class declaration).
 */

namespace folly {
namespace padded {

/**
 * A Node is a fixed-size container of as many objects of type T as would
 * fit in a region of memory of size NS.  The last NS % sizeof(T)
 * bytes are ignored and uninitialized.
 *
 * Node only works for trivial types, which is usually not a concern.  This
 * is intentional: Node itself is trivial, which means that it can be
 * serialized / deserialized using a simple memcpy.
 */
template <class T, size_t NS>
class Node {
  static_assert(
      std::is_trivial_v<T> && sizeof(T) <= NS && NS % alignof(T) == 0);

 public:
  using value_type = T;
  static constexpr size_t kNodeSize = NS;
  static constexpr size_t kElementCount = NS / sizeof(T);
  static constexpr size_t kPaddingBytes = NS % sizeof(T);

  T* data() { return storage_.data; }
  const T* data() const { return storage_.data; }

  bool operator==(const Node& other) const {
    return memcmp(data(), other.data(), sizeof(T) * kElementCount) == 0;
  }
  bool operator!=(const Node& other) const { return !(*this == other); }

  /**
   * Return the number of nodes needed to represent n values.  Rounds up.
   */
  static constexpr size_t nodeCount(size_t n) {
    return (n + kElementCount - 1) / kElementCount;
  }

  /**
   * Return the total byte size needed to represent n values, rounded up
   * to the nearest full node.
   */
  static constexpr size_t paddedByteSize(size_t n) { return nodeCount(n) * NS; }

  /**
   * Return the number of bytes used for padding n values.
   * Note that, even if n is a multiple of kElementCount, this may
   * return non-zero if kPaddingBytes != 0, as the padding at the end of
   * the last node is not included in the result.
   */
  static constexpr size_t paddingBytes(size_t n) {
    return (
        n ? (kPaddingBytes +
             (kElementCount - 1 - (n - 1) % kElementCount) * sizeof(T))
          : 0);
  }

  /**
   * Return the minimum byte size needed to represent n values.
   * Does not round up.  Even if n is a multiple of kElementCount, this
   * may be different from paddedByteSize() if kPaddingBytes != 0, as
   * the padding at the end of the last node is not included in the result.
   * Note that the calculation below works for n=0 correctly (returns 0).
   */
  static constexpr size_t unpaddedByteSize(size_t n) {
    return paddedByteSize(n) - paddingBytes(n);
  }

 private:
  union Storage {
    unsigned char bytes[NS];
    T data[kElementCount];
  } storage_;
};

template <class Iter>
class Iterator;

namespace detail {

FOLLY_CREATE_MEMBER_INVOKER(emplace_back, emplace_back);

// Helper class template to define a base class for Iterator (below) and save
// typing.
template <
    template <class> class Class,
    class Iter,
    class Traits = std::iterator_traits<Iter>,
    class Ref = typename Traits::reference,
    class Val = typename Traits::value_type::value_type>
using IteratorBase = folly::detail::IteratorAdaptor<
    Class<Iter>, // CRTC
    Iter, // Base iterator type
    Val, // Value type
    typename Traits::iterator_category,
    like_t<Ref, Val>, // Reference type
    typename Traits::difference_type>;

} // namespace detail

/**
 * Wrapper around iterators to Node to return iterators to the underlying
 * node elements.
 */
template <class Iter>
class Iterator : public detail::IteratorBase<Iterator, Iter> {
  using Super = detail::IteratorBase<Iterator, Iter>;

 public:
  using Node = typename std::iterator_traits<Iter>::value_type;

  Iterator() : pos_(0) {}

  explicit Iterator(Iter base) : Super(base), pos_(0) {}

  // Return the current node and the position inside the node
  const Node& node() const { return *this->base_reference(); }
  size_t pos() const { return pos_; }

 private:
  typename Super::reference dereference() const {
    return (*this->base_reference()).data()[pos_];
  }

  bool equal(const Iterator& other) const {
    return (
        this->base_reference() == other.base_reference() && pos_ == other.pos_);
  }

  void advance(typename Super::difference_type n) {
    constexpr ssize_t elementCount = Node::kElementCount; // signed!
    ssize_t newPos = pos_ + n;
    if (newPos >= 0 && newPos < elementCount) {
      pos_ = newPos;
      return;
    }
    ssize_t nblocks = newPos / elementCount;
    newPos %= elementCount;
    if (newPos < 0) {
      --nblocks; // negative
      newPos += elementCount;
    }
    this->base_reference() += nblocks;
    pos_ = newPos;
  }

  void increment() {
    if (++pos_ == Node::kElementCount) {
      ++this->base_reference();
      pos_ = 0;
    }
  }

  void decrement() {
    if (--pos_ == -1) {
      --this->base_reference();
      pos_ = Node::kElementCount - 1;
    }
  }

  typename Super::difference_type distance_to(const Iterator& other) const {
    constexpr ssize_t elementCount = Node::kElementCount; // signed!
    ssize_t nblocks =
        std::distance(this->base_reference(), other.base_reference());
    return nblocks * elementCount + (other.pos_ - pos_);
  }

  friend typename Super::Super;
  ssize_t pos_; // signed for easier advance() implementation
};

/**
 * Given a container to Node, return iterators to the first element in
 * the first Node / one past the last element in the last Node.
 * Note that the last node is assumed to be full; if that's not the case,
 * subtract from end() as appropriate.
 */

template <class Container>
Iterator<typename Container::const_iterator> cbegin(const Container& c) {
  return Iterator<typename Container::const_iterator>(std::begin(c));
}

template <class Container>
Iterator<typename Container::const_iterator> cend(const Container& c) {
  return Iterator<typename Container::const_iterator>(std::end(c));
}

template <class Container>
Iterator<typename Container::const_iterator> begin(const Container& c) {
  return cbegin(c);
}

template <class Container>
Iterator<typename Container::const_iterator> end(const Container& c) {
  return cend(c);
}

template <class Container>
Iterator<typename Container::iterator> begin(Container& c) {
  return Iterator<typename Container::iterator>(std::begin(c));
}

template <class Container>
Iterator<typename Container::iterator> end(Container& c) {
  return Iterator<typename Container::iterator>(std::end(c));
}

/**
 * Adaptor around a STL sequence container.
 *
 * Converts a sequence of Node into a sequence of its underlying elements
 * (with enough functionality to make it useful, although it's not fully
 * compatible with the STL container requirements, see below).
 *
 * Provides iterators (of the same category as those of the underlying
 * container), size(), front(), back(), push_back(), pop_back(), and const /
 * non-const versions of operator[] (if the underlying container supports
 * them).  Does not provide push_front() / pop_front() or arbitrary insert /
 * emplace / erase.  Also provides reserve() / capacity() if supported by the
 * underlying container.
 *
 * Yes, it's called Adaptor, not Adapter, as that's the name used by the STL.
 * Deal with it.
 *
 * Internally, we hold a container of Node and the number of elements in
 * the last block.  We don't keep empty blocks, so the number of elements in
 * the last block is always between 1 and Node::kElementCount (inclusive).
 * (this is true if the container is empty as well to make push_back() simpler,
 * see the implementation of the size() method for details).
 */
template <class Container>
class Adaptor {
 public:
  using Node = typename Container::value_type;
  using value_type = typename Node::value_type;
  using reference = value_type&;
  using const_reference = const value_type&;
  using iterator = Iterator<typename Container::iterator>;
  using const_iterator = Iterator<typename Container::const_iterator>;
  using difference_type = typename const_iterator::difference_type;
  using size_type = typename Container::size_type;

  static constexpr size_t kElementsPerNode = Node::kElementCount;
  // Constructors
  Adaptor() : lastCount_(Node::kElementCount) {}
  explicit Adaptor(Container c, size_t lastCount = Node::kElementCount)
      : c_(std::move(c)), lastCount_(lastCount) {}
  explicit Adaptor(size_t n, const value_type& value = value_type())
      : c_(Node::nodeCount(n), fullNode(value)) {
    const auto count = n % Node::kElementCount;
    lastCount_ = count != 0 ? count : Node::kElementCount;
  }

  Adaptor(const Adaptor&) = default;
  Adaptor& operator=(const Adaptor&) = default;
  Adaptor(Adaptor&& other) noexcept
      : c_(std::move(other.c_)), lastCount_(other.lastCount_) {
    other.lastCount_ = Node::kElementCount;
  }
  Adaptor& operator=(Adaptor&& other) {
    if (this != &other) {
      c_ = std::move(other.c_);
      lastCount_ = other.lastCount_;
      other.lastCount_ = Node::kElementCount;
    }
    return *this;
  }

  // Iterators
  const_iterator cbegin() const { return const_iterator(c_.begin()); }
  const_iterator cend() const {
    auto it = const_iterator(c_.end());
    if (lastCount_ != Node::kElementCount) {
      it -= (Node::kElementCount - lastCount_);
    }
    return it;
  }
  const_iterator begin() const { return cbegin(); }
  const_iterator end() const { return cend(); }
  iterator begin() { return iterator(c_.begin()); }
  iterator end() {
    auto it = iterator(c_.end());
    if (lastCount_ != Node::kElementCount) {
      it -= difference_type(Node::kElementCount - lastCount_);
    }
    return it;
  }
  void swap(Adaptor& other) {
    using std::swap;
    swap(c_, other.c_);
    swap(lastCount_, other.lastCount_);
  }
  bool empty() const { return c_.empty(); }
  size_type size() const {
    return (
        c_.empty() ? 0 : (c_.size() - 1) * Node::kElementCount + lastCount_);
  }
  size_type max_size() const {
    return (
        (c_.max_size() <=
         std::numeric_limits<size_type>::max() / Node::kElementCount)
            ? c_.max_size() * Node::kElementCount
            : std::numeric_limits<size_type>::max());
  }

  const value_type& front() const {
    assert(!empty());
    return c_.front().data()[0];
  }
  value_type& front() {
    assert(!empty());
    return c_.front().data()[0];
  }

  const value_type& back() const {
    assert(!empty());
    return c_.back().data()[lastCount_ - 1];
  }
  value_type& back() {
    assert(!empty());
    return c_.back().data()[lastCount_ - 1];
  }

  template <typename... Args>
  void emplace_back(Args&&... args) {
    new (allocate_back()) value_type(std::forward<Args>(args)...);
  }

  void push_back(value_type x) { emplace_back(std::move(x)); }

  void pop_back() {
    assert(!empty());
    if (--lastCount_ == 0) {
      c_.pop_back();
      lastCount_ = Node::kElementCount;
    }
  }

  void clear() {
    c_.clear();
    lastCount_ = Node::kElementCount;
  }

  void reserve(size_type n) {
    assert(n >= 0);
    c_.reserve(Node::nodeCount(n));
  }

  size_type capacity() const { return c_.capacity() * Node::kElementCount; }

  const value_type& operator[](size_type idx) const {
    return c_[idx / Node::kElementCount].data()[idx % Node::kElementCount];
  }
  value_type& operator[](size_type idx) {
    return c_[idx / Node::kElementCount].data()[idx % Node::kElementCount];
  }

  /**
   * Return the underlying container and number of elements in the last block,
   * and clear *this.  Useful when you want to process the data as Nodes
   * (again) and want to avoid copies.
   */
  std::pair<Container, size_t> move() {
    std::pair<Container, size_t> p(std::move(c_), lastCount_);
    lastCount_ = Node::kElementCount;
    return p;
  }

  /**
   * Return a const reference to the underlying container and the current
   * number of elements in the last block.
   */
  std::pair<const Container&, size_t> peek() const {
    return std::make_pair(std::cref(c_), lastCount_);
  }

  void padToFullNode(const value_type& padValue) {
    // the if is necessary because c_ may be empty so we can't call c_.back()
    if (lastCount_ != Node::kElementCount) {
      auto last = c_.back().data();
      std::fill(last + lastCount_, last + Node::kElementCount, padValue);
      lastCount_ = Node::kElementCount;
    }
  }

 private:
  value_type* allocate_back() {
    if (lastCount_ == Node::kElementCount) {
      if constexpr (std::is_invocable_v<detail::emplace_back, Container&>) {
        c_.emplace_back();
      } else {
        c_.push_back(typename Container::value_type());
      }
      lastCount_ = 0;
    }
    return &c_.back().data()[lastCount_++];
  }

  static Node fullNode(const value_type& value) {
    Node n;
    std::fill(n.data(), n.data() + kElementsPerNode, value);
    return n;
  }
  Container c_; // container of Nodes
  size_t lastCount_; // number of elements in last Node
};

} // namespace padded
} // namespace folly
