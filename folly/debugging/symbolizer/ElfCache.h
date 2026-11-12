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

#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <folly/Optional.h>
#include <folly/Range.h>
#include <folly/debugging/symbolizer/Elf.h>
#include <folly/hash/Hash.h>
#include <folly/lang/cstring_view.h>
#include <folly/memory/ReentrantAllocator.h>
#include <folly/portability/Config.h>

namespace folly {
namespace symbolizer {

#if FOLLY_HAVE_ELF

class ElfCacheBase {
 public:
  virtual std::shared_ptr<ElfFile> getFile(StringPiece path) = 0;
  virtual ~ElfCacheBase() = default;
  ElfCacheBase() = default;
  ElfCacheBase(const ElfCacheBase&) = delete;
  ElfCacheBase& operator=(const ElfCacheBase&) = delete;
  ElfCacheBase(ElfCacheBase&&) = delete;
  ElfCacheBase& operator=(ElfCacheBase&&) = delete;
};

/**
 * Cache ELF files. Async-signal-safe: does memory allocation via mmap.
 *
 * Not MT-safe. May not be used concurrently from multiple threads.
 */
class SignalSafeElfCache : public ElfCacheBase {
 public:
  std::shared_ptr<ElfFile> getFile(StringPiece path) override;

  /// Eagerly resolve comparison and length-check bindings before installing
  /// signal handlers: binding resolution is not async-signal-safe. No stack
  /// capture or symbolization is performed. Adds entries to a scratch cache,
  /// whose allocator aborts if mmap fails.
  static void warmUp() noexcept;

 protected:
  //  Path
  //
  //  A minimal implementation of the subset of std::string used below, as if:
  //
  //    using Path = std::basic_string<
  //        char, std::char_traits<char>, reentrant_allocator<char>>;
  //
  //  Since some library implementations of std::basic_string, as on CentOS 7,
  //  do not build when instantiated with a non-default-constructible allocator,
  //  and since other library replacements, such as folly::basic_fbstring, just
  //  ignore the allocator parameter.
  class Path {
   public:
    Path(
        char const* data,
        std::size_t size,
        reentrant_allocator<char> const& alloc) noexcept;
    ~Path() = default;
    Path() = delete;
    Path(Path const&) = delete;
    void operator=(Path const&) = delete;
    Path(Path&&) = delete;
    Path& operator=(Path&&) = delete;

    /* implicit */ operator cstring_view() const noexcept {
      return {data_.data(), data_.size() - 1};
    }

   private:
    std::vector<char, reentrant_allocator<char>> data_;
  };

 private:
  friend bool operator<(Path const& a, Path const& b) noexcept;

  struct Entry {
    Path path;
    std::shared_ptr<ElfFile> file;
    // Not part of the key, so updating it in the set is safe.
    mutable bool init = false;

    explicit Entry(StringPiece p, reentrant_allocator<char> alloc) noexcept
        : path{p.data(), p.size(), alloc},
          file{std::allocate_shared<ElfFile>(alloc)} {}
    ~Entry() = default;
    Entry(Entry const&) = delete;
    Entry& operator=(Entry const& that) = delete;
    Entry(Entry&&) = delete;
    Entry& operator=(Entry&&) = delete;

    /* implicit */ operator std::string_view() const noexcept {
      return cstring_view(path);
    }
  };

  // Orders entries by path, and finds them by path without building one.
  struct EntryLess {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept;
  };

  struct State {
    reentrant_allocator<void> alloc{
        reentrant_allocator_options().block_size_lg(16).large_size_lg(12)};
    // Must follow alloc, which it is constructed from.
    std::set<Entry, EntryLess, reentrant_allocator<Entry>> entries{alloc};
  };
  Optional<State> state_;
};

/**
 * General-purpose ELF file cache.
 *
 * LRU of given capacity. MT-safe (uses locking). Not async-signal-safe.
 */
class ElfCache : public ElfCacheBase {
 public:
  std::shared_ptr<ElfFile> getFile(StringPiece path) override;

 private:
  std::mutex mutex_;

  struct Entry {
    std::string path;
    ElfFile file;
  };

  static std::shared_ptr<ElfFile> filePtr(const std::shared_ptr<Entry>& e);

  std::unordered_map<StringPiece, std::shared_ptr<Entry>, Hash> files_;
};

#endif // FOLLY_HAVE_ELF

} // namespace symbolizer
} // namespace folly
