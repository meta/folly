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

#include <folly/cli/NestedCommandLineApp.h>

#include <iostream>
#include <stdexcept>

#include <fmt/format.h>
#include <glog/logging.h>

#include <folly/FileUtil.h>
#include <folly/String.h>
#include <folly/io/FsUtil.h>

namespace po = ::boost::program_options;

namespace folly {

namespace {

// Guess the program name as basename(executable)
std::string guessProgramName() {
  try {
    return fs::executable_path().filename().string();
  } catch (const std::exception&) {
    return "UNKNOWN";
  }
}

} // namespace

ProgramExit::ProgramExit(int status, const std::string& msg)
    : std::runtime_error(msg), status_(status) {
  // Message is only allowed for non-zero exit status
  if (status_ == 0 && !msg.empty()) {
    throw std::invalid_argument(
        "ProgramExit: a message is only allowed for a non-zero exit status");
  }
}

NestedCommandLineApp::NestedCommandLineApp(
    std::string programName,
    std::string version,
    std::string programHeading,
    std::string programHelpFooter,
    InitFunction initFunction)
    : programName_(std::move(programName)),
      programHeading_(std::move(programHeading)),
      programHelpFooter_(std::move(programHelpFooter)),
      version_(std::move(version)),
      globalOptions_("Global options"),
      optionStyle_(po::command_line_style::default_style) {
  addCommand(
      kHelpCommand.str(),
      "[command]",
      "Display help (globally or for a given command)",
      "Displays help (globally or for a given command).",
      [this](
          const po::variables_map& vm, const std::vector<std::string>& args) {
        displayHelp(vm, args);
      });
  builtinCommands_.insert(kHelpCommand);
  addAlias(kShortHelpCommand.str(), kHelpCommand.str());

  addCommand(
      kVersionCommand.str(),
      "[command]",
      "Display version information",
      "Displays version information.",
      [this](const po::variables_map&, const std::vector<std::string>&) {
        displayVersion();
      });
  builtinCommands_.insert(kVersionCommand);

  globalOptions_.add_options()(
      (kHelpCommand.str() + "," + kShortHelpCommand.str()).c_str(),
      "Display help (globally or for a given command)")(
      kVersionCommand.str().c_str(), "Display version information");

  if (initFunction) {
    callbackFunctions_.emplace_back(std::move(initFunction));
  }
}

po::options_description& NestedCommandLineApp::addCommand(
    std::string name,
    std::string argStr,
    std::string shortHelp,
    std::string fullHelp,
    Command command,
    folly::Optional<po::positional_options_description> positionalOptions) {
  CommandInfo info{
      std::move(argStr),
      std::move(shortHelp),
      std::move(fullHelp),
      std::move(command),
      po::options_description(fmt::format("Options for `{}'", name)),
      std::move(positionalOptions)};

  auto p = commands_.emplace(std::move(name), std::move(info));
  if (!p.second) {
    throw std::invalid_argument("Command already exists: " + p.first->first);
  }

  return p.first->second.options;
}

void NestedCommandLineApp::addAlias(std::string newName, std::string oldName) {
  if (!aliases_.count(oldName) && !commands_.count(oldName)) {
    throw std::invalid_argument("Alias old name does not exist: " + oldName);
  }
  if (aliases_.count(newName) || commands_.count(newName)) {
    throw std::invalid_argument("Alias new name already exists: " + newName);
  }
  aliases_.emplace(std::move(newName), std::move(oldName));
}

void NestedCommandLineApp::setOptionStyle(
    boost::program_options::command_line_style::style_t style) {
  optionStyle_ = style;
}

void NestedCommandLineApp::displayHelp(
    const po::variables_map& /* globalOptions */,
    const std::vector<std::string>& args) const {
  if (args.empty()) {
    // General help
    printf(
        "%s\nUsage: %s [global_options...] <command> [command_options...] "
        "[command_args...]\n\n",
        programHeading_.c_str(),
        programName_.c_str());
    std::cout << globalOptions_;
    printf("\nAvailable commands:\n");

    size_t maxLen = 0;
    for (auto& p : commands_) {
      maxLen = std::max(maxLen, p.first.size());
    }
    for (auto& p : aliases_) {
      maxLen = std::max(maxLen, p.first.size());
    }

    for (auto& p : commands_) {
      printf(
          "  %-*s    %s\n",
          int(maxLen),
          p.first.c_str(),
          p.second.shortHelp.c_str());
    }

    if (!aliases_.empty()) {
      printf("\nAvailable aliases:\n");
      for (auto& p : aliases_) {
        printf(
            "  %-*s => %s\n",
            int(maxLen),
            p.first.c_str(),
            resolveAlias(p.second).c_str());
      }
    }
    std::cout << "\n" << programHelpFooter_ << "\n";
  } else {
    // Help for a given command
    auto& p = findCommand(args.front());
    if (p.first != args.front()) {
      printf(
          "`%s' is an alias for `%s'; showing help for `%s'\n",
          args.front().c_str(),
          p.first.c_str(),
          p.first.c_str());
    }
    auto& info = p.second;

    printf(
        "Usage: %s [global_options...] %s%s%s%s\n\n",
        programName_.c_str(),
        p.first.c_str(),
        info.options.options().empty() ? "" : " [command_options...]",
        info.argStr.empty() ? "" : " ",
        info.argStr.c_str());

    printf("%s\n", info.fullHelp.c_str());

    std::cout << globalOptions_;

    if (!info.options.options().empty()) {
      printf("\n");
      std::cout << info.options;
    }
  }
}

void NestedCommandLineApp::displayVersion() const {
  printf("%s %s\n", programName_.c_str(), version_.c_str());
}

const std::string& NestedCommandLineApp::resolveAlias(
    const std::string& name) const {
  auto dest = &name;
  for (;;) {
    auto pos = aliases_.find(*dest);
    if (pos == aliases_.end()) {
      break;
    }
    dest = &pos->second;
  }
  return *dest;
}

auto NestedCommandLineApp::findCommand(const std::string& name) const
    -> const std::pair<const std::string, CommandInfo>& {
  auto pos = commands_.find(resolveAlias(name));
  if (pos == commands_.end()) {
    throw ProgramExit(
        1,
        fmt::format(
            "Command '{}' not found. Run '{} {}' for help.",
            name,
            programName_,
            kHelpCommand));
  }
  return *pos;
}

int NestedCommandLineApp::run(int argc, const char* const argv[]) {
  if (programName_.empty()) {
    programName_ = fs::path(argv[0]).filename().string();
  }
  return run(std::vector<std::string>(argv + 1, argv + argc));
}

int NestedCommandLineApp::run(const std::vector<std::string>& args) {
  int status;
  try {
    doRun(args);
    status = 0;
  } catch (const ProgramExit& ex) {
    if (ex.what()[0]) { // if not empty
      fprintf(stderr, "%s\n", ex.what());
    }
    status = ex.status();
  } catch (const po::error& ex) {
    fprintf(
        stderr,
        "%s",
        fmt::format(
            "{}. Run '{} help' for {}.\n",
            ex.what(),
            programName_,
            kHelpCommand)
            .c_str());
    status = 1;
  }

  if (status == 0) {
    if (ferror(stdout)) {
      fprintf(stderr, "error on standard output\n");
      status = 1;
    } else if (fflush(stdout)) {
      fprintf(
          stderr,
          "standard output flush failed: %s\n",
          errnoStr(errno).c_str());
      status = 1;
    }
  }

  return status;
}

void NestedCommandLineApp::doRun(const std::vector<std::string>& args) {
  if (programName_.empty()) {
    programName_ = guessProgramName();
  }

  bool not_clean = false;
  std::vector<std::string> cleanArgs;
  std::vector<std::string> endArgs;

  for (auto& na : args) {
    if (not_clean) {
      endArgs.push_back(na);
    } else if (na == "--") {
      not_clean = true;
    } else {
      cleanArgs.push_back(na);
    }
  }

  auto parsed = parseNestedCommandLine(cleanArgs, globalOptions_, optionStyle_);
  po::variables_map vm;
  po::store(parsed.options, vm);
  if (vm.count(kHelpCommand.str())) {
    std::vector<std::string> helpArgs;
    if (parsed.command) {
      helpArgs.push_back(*parsed.command);
    }
    displayHelp(vm, helpArgs);
    return;
  }

  if (vm.count(kVersionCommand.str())) {
    displayVersion();
    return;
  }

  if (!parsed.command) {
    throw ProgramExit(
        1,
        fmt::format(
            "Command not specified. Run '{} {}' for help.",
            programName_,
            kHelpCommand));
  }

  auto& p = findCommand(*parsed.command);
  auto& cmd = p.first;
  auto& info = p.second;

  auto parser =
      po::command_line_parser(parsed.rest)
          .options(info.options)
          .style(optionStyle_);
  if (info.positionalOptions) {
    parser = parser.positional(*info.positionalOptions);
  }
  try {
    auto cmdOptions = parser.run();

    po::store(cmdOptions, vm);
    po::notify(vm);

    // If positional arguments are specified they should get mapped to a named
    // arg and don't need to be double collected
    auto cmdArgs = po::collect_unrecognized(
        cmdOptions.options,
        info.positionalOptions
            ? po::exclude_positional
            : po::include_positional);

    cmdArgs.insert(cmdArgs.end(), endArgs.begin(), endArgs.end());

    for (const auto& callback : callbackFunctions_) {
      callback(cmd, vm, cmdArgs);
    }

    info.command(vm, cmdArgs);
  } catch (const po::required_option& ex) {
    // The top run() function won't be able to capture the command name.
    // We include both program name and command name in the error message for
    // better UX.
    throw ProgramExit(
        1,
        fmt::format(
            "Missing required option: '{}'. Run '{} {} --help' for help.",
            ex.get_option_name(),
            programName_,
            cmd));
  }
}

bool NestedCommandLineApp::isBuiltinCommand(const std::string& name) const {
  return builtinCommands_.count(name);
}

} // namespace folly
