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

#include <folly/settings/Settings.h>

#include <string_view>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <folly/Random.h>
#include <folly/String.h>
#include <folly/observer/detail/ObserverManager.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <folly/settings/Observer.h>
#include <folly/synchronization/test/Barrier.h>

#include <folly/settings/test/a.h>
#include <folly/settings/test/b.h>

namespace a_ns {
// Forward declare this flag even though it's already declared in a.h to test
// that settings can be declared in multiple places
FOLLY_SETTING_DECLARE(follytest, public_flag_to_a, int);
} // namespace a_ns

namespace some_ns {
FOLLY_SETTING_DEFINE(
    follytest,
    some_flag,
    std::string,
    "default",
    folly::settings::Mutability::Mutable,
    folly::settings::CommandLine::AcceptOverrides,
    "Description");
FOLLY_SETTING_DEFINE(
    follytest,
    unused,
    std::string,
    "unused_default",
    folly::settings::Mutability::Mutable,
    folly::settings::CommandLine::AcceptOverrides,
    "Not used, but should still be in the list");
FOLLY_SETTING_DEFINE(
    follytest,
    multi_token_type,
    unsigned int,
    123,
    folly::settings::Mutability::Mutable,
    folly::settings::CommandLine::AcceptOverrides,
    "Test that multi-token type names can be used");
// Enable to test runtime collision checking logic
#if 0
FOLLY_SETTING_DEFINE(
    follytest,
    internal_flag_to_a,
    std::string,
    "collision_with_a",
    folly::settings::Mutability::Mutable,
    folly::settings::CommandLine::AcceptOverrides,
    "Collision_with_a");
#endif

/* Test user defined type support */
struct UserDefinedType {
  explicit UserDefinedType(std::string_view value) {
    if (value == "a") {
      value_ = 0;
    } else if (value == "b") {
      value_ = 100;
    } else {
      throw std::runtime_error("Invalid value passed to UserDefinedType ctor");
    }
  }

  bool operator==(const UserDefinedType& other) const {
    return value_ == other.value_;
  }

  int value_;
};
/* Note: conversion intentionally to different strings to test that this
   function is called */
template <class String>
void toAppend(const UserDefinedType& t, String* out) {
  if (t.value_ == 0) {
    out->append("a_out");
  } else if (t.value_ == 100) {
    out->append("b_out");
  } else {
    throw std::runtime_error("Can't convert UserDefinedType to string");
  }
}

struct UserDefinedWithMeta {
  std::string value_;
};
enum class UserErrorCode {
  Error,
};
std::invalid_argument makeConversionError(
    const UserErrorCode& error, std::string_view) {
  return std::invalid_argument(folly::to<std::string>("UserErrorCode ", error));
}
folly::Expected<folly::Unit, UserErrorCode> convertTo(
    const folly::settings::SettingValueAndMetadata& src,
    UserDefinedWithMeta& out) {
  if (src.value == "error") {
    return folly::makeUnexpected(UserErrorCode::Error);
  }
  out.value_ =
      fmt::format("{}_{}->{}", src.meta.project, src.meta.name, src.value);
  return folly::unit;
}
template <class String>
void toAppend(const UserDefinedWithMeta& t, String* out) {
  out->append(t.value_);
}

struct TrivialUserDefinedType {
  int value() const { return value_; }

  int value_;
};
template <class String>
void toAppend(const TrivialUserDefinedType& t, String* out) {
  out->append(fmt::to_string(t.value_));
}
folly::Expected<folly::Unit, UserErrorCode> convertTo(
    const folly::settings::SettingValueAndMetadata& src,
    TrivialUserDefinedType& out) {
  out.value_ = folly::to<int>(src.value);
  return folly::unit;
}

FOLLY_SETTING_DEFINE(
    follytest,
    user_defined,
    UserDefinedType,
    "b",
    folly::settings::Mutability::Mutable,
    folly::settings::CommandLine::AcceptOverrides,
    "User defined type constructed from string");
FOLLY_SETTING_DEFINE(
    follytest,
    user_defined_with_meta,
    UserDefinedWithMeta,
    {"default"},
    folly::settings::Mutability::Mutable,
    folly::settings::CommandLine::AcceptOverrides,
    "User defined type constructed from string and metadata");
FOLLY_SETTING_DEFINE(
    follytest,
    trivial_user_defined,
    TrivialUserDefinedType,
    TrivialUserDefinedType{123},
    folly::settings::Mutability::Mutable,
    folly::settings::CommandLine::AcceptOverrides,
    "Trivial user defined type");
FOLLY_SETTING_DEFINE(
    follytest,
    immutable_setting,
    UserDefinedType,
    "b",
    folly::settings::Mutability::Immutable,
    folly::settings::CommandLine::AcceptOverrides,
    "User defined type constructed from string");
FOLLY_SETTING_DEFINE(
    otherproj,
    some_flag,
    std::string,
    "default",
    folly::settings::Mutability::Mutable,
    folly::settings::CommandLine::AcceptOverrides,
    "Description");

} // namespace some_ns
namespace {
MATCHER(IsOk, "success but got error") {
  if (arg.hasError()) {
    *result_listener
        << "which is: '" << ::folly::settings::toString(arg.error()) << "'";
    return false;
  }
  return true;
}
MATCHER_P(
    HasErrorCode,
    error_code,
    "error '" + std::string(::folly::settings::toString(error_code)) + "'") {
  if (!arg.hasError()) {
    *result_listener << "which is not an error";
    return false;
  }
  if (arg.error() != error_code) {
    *result_listener
        << "which is: '" << ::folly::settings::toString(arg.error()) << "'";
  }
  return true;
}
} // namespace

TEST(Settings, userDefined) {
  EXPECT_EQ(some_ns::FOLLY_SETTING(follytest, user_defined)->value_, 100);
  {
    folly::settings::Snapshot sn;
    EXPECT_THAT(
        sn.setFromString("follytest_user_defined", "a", "test"), IsOk());
    sn.publish();
    EXPECT_EQ(some_ns::FOLLY_SETTING(follytest, user_defined)->value_, 0);
  }
  {
    folly::settings::Snapshot sn;
    auto info = sn.getAsString("follytest_user_defined");
    EXPECT_TRUE(info.has_value());
    EXPECT_EQ(info->first, "a_out");
    EXPECT_EQ(info->second, "test");
  }
  {
    folly::settings::Snapshot sn;
    EXPECT_THROW(
        sn.setFromString("follytest_user_defined", "c", "test2"),
        std::runtime_error);
    sn.publish();
    EXPECT_EQ(some_ns::FOLLY_SETTING(follytest, user_defined)->value_, 0);
  }
  {
    folly::settings::Snapshot sn;
    auto info = sn.getAsString("follytest_user_defined");
    EXPECT_TRUE(info.has_value());
    EXPECT_EQ(info->first, "a_out");
    EXPECT_EQ(info->second, "test");
  }
  {
    folly::settings::Snapshot sn;
    EXPECT_THAT(sn.resetToDefault("follytest_user_defined"), IsOk());
    sn.publish();
    EXPECT_EQ(some_ns::FOLLY_SETTING(follytest, user_defined)->value_, 100);
  }
  {
    folly::settings::Snapshot sn;
    auto info = sn.getAsString("follytest_user_defined");
    EXPECT_TRUE(info.has_value());
    EXPECT_EQ(info->first, "b_out");
    EXPECT_EQ(info->second, "default");
  }
  /* Test that intentionally setting to something non-converteable fails */
  some_ns::UserDefinedType bad("a");
  bad.value_ = 50;
  EXPECT_THROW(
      some_ns::FOLLY_SETTING(follytest, user_defined).set(bad),
      std::runtime_error);
  EXPECT_EQ(some_ns::FOLLY_SETTING(follytest, user_defined)->value_, 100);
  {
    folly::settings::Snapshot sn;
    auto info = sn.getAsString("follytest_user_defined");
    EXPECT_TRUE(info.has_value());
    EXPECT_EQ(info->first, "b_out");
    EXPECT_EQ(info->second, "default");
  }
}

TEST(Settings, basic) {
  EXPECT_EQ(a_ns::a_func(), 1245);
  EXPECT_EQ(b_ns::b_func(), "testbasdf");
  EXPECT_EQ(*some_ns::FOLLY_SETTING(follytest, some_flag), "default");
  // Test -> API
  EXPECT_EQ(some_ns::FOLLY_SETTING(follytest, some_flag)->size(), 7);
  a_ns::FOLLY_SETTING(follytest, public_flag_to_a).set(100);
  EXPECT_EQ(*a_ns::FOLLY_SETTING(follytest, public_flag_to_a), 100);
  EXPECT_EQ(a_ns::getRemote(), 100);
  a_ns::setRemote(200);
  EXPECT_EQ(*a_ns::FOLLY_SETTING(follytest, public_flag_to_a), 200);
  EXPECT_EQ(a_ns::getRemote(), 200);
  {
    folly::settings::Snapshot sn;
    auto res = sn.getAsString("follytest_public_flag_to_a");
    EXPECT_TRUE(res.has_value());
    EXPECT_EQ(res->first, "200");
    EXPECT_EQ(res->second, "remote_set");
  }
  {
    auto meta = folly::settings::getSettingsMeta("follytest_public_flag_to_a");
    EXPECT_TRUE(meta.has_value());
    const auto& md = meta.value();
    EXPECT_EQ(md.project, "follytest");
    EXPECT_EQ(md.name, "public_flag_to_a");
    EXPECT_EQ(md.typeStr, "int");
    EXPECT_EQ(md.typeId, typeid(int));
  }
  {
    auto meta = folly::settings::getSettingsMeta("follytest_some_flag");
    EXPECT_TRUE(meta.has_value());
    const auto& md = meta.value();
    EXPECT_EQ(md.project, "follytest");
    EXPECT_EQ(md.name, "some_flag");
    EXPECT_EQ(md.typeStr, "std::string");
    EXPECT_EQ(md.typeId, typeid(std::string));
  }
  {
    folly::settings::Snapshot sn;
    auto res = sn.getAsString("follytest_nonexisting");
    EXPECT_FALSE(res.has_value());
  }
  {
    folly::settings::Snapshot sn;
    EXPECT_THAT(
        sn.setFromString("follytest_public_flag_to_a", "300", "from_string"),
        IsOk());
    sn.publish();
    EXPECT_EQ(*a_ns::FOLLY_SETTING(follytest, public_flag_to_a), 300);
  }
  EXPECT_EQ(a_ns::getRemote(), 300);
  {
    folly::settings::Snapshot sn;
    auto res = sn.getAsString("follytest_public_flag_to_a");
    EXPECT_TRUE(res.has_value());
    EXPECT_EQ(res->first, "300");
    EXPECT_EQ(res->second, "from_string");
  }
  {
    folly::settings::Snapshot sn;
    EXPECT_THAT(
        sn.setFromString("follytest_nonexisting", "300", "from_string"),
        HasErrorCode(folly::settings::SetErrorCode::NotFound));
  }
  EXPECT_EQ(
      some_ns::FOLLY_SETTING(follytest, multi_token_type).defaultValue(), 123);
  EXPECT_EQ(
      a_ns::FOLLY_SETTING(follytest, public_flag_to_a).defaultValue(), 456);
  EXPECT_EQ(
      b_ns::FOLLY_SETTING(follytest, public_flag_to_b).defaultValue(), "basdf");
  EXPECT_EQ(
      some_ns::FOLLY_SETTING(follytest, some_flag).defaultValue(), "default");
  EXPECT_EQ(
      some_ns::FOLLY_SETTING(follytest, user_defined).defaultValue(),
      some_ns::UserDefinedType("b"));
  EXPECT_EQ(
      *folly::settings::getDefaultValue<unsigned int>(
          "follytest_multi_token_type"),
      some_ns::FOLLY_SETTING(follytest, multi_token_type).defaultValue());
  EXPECT_EQ(
      *folly::settings::getDefaultValue<std::string>("follytest_some_flag"),
      some_ns::FOLLY_SETTING(follytest, some_flag).defaultValue());
  EXPECT_EQ(
      *folly::settings::getDefaultValue<some_ns::UserDefinedType>(
          "follytest_user_defined"),
      some_ns::FOLLY_SETTING(follytest, user_defined).defaultValue());
  // EXPECT_FALSE(folly::settings::getDefaultValue<int>("follytest_some_flag"));
  // EXPECT_FALSE(folly::settings::getDefaultValue<int>("follytest_nonexisting"));

  {
    std::string allFlags;
    auto allMeta = folly::settings::getAllSettingsMeta();
    size_t i = 0;
    folly::settings::Snapshot sn;
    sn.forEachSetting([&](const auto& setting) {
      auto& meta = setting.meta();
      auto& foundMeta = allMeta.at(i++);
      EXPECT_EQ(meta.project, foundMeta.project);
      EXPECT_EQ(meta.name, foundMeta.name);
      EXPECT_EQ(meta.typeStr, foundMeta.typeStr);
      EXPECT_EQ(meta.defaultStr, foundMeta.defaultStr);
      EXPECT_EQ(meta.description, foundMeta.description);
      EXPECT_EQ(meta.mutability, foundMeta.mutability);

      if (meta.typeId == typeid(int)) {
        EXPECT_EQ(meta.typeStr, "int");
      } else if (meta.typeId == typeid(std::string)) {
        EXPECT_EQ(meta.typeStr, "std::string");
      } else if (meta.typeId == typeid(unsigned int)) {
        EXPECT_EQ(meta.typeStr, "unsigned int");
      } else if (meta.typeId == typeid(some_ns::UserDefinedType)) {
        EXPECT_EQ(meta.typeStr, "UserDefinedType");
      } else if (meta.typeId == typeid(some_ns::UserDefinedWithMeta)) {
        EXPECT_EQ(meta.typeStr, "UserDefinedWithMeta");
      } else if (meta.typeId == typeid(some_ns::TrivialUserDefinedType)) {
        EXPECT_EQ(meta.typeStr, "TrivialUserDefinedType");
      } else {
        FAIL() << "Unexpected type: " << meta.typeStr;
      }
      auto [value, reason] = setting.valueAndReason();
      EXPECT_EQ(reason, setting.updateReason());
      allFlags += fmt::format(
          "{}/{}/{}/{}/{}/{}/{}\n",
          meta.project,
          meta.name,
          meta.typeStr,
          meta.defaultStr,
          meta.description,
          value,
          reason);
    });
    auto allFlagsString = folly::stripLeftMargin(R"MESSAGE(
      follytest/immutable_setting/UserDefinedType/"b"/User defined type constructed from string/b_out/default
      follytest/internal_flag_to_a/int/789/Desc of int/789/default
      follytest/internal_flag_to_b/std::string/"test"/Desc of str/test/default
      follytest/multi_token_type/unsigned int/123/Test that multi-token type names can be used/123/default
      follytest/public_flag_to_a/int/456/Public flag to a/300/from_string
      follytest/public_flag_to_b/std::string/"basdf"/Public flag to b/basdf/default
      follytest/some_flag/std::string/"default"/Description/default/default
      follytest/some_unused_flag/std::string/"default"/Description/default/default
      follytest/trivial_user_defined/TrivialUserDefinedType/TrivialUserDefinedType{123}/Trivial user defined type/123/default
      follytest/unused/std::string/"unused_default"/Not used, but should still be in the list/unused_default/default
      follytest/user_defined/UserDefinedType/"b"/User defined type constructed from string/b_out/default
      follytest/user_defined_with_meta/UserDefinedWithMeta/{"default"}/User defined type constructed from string and metadata/default/default
      otherproj/some_flag/std::string/"default"/Description/default/default
  )MESSAGE");
    EXPECT_EQ(allFlags, allFlagsString);
  }
  {
    folly::settings::Snapshot sn;
    EXPECT_THAT(sn.resetToDefault("follytest_public_flag_to_a"), IsOk());
    sn.publish();
    EXPECT_EQ(*a_ns::FOLLY_SETTING(follytest, public_flag_to_a), 456);
    EXPECT_EQ(a_ns::getRemote(), 456);
  }
  {
    folly::settings::Snapshot sn;
    EXPECT_THAT(
        sn.resetToDefault("follytest_nonexisting"),
        HasErrorCode(folly::settings::SetErrorCode::NotFound));
  }
}

TEST(Settings, snapshot) {
  // Test discarding a snapshot
  {
    folly::settings::Snapshot snapshot;

    EXPECT_EQ(*some_ns::FOLLY_SETTING(follytest, some_flag), "default");
    EXPECT_EQ(
        *snapshot(some_ns::FOLLY_SETTING(follytest, some_flag)), "default");

    // Set the global value, snapshot doesn't see it
    some_ns::FOLLY_SETTING(follytest, some_flag).set("global_value", "global");
    EXPECT_EQ(*some_ns::FOLLY_SETTING(follytest, some_flag), "global_value");
    EXPECT_EQ(
        *snapshot(some_ns::FOLLY_SETTING(follytest, some_flag)), "default");
    EXPECT_EQ(
        some_ns::FOLLY_SETTING(follytest, some_flag).value(snapshot),
        "default");

    // Set the value in the snapshot only
    snapshot(some_ns::FOLLY_SETTING(follytest, some_flag))
        .set("snapshot_value");
    EXPECT_EQ(*some_ns::FOLLY_SETTING(follytest, some_flag), "global_value");
    EXPECT_EQ(
        some_ns::FOLLY_SETTING(follytest, some_flag).value(), "global_value");
    EXPECT_EQ(
        *snapshot(some_ns::FOLLY_SETTING(follytest, some_flag)),
        "snapshot_value");
    EXPECT_EQ(
        some_ns::FOLLY_SETTING(follytest, some_flag).value(snapshot),
        "snapshot_value");

    // Set the update reason in the snapshot only
    EXPECT_EQ(
        some_ns::FOLLY_SETTING(follytest, some_flag).updateReason(), "global");
    EXPECT_EQ(
        some_ns::FOLLY_SETTING(follytest, some_flag).updateReason(snapshot),
        "api");
  }
  // Discard the snapshot
  EXPECT_EQ(*some_ns::FOLLY_SETTING(follytest, some_flag), "global_value");

  // Test publishing a snapshot
  {
    folly::settings::Snapshot snapshot;

    // Set the value in the snapshot only
    EXPECT_EQ(*some_ns::FOLLY_SETTING(follytest, some_flag), "global_value");
    EXPECT_EQ(
        *snapshot(some_ns::FOLLY_SETTING(follytest, some_flag)),
        "global_value");
    EXPECT_EQ(
        some_ns::FOLLY_SETTING(follytest, some_flag).value(snapshot),
        "global_value");
    snapshot(some_ns::FOLLY_SETTING(follytest, some_flag))
        .set("snapshot_value2");
    EXPECT_EQ(*some_ns::FOLLY_SETTING(follytest, some_flag), "global_value");
    EXPECT_EQ(
        *snapshot(some_ns::FOLLY_SETTING(follytest, some_flag)),
        "snapshot_value2");
    EXPECT_EQ(
        some_ns::FOLLY_SETTING(follytest, some_flag).value(snapshot),
        "snapshot_value2");

    // Set the global value, snapshot doesn't see it
    some_ns::FOLLY_SETTING(follytest, some_flag).set("global_value2");
    EXPECT_EQ(*some_ns::FOLLY_SETTING(follytest, some_flag), "global_value2");
    EXPECT_EQ(
        *snapshot(some_ns::FOLLY_SETTING(follytest, some_flag)),
        "snapshot_value2");
    EXPECT_EQ(
        some_ns::FOLLY_SETTING(follytest, some_flag).value(snapshot),
        "snapshot_value2");
    snapshot.publish();
  }

  EXPECT_EQ(*some_ns::FOLLY_SETTING(follytest, some_flag), "snapshot_value2");

  // Snapshots at different points in time
  {
    some_ns::FOLLY_SETTING(follytest, some_flag).set("a");
    a_ns::FOLLY_SETTING(follytest, public_flag_to_a).set(123);

    folly::settings::Snapshot snapshot_1;

    EXPECT_EQ(*some_ns::FOLLY_SETTING(follytest, some_flag), "a");
    EXPECT_EQ(some_ns::FOLLY_SETTING(follytest, some_flag).value(), "a");
    EXPECT_EQ(*snapshot_1(some_ns::FOLLY_SETTING(follytest, some_flag)), "a");
    EXPECT_EQ(
        some_ns::FOLLY_SETTING(follytest, some_flag).value(snapshot_1), "a");

    EXPECT_EQ(*a_ns::FOLLY_SETTING(follytest, public_flag_to_a), 123);
    EXPECT_EQ(a_ns::FOLLY_SETTING(follytest, public_flag_to_a).value(), 123);
    EXPECT_EQ(
        *snapshot_1(a_ns::FOLLY_SETTING(follytest, public_flag_to_a)), 123);
    EXPECT_EQ(
        a_ns::FOLLY_SETTING(follytest, public_flag_to_a).value(snapshot_1),
        123);

    some_ns::FOLLY_SETTING(follytest, some_flag).set("b");
    EXPECT_EQ(*some_ns::FOLLY_SETTING(follytest, some_flag), "b");
    EXPECT_EQ(some_ns::FOLLY_SETTING(follytest, some_flag).value(), "b");
    EXPECT_EQ(*snapshot_1(some_ns::FOLLY_SETTING(follytest, some_flag)), "a");
    EXPECT_EQ(
        some_ns::FOLLY_SETTING(follytest, some_flag).value(snapshot_1), "a");

    EXPECT_EQ(*a_ns::FOLLY_SETTING(follytest, public_flag_to_a), 123);
    EXPECT_EQ(a_ns::FOLLY_SETTING(follytest, public_flag_to_a).value(), 123);
    EXPECT_EQ(
        *snapshot_1(a_ns::FOLLY_SETTING(follytest, public_flag_to_a)), 123);
    EXPECT_EQ(
        a_ns::FOLLY_SETTING(follytest, public_flag_to_a).value(snapshot_1),
        123);

    folly::settings::Snapshot snapshot_2;
    EXPECT_EQ(*some_ns::FOLLY_SETTING(follytest, some_flag), "b");
    EXPECT_EQ(some_ns::FOLLY_SETTING(follytest, some_flag).value(), "b");
    EXPECT_EQ(*snapshot_1(some_ns::FOLLY_SETTING(follytest, some_flag)), "a");
    EXPECT_EQ(*snapshot_2(some_ns::FOLLY_SETTING(follytest, some_flag)), "b");
    EXPECT_EQ(
        some_ns::FOLLY_SETTING(follytest, some_flag).value(snapshot_1), "a");
    EXPECT_EQ(
        some_ns::FOLLY_SETTING(follytest, some_flag).value(snapshot_2), "b");

    EXPECT_EQ(*a_ns::FOLLY_SETTING(follytest, public_flag_to_a), 123);
    EXPECT_EQ(a_ns::FOLLY_SETTING(follytest, public_flag_to_a).value(), 123);
    EXPECT_EQ(
        *snapshot_1(a_ns::FOLLY_SETTING(follytest, public_flag_to_a)), 123);
    EXPECT_EQ(
        *snapshot_2(a_ns::FOLLY_SETTING(follytest, public_flag_to_a)), 123);
    EXPECT_EQ(
        a_ns::FOLLY_SETTING(follytest, public_flag_to_a).value(snapshot_1),
        123);
    EXPECT_EQ(
        a_ns::FOLLY_SETTING(follytest, public_flag_to_a).value(snapshot_2),
        123);

    some_ns::FOLLY_SETTING(follytest, some_flag).set("c");
    EXPECT_EQ(*some_ns::FOLLY_SETTING(follytest, some_flag), "c");
    EXPECT_EQ(some_ns::FOLLY_SETTING(follytest, some_flag).value(), "c");
    EXPECT_EQ(*snapshot_1(some_ns::FOLLY_SETTING(follytest, some_flag)), "a");
    EXPECT_EQ(*snapshot_2(some_ns::FOLLY_SETTING(follytest, some_flag)), "b");
    EXPECT_EQ(
        some_ns::FOLLY_SETTING(follytest, some_flag).value(snapshot_1), "a");
    EXPECT_EQ(
        some_ns::FOLLY_SETTING(follytest, some_flag).value(snapshot_2), "b");

    EXPECT_EQ(*a_ns::FOLLY_SETTING(follytest, public_flag_to_a), 123);
    EXPECT_EQ(a_ns::FOLLY_SETTING(follytest, public_flag_to_a).value(), 123);
    EXPECT_EQ(
        *snapshot_1(a_ns::FOLLY_SETTING(follytest, public_flag_to_a)), 123);
    EXPECT_EQ(
        *snapshot_2(a_ns::FOLLY_SETTING(follytest, public_flag_to_a)), 123);
    EXPECT_EQ(
        a_ns::FOLLY_SETTING(follytest, public_flag_to_a).value(snapshot_1),
        123);
    EXPECT_EQ(
        a_ns::FOLLY_SETTING(follytest, public_flag_to_a).value(snapshot_2),
        123);

    a_ns::FOLLY_SETTING(follytest, public_flag_to_a).set(456);
    EXPECT_EQ(*some_ns::FOLLY_SETTING(follytest, some_flag), "c");
    EXPECT_EQ(some_ns::FOLLY_SETTING(follytest, some_flag).value(), "c");
    EXPECT_EQ(*snapshot_1(some_ns::FOLLY_SETTING(follytest, some_flag)), "a");
    EXPECT_EQ(*snapshot_2(some_ns::FOLLY_SETTING(follytest, some_flag)), "b");
    EXPECT_EQ(
        some_ns::FOLLY_SETTING(follytest, some_flag).value(snapshot_1), "a");
    EXPECT_EQ(
        some_ns::FOLLY_SETTING(follytest, some_flag).value(snapshot_2), "b");

    EXPECT_EQ(*a_ns::FOLLY_SETTING(follytest, public_flag_to_a), 456);
    EXPECT_EQ(a_ns::FOLLY_SETTING(follytest, public_flag_to_a).value(), 456);
    EXPECT_EQ(
        *snapshot_1(a_ns::FOLLY_SETTING(follytest, public_flag_to_a)), 123);
    EXPECT_EQ(
        *snapshot_2(a_ns::FOLLY_SETTING(follytest, public_flag_to_a)), 123);
    EXPECT_EQ(
        a_ns::FOLLY_SETTING(follytest, public_flag_to_a).value(snapshot_1),
        123);
    EXPECT_EQ(
        a_ns::FOLLY_SETTING(follytest, public_flag_to_a).value(snapshot_2),
        123);
  }
}

TEST(SettingsTest, callback) {
  size_t callbackInvocations = 0;
  std::string lastCallbackValue;

  EXPECT_EQ(*some_ns::FOLLY_SETTING(follytest, some_flag), "default");
  {
    auto handle =
        some_ns::FOLLY_SETTING(follytest, some_flag)
            .addCallback([&](const auto& contents) {
              ++callbackInvocations;
              lastCallbackValue = contents.value;
            });

    some_ns::FOLLY_SETTING(follytest, some_flag).set("a");
    EXPECT_EQ(callbackInvocations, 1);
    EXPECT_EQ(lastCallbackValue, "a");

    size_t secondCallbackInvocations = 0;
    // Test adding multiple callbacks and letting the handle go out of scope
    {
      auto secondHandle =
          some_ns::FOLLY_SETTING(follytest, some_flag)
              .addCallback([&](const auto& /* contents */) {
                ++secondCallbackInvocations;
              });
      some_ns::FOLLY_SETTING(follytest, some_flag).set("b");
      EXPECT_EQ(callbackInvocations, 2);
      EXPECT_EQ(lastCallbackValue, "b");
      EXPECT_EQ(secondCallbackInvocations, 1);
    }

    some_ns::FOLLY_SETTING(follytest, some_flag).set("c");
    EXPECT_EQ(callbackInvocations, 3);
    EXPECT_EQ(lastCallbackValue, "c");
    // Second callback no longer invoked
    EXPECT_EQ(secondCallbackInvocations, 1);

    auto movedHandle = std::move(handle);
    some_ns::FOLLY_SETTING(follytest, some_flag).set("d");
    EXPECT_EQ(callbackInvocations, 4);
    EXPECT_EQ(lastCallbackValue, "d");
  }
  // Main callback no longer invoked
  some_ns::FOLLY_SETTING(follytest, some_flag).set("e");
  EXPECT_EQ(callbackInvocations, 4);
  EXPECT_EQ(lastCallbackValue, "d");

  folly::settings::Snapshot snapshot;
  snapshot.forEachSetting([](const auto& s) {
    EXPECT_EQ(s.hasHadCallbacks(), s.fullName() == "follytest_some_flag");
  });
}

TEST(SettingsTest, observers) {
  auto observer = folly::settings::getObserver(
      some_ns::FOLLY_SETTING(follytest, some_flag));
  EXPECT_EQ(observer.getCreatorName(), "FOLLY_SETTING_follytest_some_flag");
  size_t callbackCount = 0;
  std::string updatedFromCallback;
  auto callbackHandle = observer.addCallback([&](auto snapshot) {
    ++callbackCount;
    updatedFromCallback = *snapshot;
  });
  EXPECT_EQ(**observer, "default");
  EXPECT_EQ(updatedFromCallback, "default");
  EXPECT_EQ(callbackCount, 1);
  some_ns::FOLLY_SETTING(follytest, some_flag).set("new value");
  folly::observer_detail::ObserverManager::waitForAllUpdates();
  EXPECT_EQ(**observer, "new value");
  EXPECT_EQ(updatedFromCallback, "new value");
  EXPECT_EQ(callbackCount, 2);
  // Check that the callback is not invoked if set to the same value
  some_ns::FOLLY_SETTING(follytest, some_flag).set("new value");
  folly::observer_detail::ObserverManager::waitForAllUpdates();
  EXPECT_EQ(callbackCount, 2);

  folly::settings::Snapshot snapshot;
  snapshot.forEachSetting([](const auto& s) {
    EXPECT_EQ(s.hasHadCallbacks(), s.fullName() == "follytest_some_flag");
  });
  // Check we can create an observer for a custom setting type that doesn't
  // implement operator==
  some_ns::FOLLY_SETTING(follytest, user_defined_with_meta).observer();
}

TEST(SettingsTest, accessWithObserver) {
  auto observer1 = folly::observer::makeObserver([]() {
    return *some_ns::FOLLY_SETTING(follytest, some_flag);
  });
  auto observer2 = folly::observer::makeObserver([]() {
    return some_ns::FOLLY_SETTING(follytest, some_flag)
        .valueRegisterObserverDependency();
  });
  ASSERT_EQ(**observer1, "default");
  ASSERT_EQ(**observer2, "default");
  some_ns::FOLLY_SETTING(follytest, some_flag).set("new value");
  folly::observer_detail::ObserverManager::waitForAllUpdates();
  EXPECT_EQ(**observer1, "default");
  EXPECT_EQ(**observer2, "new value");
}

TEST(Settings, immutables) {
  EXPECT_EQ(some_ns::FOLLY_SETTING(follytest, immutable_setting)->value_, 100);
  EXPECT_EQ(*some_ns::FOLLY_SETTING(otherproj, some_flag), "default");

  // Check these can be changed before immutables are frozen
  folly::settings::Snapshot sn;
  EXPECT_THAT(
      sn.setFromString("follytest_immutable_setting", "a", "test"), IsOk());
  EXPECT_THAT(
      sn.setFromString("otherproj_some_flag", "value_1", "test"), IsOk());
  sn.publish();
  EXPECT_EQ(some_ns::FOLLY_SETTING(follytest, immutable_setting)->value_, 0);
  EXPECT_EQ(*some_ns::FOLLY_SETTING(otherproj, some_flag), "value_1");

  // Create a snapshot before freezing immutables
  folly::settings::Snapshot beforeFreezeSn;
  EXPECT_THAT(
      beforeFreezeSn.resetToDefault("follytest_immutable_setting"), IsOk());
  EXPECT_THAT(
      beforeFreezeSn.setFromString("follytest_immutable_setting", "b", "test"),
      IsOk());
  EXPECT_THAT(
      beforeFreezeSn.setFromString("otherproj_some_flag", "value_2", "test"),
      IsOk());

  ASSERT_FALSE(folly::settings::immutablesFrozen("follytest"));
  ASSERT_FALSE(folly::settings::frozenSettingProjects().contains("follytest"));

  // Freeze "follytest" immutable settings
  folly::settings::freezeImmutables({"follytest"});

  ASSERT_TRUE(folly::settings::immutablesFrozen("follytest"));
  ASSERT_TRUE(folly::settings::frozenSettingProjects().contains("follytest"));
  ASSERT_FALSE(folly::settings::immutablesFrozen("otherproj"));
  ASSERT_FALSE(folly::settings::frozenSettingProjects().contains("otherproj"));

  // Check that even though 'immutable_setting' was successfully set before
  // settings were frozen, it still can't publish the new value now that
  // immutables are frozen. However, it still can change otherproj_some_flag
  // since that project has not been frozen yet.
  beforeFreezeSn.publish();
  EXPECT_EQ(some_ns::FOLLY_SETTING(follytest, immutable_setting)->value_, 0);
  EXPECT_EQ(*some_ns::FOLLY_SETTING(otherproj, some_flag), "value_2");

  // Check newly created snapshots fail mutation attempts.
  folly::settings::Snapshot afterFreezeSn;
  EXPECT_THAT(
      afterFreezeSn.setFromString("follytest_immutable_setting", "b", "test"),
      HasErrorCode(folly::settings::SetErrorCode::FrozenImmutable));
  EXPECT_THAT(
      afterFreezeSn.resetToDefault("follytest_immutable_setting"),
      HasErrorCode(folly::settings::SetErrorCode::FrozenImmutable));
  EXPECT_THAT(
      some_ns::FOLLY_SETTING(follytest, immutable_setting)
          .set(some_ns::UserDefinedType("b")),
      HasErrorCode(folly::settings::SetErrorCode::FrozenImmutable));

  // Check we don't attempt to contruct immutable settings from strings if
  // immutables have already been frozen. If we did, the below code would throw
  // an exception.
  some_ns::UserDefinedType bad("a");
  bad.value_ = 50;
  EXPECT_THAT(
      some_ns::FOLLY_SETTING(follytest, immutable_setting).set(bad),
      HasErrorCode(folly::settings::SetErrorCode::FrozenImmutable));
  EXPECT_THAT(
      afterFreezeSn.setFromString("follytest_immutable_setting", "c", "test"),
      HasErrorCode(folly::settings::SetErrorCode::FrozenImmutable));

  // Check forceSetFromString does construct immutable settings even if
  // immutables have already been frozen.
  try {
    afterFreezeSn.forceSetFromString("follytest_immutable_setting", "c", "ds");
    FAIL();
  } catch (const std::runtime_error& ex) {
    EXPECT_STREQ(ex.what(), "Invalid value passed to UserDefinedType ctor");
  }

  {
    // Check forceSetFromString does construct immutable settings even if
    // immutables have already been frozen.
    folly::settings::Snapshot newSnapshot;
    try {
      newSnapshot.forceSetFromString("follytest_immutable_setting", "c", "dsc");
      FAIL();
    } catch (const std::runtime_error& ex) {
      EXPECT_STREQ(ex.what(), "Invalid value passed to UserDefinedType ctor");
    }

    // Check forceSetFromString can set immutable settings in the snapshot but
    // will still not publish them
    EXPECT_THAT(
        newSnapshot.forceSetFromString(
            "follytest_immutable_setting", "b", "new desc"),
        IsOk());
    auto found = newSnapshot.getAsString("follytest_immutable_setting");
    ASSERT_TRUE(found.hasValue());
    EXPECT_EQ(found->first, "b_out");
    EXPECT_EQ(found->second, "new desc");
    newSnapshot.publish();
    EXPECT_EQ(some_ns::FOLLY_SETTING(follytest, immutable_setting)->value_, 0);
  }

  {
    // Check forceResetToDefault can reset immutable settings in the snapshot
    // but will still not publish them
    folly::settings::Snapshot newSnapshot;
    auto found = newSnapshot.getAsString("follytest_immutable_setting");
    ASSERT_TRUE(found.hasValue());
    EXPECT_EQ(found->first, "a_out");
    EXPECT_EQ(found->second, "test");
    EXPECT_THAT(
        newSnapshot.forceResetToDefault("follytest_immutable_setting"), IsOk());
    found = newSnapshot.getAsString("follytest_immutable_setting");
    ASSERT_TRUE(found.hasValue());
    EXPECT_EQ(found->first, "b_out");
    EXPECT_EQ(found->second, "default");
    newSnapshot.publish();
    EXPECT_EQ(some_ns::FOLLY_SETTING(follytest, immutable_setting)->value_, 0);
  }
}

TEST(Settings, userDefinedConversionWithMetadata) {
  EXPECT_EQ(
      some_ns::FOLLY_SETTING(follytest, user_defined_with_meta)->value_,
      "default");
  folly::settings::Snapshot sn;
  EXPECT_THAT(
      sn.setFromString("follytest_user_defined_with_meta", "new", "test"),
      IsOk());
  try {
    sn.setFromString("follytest_user_defined_with_meta", "error", "test");
    FAIL();
  } catch (const std::invalid_argument& ex) {
    EXPECT_STREQ(ex.what(), "UserErrorCode 0");
  }
  sn.publish();
  EXPECT_EQ(
      some_ns::FOLLY_SETTING(follytest, user_defined_with_meta)->value_,
      "follytest_user_defined_with_meta->new");
}

TEST(Settings, accessCount) {
  {
    folly::settings::Snapshot sn;
    sn.forEachSetting([](auto setting) {
      EXPECT_EQ(setting.accessCount(), 0);
    });
  }

  // Check updateReason does not count as an access
  EXPECT_EQ(
      some_ns::FOLLY_SETTING(follytest, multi_token_type).accessCount(), 0);
  EXPECT_EQ(
      some_ns::FOLLY_SETTING(follytest, multi_token_type).updateReason(),
      "default");
  EXPECT_EQ(
      some_ns::FOLLY_SETTING(follytest, multi_token_type).accessCount(), 0);

  {
    // Check accessing a setting in a snapshot does not count as an access
    EXPECT_EQ(some_ns::FOLLY_SETTING(follytest, some_flag).accessCount(), 0);
    folly::settings::Snapshot sn;
    EXPECT_EQ(*sn(some_ns::FOLLY_SETTING(follytest, some_flag)), "default");
    EXPECT_EQ(
        some_ns::FOLLY_SETTING(follytest, some_flag).value(sn), "default");
    EXPECT_EQ(some_ns::FOLLY_SETTING(follytest, some_flag).accessCount(), 0);
  }

  // Check trivial setting access
  EXPECT_EQ(*some_ns::FOLLY_SETTING(follytest, multi_token_type), 123);
  EXPECT_EQ(*some_ns::FOLLY_SETTING(follytest, multi_token_type), 123);
  EXPECT_EQ(
      some_ns::FOLLY_SETTING(follytest, multi_token_type).accessCount(), 2);

  // Check non-trivial setting access
  EXPECT_EQ(some_ns::FOLLY_SETTING(follytest, some_flag).accessCount(), 0);
  EXPECT_EQ(*some_ns::FOLLY_SETTING(follytest, some_flag), "default");
  EXPECT_EQ(some_ns::FOLLY_SETTING(follytest, some_flag).accessCount(), 1);

  // Check trival access via ->
  static_assert(
      folly::settings::detail::IsSmallPOD<some_ns::TrivialUserDefinedType>);
  EXPECT_EQ(
      some_ns::FOLLY_SETTING(follytest, trivial_user_defined).accessCount(), 0);
  EXPECT_EQ(
      some_ns::FOLLY_SETTING(follytest, trivial_user_defined)->value(), 123);
  EXPECT_EQ(
      some_ns::FOLLY_SETTING(follytest, trivial_user_defined).accessCount(), 1);

  folly::settings::Snapshot sn;
  sn.forEachSetting([](const auto& setting) {
    EXPECT_EQ(
        setting.accessCount() > 0,
        setting.fullName() == "follytest_multi_token_type" ||
            setting.fullName() == "follytest_some_flag" ||
            setting.fullName() == "follytest_trivial_user_defined")
        << setting.fullName();
  });
}

TEST(Settings, concurrentAccessCount) {
  EXPECT_EQ(some_ns::FOLLY_SETTING(follytest, some_flag).accessCount(), 0);
  const size_t numThreads = 16;
  const size_t numAccessesPerThread = 10'000;
  std::vector<std::thread> threads;
  folly::test::Barrier barrier(numThreads + 1);
  for (size_t i = 0; i < numThreads; ++i) {
    threads.emplace_back([&]() {
      barrier.wait();
      for (size_t j = 0; j < numAccessesPerThread; ++j) {
        ASSERT_EQ(*some_ns::FOLLY_SETTING(follytest, some_flag), "default");
      }
    });
  }
  barrier.wait();
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(
      some_ns::FOLLY_SETTING(follytest, some_flag).accessCount(),
      numThreads * numAccessesPerThread);
}

TEST(Settings, concurrentReadersAndWriters) {
  some_ns::FOLLY_SETTING(follytest, some_flag).set("0");
  const size_t numGetThreads = 16;
  const size_t numGetsPerThread = 1'000;
  const size_t numSetThreads = 4;
  const size_t numSetsPerThread = 100'000;
  const size_t numPublishThreads = 4;
  const size_t numPublishesPerThread = 10'000;

  std::vector<std::thread> getThreads(numGetThreads);
  std::vector<std::thread> setThreads(numSetThreads);
  std::vector<std::thread> publishThreads(numPublishThreads);

  folly::test::Barrier barrier(
      numGetThreads + numSetThreads + numPublishThreads + 1);

  for (auto& getThread : getThreads) {
    getThread = std::thread([&]() {
      barrier.wait();
      for (size_t j = 0; j < numGetsPerThread; ++j) {
        auto& value = *some_ns::FOLLY_SETTING(follytest, some_flag);
        auto sleepMs = folly::Random::rand32(10);
        /* sleep override */
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        ASSERT_GE(folly::to<int>(value), 0);
      }
    });
  }
  for (auto& setThread : setThreads) {
    setThread = std::thread([&]() {
      barrier.wait();
      for (size_t j = 0; j < numSetsPerThread; ++j) {
        some_ns::FOLLY_SETTING(follytest, some_flag)
            .set(folly::to<std::string>(j));
      }
    });
  }
  for (auto& publishThread : publishThreads) {
    publishThread = std::thread([&]() {
      barrier.wait();
      for (size_t j = 0; j < numPublishesPerThread; ++j) {
        folly::settings::Snapshot snapshot;
        snapshot(some_ns::FOLLY_SETTING(follytest, some_flag))
            .set(folly::to<std::string>(j));
        snapshot.publish();
      }
    });
  }

  barrier.wait();
  for (auto& getThread : getThreads) {
    getThread.join();
  }
  for (auto& setThread : setThreads) {
    setThread.join();
  }
  for (auto& publishThread : publishThreads) {
    publishThread.join();
  }
}

TEST(Settings, settingReferenceGuarantees) {
  {
    auto& value = *some_ns::FOLLY_SETTING(follytest, some_flag);
    *some_ns::FOLLY_SETTING(follytest, some_flag); // Invalidates value
    if constexpr (!folly::kIsLibrarySanitizeAddress) {
      // Check this is safe (although risky) if we're not running with ASAN
      // since the setting was not actually updated.
      EXPECT_EQ(value, "default");
    }
  }
  {
    auto& value = *some_ns::FOLLY_SETTING(follytest, some_flag);
    some_ns::FOLLY_SETTING(follytest, some_flag).set("abc");
    EXPECT_EQ(value, "default");
  }
  {
    auto& value = *some_ns::FOLLY_SETTING(follytest, some_flag);
    folly::settings::Snapshot snapshot;
    snapshot(some_ns::FOLLY_SETTING(follytest, some_flag)).set("def");
    snapshot.publish();
    EXPECT_EQ(value, "abc");
  }
}
