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

#include <array>
#include <cassert>
#include <chrono>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <folly/Chrono.h>
#include <folly/Function.h>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <folly/container/Reserve.h>
#include <folly/hash/UniqueHashKey.h>
#include <folly/lang/Bits.h>

namespace folly {

/// RegexMatchCacheDynamicBitset
///
/// A dynamic bitset for use within, and optimized for, RegexMatchCache.
/// * Small, having the same size and alignment as a pointer.
/// * Optimistically non-allocating, using in-situ storage for small bitsets.
///
/// Intended for use only within RegexMatchCache.
///
/// Incomplete as a generic container.
class RegexMatchCacheDynamicBitset {
 private:
  template <typename Word>
  struct bit_span {
    Word* data;
    size_t size;

    bit_span(Word* const data_, size_t const size_) noexcept
        : data{data_}, size{size_} {}
    bit_span(bit_span const&) = default;
    bit_span& operator=(bit_span const&) = default;

    auto as_tuple() const noexcept { return std::tuple{data, size}; }

    friend bool operator==(bit_span const& a, bit_span const& b) noexcept {
      return a.as_tuple() == b.as_tuple();
    }
  };

 public:
  RegexMatchCacheDynamicBitset() = default;

  RegexMatchCacheDynamicBitset(RegexMatchCacheDynamicBitset const&) = delete;
  RegexMatchCacheDynamicBitset(RegexMatchCacheDynamicBitset&& that) noexcept
      : data_{std::exchange(that.data_, {})} {}

  ~RegexMatchCacheDynamicBitset() { reset_(); }

  void operator=(RegexMatchCacheDynamicBitset const&) = delete;
  RegexMatchCacheDynamicBitset& operator=(
      RegexMatchCacheDynamicBitset&& that) noexcept {
    reset_();
    data_ = std::exchange(that.data_, {});
    return *this;
  }

  bool get_value(size_t const index) const noexcept {
    auto data = get_bit_span_();
    if (!(index < data.size)) {
      return false;
    }
    return get_value_(data, index);
  }

  void set_value(size_t const index, bool const value) {
    constexpr auto wordbits = sizeof(uintptr_t) * 8;
    auto data = get_bit_span_();

    if (!(index < data.size) ||
        (data.size == wordbits && index == wordbits - 1)) {
      if (!value) {
        return;
      }
      data = reserve_(index);
    }
    assert(index < data.size);
    set_value_(data, index, value);
  }

  void reset() noexcept { reset_(); }

  class index_set_view {
   private:
    friend RegexMatchCacheDynamicBitset;
    bit_span<uintptr_t const> bitset_;

    explicit index_set_view(RegexMatchCacheDynamicBitset const& bitset) noexcept
        : bitset_{bitset.get_bit_span_()} {}

   public:
    using value_type = size_t;

    class const_iterator {
     public:
      using value_type = size_t;
      using difference_type = ptrdiff_t;
      using pointer = void;
      using iterator_category = std::forward_iterator_tag;

      struct reference {
       private:
        friend class const_iterator;

        size_t const index_;

        explicit reference(size_t const index) noexcept : index_{index} {}

       public:
        operator size_t() const noexcept { return index_; }
      };

     private:
      using self = const_iterator;

      bit_span<uintptr_t const> const data_;
      size_t index_;

      size_t ceil_valid_index(size_t index) const noexcept {
        constexpr auto wordbits = sizeof(uintptr_t) * 8;
        while (index < data_.size) {
          auto const wordidx = index / wordbits;
          auto const wordoff = index % wordbits;
          if (auto const word = data_.data[wordidx] >> wordoff) {
            return index + findFirstSet(word) - 1;
          }
          index = (wordidx + 1) * wordbits;
        }
        return index;
      }

     public:
      const_iterator(
          bit_span<uintptr_t const> const data, size_t const index) noexcept
          : data_{data}, index_{ceil_valid_index(index)} {}

      reference operator*() const noexcept { return reference{index_}; }
      const_iterator& operator++() noexcept {
        index_ = ceil_valid_index(index_ + 1);
        return *this;
      }

      friend bool operator==(self const& a, self const& b) noexcept {
        return a.index_ == b.index_;
      }
    };

    const_iterator begin() const noexcept { return const_iterator{bitset_, 0}; }
    const_iterator end() const noexcept {
      return const_iterator{bitset_, bitset_.size};
    }

    bool empty() const noexcept { return begin() == end(); }
  };

  index_set_view as_index_set_view() const noexcept {
    return index_set_view{*this};
  }

 private:
  bool has_capacity_(size_t const index) const noexcept {
    constexpr auto wordbits = sizeof(uintptr_t) * 8;
    auto const buf = get_bit_span_();
    return index < buf.size && !(buf.size == wordbits && index == wordbits - 1);
  }

  bit_span<uintptr_t> reserve_(size_t const index) {
    assert(!has_capacity_(index));
    constexpr auto wordbits = sizeof(uintptr_t) * 8;
    constexpr auto minsize = wordbits * 2; // min growth from in-situ to on-heap
    auto const newsize = std::max(strictNextPowTwo(index), minsize);
    assert(newsize >= minsize);
    assert(newsize % wordbits == 0);
    auto const newdata = new uintptr_t[newsize / wordbits];
    auto const buf = get_bit_span_();
    auto const buf2size = nextPowTwo(buf.size);
    std::memcpy(newdata, buf.data, buf2size / 8);
    std::memset(newdata + buf2size / wordbits, 0, (newsize - buf2size) / 8);
    if (!(to_signed(data_) < 0)) {
      auto const data = new bit_span<uintptr_t>{newdata, newsize};
      assert(!(reinterpret_cast<uintptr_t>(data) & 1));
      data_ = (reinterpret_cast<uintptr_t>(data) >> 1) | ~(~uintptr_t(0) >> 1);
      return *data;
    } else {
      auto const data = reinterpret_cast<bit_span<uintptr_t>*>(data_ << 1);
      delete[] data->data;
      *data = {newdata, newsize};
      return *data;
    }
  }

  void reset_() {
    if (!(to_signed(data_) < 0)) {
      data_ = 0;
    } else {
      auto const data = reinterpret_cast<bit_span<uintptr_t>*>(data_ << 1);
      delete[] data->data;
      delete data;
      data_ = 0;
    }
  }

  template <typename Word>
  static bool get_value_(
      bit_span<Word> const buf, size_t const index) noexcept {
    assert(index < buf.size);
    constexpr auto wordbits = sizeof(Word) * 8;
    auto const wordidx = index / wordbits;
    auto const wordoff = index % wordbits;
    auto const mask = Word(1) << wordoff;
    auto& word = buf.data[wordidx];
    return word & mask;
  }

  template <typename Word>
  static void set_value_(
      bit_span<Word> const buf, size_t const index, bool const value) noexcept {
    assert(index < buf.size);
    constexpr auto wordbits = sizeof(Word) * 8;
    assert(buf.size != wordbits || index != wordbits - 1);
    auto const wordidx = index / wordbits;
    auto const wordoff = index % wordbits;
    auto const mask = Word(1) << wordoff;
    auto& word = buf.data[wordidx];
    word = value ? word | mask : word & ~mask;
  }

  bit_span<uintptr_t const> get_bit_span_() const noexcept {
    if (!(to_signed(data_) < 0)) {
      return {&data_, sizeof(data_) * 8};
    } else {
      return *reinterpret_cast<bit_span<uintptr_t const> const*>(data_ << 1);
    }
  }

  bit_span<uintptr_t> get_bit_span_() noexcept {
    if (!(to_signed(data_) < 0)) {
      return {&data_, sizeof(data_) * 8};
    } else {
      return *reinterpret_cast<bit_span<uintptr_t> const*>(data_ << 1);
    }
  }

  uintptr_t data_{};
};

/// RegexMatchCacheIndexedVector
///
/// An indexed vector, which is a vector for which the index of any element can
/// be found efficiently.
///
/// Intended for use only within RegexMatchCache.
///
/// Incomplete as a generic container.
template <typename Value>
class RegexMatchCacheIndexedVector {
 public:
  size_t size() const noexcept { return forward_.size(); }

  bool contains_index(size_t index) const noexcept {
    return reverse_.contains(index);
  }

  bool contains_value(Value const& value) const noexcept {
    return forward_.contains(value);
  }

  std::pair<size_t, bool> insert_value(Value const& value) {
    auto [iter, inserted] = forward_.try_emplace(value);
    if (inserted) {
      auto rollback_forward = makeGuard([&, iter_ = iter] {
        forward_.erase(iter_);
      });
      if (free_.capacity() < forward_.size()) {
        grow_capacity_by(free_, forward_.size() - free_.size());
      }
      assert(!(free_.capacity() < forward_.size()));
      auto const from_free = !free_.empty();
      auto const index = from_free ? free_.back() : forward_.size() - 1;
      from_free ? free_.pop_back() : void();
      iter->second = index;
      auto rollback_free = makeGuard([&] {
        from_free ? free_.push_back(index) : void();
      });
      assert(!reverse_.contains(index));
      reverse_[index] = value;
      rollback_free.dismiss();
      rollback_forward.dismiss();
    }
    return {iter->second, inserted};
  }

  bool erase_value(Value const& value) noexcept {
    auto iter = forward_.find(value);
    if (iter == forward_.end()) {
      return false;
    }
    assert(free_.size() < free_.capacity());
    auto index = iter->second;
    free_.push_back(index);
    forward_.erase(iter);
    reverse_.erase(index);
    return true;
  }

  void clear() noexcept {
    reverse_.clear();
    forward_.clear();
    free_.clear();
  }

  Value const& value_at_index(size_t index) const { return reverse_.at(index); }

  size_t index_of_value(Value const& value) const { return forward_.at(value); }

  class forward_view {
   private:
    friend RegexMatchCacheIndexedVector;
    using map_t = folly::F14FastMap<Value, size_t>;
    map_t const& map;
    explicit forward_view(map_t const& map_) noexcept : map{map_} {}

   public:
    using value_type = typename map_t::value_type;
    using size_type = typename map_t::size_type;
    using iterator = typename map_t::const_iterator;

    size_t size() const noexcept { return map.size(); }
    iterator begin() const noexcept { return map.begin(); }
    iterator end() const noexcept { return map.end(); }
  };

  forward_view as_forward_view() const noexcept {
    return forward_view{forward_};
  }

 private:
  std::vector<size_t> free_;
  folly::F14FastMap<Value, size_t> forward_;
  folly::F14FastMap<size_t, Value> reverse_;
};

using RegexMatchCacheKeyBase = unique_hash_key_strong_sha256<32>;

/// RegexMatchCacheKey
///
/// A key derived from a string. Used with RegexMatchCache.
///
/// Intended for use only with RegexMatchCache.
///
/// Incomplete as a generic facility.
class RegexMatchCacheKey : public RegexMatchCacheKeyBase {
 public:
  explicit RegexMatchCacheKey(std::string_view regex) noexcept
      : RegexMatchCacheKeyBase{std::tuple(regex)} {}
};

} // namespace folly

namespace std {

template <>
struct hash<::folly::RegexMatchCacheKey>
    : hash<::folly::RegexMatchCacheKeyBase> {};

} // namespace std

namespace folly {

/// RegexMatchCacheKeyAndView
///
/// A composite key and view derived from a string. Used with RegexMatchCache.
///
/// Intended for use only with RegexMatchCache.
///
/// Incomplete as a generic facility.
class RegexMatchCacheKeyAndView {
 public:
  using regex_key = RegexMatchCacheKey;

  regex_key const key;
  std::string_view const view;

  explicit RegexMatchCacheKeyAndView(std::string_view regex) noexcept
      : key{regex}, view{regex} {}

  /* implicit */ operator RegexMatchCacheKey const&() const noexcept {
    return key;
  }
  /* implicit */ operator std::string_view const&() const noexcept {
    return view;
  }

 private:
  RegexMatchCacheKeyAndView(
      regex_key const& k, std::string_view const v) noexcept
      : key{k}, view{v} {}
};

/// RegexMatchCache
///
/// A cache around boost::regex_match(string, regex).
///
/// For efficiency, assumes several constraints and makes several guarantees.
///
/// The data structure owns regexes but does not own strings. The lifetimes of
/// all strings in the cache must surround their additions to the cache and
/// their subsequent removals from the cache or destruction of the cache.
///
/// The data structure is in two parts:
/// * A bidirectional match-cache contains all known matches.
/// * a bidirectional string-queue contains unknown, hypothetical matches.
///
/// Cached lookup operates only over the match-cache. When the string-queue for
/// a given regex is not empty, that regex is said to be uncoalesced. Cached
/// lookups are not permitted for an uncoalesced regex; that regex must first be
/// coalesced.
///
/// Addition of a string adds the string to the string-queue corresponding to
/// all known regexes. It does not perform any regex-match operations.
///
/// Addition and coalesce of a regex performs regex-matches for that regex only.
/// The string-queue for the given regex is removed and all elements matched
/// against the regex, and matching strings are added to the match-cache.
///
/// Lookup must follow a pattern like this:
///
///    if (!cache.isReadyToFindMatches(regex)) { // const
///      cache.prepareToFindMatches(regex); // non-const
///    }
///    auto matches = cache.findMatches(regex); // const
///
/// This is to support concurrent lookups, where the cache is protected by a
/// shared mutex.
///
/// A large string-queue may instead be coalesced without holding the exclusive
/// lock across the scan, at the cost of snapshotting the queue:
///
///    auto matcher = RegexMatchCache::compile(regex); // static
///    auto handle = // non-const
///        cache.initPrepareToFindMatches(std::move(matcher), now);
///    auto result = std::move(handle).evaluate(); // no lock held
///    cache.finiPrepareToFindMatches(std::move(result)); // non-const
///    if (!cache.isReadyToFindMatches(regex)) { // const
///      cache.prepareToFindMatches(regex); // non-const
///    }
///    auto matches = cache.findMatches(regex); // const
///
/// The strings snapshotted into the handle must stay alive and unchanged until
/// the result has been published.
///
/// The data structure is exception-safe in a sense. If an exception is thrown
/// within any non-const member function and escapes, the data structure may
/// purge all cached regexes while leaving all strings. In most such member
/// functions, only a memory-allocation failure would cause an exception to be
/// thrown. But in prepareToFindMatches, the provided regex may be syntactically
/// invalid and parsing it may throw, or it may be pathological and evaluating
/// it over a string may throw. In any event, the resolution is to clear out all
/// added regexes and to leave only the added strings. The reason is that this
/// resolution is simple and likely to be correct, while any other mechanism
/// would be complex and would be likely to have bugs.
class RegexMatchCache {
 public:
  using clock = folly::chrono::coarse_steady_clock;
  using time_point = clock::time_point;

  using regex_key = RegexMatchCacheKey;
  using regex_key_and_view = RegexMatchCacheKeyAndView;

 private:
  using regex_pointer = regex_key const*;
  using string_pointer = std::string const*;

  class RegexObject;

  struct RegexToMatchEntry : MoveOnly {
    mutable std::atomic<time_point> accessed_at{};

    folly::F14VectorSet<string_pointer> matches;
  };

  struct MatchToRegexEntry : MoveOnly {
    RegexMatchCacheDynamicBitset regexes;
  };

  struct StringQueueForwardEntry : MoveOnly {
    RegexMatchCacheDynamicBitset regexes;
  };

  struct StringQueueReverseEntry : MoveOnly {
    folly::F14VectorSet<string_pointer> strings;
  };

  RegexMatchCacheIndexedVector<regex_pointer> regexVector_;

  /// cacheRegexToMatch_
  ///
  /// A match-cache map from regexes to the sets of matching strings.
  ///
  /// The set of matching strings for a given regex may be incomplete. This
  /// happens when strings are added to the universe but have not yet been
  /// coalesced for the given regex. The set of uncoalesced strings for a
  /// given regex is in stringQueueReverse_.
  ///
  /// For each regex, includes a last-accessed-at timestamp. This timestamp
  /// is used when purging old regexes from the cache, for the caller's own
  /// definition of old.
  folly::F14NodeMap<regex_key, RegexToMatchEntry> cacheRegexToMatch_;

  /// cacheMatchToRegex_
  ///
  /// A match-cache map from strings to the sets of matching regexes.
  ///
  /// The set of matching regexes for a given string may be incomplete. This
  /// happens when strings are added to the universe but have not yet been
  /// coalesced for all regexes in the universe. The set of regexes for which
  /// a given string has not yet been coalesced is in stringQueueForward_.
  folly::F14FastMap<string_pointer, MatchToRegexEntry> cacheMatchToRegex_;

  /// stringQueueForward_
  ///
  /// A pending-coalesce map from strings to regexes for which the strings have
  /// not yet been coalesced, that is, for which it is not yet known that the
  /// strings do or do not match the given regexes.
  ///
  /// In a steady-state when all strings have been coalesced for all regexes,
  /// this map would be empty.
  folly::F14FastMap<string_pointer, StringQueueForwardEntry>
      stringQueueForward_;

  /// stringQueueReverse_
  ///
  /// A pending-coalesce map from regexes to strings which have not yet been
  /// coalesced for the given regex, that is, for which it is not yet known that
  /// the strings do or do not match the given regexes.
  ///
  /// In a steady-state when all strings have been coalesced for all regexes,
  /// this map would be empty.
  folly::F14FastMap<regex_pointer, StringQueueReverseEntry> stringQueueReverse_;

  void repair() noexcept;

  std::vector<string_pointer> getStringQueueFor(regex_key const& regex) const;
  void markAccessed(regex_key const& regex, time_point now);

 public:
  class KeyMap {
   public:
    using regex_key = RegexMatchCacheKey;
    using regex_key_and_view = RegexMatchCacheKeyAndView;

    virtual ~KeyMap() = 0;

    virtual std::string_view lookup(regex_key const& regex) const = 0;
  };

  class InspectView {
    friend RegexMatchCache;

   private:
    RegexMatchCache const& ref_;
    KeyMap const& keys_;

    explicit InspectView(
        RegexMatchCache const& ref, KeyMap const& keys) noexcept
        : ref_{ref}, keys_{keys} {}

    void print(std::ostream& o) const;

   public:
    friend std::ostream& operator<<(std::ostream& o, InspectView const view) {
      return (view.print(o), o);
    }
  };

  class ConsistencyReportMatcher {
   private:
    struct state;
    std::unique_ptr<state> state_;

   public:
    using regex_key = RegexMatchCache::regex_key;
    using regex_key_and_view = RegexMatchCache::regex_key_and_view;
    using string_pointer = RegexMatchCache::string_pointer;

    ConsistencyReportMatcher();
    virtual ~ConsistencyReportMatcher();

    virtual bool match(
        KeyMap const& keys, regex_key regex, string_pointer string);
  };

  class FindMatchesUnsafeResult {
   private:
    friend class RegexMatchCache;

    using map_t = folly::F14VectorSet<string_pointer>;

    map_t const& matches_;

    /* implicit */ FindMatchesUnsafeResult(map_t const& matches) noexcept
        : matches_{matches} {}

   public:
    using value_type = map_t::value_type;

    auto size() const noexcept { return matches_.size(); }
    auto begin() const noexcept { return matches_.begin(); }
    auto end() const noexcept { return matches_.end(); }
  };

  /// A compiled regex produced by compile(). Copies share immutable compiled
  /// state. initPrepareToFindMatches() builds the pattern it was compiled from.
  ///
  /// Assignment is deleted here and on PrepareHandle and PrepareResult:
  /// each holds a regex_key, whose underlying data member is const.
  class Matcher {
   private:
    friend class RegexMatchCache;

    regex_key key_;
    std::shared_ptr<RegexObject const> impl_;

    Matcher(regex_key key, std::shared_ptr<RegexObject const> impl) noexcept;

   public:
    Matcher(Matcher const&) = default;
    Matcher& operator=(Matcher const&) = delete;
    Matcher(Matcher&&) noexcept;
    Matcher& operator=(Matcher&&) = delete;
    ~Matcher();

    bool operator()(std::string_view string) const;
  };

  class PrepareResult;

  /// PrepareHandle
  ///
  /// Holds the compiled matcher and a snapshot of a regex's string-queue for an
  /// off-lock build; see initPrepareToFindMatches. The snapshotted string
  /// pointers are non-owning: the caller must keep the backing strings alive
  /// and unchanged until the PrepareResult from evaluate() has been consumed
  /// by finiPrepareToFindMatches. Publication resolves strings by address and
  /// never re-reads their contents, so mutating one in that window -- erase,
  /// mutate in place, re-add at the same address -- publishes the match
  /// computed for the old contents.
  class PrepareHandle {
   private:
    friend class RegexMatchCache;

    regex_key key_;
    Matcher matcher_;
    std::vector<string_pointer> queue_;

    PrepareHandle(
        regex_key key,
        Matcher matcher,
        std::vector<string_pointer> queue) noexcept;

   public:
    PrepareHandle(PrepareHandle&&) noexcept;
    PrepareHandle& operator=(PrepareHandle&&) = delete;
    ~PrepareHandle();

    /// evaluate() consumes the handle, clearing the queue: a call afterward is
    /// defined and returns zero.
    size_t queueSize() const noexcept { return queue_.size(); }

    /// Runs the match scan with no lock held, dereferencing the snapshotted
    /// string pointers, which must still be alive. Consumes the handle.
    PrepareResult evaluate() &&;
  };

  class PrepareResult {
   private:
    friend class RegexMatchCache;

    regex_key key_;
    std::vector<string_pointer> evaluated_;
    std::vector<string_pointer> matched_;

    PrepareResult(
        regex_key key,
        std::vector<string_pointer> evaluated,
        std::vector<string_pointer> matched) noexcept;

   public:
    PrepareResult(PrepareResult&&) noexcept;
    PrepareResult& operator=(PrepareResult&&) = delete;
    ~PrepareResult();
  };

  RegexMatchCache() noexcept;
  ~RegexMatchCache();

  std::vector<std::string_view> getRegexList(KeyMap const& keys) const;
  std::vector<string_pointer> getStringList() const;
  InspectView inspect(KeyMap const& keys) const noexcept {
    return InspectView{*this, keys};
  }
  void consistency(
      ConsistencyReportMatcher& crcache,
      KeyMap const& keys,
      FunctionRef<void(std::string)> report) const;

  bool hasRegex(regex_key const& regex) const noexcept;
  void addRegex(regex_key const& regex);
  void eraseRegex(regex_key const& regex);

  bool hasString(string_pointer string) const noexcept;
  void addString(string_pointer string);
  void eraseString(string_pointer string);

  std::vector<string_pointer> findMatchesUncached(std::string_view regex) const;

  bool isReadyToFindMatches(regex_key const& regex) const noexcept;
  void prepareToFindMatches(regex_key_and_view const& regex);
  FindMatchesUnsafeResult findMatchesUnsafe(
      regex_key const& regex, time_point now) const;
  std::vector<string_pointer> findMatches(
      regex_key const& regex, time_point now) const;

  static Matcher compile(std::string_view regex);

  size_t numQueuedStringsFor(regex_key const& regex) const;

  /// Begins an off-lock build of the pattern matcher was compiled from. Under
  /// the caller's lock, registers the regex, stamps it accessed-at now, and
  /// snapshots its string-queue into the returned handle, which the caller then
  /// evaluates with no lock held.
  PrepareHandle initPrepareToFindMatches(Matcher matcher, time_point now);

  /// Ends an off-lock build, publishing an evaluated result under the caller's
  /// lock. Publishes only the snapshotted strings, and must be called on the
  /// cache that produced the handle -- a caller precondition, unchecked. It is
  /// a no-op if the regex is absent when publication begins; an erase followed
  /// by a re-add mid-build publishes against the live registration.
  ///
  /// Either way the regex may still be unready afterwards, so the caller must
  /// re-check isReadyToFindMatches under the lock and drain the residual via
  /// prepareToFindMatches. Skipping that check fails silently:
  /// findMatchesUnsafe throws on an unready regex only under kIsDebug, and
  /// otherwise returns an incomplete match set.
  void finiPrepareToFindMatches(PrepareResult result);

  bool hasItemsToPurge(time_point expiry) const noexcept;

  void clear();
  void purge(time_point expiry);
};

} // namespace folly
