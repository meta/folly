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

#include <string>
#include <variant>

namespace folly {

/**
 * Variant container for socket option values: integer or string.
 * Implicit ctor/compares with int for backward compatibility.
 */
class SocketOptionValue {
 public:
  /* implicit */ SocketOptionValue(int val) : val_(val) {}
  /* implicit */ SocketOptionValue(const std::string& val) : val_(val) {}
  SocketOptionValue() : val_(0) {}

  // Return true if container holds int
  bool hasInt() const;
  // Returns int value, prior to calling must verify container holds integer via
  // hasInt()
  int asInt() const;

  // Return true if container holds string
  bool hasString() const;
  // Returns string value, prior to calling must verify container holds string
  // via hasString()
  const std::string& asString() const;

  // If holding string value, returns string value is.
  // If integer, converts to string.
  std::string toString() const;

  friend bool operator==(
      const SocketOptionValue& lhs, const SocketOptionValue& rhs);
  friend bool operator==(const SocketOptionValue& lhs, int rhs);
  friend bool operator==(const SocketOptionValue& lhs, const std::string& rhs);

 private:
  std::variant<int, std::string> val_;
};

void toAppend(const SocketOptionValue& val, std::string* result);

std::ostream& operator<<(std::ostream&, const SocketOptionValue&);

} // namespace folly
