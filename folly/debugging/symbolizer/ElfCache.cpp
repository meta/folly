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

/// Finding or adding an entry calls into the standard library's tree code.
/// Keep it out of line so warmup runs exactly the code getFile() runs. The
/// lower_bound() hint keeps a hit from building an entry, and it only ever
/// makes emplace_hint() step backward from the hint.
template <typename Entries, typename Alloc>
FOLLY_NOINLINE typename Entries::iterator findOrAdd(
    Entries& entries, StringPiece p, Alloc const& alloc) noexcept {
  auto const path = std::string_view(p.data(), p.size());
  auto it = entries.lower_bound(path);
  if (it == entries.end() || entries.key_comp()(path, *it)) {
    it = entries.emplace_hint(it, p, alloc);
  }
  return it;
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

  // Add entries as getFile() does: into an empty set, after the last entry,
  // before the first one, and between two. Use paths of realistic length so
  // that copying them takes the same branches as for a real path.
  static constexpr std::string_view kPaths[] = {
      "/warmup/nonexistent/directory/entry-b.so",
      "/warmup/nonexistent/directory/entry-d.so",
      "/warmup/nonexistent/directory/entry-a.so",
      "/warmup/nonexistent/directory/entry-c.so",
  };
  State state;
  for (std::string_view const path : kPaths) {
    auto piece = StringPiece(path.data(), path.size());
    compiler_must_not_predict(piece);
    findOrAdd(state.entries, piece, state.alloc);
  }
  for (Entry const& entry : state.entries) {
    compiler_must_not_elide(entry.init);
  }
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

bool SignalSafeElfCache::EntryLess::operator()(
    std::string_view a, std::string_view b) const noexcept {
  return pathLess(a, b);
}

std::shared_ptr<ElfFile> SignalSafeElfCache::getFile(StringPiece p) {
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

  Entry const& entry = *findOrAdd(state_->entries, p, state_->alloc);

  if (!entry.init) {
    int r = entry.file->openAndFollow(cstring_view(entry.path).c_str());
    entry.init = r == ElfFile::kSuccess;
  }
  if (!entry.init) {
    return nullptr;
  }

  return entry.file;
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
