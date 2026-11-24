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

#include <folly/cli/Args.h>

#include <cerrno>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include <fmt/core.h>

#include <folly/FileUtil.h>

namespace folly {

std::vector<char const*> cli_args_strings_to_c_strings(
    std::span<std::string const> args) {
  std::vector<char const*> argv;
  argv.reserve(args.size());
  for (auto const& arg : args) {
    argv.push_back(arg.c_str());
  }
  return argv;
}

namespace {

class cli_apply_args_files_error_category : public std::error_category {
 public:
  char const* name() const noexcept override { return "cli_apply_args_files"; }
  std::string message(int ev) const override {
    switch (static_cast<cli_apply_args_files_errc>(ev)) {
      case cli_apply_args_files_errc::max_depth_exceeded:
        return "max depth exceeded";
      default:
        return "unknown error";
    }
  }
};

} // namespace

std::error_code make_error_code(cli_apply_args_files_errc errc) {
  static cli_apply_args_files_error_category const category;
  return {static_cast<int>(errc), category};
}

namespace {

struct cli_parsed_arg {
  std::string value;
  size_t start_offset;
  size_t length;
  size_t begin_line;
  size_t begin_col;
  size_t end_line;
  size_t end_col;
  bool starts_with_at_outside_quotes; // true if first char is @ outside quotes
  bool starts_with_double_at_outside_quotes; // true if first two chars are @@
};

struct cli_parse_error {
  cli_apply_args_files_receiver::term_error code;
  // Term location (where the problematic term begins)
  size_t term_offset;
  size_t term_begin_line;
  size_t term_begin_col;
  // Error location (where the error begins within the term)
  size_t error_offset;
  size_t error_begin_line;
  size_t error_begin_col;
};

struct cli_parse_result {
  std::vector<cli_parsed_arg> args;
  std::optional<cli_parse_error> error;
};

/// Check if a character is whitespace (as recognized by std::isspace in C
/// locale) Whitespace characters: space, tab, newline, carriage return, form
/// feed, vertical tab
constexpr bool cli_is_space(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
      c == '\v';
}

/// Parse arguments from a file content string
/// Handles whitespace separation, quotes (single and double), escaping, and
/// comments
///
/// Quoting rules (simplified POSIX-like):
/// - Single quotes: completely literal, no escape processing
/// - Double quotes: process \n \t \r \\ \" escapes
/// - Unquoted: same escape processing as double quotes
/// - Unclosed quotes return an error in the result
///
/// @ handling:
/// - @ is meaningful only when it's the first character AND outside quotes
/// - @@ escapes to @ only when the first two chars are @@ AND outside quotes
cli_parse_result cli_parse_args_from_content(std::string_view content) {
  using error_t = cli_apply_args_files_receiver::term_error;

  cli_parse_result parse_result;
  std::string current;
  bool in_single_quotes = false;
  bool in_double_quotes = false;
  bool escaped = false;
  bool in_comment = false;
  size_t token_start = 0;
  bool in_token = false;

  // Track @ prefix state for the current token
  // These are set when we see @ as the first char(s) outside quotes
  bool first_char_is_at = false;
  bool second_char_is_at = false;
  size_t chars_in_token = 0; // count of chars added to current token

  // Track where quotes started for error messages
  size_t quote_start_line = 0;
  size_t quote_start_col = 0;
  size_t quote_start_offset = 0;

  // Track line and column (1-based)
  size_t line = 1;
  size_t col = 1;
  size_t token_start_line = 0;
  size_t token_start_col = 0;
  size_t token_end_line = 0;
  size_t token_end_col = 0;

  // Helper to finalize and push a token
  auto push_token = [&](size_t end_pos) {
    if (!current.empty()) {
      size_t length = end_pos - token_start;
      parse_result.args.push_back(
          {std::move(current),
           token_start,
           length,
           token_start_line,
           token_start_col,
           token_end_line,
           token_end_col,
           first_char_is_at,
           first_char_is_at && second_char_is_at});
      current.clear();
    }
    in_token = false;
    first_char_is_at = false;
    second_char_is_at = false;
    chars_in_token = 0;
  };

  for (size_t i = 0; i < content.size(); ++i) {
    char c = content[i];

    if (in_comment) {
      if (c == '\n') {
        in_comment = false;
        line++;
        col = 1;
      } else {
        col++;
      }
      continue;
    }

    // Inside single quotes: everything is literal except closing quote
    if (in_single_quotes) {
      if (c == '\'') {
        in_single_quotes = false;
        token_end_line = line;
        token_end_col = col;
        col++;
        continue;
      }
      // All characters are literal inside single quotes
      current += c;
      chars_in_token++;
      token_end_line = line;
      token_end_col = col;
      if (c == '\n') {
        line++;
        col = 1;
      } else {
        col++;
      }
      continue;
    }

    // Handle escape sequences (in double quotes or unquoted)
    if (escaped) {
      if (!in_token) {
        token_start = i - 1;
        token_start_line = line;
        token_start_col = col - 1; // the backslash was the previous char
        in_token = true;
      }
      token_end_line = line;
      token_end_col = col;
      switch (c) {
        case 'n':
          current += '\n';
          chars_in_token++;
          break;
        case 't':
          current += '\t';
          chars_in_token++;
          break;
        case 'r':
          current += '\r';
          chars_in_token++;
          break;
        case '\\':
          current += '\\';
          chars_in_token++;
          break;
        case '"':
          current += '"';
          chars_in_token++;
          break;
        case '#':
          current += '#';
          chars_in_token++;
          break;
        default:
          // Unknown escape sequence: return error
          parse_result.error = cli_parse_error{
              error_t::unrecognized_escape_sequence,
              token_start,
              token_start_line,
              token_start_col,
              i - 1, // offset of the backslash
              line,
              col - 1}; // column of the backslash
          return parse_result;
      }
      escaped = false;
      col++;
      continue;
    }

    if (c == '\\' && !in_single_quotes) {
      if (!in_token) {
        token_start = i;
        token_start_line = line;
        token_start_col = col;
        in_token = true;
      }
      token_end_line = line;
      token_end_col = col;
      escaped = true;
      col++;
      continue;
    }

    if (c == '#' && !in_double_quotes) {
      push_token(i);
      in_comment = true;
      col++;
      continue;
    }

    if (c == '\'' && !in_double_quotes) {
      if (!in_token) {
        token_start = i;
        token_start_line = line;
        token_start_col = col;
        in_token = true;
      }
      token_end_line = line;
      token_end_col = col;
      in_single_quotes = true;
      quote_start_line = line;
      quote_start_col = col;
      quote_start_offset = i;
      col++;
      continue;
    }

    if (c == '"') {
      if (!in_token) {
        token_start = i;
        token_start_line = line;
        token_start_col = col;
        in_token = true;
      }
      token_end_line = line;
      token_end_col = col;
      if (in_double_quotes) {
        in_double_quotes = false;
      } else {
        in_double_quotes = true;
        quote_start_line = line;
        quote_start_col = col;
        quote_start_offset = i;
      }
      col++;
      continue;
    }

    if (cli_is_space(c) && !in_double_quotes) {
      push_token(i);
      if (c == '\n') {
        line++;
        col = 1;
      } else {
        col++;
      }
      continue;
    }

    if (!in_token) {
      token_start = i;
      token_start_line = line;
      token_start_col = col;
      in_token = true;
    }

    // Only update end position if this is not trailing whitespace
    if (!cli_is_space(c)) {
      token_end_line = line;
      token_end_col = col;
    }

    // Track @ prefix: only meaningful when outside quotes
    if (!in_double_quotes && !in_single_quotes) {
      if (chars_in_token == 0 && c == '@') {
        first_char_is_at = true;
      } else if (chars_in_token == 1 && first_char_is_at && c == '@') {
        second_char_is_at = true;
      }
    }

    current += c;
    chars_in_token++;

    // Update line/col tracking even when inside quotes
    if (c == '\n') {
      line++;
      col = 1;
    } else {
      col++;
    }
  }

  // Check for unclosed quotes - return error instead of throwing
  // clang-format off
  auto error =
      in_single_quotes ? error_t::unclosed_single_quote :
      in_double_quotes ? error_t::unclosed_double_quote :
      error_t::success;
  // clang-format on

  if (error != error_t::success) {
    parse_result.error = cli_parse_error{
        error,
        token_start,
        token_start_line,
        token_start_col,
        quote_start_offset,
        quote_start_line,
        quote_start_col};
    return parse_result;
  }

  if (!current.empty()) {
    // Strip trailing whitespace from the last token
    // This handles cases where the file ends with whitespace inside quotes
    // or simply trailing whitespace at the end
    while (!current.empty()) {
      char c = current.back();
      if (cli_is_space(c)) {
        current.pop_back();
      } else {
        break;
      }
    }

    if (!current.empty()) {
      size_t length = content.size() - token_start;
      // Find the actual end of the token (excluding trailing whitespace)
      for (size_t i = content.size(); i > token_start; --i) {
        char c = content[i - 1];
        if (!cli_is_space(c)) {
          length = i - token_start;
          break;
        }
      }

      parse_result.args.push_back(
          {std::move(current),
           token_start,
           length,
           token_start_line,
           token_start_col,
           token_end_line,
           token_end_col,
           first_char_is_at,
           first_char_is_at && second_char_is_at});
    }
  }

  return parse_result;
}

/// Read file and return content, or error code if file cannot be read
std::pair<std::string, std::error_code> cli_read_file(
    std::filesystem::path const& filename) {
  std::string content;
  bool success = folly::readFile(filename.string().c_str(), content);
  if (!success) {
    return {"", std::error_code(errno, std::generic_category())};
  }
  return {content, {}};
}

/// Abstraction over cli_parsed_arg vs raw string for unified processing
struct cli_arg_item {
  std::string_view value;
  bool starts_with_at;
  bool starts_with_double_at;
};

/// Set of canonical paths for files currently being processed (for cycle
/// detection)
using cli_active_files_set = std::set<std::filesystem::path>;

/// Forward declaration for mutual recursion
void cli_process_file_args(
    cli_apply_args_files_receiver& receiver,
    std::filesystem::path const& current_dir,
    std::vector<cli_parsed_arg> const& file_args,
    cli_active_files_set& active_files,
    size_t depth,
    size_t max_depth);

/// Process a single arg item (handles @file references, @@ escapes, and regular
/// terms). Returns control::go to continue processing, control::stop to halt.
cli_apply_args_files_receiver::control cli_process_single_arg(
    cli_apply_args_files_receiver& receiver,
    std::filesystem::path const& current_dir,
    cli_arg_item const& item,
    cli_apply_args_files_receiver::location const& loc,
    cli_active_files_set& active_files,
    size_t depth,
    size_t max_depth) {
  // Check if this term starts with @ (args-file reference)
  if (item.starts_with_at) {
    // Check for @@ escape: leading @@ means regular term with one @ stripped
    if (item.starts_with_double_at) {
      std::string unescaped{item.value.substr(1)};
      return receiver.on_term(std::move(unescaped), loc);
    }

    std::string filename{item.value.substr(1)};

    // First, check if receiver wants to process this args-file
    auto found_result = receiver.on_file_found(filename, loc);

    switch (found_result) {
      case cli_apply_args_files_receiver::found::stop:
        return cli_apply_args_files_receiver::control::stop;
      case cli_apply_args_files_receiver::found::skip:
        return cli_apply_args_files_receiver::control::pass;
      case cli_apply_args_files_receiver::found::dive:
        if (depth >= max_depth) {
          return receiver.on_file_error(
              std::move(filename),
              loc,
              make_error_code(cli_apply_args_files_errc::max_depth_exceeded));
        }
        break; // Process this args-file normally
      default:
        break;
    }

    // Resolve relative paths against current_dir
    std::filesystem::path full_path = filename;
    if (full_path.is_relative()) {
      full_path = current_dir / filename;
    }

    // Check for cycle using canonical path
    std::error_code canonical_ec;
    auto canonical_path = std::filesystem::canonical(full_path, canonical_ec);
    if (!canonical_ec) {
      // Successfully resolved canonical path; check for cycle
      if (active_files.contains(canonical_path)) {
        auto cycle_result = receiver.on_file_cycle(
            std::move(filename), loc, std::move(canonical_path));
        switch (cycle_result) {
          case cli_apply_args_files_receiver::cycle::stop:
            return cli_apply_args_files_receiver::control::stop;
          case cli_apply_args_files_receiver::cycle::skip:
          default:
            return cli_apply_args_files_receiver::control::pass;
        }
      }
    }
    // If canonical() failed, the file likely doesn't exist; let cli_read_file
    // handle the error

    auto result = cli_read_file(full_path);
    if (result.second) {
      return receiver.on_file_error(std::move(filename), loc, result.second);
    }
    std::string_view content = result.first;

    // Track this file as active (use canonical path if available, else
    // full_path)
    std::filesystem::path const& tracking_path =
        canonical_ec ? full_path : canonical_path;
    active_files.insert(tracking_path);

    receiver.on_file_enter(std::move(filename), loc);

    // Parse the file content
    auto parse_result = cli_parse_args_from_content(content);

    // Process successfully parsed args first
    if (!parse_result.args.empty()) {
      cli_process_file_args(
          receiver,
          current_dir,
          parse_result.args,
          active_files,
          depth + 1,
          max_depth);
    }

    // If there was a parse error, report it
    if (parse_result.error) {
      auto& err = *parse_result.error;
      // Term location: where the problematic term begins
      cli_apply_args_files_receiver::location term_loc{
          parse_result.args.size(), // idx is the next arg index
          err.term_offset,
          content.size() - err.term_offset,
          {err.term_begin_line, err.term_begin_col},
          {0, 0} // end location not meaningful for error spanning to EOF
      };
      // Error location: where the error begins within the term
      cli_apply_args_files_receiver::location error_loc{
          parse_result.args.size(),
          err.error_offset,
          content.size() - err.error_offset,
          {err.error_begin_line, err.error_begin_col},
          {0, 0}};
      receiver.on_term_error(err.code, term_loc, error_loc);
    }

    receiver.on_file_leave();
    active_files.erase(tracking_path);
    return cli_apply_args_files_receiver::control::pass;
  }

  // Regular term (no @ prefix)
  return receiver.on_term(std::string{item.value}, loc);
}

/// Helper to recursively process args from files
void cli_process_file_args(
    cli_apply_args_files_receiver& receiver,
    std::filesystem::path const& current_dir,
    std::vector<cli_parsed_arg> const& file_args,
    cli_active_files_set& active_files,
    size_t depth,
    size_t max_depth) {
  for (size_t i = 0; i < file_args.size(); ++i) {
    cli_parsed_arg const& parsed = file_args[i];
    cli_apply_args_files_receiver::location loc{
        i,
        parsed.start_offset,
        parsed.length,
        {parsed.begin_line, parsed.begin_col},
        {parsed.end_line, parsed.end_col}};

    cli_arg_item item{
        parsed.value,
        parsed.starts_with_at_outside_quotes,
        parsed.starts_with_double_at_outside_quotes};

    auto control_result = cli_process_single_arg(
        receiver, current_dir, item, loc, active_files, depth, max_depth);
    if (control_result == cli_apply_args_files_receiver::control::stop) {
      return;
    }
  }
}

} // namespace

void cli_apply_args_files(
    cli_apply_args_files_receiver& receiver,
    std::filesystem::path const& current_dir,
    std::span<std::string const> args,
    cli_apply_args_files_options const& options) {
  cli_active_files_set active_files;
  for (size_t i = 0; i < args.size(); ++i) {
    std::string_view arg = args[i];
    cli_apply_args_files_receiver::location loc{i, 0, 0, {}, {}};

    bool starts_with_at = arg.size() > 1 && arg[0] == '@';
    bool starts_with_double_at = starts_with_at && arg[1] == '@';

    cli_arg_item item{arg, starts_with_at, starts_with_double_at};

    auto control_result = cli_process_single_arg(
        receiver, current_dir, item, loc, active_files, 0, options.max_depth);
    if (control_result == cli_apply_args_files_receiver::control::stop) {
      return;
    }
  }
}

namespace {

/// Receiver that collects args into a vector and throws on any error.
class cli_throwing_receiver : public cli_apply_args_files_receiver {
 public:
  explicit cli_throwing_receiver(std::vector<std::string>& result)
      : result_(result) {}

  control on_term(std::string arg, location /*loc*/) override {
    result_.push_back(std::move(arg));
    return control::pass;
  }

  static char const* on_term_error_msg(term_error error) {
    switch (error) {
      case term_error::success:
        return nullptr;
      case term_error::unclosed_single_quote:
        return "unclosed single quote";
      case term_error::unclosed_double_quote:
        return "unclosed double quote";
      case term_error::unrecognized_escape_sequence:
        return "unrecognized escape sequence";
      default:
        return "<unknown>";
    }
  }

  void on_term_error(
      term_error error, location term_loc, location /*error_loc*/) override {
    if (error == term_error::success) {
      return; // Should not happen, but handle gracefully
    }
    throw cli_apply_args_files_error(
        fmt::format(
            "error parsing args file '{}' at line {}, column {}: {}",
            current_file_,
            term_loc.b.line,
            term_loc.b.col,
            on_term_error_msg(error)));
  }

  found on_file_found(cstring_view /*file*/, location /*loc*/) override {
    return found::dive;
  }

  control on_file_error(
      std::string file, location /*loc*/, std::error_code err) override {
    throw cli_apply_args_files_error(
        fmt::format("failed to read args file '{}': {}", file, err.message()));
  }

  void on_file_enter(std::string file, location /*loc*/) override {
    file_stack_.emplace_back(std::move(current_file_));
    current_file_ = std::move(file);
  }

  void on_file_leave() override {
    if (!file_stack_.empty()) {
      current_file_ = std::move(file_stack_.back());
      file_stack_.pop_back();
    }
  }

  cycle on_file_cycle(
      std::string file,
      location /*loc*/,
      std::filesystem::path /*canonical_path*/) override {
    throw cli_apply_args_files_error(
        fmt::format("cycle detected in args file '{}'", file));
  }

 private:
  std::vector<std::string>& result_;
  std::string current_file_;
  std::vector<std::string> file_stack_;
};

} // namespace

std::vector<std::string> cli_apply_args_files(
    std::filesystem::path const& current_dir,
    std::span<std::string const> args) {
  std::vector<std::string> result;
  cli_throwing_receiver receiver(result);
  cli_apply_args_files(receiver, current_dir, args);
  return result;
}

std::vector<std::string> cli_apply_args_files(
    std::filesystem::path const& current_dir,
    int argc,
    char const* const* argv) {
  std::vector<std::string> args;
  while (argc--) {
    args.emplace_back(*argv++);
  }
  return cli_apply_args_files(current_dir, args);
}

std::vector<std::string> cli_apply_args_files(
    std::filesystem::path const& current_dir, int argc, char* const* argv) {
  return cli_apply_args_files(
      current_dir, argc, const_cast<char const* const*>(argv));
}

std::vector<std::string> cli_apply_args_files(
    int argc, char const* const* argv) {
  auto current_dir = std::filesystem::current_path();
  return cli_apply_args_files(current_dir, argc, argv);
}

std::vector<std::string> cli_apply_args_files(int argc, char* const* argv) {
  return cli_apply_args_files(argc, const_cast<char const* const*>(argv));
}

} // namespace folly
