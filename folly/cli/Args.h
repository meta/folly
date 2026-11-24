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

#include <cstddef>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <folly/lang/cstring_view.h>

namespace folly {

std::vector<char const*> cli_args_strings_to_c_strings(
    std::span<std::string const> args);

class cli_apply_args_files_receiver {
 public:
  enum class control {
    stop, // stop processing at this level, and recurse up
    pass, // continue processing at this level
  };

  enum class found {
    stop, // stop processing at this level, and recurse up
    skip, // skip this arg
    dive, // expand this arg like an args-file
  };

  enum class cycle {
    stop, // stop processing at this level, and recurse up
    skip, // skip this arg and continue processing
  };

  enum class term_error {
    success,
    unclosed_single_quote,
    unclosed_double_quote,
    unrecognized_escape_sequence,
  };

  struct human_location {
    /// 1-base line number of this entry in the containing args-file (or zero)
    size_t line = 0;

    /// 1-base column number of this entry in the containing args-file (or zero)
    size_t col = 0;

    friend bool operator==(
        human_location const& lhs,
        human_location const& rhs) noexcept = default;
  };

  struct location {
    /// 0-base logical index of this entry in the arg-list or args-file
    size_t idx = 0;

    /// 0-base byte offset of this entry in the containing args-file (or zero)
    /// if a beginning loc, offset of the beginning char
    /// if an end loc, offset of the char after the end
    size_t off = 0;

    /// length of this entry in the containing args-file (or zero)
    size_t len = 0;

    /// line and col of the beginning char
    human_location b;

    /// line and col of the ending char (not past the end!)
    human_location e;

    friend bool operator==(location const& lhs, location const& rhs) noexcept =
        default;
  };

  virtual ~cli_apply_args_files_receiver() = default;

  /// Called when an argument is just a string.
  /// Is passed ownership of the string.
  virtual control on_term(std::string arg, location loc) = 0;

  /// Called when an argument fails to parse (e.g., unclosed quotes).
  /// term_loc is the location of the term containing the error.
  /// error_loc is the location where the error begins within that term.
  /// Parsing of the current file stops, but parent files continue.
  virtual void on_term_error(
      term_error error, location term_loc, location error_loc) = 0;

  /// Called when an argument is an args-file. Returns whether to expand it.
  virtual found on_file_found(cstring_view file, location loc) = 0;

  /// Called when an argument is an args-file, but the file could not be read.
  /// Is passed ownership of the filename.
  virtual control on_file_error(
      std::string file, location loc, std::error_code err) = 0;

  /// Called when an argument is an args-file, the file was read successfully,
  /// and control recurses into the args-file.
  /// Is passed ownership of the filename.
  virtual void on_file_enter(std::string file, location loc) = 0;

  /// Called when control recurses out of an args-file.
  /// Calls to on_file_enter and on_file_leave are balanced in pairs.
  virtual void on_file_leave() = 0;

  /// Called when an argument is an args-file that would create a cycle
  /// (directly or indirectly). The file parameter is the original
  /// filename from the argument, and canonical_path is the resolved path that
  /// was detected as part of a cycle.
  virtual cycle on_file_cycle(
      std::string file, location loc, std::filesystem::path canonical_path) = 0;
};

/// Exception thrown by the simple cli_apply_args_files overload.
class cli_apply_args_files_error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct cli_apply_args_files_options {
  size_t max_depth = 64;
};

enum class cli_apply_args_files_errc : int {
  max_depth_exceeded = 1,
};

std::error_code make_error_code(cli_apply_args_files_errc errc);

} // namespace folly

namespace std {
template <>
struct is_error_code_enum<folly::cli_apply_args_files_errc> : true_type {};
} // namespace std

namespace folly {

/// Applies args-file expansion to a list of arguments.
/// Uses the receiver interface for full control over processing.
void cli_apply_args_files(
    cli_apply_args_files_receiver& receiver,
    std::filesystem::path const& current_dir,
    std::span<std::string const> args,
    cli_apply_args_files_options const& options = {});

/// Applies args-file expansion to a list of arguments.
/// Returns the expanded list of arguments.
/// Throws cli_apply_args_files_error on any error (file not found, parse
/// error).
std::vector<std::string> cli_apply_args_files(
    std::filesystem::path const& current_dir,
    std::span<std::string const> args);

std::vector<std::string> cli_apply_args_files(
    std::filesystem::path const& current_dir,
    int argc,
    char const* const* argv);

std::vector<std::string> cli_apply_args_files(
    std::filesystem::path const& current_dir, int argc, char* const* argv);

std::vector<std::string> cli_apply_args_files(
    int argc, char const* const* argv);

std::vector<std::string> cli_apply_args_files(int argc, char* const* argv);

} // namespace folly
