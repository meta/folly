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

#include <folly/detail/PerfScoped.h>

#include <folly/Conv.h>

#if FOLLY_PERF_IS_SUPPORTED
#include <fcntl.h>
#include <sys/stat.h>

#include <fmt/core.h>

#include <folly/File.h> // @manual
#include <folly/FileUtil.h> // @manual
#include <folly/Subprocess.h> // @manual
#include <folly/Utility.h> // @manual
#include <folly/lang/cstring_view.h> // @manual
#include <folly/portability/Sockets.h>
#include <folly/system/Pid.h>
#include <folly/testing/TestUtil.h>
#endif

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace folly {
namespace detail {

#if FOLLY_PERF_IS_SUPPORTED

namespace {

constexpr std::chrono::milliseconds kAttachTimeout{30000};
constexpr std::chrono::milliseconds kDetachTimeout{500};
constexpr std::chrono::milliseconds kStopTimeout{10000};
constexpr std::chrono::milliseconds kKillTimeout{5000};

constexpr std::string_view kEnableCommand = "enable\n";
constexpr std::string_view kDisableCommand = "disable\n";

std::string ctlFifoPath(const fs::path& dir) {
  return (dir / "perf_ctl").string();
}

std::string ackFifoPath(const fs::path& dir) {
  return (dir / "perf_ack").string();
}

bool isDelayArg(const std::string& arg) {
  return arg == "--delay" || arg.starts_with("--delay=") ||
      arg.starts_with("-D");
}

bool isControlArg(const std::string& arg) {
  return arg == "--control" || arg.starts_with("--control=");
}

std::vector<std::string> buildArgs(
    const std::vector<std::string>& passed,
    const fs::path& controlDir,
    const test::TemporaryFile* output) {
  for (const auto& arg : passed) {
    if (isDelayArg(arg) || isControlArg(arg)) {
      throw std::invalid_argument(
          fmt::format(
              "PerfScoped drives the perf counting window with --delay and "
              "--control, so '{}' cannot be passed to perf.",
              arg));
    }
  }

  std::vector<std::string> res{std::string(kPerfBinaryPath)};
  res.insert(res.end(), passed.begin(), passed.end());

  res.emplace_back("--delay=-1");
  res.push_back(
      fmt::format(
          "--control=fifo:{},{}",
          ctlFifoPath(controlDir),
          ackFifoPath(controlDir)));

  res.emplace_back("-p");
  res.push_back(folly::to<std::string>(get_cached_pid()));
  if (output) {
    res.emplace_back("--output");
    res.push_back(output->path().string());
  }
  return res;
}

Subprocess::Options subprocessOptions() {
  Subprocess::Options res;
  // ~Subprocess must never block: PerfScopedImpl performs its own bounded
  // waits below and abandons an unkillable child instead of reaping it.
  res.allowDestructionWhileProcessRunning(true);
  return res;
}

File makeControlFifo(const std::string& path) {
  if (::mkfifo(path.c_str(), 0600) != 0) {
    throw std::system_error(
        errno, std::generic_category(), "PerfScoped: mkfifo failed");
  }
  // O_RDWR: opening a fifo read-only blocks until perf opens the write end.
  const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    throw std::system_error(
        errno, std::generic_category(), "PerfScoped: opening fifo failed");
  }
  return File{fd, /* ownsFd */ true};
}

std::string readProcFile(cstring_view path) {
  std::string out;
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return out;
  }
  File file{fd, /* ownsFd */ true};
  char buffer[1024];
  ssize_t bytes;
  while ((bytes = readNoInt(file.fd(), buffer, sizeof(buffer))) > 0) {
    out.append(buffer, to_unsigned(bytes));
  }
  return out;
}

void logAbandonedChild(pid_t pid) {
  std::cerr
      << "PerfScoped: perf (pid " << pid
      << ") is still running after SIGINT and SIGKILL; abandoning it "
         "so the test can proceed. Kernel stack:\n"
      << readProcFile(fmt::format("/proc/{}/stack", pid)) << "wchan: "
      << readProcFile(fmt::format("/proc/{}/wchan", pid)) << std::endl;
}

} // namespace

class PerfScoped::PerfScopedImpl {
 public:
  PerfScopedImpl(const std::vector<std::string>& args, std::string* output)
      : ctlFile_(makeControlFifo(ctlFifoPath(controlDir_.path()))),
        ackFile_(makeControlFifo(ackFifoPath(controlDir_.path()))),
        proc_(
            buildArgs(
                args,
                controlDir_.path(),
                output != nullptr ? &outputFile_ : nullptr),
            subprocessOptions()),
        output_(output) {
    sendCommand(kEnableCommand, kAttachTimeout);
  }

  PerfScopedImpl(const PerfScopedImpl&) = delete;
  PerfScopedImpl(PerfScopedImpl&&) = delete;
  PerfScopedImpl& operator=(const PerfScopedImpl&) = delete;
  PerfScopedImpl& operator=(PerfScopedImpl&&) = delete;

  ~PerfScopedImpl() noexcept {
    try {
      if (running()) {
        sendCommand(kDisableCommand, kDetachTimeout);
      }
    } catch (const std::exception& e) {
      std::cerr << "PerfScoped: could not stop the perf counters cleanly: "
                << e.what() << std::endl;
    }

    try {
      if (proc_.returnCode().running()) {
        proc_.sendSignal(SIGINT);
        auto rc = proc_.waitTimeout(kStopTimeout);
        if (rc.running()) {
          // perf ignored SIGINT, do not hang the test.
          proc_.kill();
          rc = proc_.waitTimeout(kKillTimeout);
        }
        if (rc.running()) {
          // perf survived SIGKILL (e.g. uninterruptible sleep); abandon it
          // rather than hanging the test. allowDestructionWhileProcessRunning
          // makes ~Subprocess leak it instead of blocking on waitpid.
          logAbandonedChild(proc_.pid());
          return;
        }
      }
      if (output_) {
        readFile(outputFile_.fd(), *output_);
      }
    } catch (const std::exception& e) {
      std::cerr << "PerfScoped: perf teardown failed, results may be "
                << "incomplete: " << e.what() << std::endl;
    }
  }

 private:
  bool running() {
    return proc_.returnCode().running() && proc_.poll().running();
  }

  void sendCommand(
      std::string_view command, std::chrono::milliseconds timeout) {
    const auto written =
        writeFull(ctlFile_.fd(), command.data(), command.size());
    if (written != static_cast<ssize_t>(command.size())) {
      throw std::system_error(
          errno,
          std::generic_category(),
          "PerfScoped: writing perf control command failed");
    }
    awaitAck(timeout);
  }

  void awaitAck(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string got;

    while (got.find('\n') == std::string::npos) {
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now());
      if (remaining.count() <= 0) {
        throw std::runtime_error(
            fmt::format(
                "PerfScoped: timed out after {}ms waiting for perf to "
                "acknowledge a control command.",
                timeout.count()));
      }

      pollfd entry{};
      entry.fd = ackFile_.fd();
      entry.events = POLLIN;
      const int ready = ::poll(
          &entry,
          1,
          static_cast<int>(std::min<int64_t>(remaining.count(), 100)));
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw std::system_error(
            errno, std::generic_category(), "PerfScoped: poll failed");
      }
      if (ready == 0) {
        if (!running()) {
          throw std::runtime_error(
              "PerfScoped: perf exited before acknowledging a control "
              "command. Check the arguments passed to perf.");
        }
        continue;
      }

      char buffer[64];
      const auto bytes = readNoInt(ackFile_.fd(), buffer, sizeof(buffer));
      if (bytes == 0) {
        throw std::runtime_error(
            "PerfScoped: perf closed the control channel before "
            "acknowledging a control command.");
      }
      if (bytes < 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "PerfScoped: reading perf's acknowledgement failed");
      }
      got.append(buffer, static_cast<std::size_t>(bytes));
    }
  }

  test::TemporaryDirectory controlDir_;
  File ctlFile_;
  File ackFile_;
  test::TemporaryFile outputFile_;
  Subprocess proc_;
  std::string* output_;
};

PerfScoped::PerfScoped(
    const std::vector<std::string>& args, std::string* output)
    : pimpl_(std::make_unique<PerfScopedImpl>(args, output)) {}

#else // FOLLY_PERF_IS_SUPPORTED

class PerfScoped::PerfScopedImpl {};

PerfScoped::PerfScoped(
    const std::vector<std::string>& args, std::string* output) {
  (void)args;
  (void)output;
  throw std::runtime_error("Perf is not supported on Windows.");
}

#endif

PerfScoped::PerfScoped() = default;
PerfScoped::PerfScoped(PerfScoped&&) noexcept = default;
PerfScoped& PerfScoped::operator=(PerfScoped&&) noexcept = default;
PerfScoped::~PerfScoped() noexcept = default;

} // namespace detail
} // namespace folly
