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

#include <cassert>
#include <concepts>
#include <functional>
#include <iterator>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

#include <folly/Traits.h>
#include <folly/Utility.h>
#include <folly/container/Access.h>
#include <folly/detail/Iterators.h>
#include <folly/functional/Invoke.h>
#include <folly/lang/RValueReferenceWrapper.h>

namespace folly {

//  iterator_has_known_distance_v
//
//  Whether std::distance over a pair of iterators is reasonably known to give
//  the distance without advancing the iterators or copies of them.
template <typename Iter, typename SentinelIter>
inline constexpr bool iterator_has_known_distance_v =
    !require_sizeof<Iter> || !require_sizeof<SentinelIter>;
template <typename Iter>
inline constexpr bool iterator_has_known_distance_v<Iter, Iter> =
    std::is_base_of<
        std::random_access_iterator_tag,
        typename std::iterator_traits<Iter>::iterator_category>::value;

//  range_has_known_distance_v
//
//  Whether std::distance over the begin and end iterators is reasonably known
//  to give the distance without advancing the iterators or copies of them.
//
//  Useful for conditionally reserving memory in advance of iterating the range.
//
//  Note: Many use-cases are better served by range-v3 or std::ranges.
//
//  Example:
//
//      std::vector<result_type> results;
//      auto elems = /* some range */;
//      auto const elemsb = folly::access::begin(elems);
//      auto const elemse = folly::access::end(elems);
//
//      if constexpr (range_has_known_distance_v<decltype(elems)>) {
//        auto const dist = std::distance(elemsb, elemse);
//        results.reserve(static_cast<std::size_t>(dist));
//      }
//
//      for (auto elemsi = elemsb; elemsi != elemsi; ++i) {
//        results.push_back(do_work(*elemsi));
//      }
//      return results;
template <typename Range>
inline constexpr bool range_has_known_distance_v =
    iterator_has_known_distance_v<
        invoke_result_t<access::begin_fn, Range>,
        invoke_result_t<access::end_fn, Range>>;

//  iterator_category_t
//
//  Extracts iterator_category from an iterator.
template <typename Iter>
using iterator_category_t =
    typename std::iterator_traits<Iter>::iterator_category;

namespace detail {

template <typename Iter>
using traits_iterator_concept_t =
    typename std::iterator_traits<Iter>::iterator_concept;

template <typename Iter>
using member_iterator_concept_t = typename Iter::iterator_concept;

template <typename Iter>
using member_iterator_concept_or_category_t = conditional_apply_t<requires {
  typename Iter::iterator_concept;
}, member_iterator_concept_t, iterator_category_t, Iter>;

} // namespace detail

//  iterator_concept_t
//
//  Extracts iterator_concept from an iterator, falling back to
//  iterator_category, like the standard's exposition-only ITER_CONCEPT.
template <typename Iter>
using iterator_concept_t = conditional_apply_t<
    requires { typename std::iterator_traits<Iter>::iterator_concept; },
    detail::traits_iterator_concept_t,
    detail::member_iterator_concept_or_category_t,
    Iter>;

namespace detail {

template <typename Iter, typename Category, typename = void>
inline constexpr bool iterator_category_matches_v_ =
    !require_sizeof<Iter> || !require_sizeof<Category>;
template <typename Iter, typename Category>
inline constexpr bool iterator_category_matches_v_<
    Iter,
    Category,
    std::void_t<iterator_category_t<Iter>>> =
    std::is_convertible<iterator_category_t<Iter>, Category>::value;

} // namespace detail

//  iterator_category_matches_v
//
//  Whether an iterator's category matches Category (std::input_iterator_tag,
//  std::output_iterator_tag, etc). Defined for non-iterator types as well.
//
//  Useful for containers deduction guides implementation.
template <typename Iter, typename Category>
inline constexpr bool iterator_category_matches_v =
    detail::iterator_category_matches_v_<Iter, Category>;

//  iterator_value_type_t
//
//  Extracts a value type from an iterator.
template <typename Iter>
using iterator_value_type_t = typename std::iterator_traits<Iter>::value_type;

//  iterator_reference_type_t
//
//  Extracts reference from an iterator (C++20 iter_reference_t backported)
template <typename Iter>
using iterator_reference_t = decltype(*std::declval<Iter&>());

//  iterator_key_type_t
//
//  Extracts a key type from an iterator, leverages the knowledge that
//  key/value containers usually use std::pair<const K, V> as a value_type.
template <typename Iter>
using iterator_key_type_t =
    std::remove_cvref_t<typename iterator_value_type_t<Iter>::first_type>;

//  iterator_mapped_type_t
//
//  Extracts a mapped type from an iterator.
template <typename Iter>
using iterator_mapped_type_t =
    typename iterator_value_type_t<Iter>::second_type;

/**
 * Argument tuple for variadic emplace/constructor calls. Stores arguments by
 * (decayed) value. Restores original argument types with reference qualifiers
 * and adornments at unpack time to emulate perfect forwarding.
 *
 * Uses inheritance instead of a type alias to std::tuple so that emplace
 * iterators with implicit unpacking disabled can distinguish between
 * emplace_args and std::tuple parameters.
 *
 * @seealso folly::make_emplace_args
 * @seealso folly::get_emplace_arg
 */
template <typename... Args>
struct emplace_args : public std::tuple<std::decay_t<Args>...> {
  using storage_type = std::tuple<std::decay_t<Args>...>;
  using storage_type::storage_type;
};

/**
 * Pack arguments in a tuple for assignment to a folly::emplace_iterator,
 * folly::front_emplace_iterator, or folly::back_emplace_iterator. The
 * iterator's operator= will unpack the tuple and pass the unpacked arguments
 * to the container's emplace function, which in turn forwards the arguments to
 * the (multi-argument) constructor of the target class.
 *
 * Argument tuples generated with folly::make_emplace_args will be unpacked
 * before being passed to the container's emplace function, even for iterators
 * where implicit_unpack is set to false (so they will not implicitly unpack
 * std::pair or std::tuple arguments to operator=).
 *
 * Arguments are copied (lvalues) or moved (rvalues). To avoid copies and moves,
 * wrap references using std::ref(), std::cref(), and folly::rref(). Beware of
 * dangling references, especially references to temporary objects created with
 * folly::rref().
 *
 * Note that an argument pack created with folly::make_emplace_args is different
 * from an argument pack created with std::make_pair or std::make_tuple.
 * Specifically, passing a std::pair&& or std::tuple&& to an emplace iterator's
 * operator= will pass rvalue references to all fields of that tuple to the
 * container's emplace function, while passing an emplace_args&& to operator=
 * will cast those field references to the exact argument types as passed to
 * folly::make_emplace_args previously. If all arguments have been wrapped by
 * std::reference_wrappers or folly::rvalue_reference_wrappers, the result will
 * be the same as if the container's emplace function had been called directly
 * (perfect forwarding), with no temporary copies of the arguments.
 *
 * @seealso folly::rref
 *
 * @example
 *   class Widget { Widget(int, int); };
 *   std::vector<Widget> makeWidgets(const std::vector<int>& in) {
 *     std::vector<Widget> out;
 *     std::transform(
 *         in.begin(),
 *         in.end(),
 *         folly::back_emplacer(out),
 *         [](int i) { return folly::make_emplace_args(i, i); });
 *     return out;
 *   }
 */
template <typename... Args>
emplace_args<Args...> make_emplace_args(Args&&... args) noexcept(
    noexcept(emplace_args<Args...>(std::forward<Args>(args)...))) {
  return emplace_args<Args...>(std::forward<Args>(args)...);
}

namespace detail {
template <typename Arg>
decltype(auto) unwrap_emplace_arg(Arg&& arg) noexcept {
  return std::forward<Arg>(arg);
}
template <typename Arg>
decltype(auto) unwrap_emplace_arg(std::reference_wrapper<Arg> arg) noexcept {
  return arg.get();
}
template <typename Arg>
decltype(auto) unwrap_emplace_arg(
    folly::rvalue_reference_wrapper<Arg> arg) noexcept {
  return std::move(arg).get();
}
} // namespace detail

/**
 * Getter function for unpacking a single emplace argument.
 *
 * Calling get_emplace_arg on an emplace_args rvalue reference results in
 * perfect forwarding of the original input types. A special case are
 * std::reference_wrapper and folly::rvalue_reference_wrapper objects within
 * folly::emplace_args. These are also unwrapped so that the bare reference is
 * returned.
 *
 * std::get is not a customization point in the standard library, so the
 * cleanest solution was to define our own getter function.
 */
template <size_t I, typename... Args>
decltype(auto) get_emplace_arg(emplace_args<Args...>&& args) noexcept {
  using Out = std::tuple<Args...>;
  return detail::unwrap_emplace_arg(
      std::forward<std::tuple_element_t<I, Out>>(std::get<I>(args)));
}
template <size_t I, typename... Args>
decltype(auto) get_emplace_arg(emplace_args<Args...>& args) noexcept {
  return detail::unwrap_emplace_arg(std::get<I>(args));
}
template <size_t I, typename... Args>
decltype(auto) get_emplace_arg(const emplace_args<Args...>& args) noexcept {
  return detail::unwrap_emplace_arg(std::get<I>(args));
}
template <size_t I, typename Args>
decltype(auto) get_emplace_arg(Args&& args) noexcept {
  return std::get<I>(std::move(args));
}
template <size_t I, typename Args>
decltype(auto) get_emplace_arg(Args& args) noexcept {
  return std::get<I>(args);
}
template <size_t I, typename Args>
decltype(auto) get_emplace_arg(const Args& args) noexcept {
  return std::get<I>(args);
}

namespace detail {
/**
 * Emplace implementation class for folly::emplace_iterator.
 */
template <typename Container>
struct Emplace {
  Emplace(Container& c, typename Container::iterator i)
      : container(std::addressof(c)), iter(std::move(i)) {}
  template <typename... Args>
  void emplace(Args&&... args) {
    iter = container->emplace(iter, std::forward<Args>(args)...);
    ++iter;
  }
  Container* container;
  typename Container::iterator iter;
};

/**
 * Emplace implementation class for folly::hint_emplace_iterator.
 */
template <typename Container>
struct EmplaceHint {
  EmplaceHint(Container& c, typename Container::iterator i)
      : container(std::addressof(c)), iter(std::move(i)) {}
  template <typename... Args>
  void emplace(Args&&... args) {
    iter = container->emplace_hint(iter, std::forward<Args>(args)...);
    ++iter;
  }
  Container* container;
  typename Container::iterator iter;
};

/**
 * Emplace implementation class for folly::front_emplace_iterator.
 */
template <typename Container>
struct EmplaceFront {
  explicit EmplaceFront(Container& c) : container(std::addressof(c)) {}
  template <typename... Args>
  void emplace(Args&&... args) {
    container->emplace_front(std::forward<Args>(args)...);
  }
  Container* container;
};

/**
 * Emplace implementation class for folly::back_emplace_iterator.
 */
template <typename Container>
struct EmplaceBack {
  explicit EmplaceBack(Container& c) : container(std::addressof(c)) {}
  template <typename... Args>
  void emplace(Args&&... args) {
    container->emplace_back(std::forward<Args>(args)...);
  }
  Container* container;
};

/**
 * Generic base class and implementation of all emplace iterator classes.
 *
 * Uses the curiously recurring template pattern (CRTP) to cast `this*` to
 * `Derived*`; i.e., to implement covariant return types in a generic manner.
 */
template <typename Derived, typename EmplaceImpl, bool implicit_unpack>
class emplace_iterator_base;

/**
 * Partial specialization of emplace_iterator_base with implicit unpacking
 * disabled.
 */
template <typename Derived, typename EmplaceImpl>
class emplace_iterator_base<Derived, EmplaceImpl, false>
    : protected EmplaceImpl /* protected implementation inheritance */ {
 public:
  // Iterator traits.
  using iterator_category = std::output_iterator_tag;
  using value_type = void;
  using difference_type = void;
  using pointer = void;
  using reference = void;
  using container_type =
      std::remove_reference_t<decltype(*EmplaceImpl::container)>;

  using EmplaceImpl::EmplaceImpl;

  /**
   * Canonical output operator. Forwards single argument straight to container's
   * emplace function.
   */
  template <typename T>
  Derived& operator=(T&& arg) {
    this->emplace(std::forward<T>(arg));
    return static_cast<Derived&>(*this);
  }

  /**
   * Special output operator for packed arguments. Unpacks args and performs
   * variadic call to container's emplace function.
   */
  template <typename... Args>
  Derived& operator=(emplace_args<Args...>& args) {
    return unpackAndEmplace(args, std::index_sequence_for<Args...>{});
  }
  template <typename... Args>
  Derived& operator=(const emplace_args<Args...>& args) {
    return unpackAndEmplace(args, std::index_sequence_for<Args...>{});
  }
  template <typename... Args>
  Derived& operator=(emplace_args<Args...>&& args) {
    return unpackAndEmplace(
        std::move(args), std::index_sequence_for<Args...>{});
  }

  // No-ops.
  Derived& operator*() { return static_cast<Derived&>(*this); }
  Derived& operator++() { return static_cast<Derived&>(*this); }
  Derived& operator++(int) { return static_cast<Derived&>(*this); }

  // We need all of these explicit defaults because the custom operator=
  // overloads disable implicit generation of these functions.
  emplace_iterator_base(const emplace_iterator_base&) = default;
  emplace_iterator_base(emplace_iterator_base&&) noexcept = default;
  emplace_iterator_base& operator=(emplace_iterator_base&) = default;
  emplace_iterator_base& operator=(const emplace_iterator_base&) = default;
  emplace_iterator_base& operator=(emplace_iterator_base&&) noexcept = default;

 protected:
  template <typename Args, std::size_t... I>
  Derived& unpackAndEmplace(Args& args, std::index_sequence<I...>) {
    this->emplace(get_emplace_arg<I>(args)...);
    return static_cast<Derived&>(*this);
  }
  template <typename Args, std::size_t... I>
  Derived& unpackAndEmplace(const Args& args, std::index_sequence<I...>) {
    this->emplace(get_emplace_arg<I>(args)...);
    return static_cast<Derived&>(*this);
  }
  template <typename Args, std::size_t... I>
  Derived& unpackAndEmplace(Args&& args, std::index_sequence<I...>) {
    this->emplace(get_emplace_arg<I>(std::move(args))...);
    return static_cast<Derived&>(*this);
  }
};

/**
 * Partial specialization of emplace_iterator_base with implicit unpacking
 * enabled.
 *
 * Uses inheritance rather than SFINAE. operator= requires a single argument,
 * which makes it very tricky to use std::enable_if or similar.
 */
template <typename Derived, typename EmplaceImpl>
class emplace_iterator_base<Derived, EmplaceImpl, true>
    : public emplace_iterator_base<Derived, EmplaceImpl, false> {
 private:
  using Base = emplace_iterator_base<Derived, EmplaceImpl, false>;

 public:
  using Base::Base;
  using Base::operator=;

  /**
   * Special output operator for arguments packed into a std::pair. Unpacks
   * the pair and performs variadic call to container's emplace function.
   */
  template <typename... Args>
  Derived& operator=(std::pair<Args...>& args) {
    return this->unpackAndEmplace(args, std::index_sequence_for<Args...>{});
  }
  template <typename... Args>
  Derived& operator=(const std::pair<Args...>& args) {
    return this->unpackAndEmplace(args, std::index_sequence_for<Args...>{});
  }
  template <typename... Args>
  Derived& operator=(std::pair<Args...>&& args) {
    return this->unpackAndEmplace(
        std::move(args), std::index_sequence_for<Args...>{});
  }

  /**
   * Special output operator for arguments packed into a std::tuple. Unpacks
   * the tuple and performs variadic call to container's emplace function.
   */
  template <typename... Args>
  Derived& operator=(std::tuple<Args...>& args) {
    return this->unpackAndEmplace(args, std::index_sequence_for<Args...>{});
  }
  template <typename... Args>
  Derived& operator=(const std::tuple<Args...>& args) {
    return this->unpackAndEmplace(args, std::index_sequence_for<Args...>{});
  }
  template <typename... Args>
  Derived& operator=(std::tuple<Args...>&& args) {
    return this->unpackAndEmplace(
        std::move(args), std::index_sequence_for<Args...>{});
  }

  // We need all of these explicit defaults because the custom operator=
  // overloads disable implicit generation of these functions.
  emplace_iterator_base(const emplace_iterator_base&) = default;
  emplace_iterator_base(emplace_iterator_base&&) noexcept = default;
  emplace_iterator_base& operator=(emplace_iterator_base&) = default;
  emplace_iterator_base& operator=(const emplace_iterator_base&) = default;
  emplace_iterator_base& operator=(emplace_iterator_base&&) noexcept = default;
};

/**
 * Concrete instantiation of emplace_iterator_base. All emplace iterator
 * classes; folly::emplace_iterator, folly::hint_emplace_iterator,
 * folly::front_emplace_iterator, and folly::back_emplace_iterator; are just
 * type aliases of this class.
 *
 * It is not possible to alias emplace_iterator_base directly, because type
 * aliases cannot be used for CRTP.
 */
template <
    template <typename> class EmplaceImplT,
    typename Container,
    bool implicit_unpack>
class emplace_iterator_impl
    : public emplace_iterator_base<
          emplace_iterator_impl<EmplaceImplT, Container, implicit_unpack>,
          EmplaceImplT<Container>,
          implicit_unpack> {
 private:
  using Base = emplace_iterator_base<
      emplace_iterator_impl,
      EmplaceImplT<Container>,
      implicit_unpack>;

 public:
  using Base::Base;
  using Base::operator=;

  // We need all of these explicit defaults because the custom operator=
  // overloads disable implicit generation of these functions.
  emplace_iterator_impl(const emplace_iterator_impl&) = default;
  emplace_iterator_impl(emplace_iterator_impl&&) noexcept = default;
  emplace_iterator_impl& operator=(emplace_iterator_impl&) = default;
  emplace_iterator_impl& operator=(const emplace_iterator_impl&) = default;
  emplace_iterator_impl& operator=(emplace_iterator_impl&&) noexcept = default;
};
} // namespace detail

/**
 * Behaves just like std::insert_iterator except that it calls emplace()
 * instead of insert(). Uses perfect forwarding.
 */
template <typename Container, bool implicit_unpack = true>
using emplace_iterator =
    detail::emplace_iterator_impl<detail::Emplace, Container, implicit_unpack>;

/**
 * Behaves just like std::insert_iterator except that it calls emplace_hint()
 * instead of insert(). Uses perfect forwarding.
 */
template <typename Container, bool implicit_unpack = true>
using hint_emplace_iterator = detail::
    emplace_iterator_impl<detail::EmplaceHint, Container, implicit_unpack>;

/**
 * Behaves just like std::front_insert_iterator except that it calls
 * emplace_front() instead of insert(). Uses perfect forwarding.
 */
template <typename Container, bool implicit_unpack = true>
using front_emplace_iterator = detail::
    emplace_iterator_impl<detail::EmplaceFront, Container, implicit_unpack>;

/**
 * Behaves just like std::back_insert_iterator except that it calls
 * emplace_back() instead of insert(). Uses perfect forwarding.
 */
template <typename Container, bool implicit_unpack = true>
using back_emplace_iterator = detail::
    emplace_iterator_impl<detail::EmplaceBack, Container, implicit_unpack>;

/**
 * Convenience function to construct a folly::emplace_iterator, analogous to
 * std::inserter().
 *
 * Setting implicit_unpack to false will disable implicit unpacking of
 * single std::pair and std::tuple arguments to the iterator's operator=. That
 * may be desirable in case of constructors that expect a std::pair or
 * std::tuple argument.
 */
template <bool implicit_unpack = true, typename Container>
emplace_iterator<Container, implicit_unpack> emplacer(
    Container& c, typename Container::iterator i) {
  return emplace_iterator<Container, implicit_unpack>(c, std::move(i));
}

/**
 * Convenience function to construct a folly::hint_emplace_iterator, analogous
 * to std::inserter().
 *
 * Setting implicit_unpack to false will disable implicit unpacking of
 * single std::pair and std::tuple arguments to the iterator's operator=. That
 * may be desirable in case of constructors that expect a std::pair or
 * std::tuple argument.
 */
template <bool implicit_unpack = true, typename Container>
hint_emplace_iterator<Container, implicit_unpack> hint_emplacer(
    Container& c, typename Container::iterator i) {
  return hint_emplace_iterator<Container, implicit_unpack>(c, std::move(i));
}

/**
 * Convenience function to construct a folly::front_emplace_iterator, analogous
 * to std::front_inserter().
 *
 * Setting implicit_unpack to false will disable implicit unpacking of
 * single std::pair and std::tuple arguments to the iterator's operator=. That
 * may be desirable in case of constructors that expect a std::pair or
 * std::tuple argument.
 */
template <bool implicit_unpack = true, typename Container>
front_emplace_iterator<Container, implicit_unpack> front_emplacer(
    Container& c) {
  return front_emplace_iterator<Container, implicit_unpack>(c);
}

/**
 * Convenience function to construct a folly::back_emplace_iterator, analogous
 * to std::back_inserter().
 *
 * Setting implicit_unpack to false will disable implicit unpacking of
 * single std::pair and std::tuple arguments to the iterator's operator=. That
 * may be desirable in case of constructors that expect a std::pair or
 * std::tuple argument.
 */
template <bool implicit_unpack = true, typename Container>
back_emplace_iterator<Container, implicit_unpack> back_emplacer(Container& c) {
  return back_emplace_iterator<Container, implicit_unpack>(c);
}

template <typename Container>
class index_iterator;

namespace detail {

struct index_iterator_access_at {
  template <typename Container, typename Index>
  constexpr decltype(auto) operator()(Container& container, Index index) const {
    if constexpr (folly::is_tag_invocable_v<
                      index_iterator_access_at,
                      Container&,
                      Index>) {
      return folly::tag_invoke(*this, container, index);
    } else {
      return container[index];
    }
  }
};

template <typename T>
using index_iterator_size_type_t = typename std::remove_cv_t<T>::size_type;

template <typename T>
using index_iterator_difference_type_t =
    typename std::remove_cv_t<T>::difference_type;

template <typename Container>
using index_iterator_size_t =
    detected_or_t<std::size_t, index_iterator_size_type_t, Container>;

template <typename Container>
using index_iterator_reference_t = decltype(index_iterator_access_at{}(
    FOLLY_DECLVAL(Container&), index_iterator_size_t<Container>{}));

template <typename Container>
using index_iterator_facade_t = IteratorFacade<
    index_iterator<Container>,
    typename std::remove_const_t<Container>::value_type,
    std::random_access_iterator_tag,
    index_iterator_reference_t<Container>,
    detected_or_t<std::ptrdiff_t, index_iterator_difference_type_t, Container>>;

} // namespace detail

FOLLY_DEFINE_CPO(detail::index_iterator_access_at, index_iterator_access_at)

/**
 * index_iterator
 *
 * An iterator class for random access data structures that provide an
 * access by index via `operator[](size_type)`.
 *
 * Requires a `value_type` defined in a container (we cannot
 * get the value type from reference).
 *
 * Example:
 *  class Container {
 *   public:
 *    using value_type = <*>;  // we need value_type to be defined.
 *    using iterator = folly::index_iterator<Container>;
 *    using const_iterator = folly::index_iterator<const Container>;
 *    using reverse_iterator = std::reverse_iterator<iterator>;
 *    using const_reverse_iterator = std::reverse_iterator<const_iterator>;
 *
 *    some_ref_type  operator[](std::size_t index);
 *    some_cref_type operator[](std::size_t index) const;
 *   ...
 *  };
 *
 *  Note that `some_ref_type` can be any proxy reference, as long as the
 *  algorithms support that (for example from range-v3).
 *
 * NOTE: if `operator[]` doesn't work for you for some reason
 *       you can specify:
 *
 * ```
 *  friend some_ref_type tag_invoke(
 *    folly::cpo_t<index_iterator_access_at>,
 *    Container& c,
 *    std::size_t index);
 *
 *  friend some_cref_type tag_invoke(
 *    folly::cpo_t<index_iterator_access_at>,
 *    const Container& c,
 *    std::size_t index);
 * ```
 **/

template <typename Container>
class index_iterator : public detail::index_iterator_facade_t<Container> {
  using facade = detail::index_iterator_facade_t<Container>;
  friend facade;

 public:
  // index iterator specific types

  using container_type = Container;
  using size_type = detail::index_iterator_size_t<Container>;

  using typename facade::difference_type;

  static_assert(
      std::is_signed<difference_type>::value, "difference_type must be signed");

  // accessors

  // instance of `index_iterator_accessor`
  container_type* get_container() const { return container_; }
  difference_type get_index() const { return index_; }

  constexpr index_iterator() = default;

  constexpr index_iterator(container_type& container, size_type index)
      : container_(&container), index_(index) {}

  // converting constructors --

  template <
      typename OtherContainer,
      typename = std::enable_if_t<
          std::is_same<std::remove_const_t<container_type>, OtherContainer>::
              value &&
          std::is_const<container_type>::value>>
  /* implicit */ constexpr index_iterator(index_iterator<OtherContainer> other)
      : container_(other.get_container()), index_(other.get_index()) {}

 private:
  constexpr decltype(auto) dereference() const {
    return index_iterator_access_at(*container_, index_);
  }

  constexpr bool equal(const index_iterator& rhs) const {
    assert(container_ == rhs.container_);
    return index_ == rhs.index_;
  }

  constexpr void increment() { ++index_; }

  constexpr void decrement() { --index_; }

  constexpr void advance(difference_type n) {
    index_ = static_cast<size_type>(static_cast<difference_type>(index_) + n);
  }

  constexpr difference_type distance_to(const index_iterator& rhs) const {
    assert(container_ == rhs.container_);
    return static_cast<difference_type>(rhs.index_) -
        static_cast<difference_type>(index_);
  }

  container_type* container_ = nullptr;
  size_type index_ = 0;
};

//  reverse_iterator
//
//  Like std::reverse_iterator, but trivial to destroy/copy when Iterator is.
template <typename Iterator>
class reverse_iterator {
  static_assert(std::bidirectional_iterator<Iterator>);

 public:
  // types ----------------------------
  using iterator_type = Iterator;
  // iterator concept cannot reuse iterator category, there
  // are corner cases.
  using iterator_category = std::conditional_t<
      std::derived_from<
          iterator_category_t<Iterator>,
          std::random_access_iterator_tag>,
      std::random_access_iterator_tag,
      iterator_category_t<Iterator>>;
  using iterator_concept = std::conditional_t<
      std::random_access_iterator<Iterator>,
      std::random_access_iterator_tag,
      std::bidirectional_iterator_tag>;
  using value_type = std::iter_value_t<Iterator>;
  using difference_type = std::iter_difference_t<Iterator>;
  using pointer = typename std::iterator_traits<Iterator>::pointer;
  using reference = std::iter_reference_t<Iterator>;

  // constructors ----------------------
  reverse_iterator() = default;

  constexpr explicit reverse_iterator(Iterator x) : current{x} {}

  template <typename U>
  /* implicit */ constexpr reverse_iterator(const reverse_iterator<U>& u)
    requires(!std::same_as<U, Iterator>) &&
      std::convertible_to<const U&, Iterator>
      : current{u.base()} {}

  template <typename U>
  constexpr reverse_iterator& operator=(const reverse_iterator<U>& u)
    requires(!std::same_as<U, Iterator>) &&
      std::convertible_to<const U&, Iterator> &&
      std::assignable_from<Iterator&, const U&>
  {
    current = u.base();
    return *this;
  }

  // access -----------------------------

  constexpr Iterator base() const { return current; }

  constexpr reference operator*() const {
    auto tmp = current;
    return *--tmp;
  }

  constexpr pointer operator->() const
    requires(
        std::is_pointer_v<Iterator> ||
        requires(const Iterator i) { i.operator->(); })
  {
    auto tmp = current;
    --tmp;
    if constexpr (std::is_pointer_v<Iterator>) {
      return tmp;
    } else {
      return tmp.operator->();
    }
  }

  constexpr reference operator[](difference_type n) const {
    return *(*this + n);
  }

  // + / -  -----------------------------

  constexpr reverse_iterator& operator++() {
    --current;
    return *this;
  }

  constexpr reverse_iterator operator++(int) {
    auto tmp = *this;
    ++*this;
    return tmp;
  }

  constexpr reverse_iterator& operator--() {
    ++current;
    return *this;
  }

  constexpr reverse_iterator operator--(int) {
    auto tmp = *this;
    --*this;
    return tmp;
  }

  constexpr reverse_iterator operator+(difference_type n) const {
    auto tmp = *this;
    tmp += n;
    return tmp;
  }

  friend constexpr reverse_iterator operator+(
      difference_type n, const reverse_iterator& x) {
    return x + n;
  }

  constexpr reverse_iterator& operator+=(difference_type n) {
    current -= n;
    return *this;
  }

  constexpr reverse_iterator operator-(difference_type n) const {
    return *this + -n;
  }

  constexpr reverse_iterator& operator-=(difference_type n) {
    return *this += -n;
  }

  template <std::sized_sentinel_for<Iterator> IteratorR>
  friend constexpr auto operator-(
      const reverse_iterator& x, const reverse_iterator<IteratorR>& y) {
    return current_of(y) - x.current;
  }

  // ordering -----------------------

  bool operator==(const reverse_iterator&) const = default;

  template <std::equality_comparable_with<Iterator> IteratorR>
  friend constexpr bool operator==(
      const reverse_iterator& x, const reverse_iterator<IteratorR>& y) {
    return x.current == current_of(y);
  }

  //  ordering is reversed, and not every iterator that has < also has <=>

  template <typename IteratorR>
  friend constexpr auto operator<=>(
      const reverse_iterator& x, const reverse_iterator<IteratorR>& y)
    requires std::three_way_comparable_with<Iterator, IteratorR>
  {
    return current_of(y) <=> x.current;
  }

  template <std::totally_ordered_with<Iterator> IteratorR>
  friend constexpr bool operator<(
      const reverse_iterator& x, const reverse_iterator<IteratorR>& y) {
    return x.current > current_of(y);
  }

  template <std::totally_ordered_with<Iterator> IteratorR>
  friend constexpr bool operator>(
      const reverse_iterator& x, const reverse_iterator<IteratorR>& y) {
    return y < x;
  }

  template <std::totally_ordered_with<Iterator> IteratorR>
  friend constexpr bool operator<=(
      const reverse_iterator& x, const reverse_iterator<IteratorR>& y) {
    return !(y < x);
  }

  template <std::totally_ordered_with<Iterator> IteratorR>
  friend constexpr bool operator>=(
      const reverse_iterator& x, const reverse_iterator<IteratorR>& y) {
    return !(x < y);
  }

  // ranges hooks -----------------------
  // (requirements follow std strictly)

  friend constexpr std::iter_rvalue_reference_t<Iterator>
  iter_move(const reverse_iterator& x) noexcept(
      std::is_nothrow_copy_constructible_v<Iterator> &&
      noexcept(std::ranges::iter_move(--std::declval<Iterator&>()))) {
    auto tmp = x.current;
    return std::ranges::iter_move(--tmp);
  }

  template <std::indirectly_swappable<Iterator> IteratorR>
  friend constexpr void
  iter_swap(const reverse_iterator& x, const reverse_iterator<IteratorR>& y) noexcept(
      std::is_nothrow_copy_constructible_v<Iterator> &&
      std::is_nothrow_copy_constructible_v<IteratorR> &&
      noexcept(std::ranges::iter_swap(
          --std::declval<Iterator&>(), --std::declval<IteratorR&>()))) {
    auto xtmp = x.current;
    auto ytmp = current_of(y);
    std::ranges::iter_swap(--xtmp, --ytmp);
  }

 protected:
  Iterator current{}; // standard requires value init

 private:
  template <typename IteratorY>
  friend class reverse_iterator;

  // CWG1699 - on gcc can't do y.current in a hidden friend function.
  template <typename IteratorY>
  static constexpr const IteratorY& current_of(
      const reverse_iterator<IteratorY>& y) noexcept {
    return y.current;
  }
};

//  wrap_iterator
//
//  Wraps Iterator into a distinct type, like libstdc++'s __normal_iterator.
//  Tag distinguishes wrappers of the same Iterator.
template <typename Iterator, typename Tag>
class wrap_iterator;

namespace detail {

template <typename Iterator, typename Tag>
using wrap_iterator_facade_t = IteratorFacade<
    wrap_iterator<Iterator, Tag>,
    std::iter_value_t<Iterator>,
    iterator_concept_t<Iterator>,
    std::iter_reference_t<Iterator>,
    std::iter_difference_t<Iterator>>;

} // namespace detail

template <typename Iterator, typename Tag>
class wrap_iterator : public detail::wrap_iterator_facade_t<Iterator, Tag> {
  // input iterators are not supported for now
  static_assert(std::forward_iterator<Iterator>);

  using facade = detail::wrap_iterator_facade_t<Iterator, Tag>;
  friend facade;

 public:
  // types ----------------------------
  using iterator_type = Iterator;
  // To fully support std::ranges, these override the facade's: ranges
  // iterators (e.g. iota_view's) can have a weaker category than concept.
  using iterator_category = iterator_category_t<Iterator>;
  using iterator_concept = iterator_concept_t<Iterator>;
  using typename facade::difference_type;
  using pointer = typename std::iterator_traits<Iterator>::pointer;

  // constructors ----------------------
  wrap_iterator() = default;

  constexpr explicit wrap_iterator(Iterator x) : current_{x} {}

  constexpr explicit operator Iterator() const { return current_; }

  template <typename U>
  /* implicit */ constexpr wrap_iterator(const wrap_iterator<U, Tag>& u)
    requires(!std::same_as<U, Iterator>) &&
      std::convertible_to<const U&, Iterator>
      : current_{u.base()} {}

  // access -----------------------------

  constexpr const Iterator& base() const noexcept { return current_; }

  constexpr pointer operator->() const
    requires(
        std::is_pointer_v<Iterator> ||
        requires(const Iterator i) { i.operator->(); })
  {
    if constexpr (std::is_pointer_v<Iterator>) {
      return current_;
    } else {
      return current_.operator->();
    }
  }

  // ranges hooks -----------------------

  friend constexpr std::iter_rvalue_reference_t<Iterator> iter_move(
      const wrap_iterator& x) //
      noexcept(noexcept(std::ranges::iter_move(x.current_))) {
    return std::ranges::iter_move(x.current_);
  }

  template <std::indirectly_swappable<Iterator> IteratorR>
  friend constexpr void iter_swap(
      const wrap_iterator& x, const wrap_iterator<IteratorR, Tag>& y) //
      noexcept(noexcept(std::ranges::iter_swap(x.current_, y.base()))) {
    std::ranges::iter_swap(x.current_, y.base());
  }

 private:
  constexpr decltype(auto) dereference() const { return *current_; }

  constexpr bool equal(const wrap_iterator& rhs) const {
    return current_ == rhs.current_;
  }

  constexpr void increment() { ++current_; }

  constexpr void decrement() { --current_; }

  constexpr void advance(difference_type n) { current_ += n; }

  constexpr difference_type distance_to(const wrap_iterator& rhs) const {
    return rhs.current_ - current_;
  }

  Iterator current_{};
};

} // namespace folly

namespace std {

// required by std ranges
template <typename Iterator1, typename Iterator2>
  requires(!sized_sentinel_for<Iterator1, Iterator2>)
inline constexpr bool disable_sized_sentinel_for<
    folly::reverse_iterator<Iterator1>,
    folly::reverse_iterator<Iterator2>> = true;

} // namespace std
