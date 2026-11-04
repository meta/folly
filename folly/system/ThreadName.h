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

#include <string>
#include <thread>

#include <folly/Optional.h>
#include <folly/Range.h>
#include <folly/portability/Config.h>
#include <folly/portability/PThread.h>

namespace folly {

/**
 * This returns true if the current platform supports setting the name of the
 * current thread.
 */
bool canSetCurrentThreadName();

/**
 * This returns true if the current platform supports setting the name of
 * threads other than the one currently executing.
 */
bool canSetOtherThreadName();

/**
 * Get the name of the given thread, or nothing if an error occurs
 * or the functionality is not available.
 *
 * @param tid Thread id
 */
Optional<std::string> getThreadName(std::thread::id tid);

/**
 * Equivalent to getThreadName(std::this_thread::get_id());
 */
Optional<std::string> getCurrentThreadName();

/**
 * Set the name of the given thread.
 *
 * Returns false on failure, if an error occurs or the functionality
 * is not available.
 *
 * @param tid Thread id
 * @param name Name to set
 */
bool setThreadName(std::thread::id tid, StringPiece name);
bool setThreadName(pthread_t pid, StringPiece name);

/**
 * Equivalent to setThreadName(std::this_thread::get_id(), name);
 *
 * Also runs the callback set by setCurrentThreadNameCallback, if any, on the
 * calling thread.
 *
 * @param name Name to set
 */
bool setThreadName(StringPiece name);

/**
 * A function run on a thread right after it names itself through
 * setThreadName(StringPiece). It lets a program apply per-thread settings keyed
 * on the thread name (for example, choosing a memory allocator arena) to
 * threads that libraries it does not control create and name.
 *
 * It runs whether or not the platform could apply the name, may run
 * concurrently on many threads, and must not throw.
 */
using CurrentThreadNameCallback = void (*)(StringPiece name) noexcept;

/**
 * Sets the callback run by setThreadName(StringPiece), or clears it with
 * nullptr, and returns the previous one. Threads that named themselves before
 * the callback was set are not revisited.
 */
CurrentThreadNameCallback setCurrentThreadNameCallback(
    CurrentThreadNameCallback callback) noexcept;
} // namespace folly
