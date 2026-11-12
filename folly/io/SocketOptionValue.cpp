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

#include <folly/io/SocketOptionValue.h>

#include <ostream>

namespace folly {

int SocketOptionValue::asInt() const {
  return std::get<int>(val_);
}

const std::string& SocketOptionValue::asString() const {
  return std::get<std::string>(val_);
}

bool SocketOptionValue::hasInt() const {
  return std::holds_alternative<int>(val_);
}

bool SocketOptionValue::hasString() const {
  return std::holds_alternative<std::string>(val_);
}

std::string SocketOptionValue::toString() const {
  if (hasInt()) {
    char sval[20];
    int written = snprintf(sval, sizeof(sval), "%d", asInt());
    if (written > 0 && written < static_cast<int>(sizeof(sval))) {
      return std::string(sval, written);
    } else {
      return std::string();
    }
  } else {
    return asString();
  }
}

bool operator==(const SocketOptionValue& lhs, const SocketOptionValue& rhs) {
  if (lhs.hasInt() && !rhs.hasInt()) {
    return false;
  } else if (lhs.hasInt() && rhs.hasInt()) {
    return lhs.asInt() == rhs.asInt();
  } else if (lhs.hasString() && rhs.hasString()) {
    return lhs.asString() == rhs.asString();
  } else {
    return false;
  }
}

bool operator==(const SocketOptionValue& lhs, int rhs) {
  if (!lhs.hasInt()) {
    return false;
  }
  return (lhs.asInt() == rhs);
}

bool operator==(const SocketOptionValue& lhs, const std::string& rhs) {
  if (!lhs.hasString()) {
    return false;
  }
  return (lhs.asString() == rhs);
}

void toAppend(const SocketOptionValue& val, std::string* result) {
  result->append(val.toString());
}

std::ostream& operator<<(std::ostream& os, const SocketOptionValue& val) {
  os << val.toString();
  return os;
}

} // namespace folly
