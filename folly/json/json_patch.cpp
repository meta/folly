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

#include <folly/json/json_patch.h>

#include <glog/logging.h>

#include <folly/Conv.h>
#include <folly/container/Enumerate.h>

namespace folly {

// Movability relies on there being no user-declared destructor; keep this
// pinned so `try_parse()`'s implicit-move `return patch;` stays a real move.
static_assert(std::is_nothrow_move_constructible_v<json_patch>);

// JSON patch operation names
constexpr StringPiece kOperationTest = "test";
constexpr StringPiece kOperationRemove = "remove";
constexpr StringPiece kOperationAdd = "add";
constexpr StringPiece kOperationReplace = "replace";
constexpr StringPiece kOperationMove = "move";
constexpr StringPiece kOperationCopy = "copy";
// field tags in JSON patch
constexpr StringPiece kOpTag = "op";
constexpr StringPiece kValueTag = "value";
constexpr StringPiece kPathTag = "path";
constexpr StringPiece kFromTag = "from";

// static
Expected<json_patch, json_patch::parse_error> json_patch::try_parse(
    dynamic const& obj) noexcept {
  using err_code = parse_error_code;

  json_patch patch;
  if (!obj.isArray()) {
    return makeUnexpected(parse_error{err_code::invalid_shape, &obj});
  }
  for (auto const& elem : obj) {
    if (!elem.isObject()) {
      return makeUnexpected(parse_error{err_code::invalid_shape, &elem});
    }
    auto const* op_ptr = elem.get_ptr(kOpTag);
    if (!op_ptr) {
      return makeUnexpected(parse_error{err_code::missing_op, &elem});
    }
    if (!op_ptr->isString()) {
      return makeUnexpected(parse_error{err_code::malformed_op, &elem});
    }
    auto const op_str = op_ptr->asString();
    patch_operation op;

    // extract 'from' attribute
    {
      auto const* from_ptr =
          (op_str == kOperationMove || op_str == kOperationCopy)
          ? elem.get_ptr(kFromTag)
          : nullptr;
      if (from_ptr) {
        if (!from_ptr->isString()) {
          return makeUnexpected(parse_error{err_code::invalid_shape, &elem});
        }
        auto json_ptr = json_pointer::try_parse(from_ptr->asString());
        if (!json_ptr.hasValue()) {
          return makeUnexpected(
              parse_error{err_code::malformed_from_attr, &elem});
        }
        op.from = std::move(json_ptr).value();
      }
    }

    // extract 'path' attribute
    {
      auto const* path_ptr = elem.get_ptr(kPathTag);
      if (!path_ptr) {
        return makeUnexpected(parse_error{err_code::missing_path_attr, &elem});
      }
      if (!path_ptr->isString()) {
        return makeUnexpected(
            parse_error{err_code::malformed_path_attr, &elem});
      }
      auto json_ptr = json_pointer::try_parse(path_ptr->asString());
      if (!json_ptr.hasValue()) {
        return makeUnexpected(
            parse_error{err_code::malformed_path_attr, &elem});
      }
      op.path = std::move(json_ptr).value();
    }

    // extract 'value' attribute
    {
      auto const* val_ptr = elem.get_ptr(kValueTag);
      if (val_ptr) {
        op.value = *val_ptr;
      }
    }

    // check mandatory attributes - different per operation
    // NOTE: per RFC, the surplus attributes (e.g. 'from' with 'add')
    // should be simply ignored

    using op_code = patch_operation_code;

    if (op_str == kOperationTest) {
      if (!op.value) {
        return makeUnexpected(parse_error{err_code::missing_value_attr, &elem});
      }
      op.op_code = op_code::test;
    } else if (op_str == kOperationRemove) {
      op.op_code = op_code::remove;
    } else if (op_str == kOperationAdd) {
      if (!op.value) {
        return makeUnexpected(parse_error{err_code::missing_value_attr, &elem});
      }
      op.op_code = op_code::add;
    } else if (op_str == kOperationReplace) {
      if (!op.value) {
        return makeUnexpected(parse_error{err_code::missing_value_attr, &elem});
      }
      op.op_code = op_code::replace;
    } else if (op_str == kOperationMove) {
      if (!op.from) {
        return makeUnexpected(parse_error{err_code::missing_from_attr, &elem});
      }
      if (op.from->is_strict_prefix_of(op.path)) {
        return makeUnexpected(
            parse_error{err_code::overlapping_pointers, &elem});
      }
      op.op_code = op_code::move;
    } else if (op_str == kOperationCopy) {
      if (!op.from) {
        return makeUnexpected(parse_error{err_code::missing_from_attr, &elem});
      }
      op.op_code = op_code::copy;
    }

    if (op.op_code != op_code::invalid) {
      patch.ops_.emplace_back(std::move(op));
    } else {
      return makeUnexpected(parse_error{err_code::unknown_op, &elem});
    }
  }
  return patch;
}

std::vector<json_patch::patch_operation> const& json_patch::ops() const {
  return ops_;
}

namespace {

bool numbers_equal(dynamic const& lhs, dynamic const& rhs) {
  if (lhs.type() == rhs.type()) {
    return lhs == rhs;
  }
  auto const& int_value = lhs.isInt() ? lhs : rhs;
  auto const& double_value = lhs.isInt() ? rhs : lhs;
  auto const exact = tryTo<int64_t>(double_value.getDouble());
  return exact.hasValue() && *exact == int_value.getInt();
}

bool json_values_equal(dynamic const& lhs, dynamic const& rhs) {
  if (lhs.isNumber() && rhs.isNumber()) {
    return numbers_equal(lhs, rhs);
  }
  if (lhs.type() != rhs.type()) {
    return false;
  }
  if (lhs.isArray()) {
    if (lhs.size() != rhs.size()) {
      return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
      if (!json_values_equal(lhs[i], rhs[i])) {
        return false;
      }
    }
    return true;
  }
  if (lhs.isObject()) {
    if (lhs.size() != rhs.size()) {
      return false;
    }
    for (auto const& item : lhs.items()) {
      auto const it = rhs.find(item.first);
      if (it == rhs.items().end() ||
          !json_values_equal(item.second, it->second)) {
        return false;
      }
    }
    return true;
  }
  return lhs == rhs;
}

// clang-format off
Expected<Unit, json_patch::patch_application_error_code>
// clang-format on
do_remove(dynamic::resolved_json_pointer<dynamic>& ptr) {
  using error_code = json_patch::patch_application_error_code;

  if (!ptr.hasValue()) {
    return makeUnexpected(error_code::path_not_found);
  }

  auto parent = ptr->parent;

  if (!parent) {
    return makeUnexpected(error_code::other);
  }

  if (parent->isObject()) {
    parent->erase(ptr->parent_key);
    return unit;
  }

  if (parent->isArray()) {
    parent->erase(parent->begin() + ptr->parent_index);
    return unit;
  }

  return makeUnexpected(error_code::other);
}

// Moves from `value` only when the add succeeds.
// clang-format off
Expected<Unit, json_patch::patch_application_error_code>
// clang-format on
do_add(
    dynamic::resolved_json_pointer<dynamic>& ptr,
    dynamic&& value,
    json_pointer const& path) {
  using app_err_code = json_patch::patch_application_error_code;
  using res_err_code = dynamic::json_pointer_resolution_error_code;

  auto const& tokens = path.tokens();
  auto const failed_at_last_token = !tokens.empty() && !ptr.hasValue() &&
      ptr.error().index == tokens.size() - 1;

  // element found: see if parent is object or array
  if (ptr.hasValue()) {
    // root element, or key in object - replace (per RFC)
    if (ptr->parent == nullptr || ptr->parent->isObject()) {
      *ptr->value = std::move(value);
    } else if (ptr->parent->isArray()) {
      // valid index in array: insert at index and shift right
      ptr->parent->insert(
          ptr->parent->begin() + ptr->parent_index, std::move(value));
    }
  } else {
    // see if we can add value, based on pointer resolution state
    switch (ptr.error().error_code) {
      // key not found: only the final token may name a new object member
      case res_err_code::key_not_found: {
        if (!failed_at_last_token) {
          return makeUnexpected(app_err_code::other);
        }
        DCHECK(ptr.error().context->isObject());
        ptr.error().context->insert(tokens.back(), std::move(value));
        break;
      }
      // special '-' index in array - do append operation
      case res_err_code::append_requested: {
        if (!failed_at_last_token) {
          return makeUnexpected(app_err_code::other);
        }
        DCHECK(ptr.error().context->isArray());
        ptr.error().context->push_back(std::move(value));
        break;
      }
      // a numeric index equal to the array size is also an append
      case res_err_code::index_out_of_bounds: {
        auto const* context = ptr.error().context;
        if (failed_at_last_token && context != nullptr && context->isArray()) {
          if (auto const index = tryTo<size_t>(tokens.back());
              index.hasValue() && *index == context->size()) {
            ptr.error().context->push_back(std::move(value));
            break;
          }
        }
        return makeUnexpected(app_err_code::other);
      }
      case res_err_code::other:
      case res_err_code::index_not_numeric:
      case res_err_code::index_has_leading_zero:
      case res_err_code::element_not_object_or_array:
      case res_err_code::json_pointer_out_of_bounds:
      default:
        return makeUnexpected(app_err_code::other);
    }
  }
  return unit;
}
} // namespace

// clang-format off
Expected<Unit, json_patch::patch_application_error>
// clang-format on
json_patch::apply(dynamic& obj) const {
  using op_code = patch_operation_code;
  using error_code = patch_application_error_code;
  using error = patch_application_error;

  for (auto&& [index, op] : enumerate(ops_)) {
    auto resolved_path = obj.try_get_ptr(op.path);

    switch (op.op_code) {
      case op_code::test:
        if (!resolved_path.hasValue()) {
          return makeUnexpected(error{error_code::path_not_found, index});
        }
        if (!json_values_equal(*resolved_path->value, *op.value)) {
          return makeUnexpected(error{error_code::test_failed, index});
        }
        break;
      case op_code::remove: {
        auto ret = do_remove(resolved_path);
        if (ret.hasError()) {
          return makeUnexpected(error{ret.error(), index});
        }
        break;
      }
      case op_code::add: {
        DCHECK(op.value.has_value());
        auto ret = do_add(resolved_path, dynamic(*op.value), op.path);
        if (ret.hasError()) {
          return makeUnexpected(error{ret.error(), index});
        }
        break;
      }
      case op_code::replace: {
        if (resolved_path.hasValue()) {
          *resolved_path->value = *op.value;
        } else {
          return makeUnexpected(error{error_code::path_not_found, index});
        }
        break;
      }
      case op_code::move: {
        DCHECK(op.from.has_value());
        auto resolved_from = obj.try_get_ptr(*op.from);
        if (!resolved_from.hasValue()) {
          return makeUnexpected(error{error_code::from_not_found, index});
        }
        if (*op.from == op.path) {
          break;
        }

        dynamic value = std::move(*resolved_from->value);
        {
          auto ret = do_remove(resolved_from);
          if (ret.hasError()) {
            *resolved_from->value = std::move(value);
            return makeUnexpected(error{ret.error(), index});
          }
        }
        {
          auto resolved_destination = obj.try_get_ptr(op.path);
          auto ret = do_add(resolved_destination, std::move(value), op.path);
          if (ret.hasError()) {
            auto rollback_ptr = obj.try_get_ptr(*op.from);
            // NOLINTNEXTLINE(bugprone-use-after-move)
            auto rollback = do_add(rollback_ptr, std::move(value), *op.from);
            DCHECK(rollback.hasValue());
            (void)rollback;
            return makeUnexpected(error{ret.error(), index});
          }
        }
        break;
      }
      case op_code::copy: {
        DCHECK(op.from.has_value());
        auto const resolved_from = obj.try_get_ptr(*op.from);
        if (!resolved_from.hasValue()) {
          return makeUnexpected(error{error_code::from_not_found, index});
        }
        auto ret =
            do_add(resolved_path, dynamic(*resolved_from->value), op.path);
        if (ret.hasError()) {
          return makeUnexpected(error{ret.error(), index});
        }
        break;
      }
      case op_code::invalid: {
        LOG(DFATAL) << "opcode is op_code::invalid";
        return makeUnexpected(error{error_code::other, index});
      }
    }
  }
  return unit;
}

} // namespace folly
