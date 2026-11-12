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

/**
 * Likeliness annotations.
 *
 * Useful when the author has better knowledge than the compiler of whether
 * the branch condition is overwhelmingly likely to take a specific value.
 *
 * Useful when the author has better knowledge than the compiler of which code
 * paths are designed as the fast path and which are designed as the slow path,
 * and to force the compiler to optimize for the fast path, even when it is not
 * overwhelmingly likely.
 *
 * Notes:
 * * All supported compilers treat unconditionally-noreturn blocks as unlikely.
 *   This is true for blocks which unconditionally throw exceptions and for
 *   blocks which unconditionally call [[noreturn]]-annotated functions. Such
 *   cases do not require likeliness annotations.
 *
 * @file Likely.h
 * @refcode folly/docs/examples/folly/Likely.cpp
 */

#pragma once

#include <folly/lang/Builtin.h>

/**
 * Treat the condition as likely.
 *
 * @def FOLLY_LIKELY
 */
#define FOLLY_LIKELY(...) FOLLY_BUILTIN_EXPECT((__VA_ARGS__), 1)

/**
 * Treat the condition as unlikely.
 *
 * @def FOLLY_UNLIKELY
 */
#define FOLLY_UNLIKELY(...) FOLLY_BUILTIN_EXPECT((__VA_ARGS__), 0)

// Un-namespaced annotations

#undef LIKELY
#undef UNLIKELY

/**
 * Treat the condition as likely.
 *
 * @def LIKELY
 */
/**
 * Treat the condition as unlikely.
 *
 * @def UNLIKELY
 */
#if defined(__GNUC__)
#define LIKELY(x) (__builtin_expect((x), 1))
#define UNLIKELY(x) (__builtin_expect((x), 0))
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif
