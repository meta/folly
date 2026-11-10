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

#include <folly/File.h>

#include <folly/Exception.h>
#include <folly/FileUtil.h>
#ifdef _WIN32
#include <folly/ScopeGuard.h>
#else
#include <folly/portability/Filesystem.h>
#endif
#include <folly/portability/Fcntl.h>
#include <folly/portability/FmtCompile.h>
#include <folly/portability/SysFile.h>
#include <folly/portability/Unistd.h>

#include <system_error>

#include <glog/logging.h>

namespace folly {

File::File(int fd, bool ownsFd) noexcept : fd_(fd), ownsFd_(ownsFd) {
  CHECK_GE(fd, -1) << "fd must be -1 or non-negative";
  CHECK(fd != -1 || !ownsFd) << "cannot own -1";
}

File::File(const char* name, int flags, mode_t mode)
    : fd_(fileops::open(name, flags, mode)), ownsFd_(false) {
  if (fd_ == -1) {
    throwSystemError(
        fmt::format(
            FOLLY_FMT_COMPILE("open(\"{}\", {:#o}, 0{:#o}) failed"),
            name,
            flags,
            mode));
  }
  ownsFd_ = true;
}

File::File(const std::string& name, int flags, mode_t mode)
    : File(name.c_str(), flags, mode) {}

File::File(std::string_view name, int flags, mode_t mode)
    : File(std::string(name), flags, mode) {}

File::File(File&& other) noexcept : fd_(other.fd_), ownsFd_(other.ownsFd_) {
  other.release();
}

File& File::operator=(File&& other) {
  closeNoThrow();
  swap(other);
  return *this;
}

File::~File() {
  auto fd = fd_;
  if (!closeNoThrow()) { // ignore most errors
    DCHECK_NE(errno, EBADF)
        << "closing fd " << fd << ", it may already "
        << "have been closed. Another time, this might close the wrong FD.";
  }
}

/* static */ File File::temporary() {
#ifdef _WIN32
  // Windows tmpfile_s creates an auto-deleting anonymous temp file and provides
  // better error reporting than tmpfile().
  FILE* tmpFile = nullptr;
  errno_t err = tmpfile_s(&tmpFile);
  if (err != 0 || !tmpFile) {
    throwSystemErrorExplicit(err, "tmpfile_s() failed");
  }
  SCOPE_EXIT {
    fclose(tmpFile);
  };
  int fd = ::dup(fileno(tmpFile));
  checkUnixError(fd, "dup() failed");
  return File(fd, true);
#else
  // Use mkstemp rather than tmpfile() because Android bionic's tmpfile()
  // hardcodes /data/local/tmp and ignores $TMPDIR, failing with EACCES inside
  // instrumentation test APKs where the process lacks write access to that
  // directory.  fs::temp_directory_path() reads $TMPDIR and works on all POSIX
  // platforms.
  auto tmpPath = fs::temp_directory_path() / "folly_tmp_XXXXXX";
  std::string pathStr = tmpPath.string();
  // TODO(nga): consider setting close-on-exec for the resulting FD
  int fd = ::mkstemp(pathStr.data());
  checkUnixError(fd, "mkstemp() failed");
  if (::unlink(pathStr.c_str()) != 0) {
    auto errnoCopy = errno;
    ::close(fd);
    throwSystemErrorExplicit(errnoCopy, "unlink() failed");
  }
  return File(fd, true);
#endif
}

int File::release() noexcept {
  int released = fd_;
  fd_ = -1;
  ownsFd_ = false;
  return released;
}

void File::swap(File& other) noexcept {
  using std::swap;
  swap(fd_, other.fd_);
  swap(ownsFd_, other.ownsFd_);
}

void swap(File& a, File& b) noexcept {
  a.swap(b);
}

File File::dup() const {
  if (fd_ != -1) {
    int fd = ::dup(fd_);
    checkUnixError(fd, "dup() failed");

    return File(fd, true);
  }

  return File();
}

File File::dupCloseOnExec() const {
  if (fd_ != -1) {
    int fd;
#ifdef _WIN32
    fd = ::dup(fd_);
#else
    fd = ::fcntl(fd_, F_DUPFD_CLOEXEC, 0);
#endif
    checkUnixError(fd, "dup() failed");

    return File(fd, true);
  }

  return File();
}

void File::close() {
  if (!closeNoThrow()) {
    throwSystemError("close() failed");
  }
}

bool File::closeNoThrow() {
  int r = ownsFd_ ? fileops::close(fd_) : 0;
  release();
  return r == 0;
}

void File::lock() {
  doLock(LOCK_EX);
}
bool File::try_lock() {
  return doTryLock(LOCK_EX);
}
void File::lock_shared() {
  doLock(LOCK_SH);
}
bool File::try_lock_shared() {
  return doTryLock(LOCK_SH);
}

void File::doLock(int op) {
  checkUnixError(flockNoInt(fd_, op), "flock() failed (lock)");
}

bool File::doTryLock(int op) {
  int r = flockNoInt(fd_, op | LOCK_NB);
  // flock returns EWOULDBLOCK if already locked
  if (r == -1 && errno == EWOULDBLOCK) {
    return false;
  }
  checkUnixError(r, "flock() failed (try_lock)");
  return true;
}

void File::unlock() {
  checkUnixError(flockNoInt(fd_, LOCK_UN), "flock() failed (unlock)");
}
void File::unlock_shared() {
  unlock();
}

} // namespace folly
