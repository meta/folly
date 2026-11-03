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

#include <folly/debugging/symbolizer/ElfCache.h>

#include <signal.h>
#include <string_view>

#include <folly/CPortability.h>
#include <folly/ScopeGuard.h>
#include <folly/debugging/symbolizer/detail/SignalSafeLibc.h>
#include <folly/lang/Hint.h>
#include <folly/portability/Config.h>
#include <folly/portability/SysMman.h>

#if FOLLY_HAVE_ELF

namespace folly {
namespace symbolizer {

namespace {

/// Binding resolution is not async-signal-safe. Keep this comparison out of
/// line so warmup eagerly resolves its libc binding before installing handlers.
FOLLY_NOINLINE bool pathLess(
    std::string_view lhs, std::string_view rhs) noexcept {
  return lhs < rhs;
}

} // namespace

void SignalSafeElfCache::warmUp() noexcept {
  detail::signal_safe_libc::warmUp();
  const char* left = "a";
  const char* right = "b";
  compiler_must_not_predict(left);
  compiler_must_not_predict(right);
  auto lhs = cstring_view(left, 1);
  auto rhs = cstring_view(right, 1);
  compiler_must_not_predict(lhs);
  compiler_must_not_predict(rhs);
  const bool less = pathLess(lhs, rhs);
  compiler_must_not_elide(less);
}

SignalSafeElfCache::Path::Path(
    char const* const data,
    std::size_t const size,
    reentrant_allocator<char> const& alloc) noexcept
    : data_{alloc} {
  data_.reserve(size + 1);
  data_.insert(data_.end(), data, data + size);
  data_.insert(data_.end(), '\0');
}

bool operator<(
    SignalSafeElfCache::Path const& lhs,
    SignalSafeElfCache::Path const& rhs) noexcept {
  return pathLess(cstring_view(lhs), cstring_view(rhs));
}

std::shared_ptr<ElfFile> SignalSafeElfCache::getFile(StringPiece p) {
  struct cmp {
    bool operator()(Entry const& a, StringPiece b) const noexcept {
      return pathLess(
          cstring_view(a.path), std::string_view(b.data(), b.size()));
    }
    bool operator()(StringPiece a, Entry const& b) const noexcept {
      return pathLess(
          std::string_view(a.data(), a.size()), cstring_view(b.path));
    }
  };

  sigset_t newsigs;
  sigset_t oldsigs;
  if (detail::signal_safe_libc::fill_signal_set(&newsigs) != 0 ||
      detail::signal_safe_libc::set_signal_mask(
          SIG_SETMASK, &newsigs, &oldsigs) != 0) {
    return nullptr;
  }
  SCOPE_EXIT {
    detail::signal_safe_libc::set_signal_mask(SIG_SETMASK, &oldsigs, nullptr);
  };

  if (!state_) {
    state_.emplace();
  }

  auto pos = state_->map.find(p, cmp{});
  if (pos == state_->map.end()) {
    state_->list.emplace_front(p, state_->alloc);
    pos = state_->map.insert(state_->list.front()).first;
  }

  if (!pos->init) {
    int r = pos->file->openAndFollow(cstring_view(pos->path).c_str());
    pos->init = r == ElfFile::kSuccess;
  }
  if (!pos->init) {
    return nullptr;
  }

  return pos->file;
}

std::shared_ptr<ElfFile> ElfCache::getFile(StringPiece p) {
  std::lock_guard lock(mutex_);

  auto pos = files_.find(p);
  if (pos != files_.end()) {
    // Found
    auto& entry = pos->second;
    return filePtr(entry);
  }

  auto entry = std::make_shared<Entry>();
  entry->path = p.str();
  auto& path = entry->path;

  // No negative caching
  int r = entry->file.openAndFollow(path.c_str());
  if (r != ElfFile::kSuccess) {
    return nullptr;
  }

  pos = files_.emplace(path, std::move(entry)).first;

  return filePtr(pos->second);
}

std::shared_ptr<ElfFile> ElfCache::filePtr(const std::shared_ptr<Entry>& e) {
  // share ownership
  return std::shared_ptr<ElfFile>(e, &e->file);
}
} // namespace symbolizer
} // namespace folly

#endif // FOLLY_HAVE_ELF
