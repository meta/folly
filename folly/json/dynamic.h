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

//
// Docs: https://fburl.com/fbcref_dynamic
// 1-minute video primer: https://www.youtube.com/watch?v=3XubaLCDYOM
//

/**
 * dynamic is a runtime dynamically typed value.  It holds types from a specific
 * predetermined set of types: int, double, bool, nullptr_t, string, array,
 * map. In particular, it can be used as a convenient in-memory representation
 * for complete JSON objects.
 *
 * In general, dynamic can be used as if it were the type it represents
 * (although in some cases with a slightly less complete interface than the raw
 * type). If there is a runtime type mismatch, then dynamic will throw a
 * TypeError.
 *
 * See folly/json.h for serialization and deserialization functions for JSON.
 *
 * Additional documentation is in
 * https://github.com/facebook/folly/blob/main/folly/docs/Dynamic.md
 *
 * @refcode folly/docs/examples/folly/dynamic.cpp
 * @struct folly::dynamic
 */
/*
 * Some examples:
 *
 *   dynamic twelve = 12;
 *   dynamic str = "string";
 *   dynamic map = dynamic::object;
 *   map[str] = twelve;
 *   map[str + "another_str"] = dynamic::array("array", "of", 4, "elements");
 *   map.insert("null_element", nullptr);
 *   ++map[str];
 *   assert(map[str] == 13);
 *
 *   // Building a complex object with a sub array inline:
 *   dynamic d = dynamic::object
 *     ("key", "value")
 *     ("key2", dynamic::array("a", "array"))
 *     ;
 */

#pragma once

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <folly/CppAttributes.h>
#include <folly/Expected.h>
#include <folly/Range.h>
#include <folly/Traits.h>
#include <folly/container/Access.h>
#include <folly/container/F14Map.h>
#include <folly/json_pointer.h>

namespace folly {

//////////////////////////////////////////////////////////////////////

struct const_dynamic_view;
struct dynamic;
struct dynamic_view;
struct TypeError;

//////////////////////////////////////////////////////////////////////

namespace dynamic_detail {
template <typename T>
using detect_construct_string = decltype(std::string(
    FOLLY_DECLVAL(T const&).data(), FOLLY_DECLVAL(T const&).size()));
}

struct dynamic {
  enum Type {
    NULLT,
    ARRAY,
    BOOL,
    DOUBLE,
    INT64,
    OBJECT,
    STRING,
  };
  template <class T, class Enable = void>
  struct NumericTypeHelper;

  /*
   * We support direct iteration of arrays, and indirect iteration of objects.
   * See begin(), end(), keys(), values(), and items() for more.
   *
   * Array iterators dereference as the elements in the array.
   * Object key iterators dereference as the keys in the object.
   * Object value iterators dereference as the values in the object.
   * Object item iterators dereference as pairs of (key, value).
   */
 private:
  using Array = std::vector<dynamic>;

  /*
   * Violating spec, std::vector<bool>::const_reference is not bool in libcpp:
   * http://howardhinnant.github.io/onvectorbool.html
   *
   * This is used to add a public ctor which is only enabled under libcpp taking
   * std::vector<bool>::const_reference without using the preprocessor.
   */
  struct VectorBoolConstRefFake : std::false_type {};
  using VectorBoolConstRefCtorType = std::conditional_t<
      std::is_same<std::vector<bool>::const_reference, bool>::value,
      VectorBoolConstRefFake,
      std::vector<bool>::const_reference>;

 public:
  using iterator = Array::iterator;
  using const_iterator = Array::const_iterator;
  using value_type = dynamic;

  struct const_key_iterator;
  struct const_value_iterator;
  struct const_item_iterator;

  struct value_iterator;
  struct item_iterator;

  /*
   * Creation routines for making dynamic objects and arrays.  Objects
   * are maps from key to value (so named due to json-related origins
   * here).
   *
   * Example:
   *
   *   // Make a fairly complex dynamic:
   *   dynamic d = dynamic::object("key", "value1")
   *                              ("key2", dynamic::array("value",
   *                                                      "with",
   *                                                      4,
   *                                                      "words"));
   *
   *   // Build an object in a few steps:
   *   dynamic d = dynamic::object;
   *   d["key"] = 12;
   *   d["something_else"] = dynamic::array(1, 2, 3, nullptr);
   */
 private:
  struct EmptyArrayTag {};
  struct ObjectMaker;

 public:
  /**
   * @brief Used with the array-range ctor.
   */
  struct array_range_construct_t {
   private: // forbid implicit construction with {}
    friend dynamic;
    constexpr array_range_construct_t() = default;
  };
  static inline constexpr array_range_construct_t array_range_construct{};

  /**
   * Do not use.
   *
   * @methodset Array
   */
  static void array(EmptyArrayTag);

  /**
   * @brief Construct a dynamic array.
   *
   * Special syntax because `dynamic d = { ... }` dispatches to the copy
   * constructor.
   * See D3013423 and
   * [DR95](http://www.open-std.org/jtc1/sc22/wg21/docs/cwg_defects.html#1467).
   *
   * @refcode folly/docs/examples/folly/dynamic/array.cpp
   * @methodset Array
   */
  template <class... Args>
  static dynamic array(Args&&... args);

  /**
   * Construct a dynamic object.
   *
   * @refcode folly/docs/examples/folly/dynamic/object.cpp
   * @methodset Object
   */
  static ObjectMaker object();
  static ObjectMaker object(dynamic, dynamic);

  /**
   * Default constructor, initializes with nullptr.
   */
  dynamic();

  /*
   * String compatibility constructors.
   */
  /// Initializes as an empty string.
  /* implicit */ dynamic(std::nullptr_t);
  /// Initializes with strcpy.
  /* implicit */ dynamic(char const* s);
  /// Initializes as a string.
  /* implicit */ dynamic(std::string s);
  /// Initializes as a string.
  template <
      typename Stringish,
      typename = std::enable_if_t<
          is_detected_v<dynamic_detail::detect_construct_string, Stringish>>>
  /* implicit */ dynamic(Stringish&& s);

  /*
   * This is part of the plumbing for array() and object(), above.
   * Used to create a new array or object dynamic.
   */
  /// Plumbing for array() construction.
  /* implicit */ dynamic(void (*)(EmptyArrayTag));
  /// Plumbing for object() construction.
  /* implicit */ dynamic(ObjectMaker (*)());
  /// Plumbing for object() construction.
  /* implicit */ dynamic(ObjectMaker const&) = delete;
  /// Plumbing for object() construction.
  /* implicit */ dynamic(ObjectMaker&&);

  /**
   * Constructor for integral and float types.
   * Other types are SFINAEd out with NumericTypeHelper.
   */
  template <class T, class NumericType = typename NumericTypeHelper<T>::type>
  /* implicit */ dynamic(T t);

  /**
   * Special handling for vector<bool>.
   *
   * If v is vector<bool>, v[idx] is a proxy object implicitly convertible to
   * bool. Calling a function f(dynamic) with f(v[idx]) would require a double
   * implicit conversion (reference -> bool -> dynamic) which is not allowed,
   * hence we explicitly accept the reference proxy.
   */
  /* implicit */ dynamic(std::vector<bool>::reference b);
  /// Special handling for vector<bool>::const_reference.
  /* implicit */ dynamic(VectorBoolConstRefCtorType b);

  /**
   * Create a dynamic that is an array of the values from the supplied
   * iterator range. Used to construct a dynamic of array type from a supplied
   * pair of iterators or from a range-like object. Four equivalent forms - see
   * examples.
   *
   * Examples:
   *
   *   auto arr1 = dynamic(
   *       dynamic::array_range_construct, rng.begin(), rng.end());
   *   auto arr2 = dynamic(dynamic::array_range_construct, rng);
   *   auto arr3 = dynamic::array_range(rng.begin(), rng.end());
   *   auto arr4 = dynamic::array_range(rng);
   */
  template <class Iterator>
  dynamic(array_range_construct_t, Iterator first, Iterator last);
  template <class Range>
  dynamic(array_range_construct_t tag, Range&& range)
      : dynamic(tag, access::begin(range), access::end(range)) {}
  template <class Iterator>
  static dynamic array_range(Iterator first, Iterator last) {
    return dynamic(array_range_construct, first, last);
  }
  template <class Range>
  static dynamic array_range(Range&& range) {
    return dynamic(array_range_construct, std::forward<Range>(range));
  }

  dynamic(dynamic const&);
  dynamic(dynamic&&) noexcept;
  ~dynamic() noexcept;

  /**
   * Deep equality comparison.  This will compare all the way down
   * an object or array, and is potentially expensive.
   *
   * NOTE: Implicit conversion will be done between ints and doubles, so numeric
   * equality will apply between those cases. Other dynamic value comparisons of
   * different types will always return false.
   */
  friend bool operator==(dynamic const& a, dynamic const& b);

  /*
   * For all types except object this returns the natural ordering on
   * those types.  For objects, we throw TypeError.
   *
   * NOTE: Implicit conversion will be done between ints and doubles, so numeric
   * ordering will apply between those cases. Other dynamic value comparisons of
   * different types will maintain consistent ordering within a binary run.
   */
  friend bool operator<(dynamic const& a, dynamic const& b);
  friend bool operator>(dynamic const& a, dynamic const& b) { return b < a; }
  friend bool operator<=(dynamic const& a, dynamic const& b) {
    return !(b < a);
  }
  friend bool operator>=(dynamic const& a, dynamic const& b) {
    return !(a < b);
  }

  /*
   * General operators.
   *
   * These throw TypeError when used with types or type combinations
   * that don't support them.
   *
   * These functions may also throw if you use 64-bit integers with
   * doubles when the integers are too big to fit in a double.
   */
  /// @methodset Op
  dynamic& operator+=(dynamic const&);
  /// @methodset Op
  dynamic& operator-=(dynamic const&);
  /// @methodset Op
  dynamic& operator*=(dynamic const&);
  /// @methodset Op
  dynamic& operator/=(dynamic const&);
  /// @methodset Op
  dynamic& operator%=(dynamic const&);
  /// @methodset Op
  dynamic& operator|=(dynamic const&);
  /// @methodset Op
  dynamic& operator&=(dynamic const&);
  /// @methodset Op
  dynamic& operator^=(dynamic const&);
  /// @methodset Op
  dynamic& operator++();
  /// @methodset Op
  dynamic& operator--();

  friend dynamic operator+(dynamic const& a, dynamic const& b) {
    return std::move(copy(a) += b);
  }
  friend dynamic operator-(dynamic const& a, dynamic const& b) {
    return std::move(copy(a) -= b);
  }
  friend dynamic operator*(dynamic const& a, dynamic const& b) {
    return std::move(copy(a) *= b);
  }
  friend dynamic operator/(dynamic const& a, dynamic const& b) {
    return std::move(copy(a) /= b);
  }
  friend dynamic operator%(dynamic const& a, dynamic const& b) {
    return std::move(copy(a) %= b);
  }
  friend dynamic operator|(dynamic const& a, dynamic const& b) {
    return std::move(copy(a) |= b);
  }
  friend dynamic operator&(dynamic const& a, dynamic const& b) {
    return std::move(copy(a) &= b);
  }
  friend dynamic operator^(dynamic const& a, dynamic const& b) {
    return std::move(copy(a) ^= b);
  }

  friend dynamic operator+(dynamic&& a, dynamic const& b) {
    return std::move(a += b);
  }

  /// @methodset Op
  dynamic operator++(int) {
    auto self = *this;
    return ++*this, self;
  }
  /// @methodset Op
  dynamic operator--(int) {
    auto self = *this;
    return --*this, self;
  }

  /**
   * Assignment from other dynamics.
   *
   * Because of the implicit conversion to dynamic from its potential types, you
   * can use this to change the type pretty intuitively.
   *
   * Basic guarantee only.
   */
  dynamic& operator=(dynamic const&);
  dynamic& operator=(dynamic&&) noexcept;

  /*
   * Minor performance optimization: allow assignment from cheap
   * primitive types without creating a temporary dynamic.
   */
  template <class T, class NumericType = typename NumericTypeHelper<T>::type>
  dynamic& operator=(T t);

  dynamic& operator=(std::nullptr_t);

  /*
   * For simple dynamics (not arrays or objects), this prints the
   * value to an std::ostream in the expected way.  Respects the
   * formatting manipulators that have been sent to the stream
   * already.
   *
   * If the dynamic holds an object or array, this prints them in a
   * format very similar to JSON.  (It will in fact actually be JSON
   * as long as the dynamic validly represents a JSON object---i.e. it
   * can't have non-string keys.)
   */
  friend std::ostream& operator<<(std::ostream&, dynamic const&);

  /**
   * @brief Type test.
   *
   * Returns true if this dynamic is of the specified type.
   *
   * @methodset Typing
   */
  bool isString() const;
  /// @copydoc isString
  bool isObject() const;
  /// @copydoc isString
  bool isBool() const;
  /// @copydoc isString
  bool isNull() const;
  /// @copydoc isString
  bool isArray() const;
  /// @copydoc isString
  bool isDouble() const;
  /// @copydoc isString
  bool isInt() const;

  /**
   * @copydoc isString
   *
   * @return `isInt() || isDouble()`.
   */
  bool isNumber() const;

  /**
   * The type of this dynamic.
   *
   * @methodset Typing
   */
  Type type() const;

  /**
   * The type of this dynamic as a printable string.
   *
   * @methodset Typing
   */
  const char* typeName() const;

  /**
   * Type conversion.
   *
   * Extract a value while trying to convert to the specified type.
   * Throws exceptions if we cannot convert from the real type to the
   * requested type.
   *
   * C++ will implicitly convert between bools, ints, and doubles; these
   * conversion functions also try to convert between arithmetic types and
   * strings. E.g. dynamic d = "12"; d.asDouble() -> 12.0.
   *
   * Note: you can only use this to access integral types or strings,
   * since arrays and objects are generally best dealt with as a
   * dynamic.
   *
   * @methodset Conversion
   */
  std::string asString() const;
  /// @copydoc asString
  double asDouble() const;
  /// @copydoc asString
  int64_t asInt() const;
  /// @copydoc asString
  bool asBool() const;

  /**
   * Type extraction.
   *
   * Extract the value stored in this dynamic without type conversion.
   *
   * These will throw a TypeError if the dynamic has a different type.
   *
   * @methodset Extraction
   */
  const std::string& getString() const& [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]];
  /// @copydoc getString
  double getDouble() const&;
  /// @copydoc getString
  int64_t getInt() const&;
  /// @copydoc getString
  bool getBool() const&;
  std::string& getString() & [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]];
  double& getDouble() &;
  int64_t& getInt() &;
  bool& getBool() &;
  std::string&& getString() &&;
  double getDouble() &&;
  int64_t getInt() &&;
  bool getBool() &&;

  /**
   * Get the c_str pointer.
   *
   * It is occasionally useful to access a string's internal pointer
   * directly, without the type conversion of `asString()`.
   *
   * These will throw a TypeError if the dynamic is not a string.
   *
   * @methodset Extraction
   */
  const char* c_str() const&;
  const char* c_str() && = delete;
  /// @copydoc c_str
  StringPiece stringPiece() const;

  /**
   * Tests for emptiness.
   *
   * Returns: true if this dynamic is null, an empty array, an empty
   * object, or an empty string.
   *
   * @methodset Container
   */
  bool empty() const;

  /**
   * Get size.
   *
   * If this is an array or an object, returns the number of elements
   * contained.  If it is a string, returns the length.  Otherwise
   * throws TypeError.
   *
   * @methodset Container
   */
  std::size_t size() const;

  /**
   * Array iteration.
   *
   * You can iterate over the values of the array.  Calling these on
   * non-arrays will throw a TypeError.
   *
   * @methodset Iteration
   */
  const_iterator begin() const;
  /// @copydoc begin
  const_iterator end() const;
  iterator begin();
  iterator end();

 private:
  /*
   * Helper object returned by keys(), values(), and items().
   */
  template <class T>
  struct IterableProxy;

  /*
   * Helper for heterogeneous lookup and mutation on objects: at(), find(),
   * count(), erase(), operator[]
   */
  template <typename K, typename T>
  using IfIsNonStringDynamicConvertible = std::enable_if_t<
      !std::is_convertible<K, StringPiece>::value &&
          std::is_convertible<K, dynamic>::value,
      T>;

  template <typename K, typename T>
  using IfNotIterator =
      std::enable_if_t<!std::is_convertible<K, iterator>::value, T>;

 public:
  /*
   * You can iterate over the keys, values, or items (std::pair of key and
   * value) in an object.  Calling these on non-objects will throw a TypeError.
   */
  /**
   * Get the keys of an object.
   *
   * Return an iterable interface for the object's keys.
   *
   * @methodset Iteration
   */
  IterableProxy<const_key_iterator> keys() const;
  /**
   * Get the values of an object.
   *
   * Return an iterable interface for the object's values, without their
   * associated key.
   *
   * @methodset Iteration
   */
  IterableProxy<const_value_iterator> values() const;
  /**
   * Get key-value items of an object.
   *
   * Return an iterable interface for the object's key-value pairs.
   * The type of this iterable is `(const dynamic&, dynamic&)`.
   *
   * @methodset Iteration
   */
  IterableProxy<const_item_iterator> items() const;
  IterableProxy<value_iterator> values();
  IterableProxy<item_iterator> items();

  /**
   * Find by key.
   *
   * AssociativeContainer-style find interface for objects.  Throws if
   * this is not an object.
   *
   * @return items().end() if the key is not present, or a
   * const_item_iterator pointing to the item.
   *
   * @methodset Object
   */
  template <typename K>
  IfIsNonStringDynamicConvertible<K, const_item_iterator> find(K&&) const;
  template <typename K>
  IfIsNonStringDynamicConvertible<K, item_iterator> find(K&&);
  const_item_iterator find(StringPiece) const;
  item_iterator find(StringPiece);

  /**
   * Count by key.
   *
   * If this is an object, returns whether it contains a field with
   * the given name.
   * If this is an array, returns the number of elements matching
   * the supplied value.
   * Otherwise throws TypeError.
   *
   * @methodset Object
   */
  template <typename K>
  IfIsNonStringDynamicConvertible<K, std::size_t> count(K&&) const;
  std::size_t count(StringPiece) const;

  /**
   * Check if key exists.
   *
   * If this is an object, returns whether it contains a field with
   * the given name.
   * If this is an array, returns whether it contains a value matching
   * the supplied value.
   * Otherwise throws TypeError.
   */
  template <typename K>
  IfIsNonStringDynamicConvertible<K, bool> contains(K&&) const;
  bool contains(StringPiece) const;

 private:
  dynamic const& atImpl(dynamic const&) const&;

 public:
  /**
   * Access sub-field or index.
   *
   * For objects or arrays, provides access to sub-fields by index or
   * field name.
   *
   * Using these with dynamic objects that are not arrays or objects
   * will throw a TypeError.  Using an index that is out of range or
   * object-element that's not present throws std::out_of_range.
   *
   * @methodset Element access
   */
  template <typename K>
  IfIsNonStringDynamicConvertible<K, dynamic const&> at(
      K&&) const& [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]];
  template <typename K>
  IfIsNonStringDynamicConvertible<K, dynamic&> at(
      K&&) & [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]];
  template <typename K>
  IfIsNonStringDynamicConvertible<K, dynamic&&> at(K&&) &&;

  dynamic const& at(StringPiece) const& [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]];
  dynamic& at(StringPiece) & [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]];
  dynamic&& at(StringPiece) &&;

  /*
   * Locate element using JSON pointer, per RFC 6901
   */

  enum class json_pointer_resolution_error_code : uint8_t {
    other = 0,
    // key not found in object
    key_not_found,
    // array index out of bounds
    index_out_of_bounds,
    // special index "-" requesting append
    append_requested,
    // indexes in arrays must be numeric
    index_not_numeric,
    // indexes in arrays should not have leading zero
    index_has_leading_zero,
    // element not subscribable, i.e. neither object or array
    element_not_object_or_array,
    // hit document boundary, but pointer not exhausted yet
    json_pointer_out_of_bounds,
  };

  template <typename Dynamic>
  struct json_pointer_resolution_error {
    // error code encountered while resolving JSON pointer
    json_pointer_resolution_error_code error_code{};
    // index of the JSON pointer's token that caused the error
    size_t index{0};
    // Last correctly resolved element in object. You can use it,
    // for example, to add last element to the array
    Dynamic* context{nullptr};
  };

  template <typename Dynamic>
  struct json_pointer_resolved_value {
    // parent element of the value in dynamic, if exist
    Dynamic* parent{nullptr};
    // pointer to the value itself
    Dynamic* value{nullptr};
    // if parent isObject, this is the key in object to get value
    StringPiece parent_key;
    // if parent isArray, this is the index in array to get value
    size_t parent_index{0};
  };

  // clang-format off
  template <typename Dynamic>
  using resolved_json_pointer = Expected<
      json_pointer_resolved_value<Dynamic>,
      json_pointer_resolution_error<Dynamic>>;

  /**
   * Get JSON Pointer.
   *
   * See [Relative JSON Pointers RFC 6901](https://json-schema.org/draft/2019-09/relative-json-pointer.html)
   *
   * @methodset Element access
   */
  resolved_json_pointer<dynamic const>
  try_get_ptr(json_pointer const&) const&;
  resolved_json_pointer<dynamic>
  try_get_ptr(json_pointer const&) &;
  resolved_json_pointer<dynamic const>
  try_get_ptr(json_pointer const&) const&& = delete;
  resolved_json_pointer<dynamic const>
  try_get_ptr(json_pointer const&) && = delete;
  // clang-format on

  /**
   * Nullable access.
   *
   * Like `at` (for objects and arrays) and `try_get_ptr` (for json_pointer
   * lookup), but returns nullptr if the element cannot be found instead of
   * throwing a TypeError.
   *
   * For the JSON Pointer overloads, throws if pointer does not match the shape
   * of the document, e.g. uses string to index in array.
   *
   * @methodset Element access
   */
  const dynamic* get_ptr(json_pointer const&) const&;
  dynamic* get_ptr(json_pointer const&) &;
  const dynamic* get_ptr(json_pointer const&) const&& = delete;
  dynamic* get_ptr(json_pointer const&) && = delete;

 private:
  const dynamic* get_ptrImpl(dynamic const&) const&;

 public:
  /*
   * Like 'at', above, except it returns either a pointer to the contained
   * object or nullptr if it wasn't found. This allows a key to be tested for
   * containment and retrieved in one operation. Example:
   *
   *   if (auto* found = d.get_ptr(key))
   *     // use *found;
   *
   * Using these with dynamic objects that are not arrays or objects
   * will throw a TypeError.
   */
  template <typename K>
  IfIsNonStringDynamicConvertible<K, const dynamic*> get_ptr(K&&) const&;
  template <typename K>
  IfIsNonStringDynamicConvertible<K, dynamic*> get_ptr(K&&) &;
  template <typename K>
  IfIsNonStringDynamicConvertible<K, dynamic*> get_ptr(K&&) && = delete;

  const dynamic* get_ptr(StringPiece) const&;
  dynamic* get_ptr(StringPiece) &;
  dynamic* get_ptr(StringPiece) && = delete;

  /**
   * Element lookup.
   *
   * This works for access to both objects and arrays.
   *
   * In the case of an array, the index must be an integer, and this
   * will throw std::out_of_range if it is less than zero or greater
   * than size().
   *
   * In the case of an object, the non-const overload inserts a null
   * value if the key isn't present.  The const overload will throw
   * std::out_of_range if the key is not present.
   *
   * These functions do not invalidate iterators except when a null value
   * is inserted into an object as described above.
   *
   * @methodset Element access
   */
  template <typename K>
  IfIsNonStringDynamicConvertible<K, dynamic&> operator[](
      K&&) & [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]];
  template <typename K>
  IfIsNonStringDynamicConvertible<K, dynamic const&> operator[](
      K&&) const& [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]];
  template <typename K>
  IfIsNonStringDynamicConvertible<K, dynamic&&> operator[](K&&) &&;

  dynamic& operator[](StringPiece) & [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]];
  dynamic const& operator[](
      StringPiece) const& [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]];
  dynamic&& operator[](StringPiece) &&;

  /**
   * Defaulted lookup.
   *
   * getDefault will return the value associated with the supplied key, the
   * supplied default otherwise.
   *
   * Only defined for objects, throws TypeError otherwise.
   *
   * @methodset Object
   */
  template <typename K>
  IfIsNonStringDynamicConvertible<K, dynamic> getDefault(
      K&& k, const dynamic& v = dynamic::object) const&;
  template <typename K>
  IfIsNonStringDynamicConvertible<K, dynamic> getDefault(
      K&& k, dynamic&& v) const&;
  template <typename K>
  IfIsNonStringDynamicConvertible<K, dynamic> getDefault(
      K&& k, const dynamic& v = dynamic::object) &&;
  template <typename K>
  IfIsNonStringDynamicConvertible<K, dynamic> getDefault(K&& k, dynamic&& v) &&;

  dynamic getDefault(StringPiece k, const dynamic& v = dynamic::object) const&;
  dynamic getDefault(StringPiece k, dynamic&& v) const&;
  dynamic getDefault(StringPiece k, const dynamic& v = dynamic::object) &&;
  dynamic getDefault(StringPiece k, dynamic&& v) &&;

  /**
   * Maybe set.
   *
   * setDefault will set the key to the supplied default if it is not yet set,
   * otherwise leaving it. setDefault returns a reference to the existing value
   * if present, the new value otherwise.
   *
   * Only defined for objects, throws TypeError otherwise.
   *
   * @methodset Object
   */
  template <typename K, typename V>
  IfIsNonStringDynamicConvertible<K, dynamic&> setDefault(K&& k, V&& v);
  template <typename V>
  dynamic& setDefault(StringPiece k, V&& v);
  // MSVC 2015 Update 3 needs these extra overloads because if V were a
  // defaulted template parameter, it causes MSVC to consider v an rvalue
  // reference rather than a universal reference, resulting in it not being
  // able to find the correct overload to construct a dynamic with.
  template <typename K>
  IfIsNonStringDynamicConvertible<K, dynamic&> setDefault(K&& k, dynamic&& v);
  template <typename K>
  IfIsNonStringDynamicConvertible<K, dynamic&> setDefault(
      K&& k, const dynamic& v = dynamic::object);

  dynamic& setDefault(StringPiece k, dynamic&& v);
  dynamic& setDefault(StringPiece k, const dynamic& v = dynamic::object);

  /**
   * Change size.
   *
   * Resizes an array so it has at n elements, using the supplied
   * default to fill new elements.  Throws TypeError if this dynamic
   * is not an array.
   *
   * May invalidate iterators.
   *
   * Post: size() == n
   *
   * @methodset Array
   */
  void resize(std::size_t sz, dynamic const& = nullptr);

  /**
   * Pre-allocate size.
   *
   * If this is an array, an object, or a string, reserves the requested
   * capacity in the underlying container.  Otherwise throws TypeError.
   *
   * May invalidate iterators, and does not give any additional guarantees on
   * iterator invalidation on subsequent insertions; the only purpose is for
   * optimization.
   *
   * @methodset Container
   */
  void reserve(std::size_t capacity);

  /**
   * @brief Overwriting insertion.
   *
   * Inserts the supplied key-value pair to an object, or throws if
   * it's not an object. If the key already exists, insert will overwrite the
   * value, i.e., similar to insert_or_assign.
   *
   * Invalidates iterators.
   *
   * @methodset Container
   */
  template <class K, class V>
  IfNotIterator<K, void> insert(K&&, V&& val);

  /**
   * Overwriting emplacement.
   *
   * Inserts an element into an object constructed in-place with the given args
   * if there is no existing element with the key, or throws if it's not an
   * object. Returns a pair consisting of an iterator to the inserted element,
   * or the already existing element if no insertion happened, and a bool
   * denoting whether the insertion took place.
   *
   * Invalidates iterators.
   *
   * @methodset Object
   */
  template <class... Args>
  std::pair<item_iterator, bool> emplace(Args&&... args);

  /**
   * Non-overwriting emplacement.
   *
   * Inserts an element into an object with the given key and value constructed
   * in-place with the given args if there is no existing element with the key,
   * or throws if it's not an object. Returns a pair consisting of an iterator
   * to the inserted element, or the already existing element if no insertion
   * happened, and a bool denoting whether the insertion took place.
   *
   * Invalidates iterators.
   *
   * @methodset Object
   */
  template <class K, class... Args>
  std::pair<item_iterator, bool> try_emplace(K&& key, Args&&... args);

  /**
   * Inserts the supplied value into array, or throw if not array.
   * Shifts existing values in the array to the right.
   *
   * Invalidates iterators.
   */
  template <class T>
  iterator insert(const_iterator pos, T&& value);

  /**
   * Inserts elements from range [first, last) before pos into an array.
   * Throws if the type is not an array.
   *
   * Invalidates iterators.
   */
  template <class InputIt>
  iterator insert(const_iterator pos, InputIt first, InputIt last);

  /**
   * Merge objects.
   *
   * Merge two folly dynamic objects.
   * The "update" and "update_missing" functions extend the object by
   *  inserting the key/value pairs of mergeObj into the current object.
   *  For update, if key is duplicated between the two objects, it
   *  will overwrite with the value of the object being inserted (mergeObj).
   *  For "update_missing", it will prefer the value in the original object
   *
   * The "merge" function creates a new object consisting of the key/value
   * pairs of both mergeObj1 and mergeObj2
   * If the key is duplicated between the two objects,
   *  it will prefer value in the second object (mergeObj2)
   *
   * @methodset Object
   */
  void update(const dynamic& mergeObj);
  /// @copydoc update
  void update_missing(const dynamic& mergeObj1);
  /// @copydoc update
  static dynamic merge(const dynamic& mergeObj1, const dynamic& mergeObj2);

  /**
   * Merge JSON Patch.
   *
   * Implement recursive version of RFC7386: JSON merge patch. This modifies
   * the current object.
   *
   * @methodset Object
   */
  void merge_patch(const dynamic& patch);

  /**
   * Compute patch.
   *
   * Computes JSON merge patch (RFC7386) needed to mutate from source to target
   *
   * @methodset Object
   */
  static dynamic merge_diff(const dynamic& source, const dynamic& target);

  /**
   * @brief Erases elements
   *
   * Erase an element from a dynamic object, by key.
   *
   * Invalidates iterators to the element being erased.
   *
   * Returns the number of elements erased (i.e. 1 or 0).
   *
   * @methodset Container
   */
  template <typename K>
  IfIsNonStringDynamicConvertible<K, std::size_t> erase(K&&);

  std::size_t erase(StringPiece);

  /**
   * @brief Erases elements
   *
   * Erase an element from a dynamic object or array, using an
   * iterator or an iterator range.
   *
   * In arrays, invalidates iterators to elements after the element
   * being erased.  In objects, invalidates iterators to the elements
   * being erased.
   *
   * Returns a new iterator to the first element beyond any elements
   * removed, or end() if there are none.  (The iteration order does
   * not change.)
   *
   * @methodset Container
   */
  iterator erase(const_iterator it);
  iterator erase(const_iterator first, const_iterator last);

  const_key_iterator erase(const_key_iterator it);
  const_key_iterator erase(const_key_iterator first, const_key_iterator last);

  value_iterator erase(const_value_iterator it);
  value_iterator erase(const_value_iterator first, const_value_iterator last);

  item_iterator erase(const_item_iterator it);
  item_iterator erase(const_item_iterator first, const_item_iterator last);

  /**
   * Erase one object item and move its key and value into a callback.
   *
   * Use this when transforming an object by repeatedly removing entries:
   *
   *   for (auto it = obj.items().begin(); it != obj.items().end();) {
   *     it = obj.eraseInto(it, [&](dynamic&& key, dynamic&& value) {
   *       out.insert(std::move(key), std::move(value));
   *     });
   *   }
   *
   * Invalidates iterators to the element being erased.
   *
   * Returns a new iterator to the first element beyond the removed item, or
   * end() if there is none. The callback is invoked as
   * beforeDestroy(dynamic&& key, dynamic&& value).
   *
   * The entry is extracted before the callback runs. eraseInto makes the next
   * iterator, removes the item from the backing table, and only then exposes a
   * mutable key and value. This avoids mutating a resident table key through
   * items().
   *
   * @methodset Object
   */
  template <typename BeforeDestroy>
  item_iterator eraseInto(
      const_item_iterator it, BeforeDestroy&& beforeDestroy);

  /**
   * Append elements to an array.
   *
   * If this is not an array, throws TypeError.
   *
   * Invalidates iterators.
   *
   * @methodset Array
   */
  void push_back(dynamic const&);
  void push_back(dynamic&&);

  /**
   * Remove an element from the back of an array.
   *
   * If this is not an array, throws TypeError.
   *
   * Does not invalidate iterators.
   *
   * @methodset Array
   */
  void pop_back();

  /**
   * Return reference to the last element in an array.
   *
   * If this is not an array, throws TypeError.
   *
   * @methodset Array
   */
  const dynamic& back() const;

  /**
   * Hash self.
   *
   * Get a hash code.  This function is called by a std::hash<>
   * specialization.
   *
   * Note: an int64_t and double will both produce the same hash if they are
   * numerically equal before rounding. So the int64_t 2 will have the same hash
   * as the double 2.0. But no double will intentionally hash to the hash of a
   * value that only when rounded will compare as equal. E.g. No double will
   * intentionally hash to the hash of INT64_MAX (2^63 - 1) given that a double
   * cannot represent this value.
   */
  std::size_t hash() const;

 private:
  friend struct const_dynamic_view;
  friend struct dynamic_view;
  friend struct TypeError;
  struct ObjectImpl;
  template <class T>
  struct TypeInfo;
  template <class T>
  struct CompareOp;
  template <class T>
  struct GetAddrImpl;
  template <class T>
  struct PrintImpl;

  explicit dynamic(Array&& r);

  template <class T>
  T const& get() const;
  template <class T>
  T& get();

  template <class T>
  T* get_nothrow() & noexcept;
  template <class T>
  T const* get_nothrow() const& noexcept;
  template <class T>
  T* get_nothrow() && noexcept = delete;
  template <class T>
  T* getAddress() noexcept;
  template <class T>
  T const* getAddress() const noexcept;

  template <class T>
  T asImpl() const;

  static char const* typeName(Type);
  // NOTE: like ~dynamic, destroy() leaves type_ and u_ in an invalid state.
  void destroy() noexcept;
  void print(std::ostream&) const;
  void print_as_pseudo_json(std::ostream&) const; // see json.cpp

 private:
  Type type_;
  union Data {
    explicit Data() : nul(nullptr) {}
    ~Data() {}

    std::nullptr_t nul;
    Array array;
    bool boolean;
    double doubl;
    int64_t integer;
    std::string string;

    /*
     * Objects are placement new'd here.  We have to use a char buffer
     * because we don't know the type here (F14NodeMap<> with
     * dynamic would be parameterizing a std:: template with an
     * incomplete type right now).  (Note that in contrast we know it
     * is ok to do this with fbvector because we own it.)
     */
    aligned_storage_for_t<F14NodeMap<int, int>> objectBuffer;
  } u_;
};

/**
 * Like erase(vector), since C++20.
 */
template <typename Val>
size_t erase(folly::dynamic& dyn, Val const& val);

/**
 * Like erase_if(vector), erase_if(unordered_map), since C++20.
 *
 * If the predicate `pred` is formally invocable with `dynamic const&` then it
 * must type-check when so invoked. If the predicate is formally invocable with
 * `std::pair<dynamic const, dynamic> const&` then it must type-check when so
 * invoked. A lambda taking `const auto&` may be at risk of violating this rule,
 * so consider a lambda taking one of the two types explicitly, sans deduction.
 */
template <typename Pred>
size_t erase_if(folly::dynamic& dyn, Pred pred);

//////////////////////////////////////////////////////////////////////

/**
 * This is a helper class for traversing an instance of dynamic and accessing
 * the values within without risking throwing an exception. The primary use case
 * is to help write cleaner code when using dynamic instances without strict
 * schemas - eg. where keys may be missing, or present but with null values,
 * when expecting non-null values.
 *
 * Some examples:
 *
 *   dynamic twelve = 12;
 *   dynamic str = "string";
 *   dynamic map = dynamic::object("str", str)("twelve", 12);
 *
 *   dynamic_view view{map};
 *   assert(view.descend("str").string_or("bad") == "string");
 *   assert(view.descend("twelve").int_or(-1) == 12);
 *   assert(view.descend("zzz").string_or("aaa") == "aaa");
 *
 *   dynamic wrapper = dynamic::object("child", map);
 *   dynamic_view wrapper_view{wrapper};
 *
 *   assert(wrapper_view.descend("child", "str").string_or("bad") == "string");
 *   assert(wrapper_view.descend("wrong", 0, "huh").value_or(nullptr).isNull());
 */
struct const_dynamic_view {
  // Empty view.
  const_dynamic_view() noexcept = default;

  // Basic view constructor. Creates a view of the referenced dynamic.
  /* implicit */ const_dynamic_view(dynamic const& d) noexcept;

  const_dynamic_view(const_dynamic_view const&) noexcept = default;
  const_dynamic_view& operator=(const_dynamic_view const&) noexcept = default;

  // Allow conversion from mutable to immutable view.
  /* implicit */ const_dynamic_view(dynamic_view& view) noexcept;
  /* implicit */ const_dynamic_view& operator=(dynamic_view& view) noexcept;

  // Never view a temporary.
  explicit const_dynamic_view(dynamic&&) = delete;

  // Returns true if this view is backed by a valid dynamic, false otherwise.
  explicit operator bool() const noexcept;

  // Returns true if this view is not backed by a dynamic, false otherwise.
  bool empty() const noexcept;

  // Resets the view to a default constructed state not backed by any dynamic.
  void reset() noexcept;

  // Traverse a dynamic by repeatedly applying operator[].
  // If all keys are valid, then the returned view will be backed by the
  // accessed dynamic, otherwise it will be empty.
  template <typename Key, typename... Keys>
  const_dynamic_view descend(
      Key const& key, Keys const&... keys) const noexcept;

  // Untyped accessor. Returns a copy of the viewed dynamic, or the default
  // value given if this view is empty, or a null dynamic otherwise.
  dynamic value_or(dynamic&& val = nullptr) const;

  // The following accessors provide a read-only exception-safe API for
  // accessing the underlying viewed dynamic. Unlike the main dynamic APIs,
  // these follow a stricter contract, which also requires a caller-provided
  // default argument.
  //  - TypeError will not be thrown. primitive accessors further are marked
  //    noexcept.
  //  - No type conversions are performed. If the viewed dynamic does not match
  //    the requested type, the default argument is returned instead.
  //  - If the view is empty, the default argument is returned instead.
  std::string string_or(char const* val) const;
  std::string string_or(std::string val) const;
  template <
      typename Stringish,
      typename = std::enable_if_t<
          is_detected_v<dynamic_detail::detect_construct_string, Stringish>>>
  std::string string_or(Stringish&& val) const;

  double double_or(double val) const noexcept;

  int64_t int_or(int64_t val) const noexcept;

  bool bool_or(bool val) const noexcept;

 protected:
  /* implicit */ const_dynamic_view(dynamic const* d) noexcept;

  template <typename Key1, typename Key2, typename... Keys>
  dynamic const* descend_(
      Key1 const& key1, Key2 const& key2, Keys const&... keys) const noexcept;
  template <typename Key>
  dynamic const* descend_(Key const& key) const noexcept;
  template <typename Key>
  dynamic::IfIsNonStringDynamicConvertible<Key, dynamic const*>
  descend_unchecked_(Key const& key) const noexcept;
  dynamic const* descend_unchecked_(StringPiece key) const noexcept;

  dynamic const* d_ = nullptr;

  // Internal helper method for accessing a value by a type.
  template <typename T, typename... Args>
  T get_copy(Args&&... args) const;
};

struct dynamic_view : public const_dynamic_view {
  // Empty view.
  dynamic_view() noexcept = default;

  // dynamic_view can be used to view non-const dynamics only.
  /* implicit */ dynamic_view(dynamic& d) noexcept;

  dynamic_view(dynamic_view const&) noexcept = default;
  dynamic_view& operator=(dynamic_view const&) noexcept = default;

  // dynamic_view can not view const dynamics.
  explicit dynamic_view(dynamic const&) = delete;
  // dynamic_view can not be initialized from a const_dynamic_view
  explicit dynamic_view(const_dynamic_view const&) = delete;

  // Like const_dynamic_view, but returns a dynamic_view.
  template <typename Key, typename... Keys>
  dynamic_view descend(Key const& key, Keys const&... keys) const noexcept;

  // dynamic_view provides APIs which can mutably access the backed dynamic.
  // 'mutably access' in this case means extracting the viewed dynamic or
  // value to omit unnecessary copies. It does not mean writing through to
  // the backed dynamic - this is still just a view, not a mutator.

  // Moves the viewed dynamic into the returned value via std::move. If the view
  // is not backed by a dynamic, returns a provided default, or a null dynamic.
  // Postconditions for the backed dynamic are the same as for any dynamic that
  // is moved-from. this->empty() == false.
  dynamic move_value_or(dynamic&& val = nullptr) noexcept;

  // Specific optimization for strings which can allocate, unlike the other
  // scalar types. If the viewed dynamic is a string, the string value is
  // std::move'd to initialize a new instance which is returned.
  std::string move_string_or(std::string val) noexcept;
  std::string move_string_or(char const* val);
  template <
      typename Stringish,
      typename = std::enable_if_t<
          is_detected_v<dynamic_detail::detect_construct_string, Stringish>>>
  std::string move_string_or(Stringish&& val);

 private:
  template <typename T, typename... Args>
  T get_move(Args&&... args);
};

// A helper method which returns a contextually-correct dynamic_view for the
// given view. If passed a dynamic const&, returns a const_dynamic_view, and
// if passed a dynamic&, returns a dynamic_view.
inline auto make_dynamic_view(dynamic const& d) {
  return const_dynamic_view{d};
}

inline auto make_dynamic_view(dynamic& d) {
  return dynamic_view{d};
}

auto make_dynamic_view(dynamic&&) = delete;

//////////////////////////////////////////////////////////////////////

} // namespace folly

namespace std {

template <>
struct hash<::folly::dynamic> {
  using folly_is_avalanching = std::true_type;

  size_t operator()(::folly::dynamic const& d) const { return d.hash(); }
};

} // namespace std

#include <folly/json/dynamic-inl.h>
