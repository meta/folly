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
#include <string_view>
#include <system_error>

namespace folly {
namespace ext {

using test_find_resource_t = std::string(std::string_view);
extern test_find_resource_t* test_find_resource;

using test_report_resource_exhaustion_t =
    void(std::string_view, std::error_code, std::string_view) noexcept;
extern test_report_resource_exhaustion_t* test_report_resource_exhaustion;

} // namespace ext
} // namespace folly
