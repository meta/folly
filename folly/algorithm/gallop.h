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

#include <cassert>
#include <cstddef>
#include <functional>
#include <iterator>
#include <type_traits>

#include <folly/Utility.h>

/// Galloping (a.k.a. exponential, or doubling) search over a partitioned range.
///
/// These are drop-in replacements for std::partition_point, std::lower_bound,
/// and std::upper_bound, returning exactly the same position for any range they
/// accept. They differ only in how they get there: instead of bisecting the
/// whole range, they probe outward from one end in doubling strides until the
/// answer is bracketed, then bisect only that window.
///
/// Use these when the answer is expected to be *near* one end of the range --
/// the classic case being a cursor advancing through a sorted posting list,
/// where each step lands a short distance ahead of the last. Cost becomes
/// O(log d) in the distance d to the answer, rather than O(log n) in the range
/// length, and the probes touch fewer and more sequential cache lines.
///
/// Whether that pays depends entirely on d relative to the range length n.
/// Galloping performs roughly 2*log2(d) comparisons where a bisection performs
/// log2(n), so it loses once d grows large. Measured through gallop_lower_bound
/// against std::lower_bound, n from 1k to 1M (gallop_bench.cpp):
///
///   d = 0 (already at the answer)   19x - 42x faster
///   d ~ 1                           10x - 32x faster
///   d ~ 8                          3.7x - 12x faster
///   d ~ n/64                       1.2x - 1.6x faster
///   d ~ n/16 to n/8                0.6x - 1.1x, around break-even
///   d ~ n/4 and beyond             0.5x - 0.9x, slower
///
/// The break-even band is erratic rather than a clean crossover -- d ~ n/16 can
/// measure worse than d ~ n/8 -- because what remains to bisect depends on
/// where the answer happens to fall between two probes, not just on d.
///
/// So: use these when the answer is usually within a few percent of the cursor,
/// which is the case for a posting-list intersection and is not the case for an
/// arbitrary lookup. No tuning recovers the large-d case; it is inherent to the
/// probe sequence, not an artifact of the defaults.
///
/// Two further caveats:
///
///  - The comparator is invoked more often than std::lower_bound would invoke
///    it, so it must be cheap and free of side effects.
///  - Galloping is not universally a win even for short strides: an unrolled
///    branchless bisection can beat it on ranges that fit in cache, where a
///    predictable straight-line scan of the whole window costs less than the
///    dependent branches a gallop needs. Measure before assuming.

namespace folly {

/// gallop_options
///
/// Tuning for every entry point below.
///
/// These are ordinary runtime values rather than template parameters. Passing a
/// literal inlines and folds to the same code a non-type template parameter
/// would produce, so making them compile-time buys nothing measurable, while
/// costing a separate instantiation of the whole algorithm per distinct
/// threshold. Prefer a constant even so: a tuning the optimizer cannot see
/// through costs up to about 25% in the worst regime.
///
/// Two knobs that look useful are deliberately absent:
///
///  - A cap that abandons the gallop for a plain bisection once the stride
///    passes some fraction of the range, to bound the long-stride worst case.
///    It cannot: by the time such a cap fires the probes are already paid for
///    and the remaining range is still nearly the original, so it loses to both
///    pure strategies while also giving up most of the medium-stride win.
///  - A short sequential scan before the doubling starts, on the theory that
///    sequential reads are prefetcher-friendly where the probes are scattered.
///    The gallop's early probes land at offsets 1, 4, 9, 18, within the cache
///    line or two such a scan would read, so there is no locality left to win
///    at the distances where it would matter, and the probes only scatter at
///    distances where a leading scan is useless. It would also be speculative,
///    wasted whenever the answer lies past it, where the linear tail scans a
///    window already known to contain the answer.
struct gallop_options {
  /// Bisect until the window is this small, then scan it linearly. 0 disables
  /// the linear tail, giving a pure bisection.
  size_t linear_scan_threshold = 32;

  /// Distance of the first probe. Clamped to at least 1. Raising it to match
  /// linear_scan_threshold trades some of the d <= 8 win for a better showing
  /// in the d ~ n/1000 band; that is worth it for a caller whose strides
  /// cluster there rather than at the very short end.
  size_t initial_step = 1;
};

namespace detail {

/// The first probe's distance, clamped so a zero initial_step cannot stall the
/// doubling.
template <class Diff>
constexpr Diff gallop_first_step(gallop_options opts) {
  return static_cast<Diff>(opts.initial_step == 0 ? 1 : opts.initial_step);
}

template <class Pos, class PredAt>
constexpr Pos gallop_scan_linear(Pos first, Pos last, PredAt& predAt) {
  for (; first != last; ++first) {
    if (predAt(first)) {
      break;
    }
  }
  return first;
}

template <class Pos, class PredAt>
constexpr Pos gallop_scan_binary(
    Pos first, Pos last, PredAt& predAt, gallop_options opts) {
  using diff_t = decltype(last - first);
  while (last - first > static_cast<diff_t>(opts.linear_scan_threshold)) {
    const Pos mid = first + (last - first) / 2;
    if (predAt(mid)) {
      last = mid;
    } else {
      first = mid + 1;
    }
  }
  return gallop_scan_linear(first, last, predAt);
}

/// Returns the first position in [first, last) satisfying the predicate, or
/// last if none does, probing forward from first.
template <class Pos, class PredAt>
constexpr Pos gallop_scan_forward(
    Pos first, Pos last, PredAt& predAt, gallop_options opts) {
  using diff_t = decltype(last - first);
  const diff_t firstStep = gallop_first_step<diff_t>(opts);

  for (diff_t step = firstStep; step < last - first; step *= 2) {
    const Pos probe = first + step;
    if (predAt(probe)) {
      last = probe;
      break;
    }
    first = probe + 1;
  }
  return gallop_scan_binary(first, last, predAt, opts);
}

/// Returns the first position in [first, bound] satisfying the predicate,
/// probing backward from bound.
///
/// Precondition: the predicate holds at bound. bound is never dereferenced, so
/// it may be the range's past-the-end position, whose predicate is vacuously
/// true under the partitioning requirement.
template <class Pos, class PredAt>
constexpr Pos gallop_scan_backward(
    Pos first, Pos bound, PredAt& predAt, gallop_options opts) {
  using diff_t = decltype(bound - first);
  const diff_t firstStep = gallop_first_step<diff_t>(opts);

  for (diff_t step = firstStep; step < bound - first; step *= 2) {
    const Pos probe = bound - step;
    if (!predAt(probe)) {
      first = probe + 1;
      break;
    }
    bound = probe;
  }
  // Bisecting [first, bound) yields bound when nothing there satisfies the
  // predicate, which is the right answer since the predicate holds at bound.
  return gallop_scan_binary(first, bound, predAt, opts);
}

/// Returns the first position in [first, last) satisfying the predicate,
/// probing outward from hint in whichever direction the answer lies.
template <class Pos, class PredAt>
constexpr Pos gallop_scan_hinted(
    Pos first, Pos hint, Pos last, PredAt& predAt, gallop_options opts) {
  if (hint != last && !predAt(hint)) {
    return gallop_scan_forward(hint + 1, last, predAt, opts);
  }
  return gallop_scan_backward(first, hint, predAt, opts);
}

template <class It>
inline constexpr bool gallop_is_random_access_v = std::is_base_of_v<
    std::random_access_iterator_tag,
    typename std::iterator_traits<It>::iterator_category>;

// The predicate adapters below are named types rather than lambdas so that
// they are legible in a stack trace, which matters because they sit in the
// innermost frame of every search.

/// Adapts a caller's element predicate to the position predicate the scan
/// functions apply.
template <class Pred>
struct gallop_deref_pred {
  Pred& pred;

  template <class Iter>
  constexpr bool operator()(Iter it) const {
    return pred(*it);
  }
};

/// std::lower_bound's predicate: the first element not ordered before value.
template <class T, class Comp, class Proj>
struct gallop_lower_bound_pred {
  const T& value;
  Comp& comp;
  Proj& proj;

  template <class Elem>
  constexpr bool operator()(const Elem& elem) const {
    return !comp(proj(elem), value);
  }
};

/// std::upper_bound's predicate: the first element that value is ordered
/// before.
template <class T, class Comp, class Proj>
struct gallop_upper_bound_pred {
  const T& value;
  Comp& comp;
  Proj& proj;

  template <class Elem>
  constexpr bool operator()(const Elem& elem) const {
    return comp(value, proj(elem));
  }
};

} // namespace detail

/// gallop_partition_point
///
/// Like std::partition_point, but probes forward from first in doubling
/// strides. Returns the first element in [first, last) satisfying pred, or
/// last if none does.
///
/// Requires that the range be partitioned with respect to pred: if pred holds
/// for an element, it holds for every element after it.
///
/// The default tuning is for short strides, which is what this algorithm is
/// for. See gallop_options.
template <class RandomIt, class Pred>
[[nodiscard]] constexpr RandomIt gallop_partition_point(
    RandomIt first, RandomIt last, Pred pred, gallop_options opts = {}) {
  static_assert(
      detail::gallop_is_random_access_v<RandomIt>,
      "folly galloping search requires random access iterators");
  auto predAt = detail::gallop_deref_pred{pred};
  return detail::gallop_scan_forward(first, last, predAt, opts);
}

/// gallop_partition_point, hinted
///
/// As above, but probes outward from hint, which may be anywhere in the range
/// [first, last] and need not bound the answer from either side. Use this when
/// the caller holds a cursor that may overshoot; prefer the unhinted overload,
/// which carries no direction branch, when the cursor is known to be a lower
/// bound on the answer.
///
/// Passing `hint == last` gallops backward from the end -- probing last-1,
/// last-3, last-7, and so on -- which is how to search a range whose answer is
/// expected near its back. There is deliberately no separate reverse-direction
/// entry point; this is it.
///
/// This is a better alternative to using reverse iterators. Reversing a range
/// partitioned with respect to pred yields one partitioned with respect to the
/// negation of pred, so the search finds the mirror position: lower_bound and
/// upper_bound swap roles, the comparator has to be reversed too, and the
/// result needs a .base() adjustment to become the forward answer. Three
/// chances to get it wrong, for something this overload already does.
///
/// `first <= hint <= last` is a precondition, asserted and therefore checked
/// only in debug builds. Optimized builds deliberately do not clamp hint into
/// range.
///
/// Clamping was implemented and measured before being rejected; the numbers
/// are recorded here so that it is not retried. The clamp is two
/// subtract-compare-selects -- phrased as differences, since these iterators
/// are only required to subtract and need not be relationally comparable --
/// sitting on the entry path of exactly the short-distance search this
/// algorithm exists for. On aarch64, with the hint laundered so the compiler
/// could not prove it in range, it cost 0.3-0.8ns against a 1.4-3.1ns search:
/// 20-27% for a short stride, paid on every correct call in order to repair an
/// incorrect one.
///
/// Letting the caller buy that cost back does not rescue it. Promising
/// `if (hint - first < 0 || last - hint < 0) std::unreachable();` does erase
/// the clamp entirely, but the spelling anyone would actually reach for,
/// `hint < first || hint > last`, changes not one instruction: the compiler
/// does not bridge a pointer relational to the pointer difference the clamp
/// branches on. A guard that silently does nothing unless written one exact
/// way is worse than no guard.
template <class RandomIt, class Pred>
[[nodiscard]] constexpr RandomIt gallop_partition_point(
    RandomIt first,
    RandomIt hint,
    RandomIt last,
    Pred pred,
    gallop_options opts = {}) {
  static_assert(
      detail::gallop_is_random_access_v<RandomIt>,
      "folly galloping search requires random access iterators");
  assert(hint - first >= 0 && last - hint >= 0);
  auto predAt = detail::gallop_deref_pred{pred};
  return detail::gallop_scan_hinted(first, hint, last, predAt, opts);
}

/// gallop_partition_point_index
///
/// Index-domain counterpart, for sequences that are not iterable: proxy arrays
/// whose element access decodes or byte-swaps, and containers reached through
/// an accessor member function. pred takes an index rather than an element.
///
/// Returns the first index in [lo, hi) satisfying pred, or hi if none does.
template <class Index, class Pred>
[[nodiscard]] constexpr Index gallop_partition_point_index(
    Index lo, Index hi, Pred pred, gallop_options opts = {}) {
  static_assert(
      std::is_integral_v<Index>,
      "folly galloping index search requires an integral index");
  return detail::gallop_scan_forward(lo, hi, pred, opts);
}

/// gallop_partition_point_index, hinted
///
/// hint may be anywhere in [lo, hi] and need not bound the answer. As with the
/// iterator form, `hint == hi` gallops backward from the end, and `lo <= hint
/// <= hi` is an asserted precondition rather than a clamp -- see the iterator
/// overload above for the measurements behind that.
template <class Index, class Pred>
[[nodiscard]] constexpr Index gallop_partition_point_index(
    Index lo, Index hint, Index hi, Pred pred, gallop_options opts = {}) {
  static_assert(
      std::is_integral_v<Index>,
      "folly galloping index search requires an integral index");
  assert(lo <= hint && hint <= hi);
  return detail::gallop_scan_hinted(lo, hint, hi, pred, opts);
}

/// gallop_lower_bound
///
/// Like std::lower_bound, but probes forward from first in doubling strides.
/// comp compares a projected element against value; the default is
/// transparent, so value's type need not match the element type.
template <
    class RandomIt,
    class T,
    class Comp = std::less<>,
    class Proj = std::identity>
[[nodiscard]] constexpr RandomIt gallop_lower_bound(
    RandomIt first,
    RandomIt last,
    const T& value,
    Comp comp = {},
    Proj proj = {},
    gallop_options opts = {}) {
  auto const pred = detail::gallop_lower_bound_pred{value, comp, proj};
  return gallop_partition_point(first, last, pred, opts);
}

/// gallop_lower_bound, hinted
template <
    class RandomIt,
    class T,
    class Comp = std::less<>,
    class Proj = std::identity>
[[nodiscard]] constexpr RandomIt gallop_lower_bound(
    RandomIt first,
    RandomIt hint,
    RandomIt last,
    const T& value,
    Comp comp = {},
    Proj proj = {},
    gallop_options opts = {}) {
  auto const pred = detail::gallop_lower_bound_pred{value, comp, proj};
  return gallop_partition_point(first, hint, last, pred, opts);
}

/// gallop_upper_bound
///
/// Like std::upper_bound, but probes forward from first in doubling strides.
template <
    class RandomIt,
    class T,
    class Comp = std::less<>,
    class Proj = std::identity>
[[nodiscard]] constexpr RandomIt gallop_upper_bound(
    RandomIt first,
    RandomIt last,
    const T& value,
    Comp comp = {},
    Proj proj = {},
    gallop_options opts = {}) {
  auto const pred = detail::gallop_upper_bound_pred{value, comp, proj};
  return gallop_partition_point(first, last, pred, opts);
}

/// gallop_upper_bound, hinted
template <
    class RandomIt,
    class T,
    class Comp = std::less<>,
    class Proj = std::identity>
[[nodiscard]] constexpr RandomIt gallop_upper_bound(
    RandomIt first,
    RandomIt hint,
    RandomIt last,
    const T& value,
    Comp comp = {},
    Proj proj = {},
    gallop_options opts = {}) {
  auto const pred = detail::gallop_upper_bound_pred{value, comp, proj};
  return gallop_partition_point(first, hint, last, pred, opts);
}

} // namespace folly
