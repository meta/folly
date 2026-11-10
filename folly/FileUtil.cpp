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

#include <folly/FileUtil.h>

#include <cerrno>
#include <string>
#include <system_error>

#include <folly/detail/FileUtilDetail.h>
#include <folly/detail/FileUtilVectorDetail.h>
#include <folly/net/NetOps.h>
#include <folly/portability/Fcntl.h>
#include <folly/portability/Sockets.h>
#include <folly/portability/Stdlib.h>
#include <folly/portability/SysFile.h>
#include <folly/portability/SysStat.h>

namespace folly {
namespace {
iovec getIOVecFor(ByteRange);
} // namespace

using namespace fileutil_detail;

int openNoInt(const char* name, int flags, mode_t mode) {
  // Android NDK bionic with FORTIFY has this definition:
  // https://android.googlesource.com/platform/bionic/+/9349b9e51b/libc/include/bits/fortify/fcntl.h
  // ```
  // __BIONIC_ERROR_FUNCTION_VISIBILITY
  // int open(const char* pathname, int flags, mode_t modes, ...) __overloadable
  //         __errorattr(__open_too_many_args_error);
  // ```
  // This is originally to prevent open() with incorrect parameters.
  //
  // However, combined with folly wrapNotInt, template deduction will fail.
  // In this case, we create a custom lambda to bypass the error.
  // The solution is referenced from
  // https://github.com/llvm/llvm-project/commit/0a0e411204a2baa520fd73a8d69b664f98b428ba
  //
  auto openWrapper = [&] { return fileops::open(name, flags, mode); };
  return wrapNoInt(openWrapper);
}

static int filterCloseReturn(int r) {
  // Ignore EINTR.  On Linux, close() may only return EINTR after the file
  // descriptor has been closed, so you must not retry close() on EINTR --
  // in the best case, you'll get EBADF, and in the worst case, you'll end up
  // closing a different file (one opened from another thread).
  //
  // Interestingly enough, the Single Unix Specification says that the state
  // of the file descriptor is unspecified if close returns EINTR.  In that
  // case, the safe thing to do is also not to retry close() -- leaking a file
  // descriptor is definitely better than closing the wrong file.
  if (r == -1 && errno == EINTR) {
    return 0;
  }
  return r;
}

int closeNoInt(int fd) {
  return filterCloseReturn(fileops::close(fd));
}

int closeNoInt(NetworkSocket fd) {
  return filterCloseReturn(netops::close(fd));
}

int fsyncNoInt(int fd) {
  return wrapNoInt(fsync, fd);
}

int dupNoInt(int fd) {
  return wrapNoInt(dup, fd);
}

int dup2NoInt(int oldFd, int newFd) {
  return wrapNoInt(dup2, oldFd, newFd);
}

int fdatasyncNoInt(int fd) {
#if defined(__APPLE__)
  return wrapNoInt(fcntl, fd, F_FULLFSYNC);
#elif defined(__FreeBSD__) || defined(_MSC_VER)
  return wrapNoInt(fsync, fd);
#else
  return wrapNoInt(fdatasync, fd);
#endif
}

int ftruncateNoInt(int fd, off_t len) {
  return wrapNoInt(ftruncate, fd, len);
}

int truncateNoInt(const char* path, off_t len) {
  return wrapNoInt(truncate, path, len);
}

int flockNoInt(int fd, int operation) {
  return wrapNoInt(flock, fd, operation);
}

int shutdownNoInt(NetworkSocket fd, int how) {
  return wrapNoInt(netops::shutdown, fd, how);
}

ssize_t readNoInt(int fd, void* buf, size_t count) {
  return wrapNoInt(folly::fileops::read, fd, buf, count);
}

ssize_t preadNoInt(int fd, void* buf, size_t count, off_t offset) {
  return wrapNoInt(pread, fd, buf, count, offset);
}

ssize_t readvNoInt(int fd, const iovec* iov, int count) {
  return wrapNoInt(readv, fd, iov, count);
}

ssize_t preadvNoInt(int fd, const iovec* iov, int count, off_t offset) {
  return wrapNoInt(preadv, fd, iov, count, offset);
}

ssize_t writeNoInt(int fd, const void* buf, size_t count) {
  return wrapNoInt(folly::fileops::write, fd, buf, count);
}

ssize_t pwriteNoInt(int fd, const void* buf, size_t count, off_t offset) {
  return wrapNoInt(pwrite, fd, buf, count, offset);
}

ssize_t writevNoInt(int fd, const iovec* iov, int count) {
  return wrapNoInt(writev, fd, iov, count);
}

ssize_t pwritevNoInt(int fd, const iovec* iov, int count, off_t offset) {
  return wrapNoInt(pwritev, fd, iov, count, offset);
}

ssize_t readFull(int fd, void* buf, size_t count) {
  return wrapFull(folly::fileops::read, fd, buf, count);
}

ssize_t preadFull(int fd, void* buf, size_t count, off_t offset) {
  return wrapFull(pread, fd, buf, count, offset);
}

ssize_t writeFull(int fd, const void* buf, size_t count) {
  return wrapFull(folly::fileops::write, fd, const_cast<void*>(buf), count);
}

ssize_t pwriteFull(int fd, const void* buf, size_t count, off_t offset) {
  return wrapFull(pwrite, fd, const_cast<void*>(buf), count, offset);
}

#ifndef _WIN32
ssize_t readvFull(int fd, iovec* iov, int count) {
  return wrapvFull(readv, fd, iov, count);
}

ssize_t preadvFull(int fd, iovec* iov, int count, off_t offset) {
  return wrapvFull(preadv, fd, iov, count, offset);
}

ssize_t writevFull(int fd, iovec* iov, int count) {
  return wrapvFull(writev, fd, iov, count);
}

ssize_t pwritevFull(int fd, iovec* iov, int count, off_t offset) {
  return wrapvFull(pwritev, fd, iov, count, offset);
}
#else // _WIN32

// On Windows, the *vFull() functions wrap the simple read/pread/write/pwrite
// functions.  While folly/portability/SysUio.cpp does define readv() and
// writev() implementations for Windows, these attempt to lock the file to
// provide atomicity.  The *vFull() functions do not provide any atomicity
// guarantees, so we can avoid the locking logic.

ssize_t readvFull(int fd, iovec* iov, int count) {
  return wrapvFull(folly::fileops::read, fd, iov, count);
}

ssize_t preadvFull(int fd, iovec* iov, int count, off_t offset) {
  return wrapvFull(pread, fd, iov, count, offset);
}

ssize_t writevFull(int fd, iovec* iov, int count) {
  return wrapvFull(folly::fileops::write, fd, iov, count);
}

ssize_t pwritevFull(int fd, iovec* iov, int count, off_t offset) {
  return wrapvFull(pwrite, fd, iov, count, offset);
}
#endif // _WIN32

WriteFileAtomicOptions& WriteFileAtomicOptions::setPermissions(
    mode_t _permissions) {
  permissions = _permissions;
  return *this;
}

WriteFileAtomicOptions& WriteFileAtomicOptions::setSyncType(
    SyncType _syncType) {
  syncType = _syncType;
  return *this;
}

WriteFileAtomicOptions& WriteFileAtomicOptions::setTemporaryDirectory(
    std::string _temporaryDirectory) {
  temporaryDirectory = std::move(_temporaryDirectory);
  return *this;
}

namespace {
void throwIfWriteFileAtomicFailed(
    std::string_view function, std::string_view filename, std::int64_t rc) {
  if (rc != 0) {
    auto msg =
        std::string{function} + "() failed to update " + std::string{filename};
    throw std::system_error(rc, std::generic_category(), msg);
  }
}

// We write the data to a temporary file name first, then atomically rename
// it into place.
//
// If SyncType::WITH_SYNC is used, this ensures that the file contents will
// always be valid, even if we crash or are killed partway through writing out
// data.
int writeFileAtomicNoThrowImpl(
    std::string_view filename,
    iovec* iov,
    int count,
    const WriteFileAtomicOptions& options) {
  // create a null-terminated version of the filename
  auto filePathString = std::string{filename};
  auto temporaryFilePathString = fileutil_detail::getTemporaryFilePathString(
      filePathString, options.temporaryDirectory);

  auto tmpFD = mkstemp(const_cast<char*>(temporaryFilePathString.data()));
  if (tmpFD == -1) {
    return errno;
  }
  bool success = false;
  SCOPE_EXIT {
    if (tmpFD != -1) {
      fileops::close(tmpFD);
    }
    if (!success) {
      unlink(temporaryFilePathString.c_str());
    }
  };

  auto rc = writevFull(tmpFD, iov, count);
  if (rc == -1) {
    return errno;
  }

  rc = fchmod(tmpFD, options.permissions);
  if (rc == -1) {
    return errno;
  }

  // To guarantee atomicity across power failues on POSIX file systems,
  // the temporary file must be explicitly sync'ed before the rename.
  if (options.syncType == SyncType::WITH_SYNC) {
    rc = fsyncNoInt(tmpFD);
    if (rc == -1) {
      return errno;
    }
  }

  // Close the file before renaming to make sure all data has
  // been successfully written.
  rc = fileops::close(tmpFD);
  tmpFD = -1;
  if (rc == -1) {
    return errno;
  }

  rc = rename(temporaryFilePathString.c_str(), filePathString.c_str());
  if (rc == -1) {
    return errno;
  }
  success = true;
  return 0;
}
} // namespace

int writeFileAtomicNoThrow(
    std::string_view filename,
    iovec* iov,
    int count,
    mode_t permissions,
    SyncType syncType) {
  return writeFileAtomicNoThrowImpl(
      filename,
      iov,
      count,
      WriteFileAtomicOptions{}
          .setPermissions(permissions)
          .setSyncType(syncType));
}

int writeFileAtomicNoThrow(
    std::string_view filename,
    std::string_view data,
    const WriteFileAtomicOptions& options) {
  auto iov = getIOVecFor(ByteRange{data});
  return writeFileAtomicNoThrowImpl(filename, &iov, 1, options);
}

void writeFileAtomic(
    std::string_view filename,
    iovec* iov,
    int count,
    mode_t permissions,
    SyncType syncType) {
  auto rc = writeFileAtomicNoThrowImpl(
      filename,
      iov,
      count,
      WriteFileAtomicOptions{}
          .setPermissions(permissions)
          .setSyncType(syncType));
  throwIfWriteFileAtomicFailed(__func__, filename, rc);
}

void writeFileAtomic(
    std::string_view filename,
    ByteRange data,
    mode_t permissions,
    SyncType syncType) {
  auto iov = getIOVecFor(data);
  writeFileAtomic(filename, &iov, 1, permissions, syncType);
}

void writeFileAtomic(
    std::string_view filename,
    std::string_view data,
    mode_t permissions,
    SyncType syncType) {
  writeFileAtomic(filename, ByteRange(data), permissions, syncType);
}

void writeFileAtomic(
    std::string_view filename,
    std::string_view data,
    const WriteFileAtomicOptions& options) {
  auto iov = getIOVecFor(ByteRange{data});
  auto rc = writeFileAtomicNoThrowImpl(filename, &iov, 1, options);

  throwIfWriteFileAtomicFailed(__func__, filename, rc);
}

namespace {
iovec getIOVecFor(ByteRange byteRange) {
  iovec iov;
  iov.iov_base = const_cast<unsigned char*>(byteRange.data());
  iov.iov_len = byteRange.size();
  return iov;
}
} // namespace
} // namespace folly
