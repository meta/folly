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

#include <cstdlib>

#include <folly/Likely.h>
#include <folly/Portability.h>
#include <folly/Range.h>
#include <folly/logging/LogStream.h>
#include <folly/logging/Logger.h>
#include <folly/logging/LoggerDB.h>
#include <folly/logging/ObjectToString.h>
#include <folly/logging/RateLimiter.h>

/*
 * This file contains the XLOG() and XLOGF() macros.
 *
 * These macros make it easy to use the logging library without having to
 * manually pick log category names.  All XLOG() and XLOGF() statements in a
 * given file automatically use a LogCategory based on the current file name.
 *
 * For instance, in src/foo/bar.cpp, the default log category name will be
 * "src.foo.bar"
 *
 * If desired, the log category name used by XLOG() in a .cpp file may be
 * overridden using XLOG_SET_CATEGORY_NAME() macro.
 */

/**
 * Log a message to this file's default log category.
 *
 * By default the log category name is automatically picked based on the
 * current filename.  In src/foo/bar.cpp the log category name "src.foo.bar"
 * will be used.  In "lib/stuff/foo.h" the log category name will be
 * "lib.stuff.foo"
 *
 * Note that the filename is based on the __FILE__ macro defined by the
 * compiler.  This is typically dependent on the filename argument that you
 * give to the compiler.  For example, if you compile src/foo/bar.cpp by
 * invoking the compiler inside src/foo and only give it "bar.cpp" as an
 * argument, the category name will simply be "bar".  In general XLOG() works
 * best if you always invoke the compiler from the root directory of your
 * project repository.
 */

/*
 * The global value of FOLLY_XLOG_MIN_LEVEL. All the messages logged to
 * XLOG(XXX) with severity less than FOLLY_XLOG_MIN_LEVEL will not be displayed.
 * If it can be determined at compile time that the message will not be printed,
 * the statement will be compiled out.
 * FOLLY_XLOG_MIN_LEVEL should be below FATAL.
 *
 *
 * Example: to strip out messages less than ERR, use the value of ERR below.
 */
#ifndef FOLLY_XLOG_MIN_LEVEL
#define FOLLY_XLOG_MIN_LEVEL MIN_LEVEL
#endif

namespace folly {

namespace detail {
extern bool const xlog_support_buck2;
}

constexpr auto kLoggingMinLevel = LogLevel::FOLLY_XLOG_MIN_LEVEL;
static_assert(
    !isLogLevelFatal(kLoggingMinLevel),
    "Cannot set FOLLY_XLOG_MIN_LEVEL to disable fatal messages");
} // namespace folly

#define XLOG(level, ...)                   \
  XLOG_IMPL(                               \
      ::folly::LogLevel::level,            \
      ::folly::LogStreamProcessor::APPEND, \
      ##__VA_ARGS__)

/**
 * Log a message if and only if the specified condition predicate evaluates
 * to true. Note that the condition is *only* evaluated if the log-level check
 * passes.
 */
#define XLOG_IF(level, cond, ...)          \
  XLOG_IF_IMPL(                            \
      ::folly::LogLevel::level,            \
      cond,                                \
      ::folly::LogStreamProcessor::APPEND, \
      ##__VA_ARGS__)
/**
 * Log a message to this file's default log category, using a format string.
 */
#define XLOGF(level, fmt, ...)             \
  XLOG_IMPL(                               \
      ::folly::LogLevel::level,            \
      ::folly::LogStreamProcessor::FORMAT, \
      fmt,                                 \
      ##__VA_ARGS__)

/**
 * Log a message using a format string if and only if the specified condition
 * predicate evaluates to true. Note that the condition is *only* evaluated
 * if the log-level check passes.
 */
#define XLOGF_IF(level, cond, fmt, ...)    \
  XLOG_IF_IMPL(                            \
      ::folly::LogLevel::level,            \
      cond,                                \
      ::folly::LogStreamProcessor::FORMAT, \
      fmt,                                 \
      ##__VA_ARGS__)

/**
 * Similar to XLOG(...) except only log a message every @param ms
 * milliseconds.
 *
 * Note that this is threadsafe.
 */
#define XLOG_EVERY_MS(level, ms, ...)                                 \
  XLOG_IF(                                                            \
      level,                                                          \
      [_folly_detail_xlog_ms = ms] {                                  \
        static ::folly::logging::IntervalRateLimiter                  \
            folly_detail_xlog_limiter(                                \
                1, std::chrono::milliseconds(_folly_detail_xlog_ms)); \
        return folly_detail_xlog_limiter.check();                     \
      }(),                                                            \
      ##__VA_ARGS__)

/**
 * Similar to XLOG(...) except only log a message every @param ms
 * milliseconds and if the specified condition predicate evaluates to true.
 *
 * Note that this is threadsafe.
 */
#define XLOG_EVERY_MS_IF(level, cond, ms, ...)                              \
  XLOG_IF(                                                                  \
      level,                                                                \
      (cond) &&                                                             \
          [_folly_detail_xlog_ms = ms] {                                    \
            static ::folly::logging::IntervalRateLimiter                    \
                folly_detail_xlog_limiter(                                  \
                    1, ::std::chrono::milliseconds(_folly_detail_xlog_ms)); \
            return folly_detail_xlog_limiter.check();                       \
          }(),                                                              \
      ##__VA_ARGS__)

/**
 * Similar to XLOG(...) except log a message if the specified condition
 * predicate evaluates to true or every @param ms milliseconds
 *
 * Note that this is threadsafe.
 */
#define XLOG_EVERY_MS_OR(level, cond, ms, ...)                              \
  XLOG_IF(                                                                  \
      level,                                                                \
      (cond) ||                                                             \
          [_folly_detail_xlog_ms = ms] {                                    \
            static ::folly::logging::IntervalRateLimiter                    \
                folly_detail_xlog_limiter(                                  \
                    1, ::std::chrono::milliseconds(_folly_detail_xlog_ms)); \
            return folly_detail_xlog_limiter.check();                       \
          }(),                                                              \
      ##__VA_ARGS__)

/**
 * Similar to XLOGF(...) except only log a message every @param ms
 * milliseconds and if the specified condition predicate evaluates to true.
 *
 * Note that this is threadsafe.
 */
#define XLOGF_EVERY_MS_IF(level, cond, ms, fmt, ...)                        \
  XLOGF_IF(                                                                 \
      level,                                                                \
      (cond) &&                                                             \
          [_folly_detail_xlog_ms = ms] {                                    \
            static ::folly::logging::IntervalRateLimiter                    \
                folly_detail_xlog_limiter(                                  \
                    1, ::std::chrono::milliseconds(_folly_detail_xlog_ms)); \
            return folly_detail_xlog_limiter.check();                       \
          }(),                                                              \
      fmt,                                                                  \
      ##__VA_ARGS__)

/**
 * Similar to XLOGF(...) except only log a message every @param ms
 * milliseconds.
 *
 * Note that this is threadsafe.
 */
#define XLOGF_EVERY_MS(level, ms, fmt, ...) \
  XLOGF_EVERY_MS_IF(level, true, ms, fmt, ##__VA_ARGS__)

/**
 * Similar to XLOGF(...) except log a message if the specified condition
 * predicate evaluates to true or every @param ms milliseconds
 *
 * Note that this is threadsafe.
 */
#define XLOGF_EVERY_MS_OR(level, cond, ms, fmt, ...)                        \
  XLOGF_IF(                                                                 \
      level,                                                                \
      (cond) ||                                                             \
          [_folly_detail_xlog_ms = ms] {                                    \
            static ::folly::logging::IntervalRateLimiter                    \
                folly_detail_xlog_limiter(                                  \
                    1, ::std::chrono::milliseconds(_folly_detail_xlog_ms)); \
            return folly_detail_xlog_limiter.check();                       \
          }(),                                                              \
      fmt,                                                                  \
      ##__VA_ARGS__)

namespace folly {
namespace detail {

template <typename Tag>
FOLLY_EXPORT FOLLY_ALWAYS_INLINE bool xlogEveryNImpl(size_t n) {
  static std::atomic<size_t> count{0};
  auto const value = count.load(std::memory_order_relaxed);
  count.store(value + 1, std::memory_order_relaxed);
  return FOLLY_UNLIKELY((value % n) == 0);
}

} // namespace detail
} // namespace folly

/**
 * Similar to XLOG(...) except only log a message every @param n
 * invocations, approximately.
 *
 * The internal counter is process-global and threadsafe, but to
 * avoid the performance degradation of atomic-rmw operations,
 * increments are non-atomic. Some increments may be missed under
 * contention, leading to possible over-logging or under-logging
 * effects.
 */
#define XLOG_EVERY_N(level, n, ...)                                       \
  XLOG_IF(                                                                \
      level,                                                              \
      [&] {                                                               \
        struct folly_detail_xlog_tag {};                                  \
        return ::folly::detail::xlogEveryNImpl<folly_detail_xlog_tag>(n); \
      }(),                                                                \
      ##__VA_ARGS__)

/**
 * Similar to XLOGF(...) except only log a message every @param n
 * invocations, approximately.
 *
 * See concurrency discussion for XLOG_EVERY_N which applies here as well.
 */
#define XLOGF_EVERY_N(level, n, fmt, ...)                                 \
  XLOGF_IF(                                                               \
      level,                                                              \
      [&] {                                                               \
        struct folly_detail_xlog_tag {};                                  \
        return ::folly::detail::xlogEveryNImpl<folly_detail_xlog_tag>(n); \
      }(),                                                                \
      fmt,                                                                \
      ##__VA_ARGS__)

/**
 * Similar to XLOG(...) except only log a message every @param n
 * invocations, approximately, and if the specified condition predicate
 * evaluates to true.
 *
 * See concurrency discussion for XLOG_EVERY_N which applies here as well.
 */
#define XLOG_EVERY_N_IF(level, cond, n, ...)                                  \
  XLOG_IF(                                                                    \
      level,                                                                  \
      (cond) &&                                                               \
          [&] {                                                               \
            struct folly_detail_xlog_tag {};                                  \
            return ::folly::detail::xlogEveryNImpl<folly_detail_xlog_tag>(n); \
          }(),                                                                \
      ##__VA_ARGS__)

/**
 * Similar to XLOG(...) except it logs a message if the condition predicate
 * evaluates to true or approximately every @param n invocations
 *
 * See concurrency discussion for XLOG_EVERY_N which applies here as well.
 */
#define XLOG_EVERY_N_OR(level, cond, n, ...)                                  \
  XLOG_IF(                                                                    \
      level,                                                                  \
      (cond) ||                                                               \
          [&] {                                                               \
            struct folly_detail_xlog_tag {};                                  \
            return ::folly::detail::xlogEveryNImpl<folly_detail_xlog_tag>(n); \
          }(),                                                                \
      ##__VA_ARGS__)

/**
 * Similar to XLOGF(...) except only log a message every @param n
 * invocations, approximately, and if the specified condition predicate
 * evaluates to true.
 *
 * See concurrency discussion for XLOG_EVERY_N which applies here as well.
 */
#define XLOGF_EVERY_N_IF(level, cond, n, fmt, ...)                            \
  XLOGF_IF(                                                                   \
      level,                                                                  \
      (cond) &&                                                               \
          [&] {                                                               \
            struct folly_detail_xlog_tag {};                                  \
            return ::folly::detail::xlogEveryNImpl<folly_detail_xlog_tag>(n); \
          }(),                                                                \
      fmt,                                                                    \
      ##__VA_ARGS__)

/**
 * Similar to XLOGF(...) except it logs a message if the condition predicate
 * evaluates to true or approximately every @param n invocations
 *
 * See concurrency discussion for XLOG_EVERY_N which applies here as well.
 */
#define XLOGF_EVERY_N_OR(level, cond, n, fmt, ...)                            \
  XLOGF_IF(                                                                   \
      level,                                                                  \
      (cond) ||                                                               \
          [&] {                                                               \
            struct folly_detail_xlog_tag {};                                  \
            return ::folly::detail::xlogEveryNImpl<folly_detail_xlog_tag>(n); \
          }(),                                                                \
      fmt,                                                                    \
      ##__VA_ARGS__)

namespace folly {
namespace detail {

template <typename Tag>
FOLLY_EXPORT FOLLY_ALWAYS_INLINE bool xlogEveryNExactImpl(size_t n) {
  static std::atomic<size_t> count{0};
  auto const value = count.fetch_add(1, std::memory_order_relaxed);
  return FOLLY_UNLIKELY((value % n) == 0);
}

} // namespace detail
} // namespace folly

/**
 * Similar to XLOG(...) except only log a message every @param n
 * invocations, exactly.
 *
 * The internal counter is process-global and threadsafe and
 * increments are atomic. The over-logging and under-logging
 * schenarios of XLOG_EVERY_N(...) are avoided, traded off for
 * the performance degradation of atomic-rmw operations.
 */
#define XLOG_EVERY_N_EXACT(level, n, ...)                                      \
  XLOG_IF(                                                                     \
      level,                                                                   \
      [&] {                                                                    \
        struct folly_detail_xlog_tag {};                                       \
        return ::folly::detail::xlogEveryNExactImpl<folly_detail_xlog_tag>(n); \
      }(),                                                                     \
      ##__VA_ARGS__)

namespace folly {
namespace detail {

size_t& xlogEveryNThreadEntry(void const* const key);

template <typename Tag>
FOLLY_EXPORT FOLLY_ALWAYS_INLINE bool xlogEveryNThreadImpl(size_t n) {
  static char key;
  auto& count = xlogEveryNThreadEntry(&key);
  return FOLLY_UNLIKELY((count++ % n) == 0);
}

} // namespace detail
} // namespace folly

/**
 * Similar to XLOG(...) except only log a message every @param n
 * invocations per thread.
 *
 * The internal counter is thread-local, avoiding the contention
 * which the XLOG_EVERY_N variations which use a global counter
 * may suffer. If a given XLOG_EVERY_N or variation expansion is
 * encountered concurrently by multiple threads in a hot code
 * path and the global counter in the expansion is observed to
 * be contended, then switching to XLOG_EVERY_N_THREAD can help.
 *
 * Every thread that invokes this expansion has a counter for
 * this expansion. The internal counters are all stored in a
 * single thread-local map to control TLS overhead, at the cost
 * of a small runtime performance hit.
 */
#define XLOG_EVERY_N_THREAD(level, n, ...)                                   \
  XLOG_IF(                                                                   \
      level,                                                                 \
      [&] {                                                                  \
        struct folly_detail_xlog_tag {};                                     \
        return ::folly::detail::xlogEveryNThreadImpl<folly_detail_xlog_tag>( \
            n);                                                              \
      }(),                                                                   \
      ##__VA_ARGS__)

/**
 * Similar to XLOG(...) except only log at most @param count messages
 * per @param ms millisecond interval.
 *
 * The internal counters are process-global and threadsafe.
 */
#define XLOG_N_PER_MS(level, count, ms, ...)               \
  XLOG_IF(                                                 \
      level,                                               \
      [] {                                                 \
        static ::folly::logging::IntervalRateLimiter       \
            folly_detail_xlog_limiter(                     \
                (count), ::std::chrono::milliseconds(ms)); \
        return folly_detail_xlog_limiter.check();          \
      }(),                                                 \
      ##__VA_ARGS__)

namespace folly {
namespace detail {

template <typename Tag>
FOLLY_EXPORT FOLLY_ALWAYS_INLINE bool xlogFirstNExactImpl(std::size_t n) {
  static std::atomic<std::size_t> counter{0};
  auto const value = counter.load(std::memory_order_relaxed);
  return value < n && counter.fetch_add(1, std::memory_order_relaxed) < n;
}

} // namespace detail
} // namespace folly

/**
 * Similar to XLOG(...) except only log a message the first n times, exactly.
 *
 * The internal counter is process-global and threadsafe and exchanges are
 * atomic.
 */
#define XLOG_FIRST_N(level, n, ...)                                            \
  XLOG_IF(                                                                     \
      level,                                                                   \
      [&] {                                                                    \
        struct folly_detail_xlog_tag {};                                       \
        return ::folly::detail::xlogFirstNExactImpl<folly_detail_xlog_tag>(n); \
      }(),                                                                     \
      ##__VA_ARGS__)

/**
 * Similar to XLOGF(...) except only log a message the first n times, exactly.
 *
 * The internal counter is process-global and threadsafe and exchanges are
 * atomic.
 */
#define XLOGF_FIRST_N(level, n, fmt, ...)                                      \
  XLOGF_IF(                                                                    \
      level,                                                                   \
      [&] {                                                                    \
        struct folly_detail_xlog_tag {};                                       \
        return ::folly::detail::xlogFirstNExactImpl<folly_detail_xlog_tag>(n); \
      }(),                                                                     \
      fmt,                                                                     \
      ##__VA_ARGS__)

/**
 * FOLLY_XLOG_STRIP_PREFIXES can be defined to a string containing a
 * colon-separated list of directory prefixes to strip off from the filename
 * before using it to compute the log category name.
 *
 * If this is defined, use xlogStripFilename() to strip off directory prefixes;
 * otherwise just use __FILE__ literally.  xlogStripFilename() is a constexpr
 * expression so that this stripping can be performed fully at compile time.
 * (There is no guarantee that the compiler will evaluate it at compile time,
 * though.)
 */
#ifdef FOLLY_XLOG_STRIP_PREFIXES
#define XLOG_FILENAME        \
  (static_cast<char const*>( \
      ::folly::xlogStripFilename(__FILE__, FOLLY_XLOG_STRIP_PREFIXES)))
#else
#define XLOG_FILENAME (static_cast<char const*>(__FILE__))
#endif

#define XLOG_IMPL(level, type, ...) \
  XLOG_ACTUAL_IMPL(                 \
      level, true, ::folly::isLogLevelFatal(level), type, ##__VA_ARGS__)

#define XLOG_IF_IMPL(level, cond, type, ...) \
  XLOG_ACTUAL_IMPL(level, cond, false, type, ##__VA_ARGS__)

/**
 * Helper macro used to implement XLOG() and XLOGF()
 *
 * Beware that the level argument is evaluated twice.
 *
 * This macro is somewhat tricky:
 *
 * - In order to support streaming argument support (with the << operator),
 *   the macro must expand to a single ternary ? expression.  This is the only
 *   way we can avoid evaluating the log arguments if the log check fails,
 *   and still have the macro behave as expected when used as the body of an if
 *   or else statement.
 *
 * - We need to store some static-scope local state in order to track the
 *   LogCategory to use.  This is a bit tricky to do and still meet the
 *   requirements of being a single expression, but fortunately static
 *   variables inside a lambda work for this purpose.
 *
 *   Inside header files, each XLOG() statement defines two static variables:
 *   - the LogLevel for this category
 *   - a pointer to the LogCategory
 *
 *   If the __INCLUDE_LEVEL__ macro is available (both gcc and clang support
 *   this), then we we can detect when we are inside a .cpp file versus a
 *   header file.  If we are inside a .cpp file, we can avoid declaring these
 *   variables once per XLOG() statement, and instead we only declare one copy
 *   of these variables for the entire file.
 *
 * - We want to make sure this macro is safe to use even from inside static
 *   initialization code that runs before main.  We also want to make the log
 *   admittance check as cheap as possible, so that disabled debug logs have
 *   minimal overhead, and can be left in place even in performance senstive
 *   code.
 *
 *   In order to do this, we rely on zero-initialization of variables with
 *   static storage duration.  The LogLevel variable will always be
 *   0-initialized before any code runs.  Therefore the very first time an
 *   XLOG() statement is hit the initial log level check will always pass
 *   (since all level values are greater or equal to 0), and we then do a
 *   second check to see if the log level and category variables need to be
 *   initialized.  On all subsequent calls, disabled log statements can be
 *   skipped with just a single check of the LogLevel.
 */
#define XLOG_ACTUAL_IMPL(level, cond, always_fatal, type, ...)              \
  /* NOLINTNEXTLINE(readability-simplify-boolean-expr) */                   \
  (!XLOG_IS_ON_IMPL(level) || !(cond))                                      \
      ? ::folly::logDisabledHelper(::std::bool_constant<always_fatal>{})    \
      : ::folly::LogStreamVoidify<::folly::isLogLevelFatal(level)>{} &      \
          ::folly::LogStreamProcessor(                                      \
              [] {                                                          \
                static ::folly::XlogCategoryInfo<XLOG_IS_IN_HEADER_FILE>    \
                    folly_detail_xlog_category;                             \
                return folly_detail_xlog_category.getInfo(                  \
                    &::folly::detail::custom::xlogFileScopeInfo);           \
              }(),                                                          \
              (level),                                                      \
              [] {                                                          \
                constexpr auto* folly_detail_xlog_filename = XLOG_FILENAME; \
                return ::folly::detail::custom::getXlogCategoryName(        \
                    folly_detail_xlog_filename, 0);                         \
              }(),                                                          \
              ::folly::detail::custom::isXlogCategoryOverridden(0),         \
              XLOG_FILENAME,                                                \
              __LINE__,                                                     \
              __func__,                                                     \
              (type),                                                       \
              ##__VA_ARGS__)                                                \
              .stream()

/**
 * Check if an XLOG() statement with the given log level would be enabled.
 *
 * The level parameter must be an unqualified LogLevel enum value.
 */
#define XLOG_IS_ON(level) XLOG_IS_ON_IMPL(::folly::LogLevel::level)

/**
 * Expects a fully qualified LogLevel enum value.
 *
 * This helper macro invokes XLOG_IS_ON_IMPL_HELPER() to perform the real
 * log level check, with a couple additions:
 * - If the log level is less than FOLLY_XLOG_MIN_LEVEL it evaluates to false
 *   to allow the compiler to completely optimize out the check and log message
 *   if the level is less than this compile-time fixed constant.
 * - If the log level is fatal, this has an extra check at the end to ensure the
 *   compiler can detect that it always evaluates to true.  This helps the
 *   compiler detect that statements like XCHECK(false) never return.  Note that
 *   XLOG_IS_ON_IMPL_HELPER() must still be invoked first for fatal log levels
 *   in order to initialize folly::detail::custom::xlogFileScopeInfo.
 */
#define XLOG_IS_ON_IMPL(level)                              \
  ((((level) >= ::folly::LogLevel::FOLLY_XLOG_MIN_LEVEL) && \
    XLOG_IS_ON_IMPL_HELPER(level)) ||                       \
   ((level) >= ::folly::kMinFatalLogLevel))

/**
 * Helper macro to implement of XLOG_IS_ON()
 *
 * This macro is used in the XLOG() implementation, and therefore must be as
 * cheap as possible.  It stores the category's LogLevel as a local static
 * variable.  The very first time this macro is evaluated it will look up the
 * correct LogCategory and initialize the LogLevel.  Subsequent calls then
 * are only a single conditional log level check.
 *
 * The LogCategory object keeps track of this local LogLevel variable and
 * automatically keeps it up-to-date when the category's effective level is
 * changed.
 *
 * See XlogLevelInfo for the implementation details.
 */
#define XLOG_IS_ON_IMPL_HELPER(level)                           \
  ([] {                                                         \
    /* NOLINTNEXTLINE(misc-const-correctness) */                \
    static ::folly::XlogLevelInfo<XLOG_IS_IN_HEADER_FILE>       \
        folly_detail_xlog_level;                                \
    constexpr auto* folly_detail_xlog_filename = XLOG_FILENAME; \
    constexpr folly::StringPiece folly_detail_xlog_category =   \
        ::folly::detail::custom::getXlogCategoryName(           \
            folly_detail_xlog_filename, 0);                     \
    return folly_detail_xlog_level.check(                       \
        (level),                                                \
        folly_detail_xlog_category,                             \
        ::folly::detail::custom::isXlogCategoryOverridden(0),   \
        &::folly::detail::custom::xlogFileScopeInfo);           \
  }())

/**
 * Get the name of the log category that will be used by XLOG() statements
 * in this file.
 */
#define XLOG_GET_CATEGORY_NAME()                                        \
  (::folly::detail::custom::isXlogCategoryOverridden(0)                 \
       ? ::folly::detail::custom::getXlogCategoryName(XLOG_FILENAME, 0) \
       : ::folly::getXlogCategoryNameForFile(XLOG_FILENAME))

/**
 * Get a pointer to the LogCategory that will be used by XLOG() statements in
 * this file.
 *
 * This is just a small wrapper around a LoggerDB::getCategory() call.
 * This must be implemented as a macro since it uses __FILE__, and that must
 * expand to the correct filename based on where the macro is used.
 */
#define XLOG_GET_CATEGORY() \
  (::folly::LoggerDB::get().getCategory(XLOG_GET_CATEGORY_NAME()))

/**
 * XLOG_SET_CATEGORY_NAME() can be used to explicitly define the log category
 * name used by all XLOG() and XLOGF() calls in this translation unit.
 *
 * This overrides the default behavior of picking a category name based on the
 * current filename.
 *
 * This should be used at the top-level scope in a .cpp file, before any XLOG()
 * or XLOGF() macros have been used in the file.
 *
 * XLOG_SET_CATEGORY_NAME() cannot be used inside header files.
 */
#ifdef __INCLUDE_LEVEL__
#define XLOG_SET_CATEGORY_CHECK \
  static_assert(                \
      __INCLUDE_LEVEL__ == 0,   \
      "XLOG_SET_CATEGORY_NAME() should not be used in header files");
#else
#define XLOG_SET_CATEGORY_CHECK
#endif

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 9
// gcc 8.x crashes with an internal compiler error when trying to evaluate
// getXlogCategoryName() in a constexpr expression.  Keeping category name
// constexpr is important for performance, and XLOG_SET_CATEGORY_NAME() is
// fairly rarely used, so simply let it be a no-op if compiling with older
// versions of gcc.
#define XLOG_SET_CATEGORY_NAME(category)
#else
#define XLOG_SET_CATEGORY_NAME(category)                                     \
  namespace folly {                                                          \
  namespace detail {                                                         \
  namespace custom {                                                         \
  namespace {                                                                \
  struct xlog_correct_usage;                                                 \
  static_assert(                                                             \
      ::std::is_same<                                                        \
          xlog_correct_usage,                                                \
          ::folly::detail::custom::xlog_correct_usage>::value,               \
      "XLOG_SET_CATEGORY_NAME() should not be used within namespace scope"); \
  XLOG_SET_CATEGORY_CHECK                                                    \
  consteval inline StringPiece getXlogCategoryName(StringPiece, int) {       \
    return category;                                                         \
  }                                                                          \
  consteval inline bool isXlogCategoryOverridden(int) {                      \
    return true;                                                             \
  }                                                                          \
  }                                                                          \
  }                                                                          \
  }                                                                          \
  }
#endif

/**
 * Assert that a condition is true.
 *
 * This crashes the program with an XLOG(FATAL) message if the condition is
 * false.  Unlike assert() CHECK statements are always enabled, regardless of
 * the setting of NDEBUG.
 */
#define XCHECK(cond, ...)         \
  XLOG_IF(                        \
      FATAL,                      \
      FOLLY_UNLIKELY(!(cond)),    \
      "Check failed: " #cond " ", \
      ##__VA_ARGS__)

namespace folly {
namespace detail {
template <typename Arg1, typename Arg2, typename CmpFn>
std::unique_ptr<std::string> XCheckOpImpl(
    folly::StringPiece checkStr,
    const Arg1& arg1,
    const Arg2& arg2,
    CmpFn&& cmp_fn) {
  if (cmp_fn(arg1, arg2)) [[likely]] {
    return nullptr;
  }
  return std::make_unique<std::string>(folly::to<std::string>(
      "Check failed: ",
      checkStr,
      " (",
      ::folly::logging::objectToString(arg1),
      " vs. ",
      folly::logging::objectToString(arg2),
      ")"));
}
} // namespace detail
} // namespace folly

#define XCHECK_OP(op, arg1, arg2, ...)                                     \
  while (                                                                  \
      auto _folly_logging_check_result = ::folly::detail::XCheckOpImpl(    \
          #arg1 " " #op " " #arg2,                                         \
          (arg1),                                                          \
          (arg2),                                                          \
          [](const auto& _folly_check_arg1, const auto& _folly_check_arg2) \
              -> bool { return _folly_check_arg1 op _folly_check_arg2; })) \
  XLOG(FATAL, *_folly_logging_check_result, ##__VA_ARGS__)

/**
 * Assert a comparison relationship between two arguments.
 *
 * If the comparison check fails the values of both expressions being compared
 * will be included in the failure message.  This is the main benefit of using
 * these specific comparison macros over XCHECK().  XCHECK() will simply log
 * that the expression evaluated was false, while these macros include the
 * specific values being compared.
 */
#define XCHECK_EQ(arg1, arg2, ...) XCHECK_OP(==, arg1, arg2, ##__VA_ARGS__)
#define XCHECK_NE(arg1, arg2, ...) XCHECK_OP(!=, arg1, arg2, ##__VA_ARGS__)
#define XCHECK_LT(arg1, arg2, ...) XCHECK_OP(<, arg1, arg2, ##__VA_ARGS__)
#define XCHECK_GT(arg1, arg2, ...) XCHECK_OP(>, arg1, arg2, ##__VA_ARGS__)
#define XCHECK_LE(arg1, arg2, ...) XCHECK_OP(<=, arg1, arg2, ##__VA_ARGS__)
#define XCHECK_GE(arg1, arg2, ...) XCHECK_OP(>=, arg1, arg2, ##__VA_ARGS__)

/**
 * Assert that a condition is true in debug builds only.
 *
 * When NDEBUG is not defined this behaves like XCHECK().
 * When NDEBUG is defined XDCHECK statements are not evaluated and will never
 * log.
 *
 * You can use `XLOG_IF(DFATAL, condition)` instead if you want the condition to
 * be evaluated in release builds but log a message without crashing the
 * program.
 */
#define XDCHECK(cond, ...) \
  (!::folly::kIsDebug) ? static_cast<void>(0) : XCHECK(cond, ##__VA_ARGS__)

/*
 * It would be nice to rely solely on folly::kIsDebug here rather than NDEBUG.
 * However doing so would make the code substantially more complicated.  It is
 * much simpler to simply change the definition of XDCHECK_OP() based on NDEBUG.
 */
#ifdef NDEBUG
#define XDCHECK_OP(op, arg1, arg2, ...) \
  while (false)                         \
  XCHECK_OP(op, arg1, arg2, ##__VA_ARGS__)
#else
#define XDCHECK_OP(op, arg1, arg2, ...) XCHECK_OP(op, arg1, arg2, ##__VA_ARGS__)
#endif

/**
 * Assert a comparison relationship between two arguments in debug builds.
 *
 * When NDEBUG is not set these macros behaves like the corresponding
 * XCHECK_XX() versions (XCHECK_EQ(), XCHECK_NE(), etc).
 *
 * When NDEBUG is defined these statements are not evaluated and will never log.
 */
#define XDCHECK_EQ(arg1, arg2, ...) XDCHECK_OP(==, arg1, arg2, ##__VA_ARGS__)
#define XDCHECK_NE(arg1, arg2, ...) XDCHECK_OP(!=, arg1, arg2, ##__VA_ARGS__)
#define XDCHECK_LT(arg1, arg2, ...) XDCHECK_OP(<, arg1, arg2, ##__VA_ARGS__)
#define XDCHECK_GT(arg1, arg2, ...) XDCHECK_OP(>, arg1, arg2, ##__VA_ARGS__)
#define XDCHECK_LE(arg1, arg2, ...) XDCHECK_OP(<=, arg1, arg2, ##__VA_ARGS__)
#define XDCHECK_GE(arg1, arg2, ...) XDCHECK_OP(>=, arg1, arg2, ##__VA_ARGS__)

/**
 * XLOG_IS_IN_HEADER_FILE evaluates to false if we can definitively tell if we
 * are not in a header file.  Otherwise, it evaluates to true.
 */
#ifdef __INCLUDE_LEVEL__
#define XLOG_IS_IN_HEADER_FILE bool(__INCLUDE_LEVEL__ > 0)
#else
// Without __INCLUDE_LEVEL__ we canot tell if we are in a header file or not,
// and must pessimstically assume we are always in a header file.
#define XLOG_IS_IN_HEADER_FILE true
#endif

namespace folly {

class XlogFileScopeInfo {
 public:
  std::atomic<::folly::LogLevel> level{folly::LogLevel::UNINITIALIZED};
  ::folly::LogCategory* category{nullptr};
};

/**
 * A file-static XlogLevelInfo and XlogCategoryInfo object is declared for each
 * XLOG() statement.
 *
 * We intentionally do not provide constructors for these structures, and rely
 * on their members to be zero-initialized when the program starts.  This
 * ensures that everything will work as desired even if XLOG() statements are
 * used during dynamic object initialization before main().
 */
template <bool IsInHeaderFile>
class XlogLevelInfo {
 public:
  bool check(
      LogLevel levelToCheck,
      folly::StringPiece categoryName,
      bool isOverridden,
      XlogFileScopeInfo*) {
    // Do an initial relaxed check.  If this fails we know the category level
    // is initialized and the log admittance check failed.
    // Use FOLLY_LIKELY() to optimize for the case of disabled debug statements:
    // we disabled debug statements to be cheap.  If the log message is
    // enabled then this check will still be minimal perf overhead compared to
    // the overall cost of logging it.
    if (levelToCheck < level_.load(std::memory_order_relaxed)) [[likely]] {
      return false;
    }

    // If we are still here, then either:
    // - The log level check actually passed, or
    // - level_ has not been initialized yet, and we have to initialize it and
    //   then re-perform the check.
    //
    // Do this work in a separate helper method.  It is intentionally defined
    // in the xlog.cpp file to avoid inlining, to reduce the amount of code
    // emitted for each XLOG() statement.
    auto currentLevel = loadLevelFull(categoryName, isOverridden);
    return levelToCheck >= currentLevel;
  }

 private:
  LogLevel loadLevelFull(folly::StringPiece categoryName, bool isOverridden);

  // XlogLevelInfo objects are always defined with static storage.
  // This member will always be zero-initialized on program start.
  std::atomic<LogLevel> level_;
};

template <bool IsInHeaderFile>
class XlogCategoryInfo {
 public:
  bool isInitialized() const {
    return isInitialized_.load(std::memory_order_acquire);
  }

  LogCategory* init(folly::StringPiece categoryName, bool isOverridden);

  LogCategory* getCategory(XlogFileScopeInfo*) { return category_; }

  /**
   * Get a pointer to pass into the LogStreamProcessor constructor,
   * so that it is able to look up the LogCategory information.
   */
  XlogCategoryInfo<IsInHeaderFile>* getInfo(XlogFileScopeInfo*) { return this; }

 private:
  // These variables will always be zero-initialized on program start.
  std::atomic<bool> isInitialized_;
  LogCategory* category_;
};

/**
 * Specialization of XlogLevelInfo for XLOG() statements in the .cpp file being
 * compiled.  In this case we only define a single file-static LogLevel object
 * for the entire file, rather than defining one for each XLOG() statement.
 */
template <>
class XlogLevelInfo<false> {
 public:
  static bool check(
      LogLevel levelToCheck,
      folly::StringPiece categoryName,
      bool isOverridden,
      XlogFileScopeInfo* fileScopeInfo) {
    // As above in the non-specialized XlogFileScopeInfo code, do a simple
    // relaxed check first.
    if (levelToCheck < fileScopeInfo->level.load(::std::memory_order_relaxed))
        [[likely]] {
      return false;
    }

    // If we are still here we the file-scope log level either needs to be
    // initialized, or the log level check legitimately passed.
    auto currentLevel =
        loadLevelFull(categoryName, isOverridden, fileScopeInfo);
    return levelToCheck >= currentLevel;
  }

 private:
  static LogLevel loadLevelFull(
      folly::StringPiece categoryName,
      bool isOverridden,
      XlogFileScopeInfo* fileScopeInfo);
};

/**
 * Specialization of XlogCategoryInfo for XLOG() statements in the .cpp file
 * being compiled.  In this case we only define a single file-static LogLevel
 * object for the entire file, rather than defining one for each XLOG()
 * statement.
 */
template <>
class XlogCategoryInfo<false> {
 public:
  /**
   * Get a pointer to pass into the LogStreamProcessor constructor,
   * so that it is able to look up the LogCategory information.
   */
  XlogFileScopeInfo* getInfo(XlogFileScopeInfo* fileScopeInfo) {
    return fileScopeInfo;
  }
};

/**
 * Get the default XLOG() category name for the given filename.
 *
 * This function returns the category name that will be used by XLOG() if
 * XLOG_SET_CATEGORY_NAME() has not been used.
 */
folly::StringPiece getXlogCategoryNameForFile(folly::StringPiece filename);

consteval bool xlogIsDirSeparator(char c) {
  return c == '/' || (kIsWindows && c == '\\');
}

namespace detail {
consteval const char* xlogStripFilenameRecursive(
    const char* filename,
    const char* prefixes,
    size_t prefixIdx,
    size_t filenameIdx,
    bool match);
consteval const char* xlogStripFilenameMatchFound(
    const char* filename,
    const char* prefixes,
    size_t prefixIdx,
    size_t filenameIdx) {
  return (filename[filenameIdx] == '\0')
      ? xlogStripFilenameRecursive(filename, prefixes, prefixIdx + 1, 0, true)
      : (xlogIsDirSeparator(filename[filenameIdx])
             ? xlogStripFilenameMatchFound(
                   filename, prefixes, prefixIdx, filenameIdx + 1)
             : (filename + filenameIdx));
}
consteval const char* xlogStripFilenameRecursive(
    const char* filename,
    const char* prefixes,
    size_t prefixIdx,
    size_t filenameIdx,
    bool match) {
  // This would be much easier to understand if written as a while loop.
  // However, in order to maintain compatibility with pre-C++14 compilers we
  // have implemented it recursively to adhere to C++11 restrictions for
  // constexpr functions.
  return ((prefixes[prefixIdx] == ':' && filename[filenameIdx] != ':') ||
          prefixes[prefixIdx] == '\0')
      ? ((match && filenameIdx > 0 &&
          (xlogIsDirSeparator(prefixes[filenameIdx - 1]) ||
           xlogIsDirSeparator(filename[filenameIdx])))
             ? (xlogStripFilenameMatchFound(
                   filename, prefixes, prefixIdx, filenameIdx))
             : ((prefixes[prefixIdx] == '\0')
                    ? filename
                    : xlogStripFilenameRecursive(
                          filename, prefixes, prefixIdx + 1, 0, true)))
      : ((match &&
          ((prefixes[prefixIdx] == filename[filenameIdx]) ||
           (xlogIsDirSeparator(prefixes[prefixIdx]) &&
            xlogIsDirSeparator(filename[filenameIdx]))))
             ? xlogStripFilenameRecursive(
                   filename, prefixes, prefixIdx + 1, filenameIdx + 1, true)
             : xlogStripFilenameRecursive(
                   filename, prefixes, prefixIdx + 1, 0, false));
}
} // namespace detail

/**
 * Strip directory prefixes from a filename before using it in XLOG macros.
 *
 * This is primarily used to strip off the initial project directory path for
 * projects that invoke the compiler with absolute path names.
 *
 * The filename argument is the filename to process.  This is normally the
 * contents of the __FILE__ macro from the invoking file.
 *
 * prefixes is a colon-separated list of directory prefixes to strip off if
 * present at the beginning of the filename.  The prefix list is searched in
 * order, and only the first match found will be stripped.
 *
 * e.g., xlogStripFilename("/my/project/src/foo.cpp", "/tmp:/my/project")
 * would return "src/foo.cpp"
 */
consteval const char* xlogStripFilename(
    const char* filename, const char* prefixes) {
  return detail::xlogStripFilenameRecursive(filename, prefixes, 0, 0, true);
}

namespace detail {

/*
 * We intentionally use an unnamed namespace inside a header file here.
 *
 * We want each .cpp file that uses xlog.h to get its own separate
 * implementation of the following functions and variables.
 */
namespace custom {
namespace {
struct xlog_correct_usage;

/**
 * The default getXlogCategoryName() function.
 *
 * By default this simply returns the filename argument passed in.
 * The default isXlogCategoryOverridden() function returns false, indicating
 * that the return value from getXlogCategoryName() needs to be converted
 * using getXlogCategoryNameForFile().
 *
 * These are two separate steps because getXlogCategoryName() itself needs to
 * remain constexpr--it is always evaluated in XLOG() statements, but we only
 * want to call getXlogCategoryNameForFile() the very first time through, when
 * we have to initialize the LogCategory object.
 *
 * This is a template function purely so that XLOG_SET_CATEGORY_NAME() can
 * define a more specific version of this function that will take precedence
 * over this one.
 */
template <typename T>
consteval inline StringPiece getXlogCategoryName(StringPiece filename, T) {
  return filename;
}

/**
 * The default isXlogCategoryOverridden() function.
 *
 * This returns false indicating that the category name has not been
 * overridden, so getXlogCategoryName() returns a raw filename that needs
 * to be translated with getXlogCategoryNameForFile().
 *
 * This is a template function purely so that XLOG_SET_CATEGORY_NAME() can
 * define a more specific version of this function that will take precedence
 * over this one.
 */
template <typename T>
consteval inline bool isXlogCategoryOverridden(T) {
  return false;
}

/**
 * File-scope LogLevel and LogCategory data for XLOG() statements,
 * if __INCLUDE_LEVEL__ is supported.
 *
 * This allows us to only have one LogLevel and LogCategory pointer for the
 * entire .cpp file, rather than needing a separate copy for each XLOG()
 * statement.
 */
constinit XlogFileScopeInfo xlogFileScopeInfo;
} // namespace
} // namespace custom

} // namespace detail
} // namespace folly
