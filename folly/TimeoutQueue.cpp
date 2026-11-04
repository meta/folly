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

#include <folly/TimeoutQueue.h>

#include <iterator>
#include <limits>
#include <utility>
#include <vector>

namespace folly {

void TimeoutQueue::insert(int64_t expiration, Event event) {
  Id id = event.id;
  auto it = byExpiration_.emplace(expiration, std::move(event));
  try {
    byId_.emplace(id, it);
  } catch (...) {
    byExpiration_.erase(it);
    throw;
  }
}

TimeoutQueue::Id TimeoutQueue::add(
    int64_t now, int64_t delay, Callback callback) {
  Id id = nextId_++;
  insert(now + delay, {id, -1, std::move(callback)});
  return id;
}

TimeoutQueue::Id TimeoutQueue::addRepeating(
    int64_t now, int64_t interval, Callback callback) {
  Id id = nextId_++;
  insert(now + interval, {id, interval, std::move(callback)});
  return id;
}

int64_t TimeoutQueue::nextExpiration() const {
  return (
      byExpiration_.empty()
          ? std::numeric_limits<int64_t>::max()
          : byExpiration_.begin()->first);
}

bool TimeoutQueue::erase(Id id) {
  auto it = byId_.find(id);
  if (it == byId_.end()) {
    return false;
  }
  byExpiration_.erase(it->second);
  byId_.erase(it);
  return true;
}

int64_t TimeoutQueue::runInternal(int64_t now, bool onceOnly) {
  int64_t nextExp;
  do {
    const auto end = byExpiration_.upper_bound(now);
    // Do everything that can throw before changing either index. One-shot
    // callbacks are moved out below; repeating ones stay queued, so copy them.
    std::vector<Event> expired;
    expired.reserve(std::distance(byExpiration_.begin(), end));
    for (auto it = byExpiration_.begin(); it != end; ++it) {
      const Event& event = it->second;
      if (event.repeatInterval < 0) {
        expired.push_back({event.id, event.repeatInterval, {}});
      } else {
        expired.push_back(event);
      }
    }

    // Nothing below throws. Rescheduled expirations are >= now, so rescheduled
    // events go after the expired events not yet processed.
    for (auto& event : expired) {
      auto it = byExpiration_.begin();
      auto idIt = byId_.find(event.id);
      if (event.repeatInterval < 0) {
        event.callback.swap(it->second.callback);
        byExpiration_.erase(it);
        byId_.erase(idIt);
      } else {
        // Reschedule before running callbacks so they can erase the event.
        auto node = byExpiration_.extract(it);
        node.key() = now + event.repeatInterval;
        idIt->second = byExpiration_.insert(std::move(node));
      }
    }

    for (const auto& event : expired) {
      event.callback(event.id, now);
    }
    nextExp = nextExpiration();
  } while (!onceOnly && nextExp <= now);
  return nextExp;
}

} // namespace folly
