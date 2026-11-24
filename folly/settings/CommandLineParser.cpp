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

#include <folly/settings/CommandLineParser.h>

#include <exception>
#include <iostream>
#include <optional>
#include <string>

#include <fmt/core.h>
#include <fmt/format.h>
#include <folly/ExceptionString.h>
#include <folly/Function.h>
#include <folly/String.h>
#include <folly/logging/xlog.h>

namespace folly::settings {

namespace {

/**
 * This class facilitates recursive declaration of a folly::Function which
 * returns an instance of itself. This is normally not possible to express in
 * C++ without typecast from void*. This class uses type cast operator to
 * achieve the same result, so it is transparent for users.
 */
struct RecursiveStateHelper {
  using type = Function<RecursiveStateHelper()>;
  /* implicit */ RecursiveStateHelper(type f) : func(std::move(f)) {}
  explicit operator type() { return std::move(func); }
  type func;
};

using State = RecursiveStateHelper::type;

std::string_view stripPrefix(std::string_view str, char ch) {
  if (str.starts_with(ch)) {
    return str.substr(1);
  }
  return str;
}
} // namespace

class CommandLineParser::Impl {
 public:
  explicit Impl(int& argc, char**& argv, SettingsAccessorProxy& flags_info)
      : end_(argc), argc_(argc), argv_(argv), flagsInfo_(flags_info) {
    end_ =
        std::find_if(
            argv_ + pos_,
            argv_ + end_,
            [](const auto& arg) { return std::strcmp(arg, "--") == 0; }) -
        argv_;
    last_ = end_;
  }

  ArgParsingResult parse() {
    result_ = ArgParsingResult::OK;

    State state = parseFlagState();
    while (state) {
      state = state();
    }

    return result_;
  }

 private:
  /* BEGIN State machine methods */
  /**
   * Parses next arg into a flag:value pair. Can transition into:
   * - moveBackState
   * - endOfArgsState
   * - helpFlagState
   * - parseFlagValueState
   * - setSettingState
   */
  State parseFlagState() {
    return [this] {
      value_in_arg_ = true;
      if (!haveMoreArgs()) {
        return doneState();
      }

      auto arg = getNext();
      if (!arg.starts_with('-') // positional
          || arg == "-" // stdin
          || arg.empty() // empty?
      ) {
        return moveBackState();
      }

      // Like gflags allow one or two '-' in front
      arg = stripPrefix(arg, '-');
      arg = stripPrefix(arg, '-');

      if (arg.empty()) { // reached '--' it is end of args
        return endOfArgsState();
      }

      if (!split<false>('=', arg, flag_, value_)) {
        flag_ = arg;
        if (isHelpFlag()) {
          return helpFlagState();
        }

        return parseFlagValueState();
      }

      if (isHelpFlag()) {
        return helpFlagState();
      }

      return setSettingState();
    };
  }

  /**
   * Parses value for current flag if flag's arg didn't contain a value. Can
   * transition into:
   * - errorFlagValueState
   * - setSettingState
   */
  State parseFlagValueState() {
    return [this] {
      auto no_value = [this] {
        bool is_bool = is_bool_arg(flag_);
        if (is_bool) {
          value_ = "true";
          return setSettingState();
        }
        return errorFlagValueState();
      };

      if (!haveMoreArgs()) {
        return no_value();
      }

      value_in_arg_ = false;
      auto arg = getNext();
      if (arg.starts_with('-')) {
        pos_ -= 1;
        value_in_arg_ = true;
        return no_value();
      }

      value_ = arg;
      return setSettingState();
    };
  }

  /**
   * Handles case when there is no value for a flag and flag is not bool flag.
   * Can transition into:
   * - moveBackState
   */
  State errorFlagValueState() {
    return [this] {
      if (!is_folly_setting(flag_)) {
        return moveBackState();
      }

      fprintf(stderr, "Flag %s requires a value\n", flag_.data());
      result_ = ArgParsingResult::ERROR;
      return moveBackState();
    };
  }

  /**
   * Sets folly::settings setting for current flag. Can transition into:
   * - parseFlagState
   * - moveBackState
   * - errorFlagSetState
   */
  State setSettingState() {
    return [this] {
      auto settingMeta = flagsInfo_.getSettingMetadata(flag_);
      if (!settingMeta.has_value() ||
          settingMeta->get().commandLine == CommandLine::RejectOverrides) {
        return moveBackState();
      }

      std::optional<std::string> errorMessage;
      try {
        auto result = flagsInfo_.setFromString(flag_, value_, "cli");
        if (result.hasError()) {
          errorMessage = toString(result.error());
        }
      } catch (...) {
        errorMessage = std::string(exceptionStr(std::current_exception()));
      }

      if (errorMessage.has_value()) {
        // move unparsed flags
        return errorFlagSetState(std::move(errorMessage).value());
      }

      return parseFlagState();
    };
  }

  /**
   * Handles --help flag. Can transition into:
   * - moveBackState
   */
  State helpFlagState() {
    if (result_ == ArgParsingResult::OK) { // Do not reset error
      result_ = ArgParsingResult::HELP;
    }
    return moveBackState();
  }

  /**
   * Handles case when folly::setting cannot be set. Can transition into:
   * - moveBackState
   */
  State errorFlagSetState(std::string&& errorMessage) {
    return [this, errorMessage = std::move(errorMessage)] {
      XLOG(ERR) << fmt::format(
          "Failed setting '{}' with '{}' due to '{}'",
          flag_,
          value_,
          errorMessage);
      result_ = ArgParsingResult::ERROR;
      return moveBackState();
    };
  }

  /**
   * Moves arguments we couldn't recognize as folly::settings arguments into the
   * back of the argv. This includes move of "argumen value", if next argument
   * doesn't start with '--'. Can transition into:
   * - parseFlagState
   */
  State moveBackState() {
    return [this] {
      // If value_in_arg_ == true we need to move both flag and value
      move_args(value_in_arg_ ? 1 : 2);

      return parseFlagState();
    };
  }

  /**
   * Handles case when we encounter '--' stops parsing. Can transition into:
   * - doneState
   */
  State endOfArgsState() {
    return [this] { return doneState(); };
  }

  /**
   * Stops argument parsing state machine.
   */
  State doneState() {
    return [this] {
      argv_[end_ - 1] = argv_[0];
      argv_ += end_ - 1;
      argc_ -= end_ - 1;
      return State();
    };
  }
  /* END State machine methods */

  /**
   * Actual function which moves argument at pos_ to the end of argv
   */
  void move_args(int num_positions) {
    std::rotate(argv_ + pos_ - num_positions, argv_ + pos_, argv_ + last_);
    end_ -= num_positions;
    pos_ -= num_positions;
  }

  /**
   * Returns true if a flag is folly::settings flag and it is of type bool
   *
   * @param arg flag name
   * @return is bool folly::settings
   */
  bool is_bool_arg(std::string_view arg) {
    return flagsInfo_.isBooleanFlag(arg);
  }

  bool is_folly_setting(std::string_view arg) {
    return flagsInfo_.hasFlag(arg);
  }

  bool isHelpFlag() { return flag_ == kHelpFlag; }

  bool haveMoreArgs() { return pos_ < end_; }

  /**
   * @return next argument from argv
   */
  std::string_view getNext() { return argv_[pos_++]; }

  ArgParsingResult result_;
  bool value_in_arg_{true};
  ptrdiff_t pos_{1}, end_, last_;
  int& argc_;
  char**& argv_;

  std::string_view flag_, value_;

  SettingsAccessorProxy& flagsInfo_;
};

CommandLineParser::CommandLineParser(
    int& argc, char**& argv, SettingsAccessorProxy& flags_info)
    : impl_(std::make_unique<Impl>(argc, argv, flags_info)) {}

CommandLineParser::CommandLineParser(CommandLineParser&&) noexcept = default;
CommandLineParser& CommandLineParser::operator=(CommandLineParser&&) noexcept =
    default;
CommandLineParser::~CommandLineParser() = default;

ArgParsingResult CommandLineParser::parse() {
  return impl_->parse();
}

void printHelpIfNeeded(std::string_view app, bool exit_on_help) {
  std::cerr << fmt::format("{}:\n", app);
  Snapshot snapshot;
  for (const auto& kv : SettingsAccessorProxy(snapshot).getSettingsMetadata()) {
    auto s = fmt::format("--{} {}", kv.first, kv.second.description);
    std::cerr << fmt::format("\t{}\n", s);

    auto type = kv.second.typeStr;
    auto def = kv.second.defaultStr;

    std::cerr << fmt::format("\t  type: {}", type);
    if (type != "bool") {
      std::cerr << fmt::format(" default: {}", def);
    }
    std::cerr << "\n";
  }

  if (exit_on_help) {
    exit(0);
  }
}

ArgParsingResult parseCommandLineArguments(
    int& argc,
    char**& argv,
    std::string_view project,
    Snapshot& snapshot,
    const SettingsAccessorProxy::SettingAliases& aliases) {
  SettingsAccessorProxy flags_info(snapshot, project, aliases);
  auto result = CommandLineParser(argc, argv, flags_info).parse();
  if (result == ArgParsingResult::OK) {
    snapshot.publish();
  }
  return result;
}

ArgParsingResult parseCommandLineArguments(
    int& argc,
    char**& argv,
    std::string_view project,
    Snapshot* snapshot,
    const SettingsAccessorProxy::SettingAliases& aliases) {
  if (snapshot) {
    return parseCommandLineArguments(argc, argv, project, *snapshot, aliases);
  }

  Snapshot snap;
  return parseCommandLineArguments(argc, argv, project, snap, aliases);
}

} // namespace folly::settings
