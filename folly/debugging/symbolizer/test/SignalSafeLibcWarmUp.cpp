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

#include <cerrno>
#include <cstdio>

#include <folly/debugging/symbolizer/detail/SignalSafeLibc.h>
#include <folly/lang/CString.h>
#include <folly/lang/Hint.h>

int main(int argc, char**) {
  namespace libc = folly::symbolizer::detail::signal_safe_libc;
  errno = ERANGE;
  if (argc == 1) {
    libc::warmUp();
    if (errno != ERANGE) {
      return 1;
    }
  }

  std::fputs("libc-operations-begin\n", stderr);
  const int fd = libc::open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
  if (fd == -1) {
    return 1;
  }
  struct stat stat{};
  char byte{};
  char path[32];
  const bool fileOk = libc::fstat(fd, &stat) == 0 && stat.st_size > 0 &&
      libc::lseek(fd, 0, SEEK_SET) == 0 && libc::pread(fd, &byte, 1, 0) == 1 &&
      libc::read(fd, &byte, 1) == 1 &&
      libc::readlink("/proc/self/exe", path, sizeof(path)) > 0;
  const bool closed = libc::close(fd) == 0;
  auto* mapping = libc::mmap(
      nullptr, 1, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED) {
    return 1;
  }
  const char* left = "a";
  const char* right = "b";
  folly::compiler_must_not_predict(left);
  folly::compiler_must_not_predict(right);
  const bool stringOk =
      libc::memcmp(left, right, 1) < 0 && libc::strrchr(left, 'a') == left;
  char copied[2];
  (void)libc::memcpy(mapping, left, 1);
  const bool memoryOk = *static_cast<char*>(mapping) == 'a' &&
      libc::strlcpy(copied, left, sizeof(copied)) == 1 && copied[0] == 'a' &&
      copied[1] == '\0';
  const bool unmapped = libc::munmap(mapping, 1) == 0;
  sigset_t set;
  const bool signalOk = libc::fill_signal_set(&set) == 0 &&
      libc::set_signal_mask(SIG_SETMASK, nullptr, &set) == 0;
  std::fputs("libc-operations-end\n", stderr);
  return fileOk && closed && stringOk && memoryOk && unmapped && signalOk
      ? 0
      : 1;
}
