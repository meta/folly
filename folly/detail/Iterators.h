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

#include <compare>
#include <cstddef>
#include <iterator>
#include <memory>
#include <type_traits>
#include <utility>

/*
 * This contains stripped-down workalikes of some Boost classes:
 *
 *   iterator_adaptor
 *   iterator_facade
 *
 * Rationale: the boost headers providing those classes are surprisingly large.
 * The bloat comes from the headers themselves, but more so, their transitive
 * includes.
 *
 * These implementations are simple and minimal.  They may be missing features
 * provided by the Boost classes mentioned above.  They provide just enough for
 * the uses within Folly libs; more features will be slapped in here if and
 * when they're needed.
 *
 * These classes may possibly add features as well.  Care is taken not to
 * change functionality where it's expected to be the same (e.g. `dereference`
 * will do the same thing).
 *
 * These are currently only intended for use within Folly, hence their living
 * under detail.  Use outside Folly is not recommended.
 *
 * To see how to use these classes, find the instances where this is used within
 * Folly libs.  Common use cases can also be found in `IteratorsTest.cpp`.
 */

namespace folly {
namespace detail {

/**
 * Supports input, forward, bidirectional and random-access iteration.  The
 * derived class must have definitions for these methods:
 *
 *   void increment();
 *   void decrement(); // optional, to be used with bidirectional
 *   reference dereference() const;
 *   bool equal([appropriate iterator type] const& rhs) const;
 *   void advance(difference_type n); // random-access only
 *   difference_type distance_to(D const& rhs) const; // random-access only
 *
 * distance_to returns rhs - *this, the number of increments that take *this
 * to rhs.
 *
 * These names are consistent with those used by the Boost iterator
 * facade / adaptor classes to ease migration classes in this file.  The
 * facade calls them as members of D, so they may be private if D befriends
 * the facade.
 *
 * Template parameters:
 * D: the deriving class (CRTP)
 * V: value type
 * Tag: the iterator category, one of:
 *   std::input_iterator_tag
 *   std::forward_iterator_tag
 *   std::bidirectional_iterator_tag
 *   std::random_access_iterator_tag
 * R: the reference type, which may be a proxy
 * Diff: the difference type
 */
template <
    class D,
    class V,
    class Tag,
    class R = V&,
    class Diff = std::ptrdiff_t>
class IteratorFacade {
  static constexpr bool kRandomAccess =
      std::is_base_of_v<std::random_access_iterator_tag, Tag>;

  // What operator-> returns when R is a proxy rather than a reference.
  class ArrowProxy {
   public:
    explicit ArrowProxy(R ref) : ref_(std::move(ref)) {}
    R* operator->() { return std::addressof(ref_); }

   private:
    R ref_;
  };

 public:
  using value_type = V;
  using reference = R;
  using pointer = std::
      conditional_t<std::is_reference_v<R>, std::add_pointer_t<R>, ArrowProxy>;
  using difference_type = Diff;
  using iterator_category = Tag;

  friend bool operator==(D const& lhs, D const& rhs) { return equal(lhs, rhs); }

  friend bool operator!=(D const& lhs, D const& rhs) { return !(lhs == rhs); }

  R operator*() const { return asDerivedConst().dereference(); }

  pointer operator->() const {
    if constexpr (std::is_reference_v<R>) {
      return std::addressof(operator*());
    } else {
      return pointer(operator*());
    }
  }

  D& operator++() {
    asDerived().increment();
    return asDerived();
  }

  D operator++(int) {
    auto ret = asDerived(); // copy
    asDerived().increment();
    return ret;
  }

  D& operator--() {
    asDerived().decrement();
    return asDerived();
  }

  D operator--(int) {
    auto ret = asDerived(); // copy
    asDerived().decrement();
    return ret;
  }

  D& operator+=(Diff n)
    requires kRandomAccess
  {
    asDerived().advance(n);
    return asDerived();
  }

  D& operator-=(Diff n)
    requires kRandomAccess
  {
    asDerived().advance(-n);
    return asDerived();
  }

  R operator[](Diff n) const
    requires kRandomAccess
  {
    D it = asDerivedConst();
    it += n;
    return *it;
  }

  friend D operator+(D it, Diff n)
    requires kRandomAccess
  {
    return it += n;
  }

  friend D operator+(Diff n, D it)
    requires kRandomAccess
  {
    return it += n;
  }

  friend D operator-(D it, Diff n)
    requires kRandomAccess
  {
    return it -= n;
  }

  friend Diff operator-(D const& lhs, D const& rhs)
    requires kRandomAccess
  {
    return -distance(lhs, rhs);
  }

  friend std::strong_ordering operator<=>(D const& lhs, D const& rhs)
    requires kRandomAccess
  {
    return Diff(0) <=> distance(lhs, rhs);
  }

 private:
  D& asDerived() { return static_cast<D&>(*this); }

  D const& asDerivedConst() const { return static_cast<D const&>(*this); }

  static bool equal(D const& lhs, D const& rhs) { return lhs.equal(rhs); }

  static Diff distance(D const& lhs, D const& rhs) {
    return lhs.distance_to(rhs);
  }
};

/**
 * Wrap one iterator while providing an interator interface with e.g. a
 * different value_type.
 *
 * Template parameters:
 * D: the deriving class (CRTP)
 * I: the wrapper iterator type
 * V: value type
 * Tag, R, Diff: as for IteratorFacade
 */
template <
    class D,
    class I,
    class V,
    class Tag,
    class R = V&,
    class Diff = std::ptrdiff_t>
class IteratorAdaptor : public IteratorFacade<D, V, Tag, R, Diff> {
 public:
  using Super = IteratorFacade<D, V, Tag, R, Diff>;
  using value_type = typename Super::value_type;
  using iterator_category = typename Super::iterator_category;
  using reference = typename Super::reference;
  using pointer = typename Super::pointer;
  using difference_type = typename Super::difference_type;
  using base_type = I;

  IteratorAdaptor() = default;
  explicit IteratorAdaptor(I base) : base_(std::move(base)) {}

  void increment() { ++base_; }

  void decrement() { --base_; }

  void advance(Diff n) { base_ += n; }

  R dereference() const { return *base_; }

  bool equal(D const& rhs) const { return base_ == rhs.base_; }

  Diff distance_to(D const& rhs) const { return rhs.base_ - base_; }

  I const& base() const { return base_; }
  I& base() { return base_; }

 protected:
  I const& base_reference() const { return base_; }
  I& base_reference() { return base_; }

 private:
  I base_;
};

} // namespace detail
} // namespace folly
