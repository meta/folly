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

#include <folly/portability/Stdio.h>

#ifdef _WIN32

#include <cstdlib>

#include <folly/ScopeGuard.h>
#include <folly/portability/Unistd.h>

extern "C" {
int dprintf(int fd, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  SCOPE_EXIT {
    va_end(args);
  };

  // vsnprintf will consume it, so better copy args before using them
  va_list argsCopy;
  va_copy(argsCopy, args);
  int ret = vsnprintf(nullptr, 0, fmt, argsCopy);
  va_end(argsCopy);
  if (ret <= 0) {
    return -1;
  }
  size_t len = size_t(ret);
  char* buf = new char[len + 1];
  SCOPE_EXIT {
    delete[] buf;
  };
  if (size_t(vsnprintf(buf, len + 1, fmt, args)) == len &&
      folly::fileops::write(fd, buf, len) == ssize_t(len)) {
    return ret;
  }

  return -1;
}

int pclose(FILE* f) {
  return _pclose(f);
}

FILE* popen(const char* name, const char* mode) {
  return _popen(name, mode);
}

void setbuffer(FILE* f, char* buf, size_t size) {
  setvbuf(f, buf, _IOFBF, size);
}

int vasprintf(char** dest, const char* format, va_list ap) {
  va_list ap1;
  va_copy(ap1, ap);
  int len = vsnprintf(nullptr, 0, format, ap1);
  va_end(ap1);

  if (len <= 0) {
    return -1;
  }

  char* buf = (char*)malloc(size_t(len + 1));
  if (!buf) {
    return -1;
  }

  va_list ap2;
  va_copy(ap2, ap);
  int const rc = vsnprintf(buf, size_t(len + 1), format, ap2);
  va_end(ap2);

  if (rc == len) {
    *dest = buf;
    return len;
  }

  free(buf);
  *dest = nullptr;
  return -1;
}
}

#endif