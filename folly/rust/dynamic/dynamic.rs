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

#[cxx::bridge]
mod bridge {
    #[namespace = "folly"]
    unsafe extern "C++" {
        include!("folly/json/dynamic.h");
        type dynamic;

        // Renamed to avoid shadowing with UniquePtr is_null
        #[cxx_name = "isNull"]
        fn is_null_value(self: &dynamic) -> bool;
        #[cxx_name = "isBool"]
        fn is_bool(self: &dynamic) -> bool;
        #[cxx_name = "isInt"]
        fn is_int(self: &dynamic) -> bool;
        #[cxx_name = "isDouble"]
        fn is_double(self: &dynamic) -> bool;
        #[cxx_name = "isNumber"]
        fn is_number(self: &dynamic) -> bool;
        #[cxx_name = "isString"]
        fn is_string(self: &dynamic) -> bool;
        #[cxx_name = "isArray"]
        fn is_array(self: &dynamic) -> bool;
        #[cxx_name = "isObject"]
        fn is_object(self: &dynamic) -> bool;

        fn empty(self: &dynamic) -> Result<bool>;
        fn size(self: &dynamic) -> Result<usize>;

        #[cxx_name = "asBool"]
        fn as_bool(self: &dynamic) -> Result<bool>;
        #[cxx_name = "asInt"]
        fn as_int(self: &dynamic) -> Result<i64>;
        #[cxx_name = "asDouble"]
        fn as_double(self: &dynamic) -> Result<f64>;
        #[cxx_name = "stringPiece"]
        fn as_string_piece(self: &dynamic) -> Result<StringPiece>;
    }

    #[namespace = "folly"]
    extern "C++" {
        type StringPiece<'a> = string::StringPiece<'a>;
    }

    #[namespace = "facebook::folly_rust::dynamic"]
    unsafe extern "C++" {
        include!("folly/rust/dynamic/dynamic.h");

        fn new_dynamic_null() -> UniquePtr<dynamic>;
        fn new_dynamic_bool(value: bool) -> UniquePtr<dynamic>;
        fn new_dynamic_int(value: i64) -> UniquePtr<dynamic>;
        fn new_dynamic_double(value: f64) -> UniquePtr<dynamic>;
        fn new_dynamic_string_bytes(value: &[u8]) -> UniquePtr<dynamic>;
        fn new_dynamic_array() -> UniquePtr<dynamic>;
        fn new_dynamic_object() -> UniquePtr<dynamic>;

        fn get_string(d: &dynamic) -> Result<String>;
        fn get_borrowed_bytes<'d>(d: &'d dynamic) -> Result<&'d [u8]>;

        fn array_push_back(d: Pin<&mut dynamic>, value: UniquePtr<dynamic>) -> Result<()>;
        fn array_pop_back(d: Pin<&mut dynamic>) -> Result<()>;
        fn at(d: &dynamic, index: usize) -> Result<&dynamic>;
        fn at_mut(d: Pin<&mut dynamic>, index: usize) -> Result<Pin<&mut dynamic>>;
        fn array_set(d: Pin<&mut dynamic>, index: usize, value: UniquePtr<dynamic>) -> Result<()>;

        fn object_contains_bytes(d: &dynamic, key: &[u8]) -> Result<bool>;
        fn get_bytes<'val>(d: &'val dynamic, key: &[u8]) -> Result<&'val dynamic>;
        fn get_mut_bytes<'val>(
            d: Pin<&'val mut dynamic>,
            key: &[u8],
        ) -> Result<Pin<&'val mut dynamic>>;
        fn object_set_bytes(
            d: Pin<&mut dynamic>,
            key: &[u8],
            value: UniquePtr<dynamic>,
        ) -> Result<()>;
        fn object_erase_bytes(d: Pin<&mut dynamic>, key: &[u8]) -> Result<bool>;
        fn object_keys(d: &dynamic) -> Result<Vec<String>>;

        fn clone_dynamic(d: &dynamic) -> UniquePtr<dynamic>;

        fn to_json(d: &dynamic) -> String;
        fn to_pretty_json(d: &dynamic) -> String;
        fn parse_json_bytes(json: &[u8]) -> Result<UniquePtr<dynamic>>;

        fn set_null(d: Pin<&mut dynamic>);
        fn set_bool(d: Pin<&mut dynamic>, value: bool);
        fn set_int(d: Pin<&mut dynamic>, value: i64);
        fn set_double(d: Pin<&mut dynamic>, value: f64);
        fn set_string_bytes(d: Pin<&mut dynamic>, value: &[u8]);
        fn set_dynamic(d: Pin<&mut dynamic>, value: &dynamic);

        fn equals(lhs: &dynamic, rhs: &dynamic) -> bool;
    }
}

pub type Dynamic = bridge::dynamic;

use std::fmt;
use std::ops::Index;
use std::str;
use std::str::Utf8Error;

use cxx::UniquePtr;

/// Error type for the `as_str` method
#[derive(Debug)]
pub enum AsStrError {
    CxxException(cxx::Exception),
    Utf8Error(Utf8Error),
}

impl std::fmt::Display for AsStrError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            AsStrError::CxxException(e) => e.fmt(f),
            AsStrError::Utf8Error(e) => e.fmt(f),
        }
    }
}
impl std::error::Error for AsStrError {}

impl From<cxx::Exception> for AsStrError {
    fn from(e: cxx::Exception) -> Self {
        AsStrError::CxxException(e)
    }
}

impl From<Utf8Error> for AsStrError {
    fn from(e: Utf8Error) -> Self {
        AsStrError::Utf8Error(e)
    }
}

// Note that we can't implement IndexMut because we are unable to return
// a Pin<&mut Dynamic>, it would have to be an &mut Dynamic
impl Index<usize> for Dynamic {
    type Output = Dynamic;

    fn index(&self, index: usize) -> &Self::Output {
        bridge::at(self, index).unwrap()
    }
}

impl Index<&str> for Dynamic {
    type Output = Dynamic;

    fn index(&self, key: &str) -> &Self::Output {
        bridge::get_bytes(self, key.as_bytes()).unwrap()
    }
}

impl Dynamic {
    pub fn new_null() -> UniquePtr<Self> {
        bridge::new_dynamic_null()
    }

    pub fn new_bool(value: bool) -> UniquePtr<Self> {
        bridge::new_dynamic_bool(value)
    }

    pub fn new_int(value: i64) -> UniquePtr<Self> {
        bridge::new_dynamic_int(value)
    }

    pub fn new_double(value: f64) -> UniquePtr<Self> {
        bridge::new_dynamic_double(value)
    }

    pub fn new_string<'a, S: Into<bridge::StringPiece<'a>>>(value: S) -> UniquePtr<Self> {
        let value: bridge::StringPiece<'a> = value.into();
        bridge::new_dynamic_string_bytes(value.as_slice())
    }

    pub fn new_array() -> UniquePtr<Self> {
        bridge::new_dynamic_array()
    }

    pub fn new_object() -> UniquePtr<Self> {
        bridge::new_dynamic_object()
    }

    pub fn as_string(&self) -> Result<String, cxx::Exception> {
        bridge::get_string(self)
    }

    /// Borrow this string without allocating; validate its UTF-8 bytes.
    ///
    /// ```
    /// use folly_dynamic::Dynamic;
    /// let d = Dynamic::new_string("borrowed");
    /// assert_eq!(d.as_str().unwrap(), "borrowed");
    /// ```
    ///
    /// ```compile_fail
    /// use folly_dynamic::Dynamic;
    /// let borrowed = {
    ///     let d = Dynamic::new_string("borrowed");
    ///     d.as_str().unwrap()
    /// };
    /// assert_eq!(borrowed, "borrowed");
    /// ```
    ///
    /// ```compile_fail
    /// use folly_dynamic::{Dynamic, DynamicMut};
    /// let mut d = Dynamic::new_string("borrowed");
    /// let borrowed = d.as_str().unwrap();
    /// d.set_value("changed");
    /// assert_eq!(borrowed, "borrowed");
    /// ```
    pub fn as_str(&self) -> Result<&str, AsStrError> {
        Ok(str::from_utf8(bridge::get_borrowed_bytes(self)?)?)
    }

    /// Clone this dynamic value
    pub fn clone(&self) -> UniquePtr<Self> {
        bridge::clone_dynamic(self)
    }

    /// Check if an object contains a key
    pub fn contains<'a, K: Into<bridge::StringPiece<'a>>>(
        &self,
        key: K,
    ) -> Result<bool, cxx::Exception> {
        let key: bridge::StringPiece<'a> = key.into();
        bridge::object_contains_bytes(self, key.as_slice())
    }

    /// Get the keys of an object
    pub fn keys(&self) -> Result<Vec<String>, cxx::Exception> {
        bridge::object_keys(self)
    }

    /// Serialize to JSON string
    pub fn to_json(&self) -> String {
        bridge::to_json(self)
    }

    /// Serialize to pretty JSON string
    pub fn to_pretty_json(&self) -> String {
        bridge::to_pretty_json(self)
    }

    /// Parse JSON string into a Dynamic
    pub fn from_json<'a, S: Into<bridge::StringPiece<'a>>>(
        json: S,
    ) -> Result<UniquePtr<Self>, cxx::Exception> {
        let json: bridge::StringPiece<'a> = json.into();
        bridge::parse_json_bytes(json.as_slice())
    }

    pub fn get<'a, K: Into<bridge::StringPiece<'a>>>(
        &self,
        key: K,
    ) -> Result<&Self, cxx::Exception> {
        let key: bridge::StringPiece<'a> = key.into();
        bridge::get_bytes(self, key.as_slice())
    }

    pub fn at(&self, index: usize) -> Result<&Self, cxx::Exception> {
        bridge::at(self, index)
    }
}

/// Trait for types that can be converted into a dynamic value for setting
pub trait IntoDynamic {
    fn set_into(self, d: std::pin::Pin<&mut Dynamic>);

    /// Convert this value into a UniquePtr<Dynamic>
    fn into_dynamic(self) -> UniquePtr<Dynamic>;
}

impl IntoDynamic for () {
    fn set_into(self, d: std::pin::Pin<&mut Dynamic>) {
        bridge::set_null(d);
    }

    fn into_dynamic(self) -> UniquePtr<Dynamic> {
        Dynamic::new_null()
    }
}

impl IntoDynamic for bool {
    fn set_into(self, d: std::pin::Pin<&mut Dynamic>) {
        bridge::set_bool(d, self);
    }

    fn into_dynamic(self) -> UniquePtr<Dynamic> {
        Dynamic::new_bool(self)
    }
}

impl IntoDynamic for i64 {
    fn set_into(self, d: std::pin::Pin<&mut Dynamic>) {
        bridge::set_int(d, self);
    }

    fn into_dynamic(self) -> UniquePtr<Dynamic> {
        Dynamic::new_int(self)
    }
}

impl IntoDynamic for i32 {
    fn set_into(self, d: std::pin::Pin<&mut Dynamic>) {
        bridge::set_int(d, self as i64);
    }

    fn into_dynamic(self) -> UniquePtr<Dynamic> {
        Dynamic::new_int(self as i64)
    }
}

impl IntoDynamic for u32 {
    fn set_into(self, d: std::pin::Pin<&mut Dynamic>) {
        bridge::set_int(d, self as i64);
    }

    fn into_dynamic(self) -> UniquePtr<Dynamic> {
        Dynamic::new_int(self as i64)
    }
}

impl IntoDynamic for f64 {
    fn set_into(self, d: std::pin::Pin<&mut Dynamic>) {
        bridge::set_double(d, self);
    }

    fn into_dynamic(self) -> UniquePtr<Dynamic> {
        Dynamic::new_double(self)
    }
}

impl IntoDynamic for f32 {
    fn set_into(self, d: std::pin::Pin<&mut Dynamic>) {
        bridge::set_double(d, self as f64);
    }

    fn into_dynamic(self) -> UniquePtr<Dynamic> {
        Dynamic::new_double(self as f64)
    }
}

impl IntoDynamic for &str {
    fn set_into(self, d: std::pin::Pin<&mut Dynamic>) {
        bridge::set_string_bytes(d, self.as_bytes());
    }

    fn into_dynamic(self) -> UniquePtr<Dynamic> {
        Dynamic::new_string(self)
    }
}

impl IntoDynamic for String {
    fn set_into(self, d: std::pin::Pin<&mut Dynamic>) {
        bridge::set_string_bytes(d, self.as_bytes());
    }

    fn into_dynamic(self) -> UniquePtr<Dynamic> {
        Dynamic::new_string(self.as_str())
    }
}

impl IntoDynamic for &String {
    fn set_into(self, d: std::pin::Pin<&mut Dynamic>) {
        bridge::set_string_bytes(d, self.as_bytes());
    }

    fn into_dynamic(self) -> UniquePtr<Dynamic> {
        Dynamic::new_string(self.as_str())
    }
}

impl IntoDynamic for &Dynamic {
    fn set_into(self, d: std::pin::Pin<&mut Dynamic>) {
        bridge::set_dynamic(d, self);
    }

    fn into_dynamic(self) -> UniquePtr<Dynamic> {
        bridge::clone_dynamic(self)
    }
}

impl IntoDynamic for UniquePtr<Dynamic> {
    fn set_into(self, d: std::pin::Pin<&mut Dynamic>) {
        bridge::set_dynamic(d, &self);
    }

    fn into_dynamic(self) -> UniquePtr<Dynamic> {
        self
    }
}

/// Trait for creating dynamic values from iterators
pub trait DynamicFromIterator<T> {
    fn from_iter<I: IntoIterator<Item = T>>(iter: I) -> UniquePtr<Dynamic>;
}

/// Implement DynamicFromIterator for arrays from IntoDynamic items
impl<T: IntoDynamic> DynamicFromIterator<T> for Dynamic {
    fn from_iter<I: IntoIterator<Item = T>>(iter: I) -> UniquePtr<Dynamic> {
        let mut array = Dynamic::new_array();
        for item in iter {
            let mut value = Dynamic::new_null();
            item.set_into(value.pin_mut());
            array.push_back(value).unwrap();
        }
        array
    }
}

/// Implement DynamicFromIterator for objects from (Into<StringPiece>, IntoDynamic) tuples
impl<'a, K, V> DynamicFromIterator<(K, V)> for Dynamic
where
    K: Into<bridge::StringPiece<'a>>,
    V: IntoDynamic,
{
    fn from_iter<I: IntoIterator<Item = (K, V)>>(iter: I) -> UniquePtr<Dynamic> {
        let mut object = Dynamic::new_object();
        for (key, value) in iter {
            let mut dynamic_value = Dynamic::new_null();
            value.set_into(dynamic_value.pin_mut());
            object.set_key(key, dynamic_value).unwrap();
        }
        object
    }
}

/// Trait for ergonomic mutable dynamic operations
pub trait DynamicMut {
    /// Push a value to the back of an array
    fn push_back<T: IntoDynamic>(&mut self, value: T) -> Result<(), cxx::Exception>;

    /// Pop a value from the back of an array
    fn pop_back(&mut self) -> Result<(), cxx::Exception>;

    /// Set a value at a specific index in an array
    fn set_at<T: IntoDynamic>(&mut self, index: usize, value: T) -> Result<(), cxx::Exception>;

    /// Set a key-value pair in an object
    fn set_key<'a, K: Into<bridge::StringPiece<'a>>, T: IntoDynamic>(
        &mut self,
        key: K,
        value: T,
    ) -> Result<(), cxx::Exception>;

    /// Erase a key from an object
    fn erase<'a, K: Into<bridge::StringPiece<'a>>>(
        &mut self,
        key: K,
    ) -> Result<bool, cxx::Exception>;

    /// Get a mutable reference to an array element at the specified index
    fn at_mut(&mut self, index: usize) -> Result<std::pin::Pin<&mut Dynamic>, cxx::Exception>;

    /// Get a mutable reference to an object value by key
    fn get_mut<'a, K: Into<bridge::StringPiece<'a>>>(
        &mut self,
        key: K,
    ) -> Result<std::pin::Pin<&mut Dynamic>, cxx::Exception>;

    /// Set the value of this dynamic to the provided value
    fn set_value<T: IntoDynamic>(&mut self, value: T);

    /// Extend an array with values from an iterator
    fn extend_array<I, T>(&mut self, iter: I) -> Result<(), cxx::Exception>
    where
        I: IntoIterator<Item = T>,
        T: IntoDynamic;

    /// Extend an object with key-value pairs from an iterator
    fn extend_object<'a, I, K, V>(&mut self, iter: I) -> Result<(), cxx::Exception>
    where
        I: IntoIterator<Item = (K, V)>,
        K: Into<bridge::StringPiece<'a>>,
        V: IntoDynamic;
}

// Implement the trait for UniquePtr<Dynamic>
impl DynamicMut for UniquePtr<Dynamic> {
    /// Push a value to the back of an array
    fn push_back<T: IntoDynamic>(&mut self, value: T) -> Result<(), cxx::Exception> {
        bridge::array_push_back(self.pin_mut(), value.into_dynamic())
    }

    /// Pop a value from the back of an array
    fn pop_back(&mut self) -> Result<(), cxx::Exception> {
        bridge::array_pop_back(self.pin_mut())
    }

    /// Set a value at a specific index in an array
    fn set_at<T: IntoDynamic>(&mut self, index: usize, value: T) -> Result<(), cxx::Exception> {
        bridge::array_set(self.pin_mut(), index, value.into_dynamic())
    }

    /// Set a key-value pair in an object
    fn set_key<'a, K: Into<bridge::StringPiece<'a>>, T: IntoDynamic>(
        &mut self,
        key: K,
        value: T,
    ) -> Result<(), cxx::Exception> {
        let key: bridge::StringPiece<'a> = key.into();
        bridge::object_set_bytes(self.pin_mut(), key.as_slice(), value.into_dynamic())
    }

    /// Erase a key from an object
    fn erase<'a, K: Into<bridge::StringPiece<'a>>>(
        &mut self,
        key: K,
    ) -> Result<bool, cxx::Exception> {
        let key: bridge::StringPiece<'a> = key.into();
        bridge::object_erase_bytes(self.pin_mut(), key.as_slice())
    }

    /// Get a mutable reference to an array element at the specified index
    fn at_mut(&mut self, index: usize) -> Result<std::pin::Pin<&mut Dynamic>, cxx::Exception> {
        bridge::at_mut(self.pin_mut(), index)
    }

    /// Get a mutable reference to an object value by key
    fn get_mut<'a, K: Into<bridge::StringPiece<'a>>>(
        &mut self,
        key: K,
    ) -> Result<std::pin::Pin<&mut Dynamic>, cxx::Exception> {
        let key: bridge::StringPiece<'a> = key.into();
        bridge::get_mut_bytes(self.pin_mut(), key.as_slice())
    }

    /// Set the value of this dynamic to the provided value
    fn set_value<T: IntoDynamic>(&mut self, value: T) {
        value.set_into(self.pin_mut());
    }

    /// Extend an array with values from an iterator
    fn extend_array<I, T>(&mut self, iter: I) -> Result<(), cxx::Exception>
    where
        I: IntoIterator<Item = T>,
        T: IntoDynamic,
    {
        for item in iter {
            self.push_back(item)?;
        }
        Ok(())
    }

    /// Extend an object with key-value pairs from an iterator
    fn extend_object<'a, I, K, V>(&mut self, iter: I) -> Result<(), cxx::Exception>
    where
        I: IntoIterator<Item = (K, V)>,
        K: Into<bridge::StringPiece<'a>>,
        V: IntoDynamic,
    {
        for (key, value) in iter {
            self.set_key(key, value)?;
        }
        Ok(())
    }
}

// Implement the trait for Pin<&mut Dynamic>
impl DynamicMut for std::pin::Pin<&mut Dynamic> {
    fn push_back<T: IntoDynamic>(&mut self, value: T) -> Result<(), cxx::Exception> {
        bridge::array_push_back(self.as_mut(), value.into_dynamic())
    }

    fn pop_back(&mut self) -> Result<(), cxx::Exception> {
        bridge::array_pop_back(self.as_mut())
    }

    fn set_at<T: IntoDynamic>(&mut self, index: usize, value: T) -> Result<(), cxx::Exception> {
        bridge::array_set(self.as_mut(), index, value.into_dynamic())
    }

    fn set_key<'a, K: Into<bridge::StringPiece<'a>>, T: IntoDynamic>(
        &mut self,
        key: K,
        value: T,
    ) -> Result<(), cxx::Exception> {
        let key: bridge::StringPiece<'a> = key.into();
        bridge::object_set_bytes(self.as_mut(), key.as_slice(), value.into_dynamic())
    }

    fn erase<'a, K: Into<bridge::StringPiece<'a>>>(
        &mut self,
        key: K,
    ) -> Result<bool, cxx::Exception> {
        let key: bridge::StringPiece<'a> = key.into();
        bridge::object_erase_bytes(self.as_mut(), key.as_slice())
    }

    fn at_mut(&mut self, index: usize) -> Result<std::pin::Pin<&mut Dynamic>, cxx::Exception> {
        bridge::at_mut(self.as_mut(), index)
    }

    fn get_mut<'a, K: Into<bridge::StringPiece<'a>>>(
        &mut self,
        key: K,
    ) -> Result<std::pin::Pin<&mut Dynamic>, cxx::Exception> {
        let key: bridge::StringPiece<'a> = key.into();
        bridge::get_mut_bytes(self.as_mut(), key.as_slice())
    }

    fn set_value<T: IntoDynamic>(&mut self, value: T) {
        value.set_into(self.as_mut());
    }

    fn extend_array<I, T>(&mut self, iter: I) -> Result<(), cxx::Exception>
    where
        I: IntoIterator<Item = T>,
        T: IntoDynamic,
    {
        for item in iter {
            self.push_back(item)?;
        }
        Ok(())
    }

    /// Extend an object with key-value pairs from an iterator
    fn extend_object<'a, I, K, V>(&mut self, iter: I) -> Result<(), cxx::Exception>
    where
        I: IntoIterator<Item = (K, V)>,
        K: Into<bridge::StringPiece<'a>>,
        V: IntoDynamic,
    {
        for (key, value) in iter {
            self.set_key(key, value)?;
        }
        Ok(())
    }
}

impl fmt::Display for Dynamic {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.to_json())
    }
}

impl fmt::Debug for Dynamic {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.to_pretty_json())
    }
}

impl PartialEq for Dynamic {
    fn eq(&self, other: &Self) -> bool {
        bridge::equals(self, other)
    }
}

impl Eq for Dynamic {}

#[cfg(test)]
mod tests {
    use super::Dynamic;
    use super::DynamicFromIterator;
    use super::DynamicMut;

    #[test]
    fn test_new_dynamic_null() {
        let d = Dynamic::new_null();
        assert!(d.is_null_value());
        assert!(!d.is_bool());
        assert!(!d.is_int());
        assert!(!d.is_string());
        assert!(!d.is_array());
        assert!(!d.is_object());
    }

    #[test]
    fn test_new_dynamic_bool() {
        let d = Dynamic::new_bool(true);
        assert!(d.is_bool());
        assert!(!d.is_null_value());
        assert!(d.as_bool().unwrap());

        let d = Dynamic::new_bool(false);
        assert!(d.is_bool());
        assert!(!d.as_bool().unwrap());
    }

    #[test]
    fn test_new_dynamic_int() {
        let d = Dynamic::new_int(42);
        assert!(d.is_int());
        assert!(d.is_number());
        assert!(!d.is_null_value());
        assert_eq!(d.as_int().unwrap(), 42);
    }

    #[test]
    fn test_new_dynamic_double() {
        let d = Dynamic::new_double(3.14);
        assert!(d.is_double());
        assert!(d.is_number());
        assert!(!d.is_null_value());
        assert_eq!(d.as_double().unwrap(), 3.14);
    }

    #[test]
    fn test_new_dynamic_string() {
        let d = Dynamic::new_string("hello");
        assert!(d.is_string());
        assert!(!d.is_null_value());
        assert_eq!(d.as_string().unwrap(), "hello");
    }

    #[test]
    fn test_new_dynamic_array() {
        let mut d = Dynamic::new_array();
        assert!(d.is_array());
        assert!(!d.is_null_value());
        assert!(d.empty().unwrap());
        assert_eq!(d.size().unwrap(), 0);

        d.push_back(1i64).unwrap();
        d.push_back("test").unwrap();

        assert_eq!(d.size().unwrap(), 2);
        assert!(!d.empty().unwrap());

        let first = &d[0];
        assert!(first.is_int());
        assert_eq!(first.as_int().unwrap(), 1);

        let second = &d[1];
        assert!(second.is_string());
        assert_eq!(second.as_string().unwrap(), "test");

        d.set_at(0, 99i64).unwrap();
        let modified = &d[0];
        assert_eq!(modified.as_int().unwrap(), 99);

        d.pop_back().unwrap();
        assert_eq!(d.size().unwrap(), 1);
    }

    #[test]
    fn test_new_dynamic_object() {
        let mut d = Dynamic::new_object();
        assert!(d.is_object());
        assert!(!d.is_null_value());
        assert!(d.empty().unwrap());
        assert_eq!(d.size().unwrap(), 0);

        d.set_key("number", 42i64).unwrap();
        d.set_key("text", "world").unwrap();

        assert_eq!(d.size().unwrap(), 2);
        assert!(!d.empty().unwrap());

        assert!(d.contains("number").unwrap());
        assert!(d.contains("text").unwrap());
        assert!(!d.contains("missing").unwrap());

        let retrieved_number = &d["number"];
        assert!(retrieved_number.is_int());
        assert_eq!(retrieved_number.as_int().unwrap(), 42);

        let retrieved_text = &d["text"];
        assert!(retrieved_text.is_string());
        assert_eq!(retrieved_text.as_string().unwrap(), "world");

        let keys = d.keys().unwrap();
        assert_eq!(keys.len(), 2);
        assert!(keys.contains(&"number".to_string()));
        assert!(keys.contains(&"text".to_string()));

        assert!(d.erase("number").unwrap());
        assert_eq!(d.size().unwrap(), 1);
        assert!(!d.contains("number").unwrap());
        assert!(!d.erase("missing").unwrap());
    }

    #[test]
    fn test_clone_dynamic() {
        let original = Dynamic::new_string("test");
        let cloned = original.clone();

        assert!(cloned.is_string());
        assert_eq!(cloned.as_string().unwrap(), "test");
        assert_eq!(original.as_string().unwrap(), cloned.as_string().unwrap());
    }

    #[test]
    fn test_to_string() {
        let d_null = Dynamic::new_null();
        let s_null = d_null.to_string();
        assert_eq!(s_null, "null");

        let d_bool = Dynamic::new_bool(true);
        let s_bool = d_bool.to_string();
        assert_eq!(s_bool, "true");

        let d_int = Dynamic::new_int(42);
        let s_int = d_int.to_string();
        assert_eq!(s_int, "42");

        let d_string = Dynamic::new_string("hello");
        let s_string = d_string.to_string();
        assert_eq!(s_string, "\"hello\"");
    }

    #[test]
    fn test_complex_nested_structure() {
        // Create a complex nested structure: {"users": [{"name": "Alice", "age": 30}, {"name": "Bob", "age": 25}]}
        let mut root = Dynamic::new_object();
        let mut users_array = Dynamic::new_array();

        let mut user1 = Dynamic::new_object();
        user1.set_key("name", "Alice").unwrap();
        user1.set_key("age", 30i64).unwrap();

        let mut user2 = Dynamic::new_object();
        user2.set_key("name", "Bob").unwrap();
        user2.set_key("age", 25i64).unwrap();

        users_array.push_back(user1).unwrap();
        users_array.push_back(user2).unwrap();

        root.set_key("users", users_array).unwrap();

        assert!(root.is_object());
        assert!(root.contains("users").unwrap());

        let users = &root["users"];
        assert!(users.is_array());
        assert_eq!(users.size().unwrap(), 2);

        let first_user = &users[0];
        assert!(first_user.is_object());
        assert!(first_user.contains("name").unwrap());
        assert!(first_user.contains("age").unwrap());

        let first_name = &first_user["name"];
        assert_eq!(first_name.as_string().unwrap(), "Alice");

        let first_age = &first_user["age"];
        assert_eq!(first_age.as_int().unwrap(), 30);
    }

    #[test]
    fn test_indexing_and_ergonomic_api() {
        let mut root = Dynamic::new_object();
        let mut users_array = Dynamic::new_array();

        let mut user1 = Dynamic::new_object();
        user1.set_key("name", "Alice").unwrap();
        user1.set_key("age", 30i64).unwrap();

        let mut user2 = Dynamic::new_object();
        user2.set_key("name", "Bob").unwrap();
        user2.set_key("age", 25i64).unwrap();

        users_array.push_back(user1).unwrap();
        users_array.push_back(user2).unwrap();

        root.set_key("users", users_array).unwrap();

        let users = &root["users"];
        assert!(users.is_array());
        assert_eq!(users.size().unwrap(), 2);

        let first_user = &users[0];
        assert!(first_user.is_object());

        let first_name = &first_user["name"];
        assert_eq!(first_name.as_string().unwrap(), "Alice");

        let first_age = &first_user["age"];
        assert_eq!(first_age.as_int().unwrap(), 30);

        assert!(root.contains("users").unwrap());
        assert!(!root.contains("missing").unwrap());
        assert_eq!(root.size().unwrap(), 1);
        assert!(!root.empty().unwrap());

        let cloned_root = root.clone();
        let cloned_first_name = &cloned_root["users"][0]["name"];
        assert_eq!(cloned_first_name.as_string().unwrap(), "Alice");
    }

    #[test]
    fn test_mutable_access() {
        let mut data_array = Dynamic::new_array();
        data_array.extend_array([10i64, 20i64, 30i64]).unwrap();

        {
            let mut second_element = data_array.at_mut(1).unwrap();
            assert!(second_element.is_int());
            assert_eq!(second_element.as_int().unwrap(), 20);

            second_element.set_value(99i64);
            assert_eq!(second_element.as_int().unwrap(), 99);
        }

        let mut config_obj = Dynamic::new_object();
        config_obj.set_key("enabled", true).unwrap();
        config_obj.set_key("count", 42i64).unwrap();
        config_obj.set_key("name", "test").unwrap();
        config_obj.set_key("score", 3.14f64).unwrap();

        {
            let mut enabled_value = config_obj.get_mut("enabled").unwrap();
            assert!(enabled_value.is_bool());
            assert!(enabled_value.as_bool().unwrap());

            enabled_value.set_value(false);
            assert!(!enabled_value.as_bool().unwrap());
        }

        {
            let mut count_value = config_obj.get_mut("count").unwrap();
            assert!(count_value.is_int());
            assert_eq!(count_value.as_int().unwrap(), 42);

            count_value.set_value(100i64);
            assert_eq!(count_value.as_int().unwrap(), 100);
        }

        {
            let mut name_value = config_obj.get_mut("name").unwrap();
            assert!(name_value.is_string());
            assert_eq!(name_value.as_string().unwrap(), "test");

            name_value.set_value("modified");
            assert_eq!(name_value.as_string().unwrap(), "modified");
        }

        {
            let mut score_value = config_obj.get_mut("score").unwrap();
            assert!(score_value.is_double());
            assert_eq!(score_value.as_double().unwrap(), 3.14);

            score_value.set_value(2.71f64);
            assert_eq!(score_value.as_double().unwrap(), 2.71);
        }

        assert_eq!(data_array[0].as_int().unwrap(), 10);
        assert_eq!(data_array[1].as_int().unwrap(), 99);
        assert_eq!(data_array[2].as_int().unwrap(), 30);
        assert!(!config_obj["enabled"].as_bool().unwrap());
        assert_eq!(config_obj["count"].as_int().unwrap(), 100);
        assert_eq!(config_obj["name"].as_string().unwrap(), "modified");
        assert_eq!(config_obj["score"].as_double().unwrap(), 2.71);
    }

    #[test]
    fn test_ergonomic_setters() {
        let mut obj = Dynamic::new_object();

        {
            let mut null_field = obj.get_mut("null_field").unwrap();
            null_field.set_value(());
            assert!(null_field.is_null_value());
        }

        {
            let mut bool_field = obj.get_mut("bool_field").unwrap();
            bool_field.set_value(true);
            assert!(bool_field.is_bool());
            assert!(bool_field.as_bool().unwrap());
        }

        {
            let mut int_field = obj.get_mut("int_field").unwrap();
            int_field.set_value(42i64);
            assert!(int_field.is_int());
            assert_eq!(int_field.as_int().unwrap(), 42);
        }

        {
            let mut int32_field = obj.get_mut("int32_field").unwrap();
            int32_field.set_value(123i32);
            assert!(int32_field.is_int());
            assert_eq!(int32_field.as_int().unwrap(), 123);
        }

        {
            let mut double_field = obj.get_mut("double_field").unwrap();
            double_field.set_value(3.14f64);
            assert!(double_field.is_double());
            assert_eq!(double_field.as_double().unwrap(), 3.14);
        }

        {
            let mut float_field = obj.get_mut("float_field").unwrap();
            float_field.set_value(2.5f32);
            assert!(float_field.is_double());
            assert_eq!(float_field.as_double().unwrap(), 2.5);
        }

        {
            let mut str_field = obj.get_mut("str_field").unwrap();
            str_field.set_value("hello");
            assert!(str_field.is_string());
            assert_eq!(str_field.as_string().unwrap(), "hello");
        }

        {
            let mut string_field = obj.get_mut("string_field").unwrap();
            string_field.set_value(String::from("world"));
            assert!(string_field.is_string());
            assert_eq!(string_field.as_string().unwrap(), "world");
        }

        {
            let s = String::from("reference");
            let mut string_ref_field = obj.get_mut("string_ref_field").unwrap();
            string_ref_field.set_value(&s);
            assert!(string_ref_field.is_string());
            assert_eq!(string_ref_field.as_string().unwrap(), "reference");
        }

        {
            let source = Dynamic::new_string("copied");
            let mut dynamic_field = obj.get_mut("dynamic_field").unwrap();
            dynamic_field.set_value(&*source);
            assert!(dynamic_field.is_string());
            assert_eq!(dynamic_field.as_string().unwrap(), "copied");
        }

        {
            let source = Dynamic::new_int(999);
            let mut unique_field = obj.get_mut("unique_field").unwrap();
            unique_field.set_value(source);
            assert!(unique_field.is_int());
            assert_eq!(unique_field.as_int().unwrap(), 999);
        }

        assert!(obj["null_field"].is_null_value());
        assert!(obj["bool_field"].as_bool().unwrap());
        assert_eq!(obj["int_field"].as_int().unwrap(), 42);
        assert_eq!(obj["int32_field"].as_int().unwrap(), 123);
        assert_eq!(obj["double_field"].as_double().unwrap(), 3.14);
        assert_eq!(obj["float_field"].as_double().unwrap(), 2.5);
        assert_eq!(obj["str_field"].as_string().unwrap(), "hello");
        assert_eq!(obj["string_field"].as_string().unwrap(), "world");
        assert_eq!(obj["string_ref_field"].as_string().unwrap(), "reference");
        assert_eq!(obj["dynamic_field"].as_string().unwrap(), "copied");
        assert_eq!(obj["unique_field"].as_int().unwrap(), 999);
    }

    #[test]
    fn test_ergonomic_setters_array() {
        let mut arr = Dynamic::new_array();
        arr.push_back(Dynamic::new_null()).unwrap();
        arr.push_back(Dynamic::new_null()).unwrap();
        arr.push_back(Dynamic::new_null()).unwrap();

        {
            let mut first = arr.at_mut(0).unwrap();
            first.set_value(100i64);
        }

        {
            let mut second = arr.at_mut(1).unwrap();
            second.set_value("array_string");
        }

        {
            let mut third = arr.at_mut(2).unwrap();
            third.set_value(true);
        }

        assert_eq!(arr[0].as_int().unwrap(), 100);
        assert_eq!(arr[1].as_str().unwrap(), "array_string");
        assert!(arr[2].as_bool().unwrap());
    }

    #[test]
    fn test_json_serialization() {
        let d = Dynamic::new_string("hello");
        let json = d.to_json();
        assert_eq!(json, "\"hello\"");

        let d = Dynamic::new_int(42);
        let json = d.to_json();
        assert_eq!(json, "42");

        let d = Dynamic::new_bool(true);
        let json = d.to_json();
        assert_eq!(json, "true");

        let d = Dynamic::new_null();
        let json = d.to_json();
        assert_eq!(json, "null");

        let mut arr = Dynamic::new_array();
        arr.push_back(1i64).unwrap();
        arr.push_back("test").unwrap();
        arr.push_back(false).unwrap();
        let json = arr.to_json();
        assert_eq!(json, "[1,\"test\",false]");

        let mut obj = Dynamic::new_object();
        obj.set_key("name", "Alice").unwrap();
        obj.set_key("age", 30i64).unwrap();
        let json = obj.to_json();
        assert!(
            json == "{\"age\":30,\"name\":\"Alice\"}" || json == "{\"name\":\"Alice\",\"age\":30}"
        );
    }

    #[test]
    fn test_pretty_json_serialization() {
        let mut obj = Dynamic::new_object();
        obj.set_key("name", Dynamic::new_string("Alice")).unwrap();
        obj.set_key("age", Dynamic::new_int(30)).unwrap();
        let pretty_json = obj.to_pretty_json();

        assert!(pretty_json.contains('\n'));
        assert!(pretty_json.contains("  "));
        assert!(pretty_json.contains("\"name\""));
        assert!(pretty_json.contains("\"Alice\""));
        assert!(pretty_json.contains("\"age\""));
        assert!(pretty_json.contains("30"));
    }

    #[test]
    fn test_simple_null() {
        let manual_null = Dynamic::new_null();
        assert!(
            manual_null.is_null_value(),
            "C++ wrapper function should return true for manually created null"
        );
    }

    #[test]
    fn test_json_parsing() {
        let d = Dynamic::from_json("\"hello\"").unwrap();
        assert!(d.is_string());
        assert_eq!(d.as_string().unwrap(), "hello");

        let d = Dynamic::from_json("42").unwrap();
        assert!(d.is_int());
        assert_eq!(d.as_int().unwrap(), 42);

        let d = Dynamic::from_json("true").unwrap();
        assert!(d.is_bool());
        assert!(d.as_bool().unwrap());

        let d = Dynamic::from_json("null").unwrap();
        assert!(
            d.is_null_value(),
            "C++ wrapper function should return true for parsed null"
        );
        assert!(d.is_null_value());

        let d = Dynamic::from_json("[1, \"test\", false]").unwrap();
        assert!(d.is_array());
        assert_eq!(d.size().unwrap(), 3);
        assert_eq!(d[0].as_int().unwrap(), 1);
        assert_eq!(d[1].as_string().unwrap(), "test");
        assert!(!d[2].as_bool().unwrap());

        let d = Dynamic::from_json("{\"name\": \"Alice\", \"age\": 30}").unwrap();
        assert!(d.is_object());
        assert_eq!(d.size().unwrap(), 2);
        assert_eq!(d["name"].as_string().unwrap(), "Alice");
        assert_eq!(d["age"].as_int().unwrap(), 30);
    }

    #[test]
    fn test_json_roundtrip() {
        let mut original = Dynamic::new_object();
        original
            .set_key("users", {
                let mut users = Dynamic::new_array();
                let mut user1 = Dynamic::new_object();
                user1.set_key("name", Dynamic::new_string("Alice")).unwrap();
                user1.set_key("age", Dynamic::new_int(30)).unwrap();
                users.push_back(user1).unwrap();
                users
            })
            .unwrap();

        let json = original.to_json();
        let parsed = Dynamic::from_json(&json).unwrap();

        assert!(parsed.is_object());
        assert!(parsed["users"].is_array());
        assert_eq!(parsed["users"].size().unwrap(), 1);
        assert_eq!(parsed["users"][0]["name"].as_string().unwrap(), "Alice");
        assert_eq!(parsed["users"][0]["age"].as_int().unwrap(), 30);
    }

    #[test]
    fn test_as_str() {
        use super::AsStrError;

        let d = Dynamic::new_string("hello world");

        let str_slice = d.as_str().unwrap();
        assert_eq!(str_slice, "hello world");

        let string_owned = d.as_string().unwrap();
        assert_eq!(str_slice, string_owned.as_str());

        let empty_d = Dynamic::new_string("");
        let empty_str = empty_d.as_str().unwrap();
        assert_eq!(empty_str, "");

        let unicode_d = Dynamic::new_string("Hello 世界 🌍");
        let unicode_str = unicode_d.as_str().unwrap();
        assert_eq!(unicode_str, "Hello 世界 🌍");

        let int_d = Dynamic::new_int(42);
        match int_d.as_str() {
            Err(AsStrError::CxxException(_)) => {}
            _ => panic!("Expected CxxException for non-string type"),
        }

        let bool_d = Dynamic::new_bool(true);
        match bool_d.as_str() {
            Err(AsStrError::CxxException(_)) => {}
            _ => panic!("Expected CxxException for non-string type"),
        }

        let null_d = Dynamic::new_null();
        match null_d.as_str() {
            Err(AsStrError::CxxException(_)) => {}
            _ => panic!("Expected CxxException for non-string type"),
        }
    }

    #[test]
    fn test_equals_and_partial_eq() {
        // Test basic equality for different types
        let null1 = Dynamic::new_null();
        let null2 = Dynamic::new_null();
        assert_eq!(null1, null2);

        let bool1 = Dynamic::new_bool(true);
        let bool2 = Dynamic::new_bool(true);
        let bool3 = Dynamic::new_bool(false);
        assert_eq!(bool1, bool2);
        assert_ne!(bool1, bool3);

        let int1 = Dynamic::new_int(42);
        let int2 = Dynamic::new_int(42);
        let int3 = Dynamic::new_int(43);
        assert_eq!(int1, int2);
        assert_ne!(int1, int3);

        let double1 = Dynamic::new_double(3.14);
        let double2 = Dynamic::new_double(3.14);
        let double3 = Dynamic::new_double(2.71);
        assert_eq!(double1, double2);
        assert_ne!(double1, double3);

        let string1 = Dynamic::new_string("hello");
        let string2 = Dynamic::new_string("hello");
        let string3 = Dynamic::new_string("world");
        assert_eq!(string1, string2);
        assert_ne!(string1, string3);

        // Test inequality between different types
        assert_ne!(null1, bool1);
        assert_ne!(bool1, int1);
        assert_ne!(int1, double1);
        assert_ne!(double1, string1);
    }

    #[test]
    fn test_equals_arrays() {
        let mut arr1 = Dynamic::new_array();
        arr1.push_back(Dynamic::new_int(1)).unwrap();
        arr1.push_back(Dynamic::new_string("test")).unwrap();
        arr1.push_back(Dynamic::new_bool(true)).unwrap();

        let mut arr2 = Dynamic::new_array();
        arr2.push_back(Dynamic::new_int(1)).unwrap();
        arr2.push_back(Dynamic::new_string("test")).unwrap();
        arr2.push_back(Dynamic::new_bool(true)).unwrap();

        let mut arr3 = Dynamic::new_array();
        arr3.push_back(Dynamic::new_int(1)).unwrap();
        arr3.push_back(Dynamic::new_string("test")).unwrap();
        arr3.push_back(Dynamic::new_bool(false)).unwrap();

        let mut arr4 = Dynamic::new_array();
        arr4.push_back(Dynamic::new_int(1)).unwrap();
        arr4.push_back(Dynamic::new_string("test")).unwrap();

        assert_eq!(arr1, arr2);
        assert_ne!(arr1, arr3);
        assert_ne!(arr1, arr4);

        let empty1 = Dynamic::new_array();
        let empty2 = Dynamic::new_array();
        assert_eq!(empty1, empty2);
        assert_ne!(empty1, arr1);
    }

    #[test]
    fn test_equals_objects() {
        let mut obj1 = Dynamic::new_object();
        obj1.set_key("name", Dynamic::new_string("Alice")).unwrap();
        obj1.set_key("age", Dynamic::new_int(30)).unwrap();
        obj1.set_key("active", Dynamic::new_bool(true)).unwrap();

        let mut obj2 = Dynamic::new_object();
        obj2.set_key("name", Dynamic::new_string("Alice")).unwrap();
        obj2.set_key("age", Dynamic::new_int(30)).unwrap();
        obj2.set_key("active", Dynamic::new_bool(true)).unwrap();

        let mut obj3 = Dynamic::new_object();
        obj3.set_key("name", Dynamic::new_string("Bob")).unwrap(); // Different value
        obj3.set_key("age", Dynamic::new_int(30)).unwrap();
        obj3.set_key("active", Dynamic::new_bool(true)).unwrap();

        let mut obj4 = Dynamic::new_object();
        obj4.set_key("name", Dynamic::new_string("Alice")).unwrap();
        obj4.set_key("age", Dynamic::new_int(30)).unwrap();
        // Missing "active" key

        assert_eq!(obj1, obj2);
        assert_ne!(obj1, obj3);
        assert_ne!(obj1, obj4);

        let empty1 = Dynamic::new_object();
        let empty2 = Dynamic::new_object();
        assert_eq!(empty1, empty2);
        assert_ne!(empty1, obj1);
    }

    #[test]
    fn test_equals_nested_structures() {
        // Create nested structure: {"users": [{"name": "Alice", "age": 30}]}
        let mut nested1 = Dynamic::new_object();
        let mut users1 = Dynamic::new_array();
        let mut user1 = Dynamic::new_object();
        user1.set_key("name", Dynamic::new_string("Alice")).unwrap();
        user1.set_key("age", Dynamic::new_int(30)).unwrap();
        users1.push_back(user1).unwrap();
        nested1.set_key("users", users1).unwrap();

        // Create identical nested structure
        let mut nested2 = Dynamic::new_object();
        let mut users2 = Dynamic::new_array();
        let mut user2 = Dynamic::new_object();
        user2.set_key("name", Dynamic::new_string("Alice")).unwrap();
        user2.set_key("age", Dynamic::new_int(30)).unwrap();
        users2.push_back(user2).unwrap();
        nested2.set_key("users", users2).unwrap();

        // Create different nested structure
        let mut nested3 = Dynamic::new_object();
        let mut users3 = Dynamic::new_array();
        let mut user3 = Dynamic::new_object();
        user3.set_key("name", Dynamic::new_string("Bob")).unwrap(); // Different name
        user3.set_key("age", Dynamic::new_int(30)).unwrap();
        users3.push_back(user3).unwrap();
        nested3.set_key("users", users3).unwrap();

        assert_eq!(nested1, nested2);
        assert_ne!(nested1, nested3);
    }

    #[test]
    fn test_equals_json_parsed() {
        let json1 = r#"{"name": "Alice", "age": 30, "scores": [95, 87, 92]}"#;
        let json2 = r#"{"name": "Alice", "age": 30, "scores": [95, 87, 92]}"#;
        let json3 = r#"{"name": "Bob", "age": 30, "scores": [95, 87, 92]}"#;

        let parsed1 = Dynamic::from_json(json1).unwrap();
        let parsed2 = Dynamic::from_json(json2).unwrap();
        let parsed3 = Dynamic::from_json(json3).unwrap();

        assert_eq!(parsed1, parsed2);
        assert_ne!(parsed1, parsed3);

        let mut manual = Dynamic::new_object();
        manual
            .set_key("name", Dynamic::new_string("Alice"))
            .unwrap();
        manual.set_key("age", Dynamic::new_int(30)).unwrap();
        let mut scores = Dynamic::new_array();
        scores.push_back(Dynamic::new_int(95)).unwrap();
        scores.push_back(Dynamic::new_int(87)).unwrap();
        scores.push_back(Dynamic::new_int(92)).unwrap();
        manual.set_key("scores", scores).unwrap();

        assert_eq!(parsed1, manual);
    }

    #[test]
    fn test_equals_special_values() {
        // Test with special floating point values
        let nan1 = Dynamic::new_double(f64::NAN);
        let nan2 = Dynamic::new_double(f64::NAN);
        // NaN != NaN in IEEE 754, so this should be false
        assert_ne!(nan1, nan2);

        let inf1 = Dynamic::new_double(f64::INFINITY);
        let inf2 = Dynamic::new_double(f64::INFINITY);
        assert_eq!(inf1, inf2);

        let neg_inf1 = Dynamic::new_double(f64::NEG_INFINITY);
        let neg_inf2 = Dynamic::new_double(f64::NEG_INFINITY);
        assert_eq!(neg_inf1, neg_inf2);

        assert_ne!(inf1, neg_inf1);

        // Test with zero values
        let zero1 = Dynamic::new_double(0.0);
        let zero2 = Dynamic::new_double(-0.0);
        // In IEEE 754, 0.0 == -0.0
        assert_eq!(zero1, zero2);
    }

    #[test]
    fn test_collect_array() {
        let values = vec![1i64, 2i64, 3i64, 4i64, 5i64];
        let array = Dynamic::from_iter(values);

        assert!(array.is_array());
        assert_eq!(array.size().unwrap(), 5);
        assert_eq!(array[0].as_int().unwrap(), 1);
        assert_eq!(array[1].as_int().unwrap(), 2);
        assert_eq!(array[2].as_int().unwrap(), 3);
        assert_eq!(array[3].as_int().unwrap(), 4);
        assert_eq!(array[4].as_int().unwrap(), 5);

        let strings = vec!["apple", "banana", "cherry"];
        let string_array = Dynamic::from_iter(strings);
        assert!(string_array.is_array());
        assert_eq!(string_array.size().unwrap(), 3);
        assert_eq!(string_array[0].as_string().unwrap(), "apple");
        assert_eq!(string_array[1].as_string().unwrap(), "banana");
        assert_eq!(string_array[2].as_string().unwrap(), "cherry");
    }

    #[test]
    fn test_collect_object() {
        let pairs = vec![("name", "Alice"), ("city", "New York"), ("country", "USA")];
        let object = Dynamic::from_iter(pairs);

        assert!(object.is_object());
        assert_eq!(object.size().unwrap(), 3);
        assert_eq!(object["name"].as_string().unwrap(), "Alice");
        assert_eq!(object["city"].as_string().unwrap(), "New York");
        assert_eq!(object["country"].as_string().unwrap(), "USA");

        let int_pairs = vec![("first", 1i64), ("second", 2i64), ("third", 3i64)];
        let int_object = Dynamic::from_iter(int_pairs);
        assert!(int_object.is_object());
        assert_eq!(int_object.size().unwrap(), 3);
        assert_eq!(int_object["first"].as_int().unwrap(), 1);
        assert_eq!(int_object["second"].as_int().unwrap(), 2);
        assert_eq!(int_object["third"].as_int().unwrap(), 3);
    }

    #[test]
    fn test_extend_array() {
        use super::DynamicMut;

        let mut array = Dynamic::new_array();
        array.push_back(Dynamic::new_int(1)).unwrap();
        array.push_back(Dynamic::new_int(2)).unwrap();

        let additional_values = vec![3i64, 4i64, 5i64];
        array.extend_array(additional_values).unwrap();

        assert_eq!(array.size().unwrap(), 5);
        assert_eq!(array[0].as_int().unwrap(), 1);
        assert_eq!(array[1].as_int().unwrap(), 2);
        assert_eq!(array[2].as_int().unwrap(), 3);
        assert_eq!(array[3].as_int().unwrap(), 4);
        assert_eq!(array[4].as_int().unwrap(), 5);

        let string_values = vec!["hello", "world"];
        array.extend_array(string_values).unwrap();

        assert_eq!(array.size().unwrap(), 7);
        assert_eq!(array[5].as_string().unwrap(), "hello");
        assert_eq!(array[6].as_string().unwrap(), "world");
    }

    #[test]
    fn test_extend_object() {
        let mut object = Dynamic::new_object();
        object
            .set_key("initial", Dynamic::new_string("value"))
            .unwrap();

        let additional_pairs = vec![("name", "Alice"), ("city", "Boston")];
        object.extend_object(additional_pairs).unwrap();

        assert_eq!(object.size().unwrap(), 3);
        assert_eq!(object["initial"].as_string().unwrap(), "value");
        assert_eq!(object["name"].as_string().unwrap(), "Alice");
        assert_eq!(object["city"].as_string().unwrap(), "Boston");

        let int_pairs = vec![("age", 25i64), ("score", 100i64)];
        object.extend_object(int_pairs).unwrap();

        assert_eq!(object.size().unwrap(), 5);
        assert_eq!(object["age"].as_int().unwrap(), 25);
        assert_eq!(object["score"].as_int().unwrap(), 100);

        let overwrite_pairs = vec![("name", "Bob")];
        object.extend_object(overwrite_pairs).unwrap();

        assert_eq!(object.size().unwrap(), 5);
        assert_eq!(object["name"].as_string().unwrap(), "Bob");
    }

    #[test]
    fn test_extend_with_pin_mut() {
        use super::DynamicMut;

        let mut array = Dynamic::new_array();
        array.push_back(Dynamic::new_int(1)).unwrap();

        let values = vec![2i64, 3i64];
        array.extend_array(values).unwrap();

        assert_eq!(array.size().unwrap(), 3);
        assert_eq!(array[0].as_int().unwrap(), 1);
        assert_eq!(array[1].as_int().unwrap(), 2);
        assert_eq!(array[2].as_int().unwrap(), 3);
    }

    #[test]
    fn test_safe_get_method() {
        let mut obj = Dynamic::new_object();
        obj.set_key("existing_key", Dynamic::new_string("value"))
            .unwrap();

        match obj.get("existing_key") {
            Ok(value) => {
                assert_eq!(value.as_string().unwrap(), "value");
            }
            Err(_) => {
                panic!("Expected successful get for existing key");
            }
        }

        match obj.get("non_existent_key") {
            Ok(_) => {
                panic!("Expected error for non-existent key");
            }
            Err(_) => {
                // Expected error
            }
        }
    }

    #[test]
    fn test_safe_at_method() {
        let mut arr = Dynamic::new_array();
        arr.push_back(Dynamic::new_int(10)).unwrap();
        arr.push_back(Dynamic::new_int(20)).unwrap();
        arr.push_back(Dynamic::new_int(30)).unwrap();

        match arr.at(1) {
            Ok(value) => {
                assert_eq!(value.as_int().unwrap(), 20);
            }
            Err(_) => {
                panic!("Expected successful at for valid index");
            }
        }

        match arr.at(10) {
            Ok(_) => {
                panic!("Expected error for out-of-bounds index");
            }
            Err(_) => {
                // Expected error
            }
        }
    }

    #[test]
    fn test_safe_vs_index_trait() {
        let mut arr = Dynamic::new_array();
        arr.push_back(Dynamic::new_int(42)).unwrap();

        assert_eq!(arr.at(0).unwrap().as_int().unwrap(), 42);
        assert_eq!(arr[0].as_int().unwrap(), 42);

        let mut obj = Dynamic::new_object();
        obj.set_key("key", "value").unwrap();

        assert_eq!(obj.get("key").unwrap().as_string().unwrap(), "value");
        assert_eq!(obj["key"].as_string().unwrap(), "value");
    }

    #[test]
    fn test_string_byte_subview_and_nul() {
        let input = b"!a\0b?";
        let d = Dynamic::new_string(&input[1..4]);
        let borrowed = d.as_str().unwrap();
        assert_eq!(borrowed, "a\0b");
        assert_eq!(
            borrowed.as_ptr(),
            d.as_string_piece().unwrap().as_slice().as_ptr()
        );
    }

    #[test]
    fn test_invalid_utf8_error_parity() {
        let input = [b'a', 0xff, 0];
        let d = Dynamic::new_string(input.as_slice());
        let super::AsStrError::Utf8Error(error) = d.as_str().unwrap_err() else {
            panic!("byte strings must fail UTF-8 validation, not construction");
        };
        let legacy = d.as_string_piece().unwrap().as_str().unwrap_err();
        assert_eq!(error.valid_up_to(), legacy.valid_up_to());
        assert_eq!(error.error_len(), legacy.error_len());
        assert!(d.as_string().is_err());
    }

    #[test]
    fn test_string_input_ownership_and_nul_setter() {
        let mut input = vec![b'v'; 48];
        let mut d = Dynamic::new_string(input.as_slice());
        input.fill(b'x');
        drop(input);
        assert_eq!(d.as_str().unwrap(), "v".repeat(48));
        d.set_value(String::from("replacement\0tail"));
        assert_eq!(d.as_str().unwrap(), "replacement\0tail");
    }

    #[test]
    fn test_byte_keys_and_json_subviews() {
        let key = b"!\xff\0k?";
        let key = &key[1..4];
        let mut d = Dynamic::new_object();
        d.set_key(key, "before").unwrap();
        assert!(d.contains(key).unwrap());
        assert_eq!(d.get(key).unwrap().as_str().unwrap(), "before");
        d.get_mut(key).unwrap().set_value("after\0tail");
        assert_eq!(d.get(key).unwrap().as_str().unwrap(), "after\0tail");
        assert!(d.keys().is_err());
        assert!(d.erase(key).unwrap());
        let json = b"![\"a\\u0000b\"]?";
        let parsed = Dynamic::from_json(&json[1..json.len() - 1]).unwrap();
        assert_eq!(parsed[0].as_str().unwrap(), "a\0b");
    }
}
