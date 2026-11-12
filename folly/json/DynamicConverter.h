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

/**
 * DynamicConverter provides a simple, generic interface by which dynamic
 * objects can be converted to/from concrete C++ types.
 *
 * Can be used in conjunction with folly/json.h to read a JSON value (which
 * returns a folly::dynamic) and then turn that JSON value into a well-typed
 * representation.
 *
 * @file DynamicConverter.h
 * @refcode folly/docs/examples/folly/DynamicConverter.cpp
 */

#pragma once

#include <concepts>
#include <iterator>
#include <type_traits>
#include <utility>
#include <vector>

#include <folly/Likely.h>
#include <folly/Optional.h>
#include <folly/Traits.h>
#include <folly/Utility.h>
#include <folly/json/dynamic.h>
#include <folly/lang/Exception.h>

namespace folly {

/**
 * Return a well-typed representation of a dynamic.
 *
 * See docs/DynamicConverter.md for supported types and customization.
 *
 * @tparam T  A type representing the structure of the dynamic argument.
 * @refcode folly/docs/examples/folly/DynamicConverter.cpp
 */
template <typename T>
T convertTo(const dynamic&);

/**
 * Turn an arbitrary type into a dynamic.
 *
 * @see convertTo for customization
 */
template <typename T>
dynamic toDynamic(const T&);
} // namespace folly

namespace folly {

///////////////////////////////////////////////////////////////////////////////
// traits

namespace dynamicconverter_detail {

template <typename T>
concept dyn_range = requires {
  typename T::value_type;
  typename T::iterator;
};

// Customization point: specialize with value = false to keep a type with an
// iterator from being treated as a container.
template <typename T>
struct iterator_class_is_container
    : std::bool_constant<(
          dyn_range<T> &&
          std::is_constructible_v<
              T,
              typename T::iterator,
              typename T::iterator>)> {};

template <typename T>
using class_is_container = std::conjunction<
    std::bool_constant<(requires { typename T::iterator; })>,
    iterator_class_is_container<T>>;

template <typename T>
concept dyn_container = std::is_class_v<T> && class_is_container<T>::value;

// Ranges other than strings, which convert to dynamic as strings.
template <typename T>
concept dyn_array =
    dyn_range<T> && !std::is_constructible_v<StringPiece, const T&>;

template <typename T>
concept dyn_map = dyn_array<T> && requires { typename T::mapped_type; };

template <typename T>
concept dyn_dereferenceable = requires(const T& t) {
  static_cast<bool>(t);
  *t;
};

// Excludes string literals.
template <typename T>
concept dyn_like_pointer =
    !std::is_convertible_v<T, StringPiece> && dyn_dereferenceable<T>;

template <typename T>
concept dyn_optional = dyn_dereferenceable<T> && requires {
  T(std::declval<typename T::value_type>());
};

template <typename T>
using is_range = std::bool_constant<dyn_range<T>>;

template <typename T>
using is_container = std::bool_constant<dyn_container<T>>;

template <typename T>
using is_map =
    std::bool_constant<(dyn_range<T> && requires { typename T::mapped_type; })>;

template <typename T>
using is_associative =
    std::bool_constant<(dyn_range<T> && requires { typename T::key_type; })>;

template <typename T>
using is_like_pointer = std::bool_constant<dyn_like_pointer<T>>;

template <typename T>
using is_optional = std::bool_constant<dyn_optional<T>>;

} // namespace dynamicconverter_detail

///////////////////////////////////////////////////////////////////////////////
// element conversion

namespace dynamicconverter_detail {

template <typename T>
struct ItemConverter {
  static T convert(const dynamic::const_item_iterator::value_type& /* kv */) {
    throw_exception<TypeError>("array", dynamic::Type::OBJECT);
  }
};

template <typename F, typename S>
struct ItemConverter<std::pair<F, S>> {
  static std::pair<F, S> convert(
      const dynamic::const_item_iterator::value_type& kv) {
    return {convertTo<F>(kv.first), convertTo<S>(kv.second)};
  }
};

// Converts the elements of an array, or the items of an object, to T.
template <typename T>
std::vector<T> convertElements(const dynamic& d) {
  std::vector<T> elements;
  if (d.isArray()) {
    elements.reserve(d.size());
    for (const auto& v : d) {
      elements.push_back(convertTo<T>(v));
    }
  } else if (d.isObject()) {
    elements.reserve(d.size());
    for (const auto& kv : d.items()) {
      elements.push_back(ItemConverter<T>::convert(kv));
    }
  } else {
    throw_exception<TypeError>("object or array", d.type());
  }
  return elements;
}

} // namespace dynamicconverter_detail

///////////////////////////////////////////////////////////////////////////////
// DynamicConverter specializations

/**
 * Each specialization of DynamicConverter has the function
 *     'static T convert(const dynamic&);'
 */

// default - intentionally unimplemented
template <typename T, typename Enable = void>
struct DynamicConverter;

// dynamic
template <>
struct DynamicConverter<dynamic> {
  static dynamic convert(const dynamic& d) { return d; }
};

// boolean
template <>
struct DynamicConverter<bool> {
  static bool convert(const dynamic& d) { return d.asBool(); }
};

// integrals
template <typename T>
  requires std::integral<T>
struct DynamicConverter<T, void> {
  static T convert(const dynamic& d) { return folly::to<T>(d.asInt()); }
};

// enums
template <typename T>
  requires std::is_enum_v<T>
struct DynamicConverter<T, void> {
  static T convert(const dynamic& d) {
    using type = typename std::underlying_type<T>::type;
    return static_cast<T>(DynamicConverter<type>::convert(d));
  }
};

// floating point
template <typename T>
  requires std::floating_point<T>
struct DynamicConverter<T, void> {
  static T convert(const dynamic& d) { return folly::to<T>(d.asDouble()); }
};

// fbstring
template <>
struct DynamicConverter<folly::fbstring> {
  static folly::fbstring convert(const dynamic& d) { return d.asString(); }
};

// std::string
template <>
struct DynamicConverter<std::string> {
  static std::string convert(const dynamic& d) { return d.asString(); }
};

// std::pair
template <typename F, typename S>
struct DynamicConverter<std::pair<F, S>> {
  static std::pair<F, S> convert(const dynamic& d) {
    if (d.isArray() && d.size() == 2) {
      return std::make_pair(convertTo<F>(d[0]), convertTo<S>(d[1]));
    } else if (d.isObject() && d.size() == 1) {
      auto it = d.items().begin();
      return std::make_pair(convertTo<F>(it->first), convertTo<S>(it->second));
    } else {
      throw_exception<TypeError>("array (size 2) or object (size 1)", d.type());
    }
  }
};

// optionals and other pointer-like types.
template <typename T>
  requires dynamicconverter_detail::dyn_optional<T>
struct DynamicConverter<T, void> {
  static T convert(const dynamic& d) {
    if (d.isNull()) {
      return {};
    }
    return DynamicConverter<typename T::value_type>::convert(d);
  }
};

// containers
template <typename C>
  requires dynamicconverter_detail::dyn_container<C>
struct DynamicConverter<C, void> {
  static C convert(const dynamic& d) {
    auto elements =
        dynamicconverter_detail::convertElements<typename C::value_type>(d);
    return C(
        std::make_move_iterator(elements.begin()),
        std::make_move_iterator(elements.end()));
  }
};

///////////////////////////////////////////////////////////////////////////////
// DynamicConstructor specializations

/**
 * Each specialization of DynamicConstructor has the function
 *     'static dynamic construct(const C&);'
 */

// default
template <typename C, typename Enable = void>
struct DynamicConstructor {
  static dynamic construct(const C& x) { return dynamic(x); }
};

// identity
template <>
struct DynamicConstructor<dynamic, void> {
  static dynamic construct(const dynamic& x) { return x; }
};

// enums
template <typename C>
  requires std::is_enum_v<C>
struct DynamicConstructor<C, void> {
  static dynamic construct(const C& x) { return dynamic(to_underlying(x)); }
};

// maps
template <typename C>
  requires dynamicconverter_detail::dyn_map<C>
struct DynamicConstructor<C, void> {
  static dynamic construct(const C& x) {
    dynamic d = dynamic::object;
    for (const auto& pair : x) {
      d.insert(toDynamic(pair.first), toDynamic(pair.second));
    }
    return d;
  }
};

// other ranges
template <typename C>
  requires dynamicconverter_detail::dyn_array<C>
struct DynamicConstructor<C, void> {
  static dynamic construct(const C& x) {
    dynamic d = dynamic::array;
    for (const auto& item : x) {
      d.push_back(toDynamic(item));
    }
    return d;
  }
};

// pair
template <typename A, typename B>
struct DynamicConstructor<std::pair<A, B>, void> {
  static dynamic construct(const std::pair<A, B>& x) {
    dynamic d = dynamic::array;
    d.push_back(toDynamic(x.first));
    d.push_back(toDynamic(x.second));
    return d;
  }
};

// optionals and other pointer-like types.
template <typename T>
  requires dynamicconverter_detail::dyn_like_pointer<T>
struct DynamicConstructor<T, void> {
  static dynamic construct(const T& x) { return x ? toDynamic(*x) : dynamic(); }
};

// vector<bool>
template <>
struct DynamicConstructor<std::vector<bool>, void> {
  static dynamic construct(const std::vector<bool>& x) {
    dynamic d = dynamic::array;
    // Intentionally specifying the type as bool here.
    // std::vector<bool>'s iterators return a proxy which is a prvalue
    // and hence cannot bind to an lvalue reference such as auto&
    for (bool item : x) {
      d.push_back(toDynamic(item));
    }
    return d;
  }
};

///////////////////////////////////////////////////////////////////////////////
// implementation

template <typename T>
T convertTo(const dynamic& d) {
  return DynamicConverter<typename std::remove_cv<T>::type>::convert(d);
}

template <typename T>
dynamic toDynamic(const T& x) {
  return DynamicConstructor<typename std::remove_cv<T>::type>::construct(x);
}

} // namespace folly
