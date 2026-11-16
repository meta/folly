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

#include <folly/io/async/EventBaseAtomicNotificationQueue.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/EventBase.h>
#include <folly/portability/GTest.h>
#include <folly/portability/Windows.h>

#ifdef _WIN32

#include <iphlpapi.h> // @manual=//folly/portability/provide:iphlpapi

namespace folly {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr auto kWait = 10s;
constexpr size_t kProducers = 4;

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void requireWindowsSuccess(DWORD status, const char* operation) {
  if (status != ERROR_SUCCESS) {
    throw std::runtime_error(
        std::string(operation) + ": Windows error " + std::to_string(status));
  }
}

using TcpKey = std::tuple<DWORD, DWORD, DWORD, DWORD>;
using TcpKeys = std::set<TcpKey>;

TcpKey tcpKey(const MIB_TCPROW_OWNER_PID& row) {
  return {row.dwLocalAddr, row.dwLocalPort, row.dwRemoteAddr, row.dwRemotePort};
}

bool isLoopback(DWORD address) {
  return (ntohl(address) >> 24) == 127;
}

std::vector<MIB_TCPROW_OWNER_PID> connectedRows() {
  DWORD bytes = 0;
  auto status = GetExtendedTcpTable(
      nullptr, &bytes, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
  require(
      status == ERROR_INSUFFICIENT_BUFFER,
      "GetExtendedTcpTable did not return its buffer size");

  for (size_t attempt = 0; attempt < 4; ++attempt) {
    std::vector<DWORD> storage((bytes + sizeof(DWORD) - 1) / sizeof(DWORD));
    status = GetExtendedTcpTable(
        storage.data(), &bytes, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (status == ERROR_INSUFFICIENT_BUFFER) {
      continue;
    }
    requireWindowsSuccess(status, "GetExtendedTcpTable");
    const auto* table =
        reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(storage.data());
    std::vector<MIB_TCPROW_OWNER_PID> rows;
    for (DWORD index = 0; index < table->dwNumEntries; ++index) {
      const auto& row = table->table[index];
      if (row.dwOwningPid != GetCurrentProcessId() ||
          row.dwState != MIB_TCP_STATE_ESTAB) {
        continue;
      }
      require(
          isLoopback(row.dwLocalAddr) && isLoopback(row.dwRemoteAddr),
          "test PID has a non-loopback connection; refusing reset injection");
      rows.push_back(row);
    }
    return rows;
  }
  throw std::runtime_error("TCP table kept growing during bounded enumeration");
}

TcpKeys connectedKeys() {
  TcpKeys keys;
  for (const auto& row : connectedRows()) {
    keys.insert(tcpKey(row));
  }
  return keys;
}

bool hasPairedEndpoints(const TcpKeys& keys) {
  return !keys.empty() &&
      std::all_of(keys.begin(), keys.end(), [&](const TcpKey& key) {
        const auto& [localAddress, localPort, remoteAddress, remotePort] = key;
        return keys.count({remoteAddress, remotePort, localAddress, localPort});
      });
}

TcpKeys resetTestProcessConnections(bool requirePairs = true) {
  const auto baseline = connectedKeys();
  if (requirePairs) {
    require(hasPairedEndpoints(baseline), "no complete loopback TCP pairs");
  }

  for (const auto& key : baseline) {
    // Deleting either endpoint can also remove its peer's row. Refresh the
    // owner table before each deletion, never using the unowned TCP table.
    const auto rows = connectedRows();
    const auto found =
        std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
          return tcpKey(row) == key;
        });
    if (found == rows.end()) {
      continue;
    }
    const auto& row = *found;
    require(
        row.dwOwningPid == GetCurrentProcessId() &&
            isLoopback(row.dwLocalAddr) && isLoopback(row.dwRemoteAddr),
        "refusing SetTcpEntry for a foreign PID or non-loopback endpoint");
    MIB_TCPROW deletion{};
    deletion.dwState = MIB_TCP_STATE_DELETE_TCB;
    deletion.dwLocalAddr = row.dwLocalAddr;
    deletion.dwLocalPort = row.dwLocalPort;
    deletion.dwRemoteAddr = row.dwRemoteAddr;
    deletion.dwRemotePort = row.dwRemotePort;
    const auto status = SetTcpEntry(&deletion);
    if ((status == ERROR_NOT_FOUND || status == ERROR_INVALID_PARAMETER ||
         status == ERROR_MR_MID_NOT_FOUND) &&
        !connectedKeys().count(key)) {
      continue;
    }
    requireWindowsSuccess(status, "SetTcpEntry (elevated Windows RE required)");
  }
  return baseline;
}

struct DeliveryLog {
  std::mutex mutex;
  std::condition_variable changed;
  std::array<std::vector<int>, kProducers> received;
  bool ready{false};

  void record(size_t producer, int sequence) noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    received.at(producer).push_back(sequence);
    changed.notify_all();
  }

  bool waitFor(size_t producer, size_t count) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, kWait, [&] {
      return received.at(producer).size() >= count;
    });
  }

  std::vector<int> messages(size_t producer) {
    std::lock_guard<std::mutex> lock(mutex);
    return received.at(producer);
  }
};

class IdleMarker : public EventBase::LoopCallback {
 public:
  IdleMarker(EventBase& base, DeliveryLog& log) : base_(base), log_(log) {}

  void runLoopCallback() noexcept override {
    if (base_.getNotificationQueueSize() || base_.getNumLoopCallbacks()) {
      base_.runAfterLoop(this);
      return;
    }
    std::lock_guard<std::mutex> lock(log_.mutex);
    log_.ready = true;
    log_.changed.notify_all();
  }

 private:
  EventBase& base_;
  DeliveryLog& log_;
};

class IdleEventBase {
 public:
  IdleEventBase() : worker_([this] { doWorker(); }) {}

  ~IdleEventBase() { finish(); }

  bool ready() {
    std::unique_lock<std::mutex> lock(log_.mutex);
    return log_.changed.wait_for(lock, kWait, [&] { return log_.ready; });
  }

  void post(size_t producer, int sequence) {
    eventBase_->runInEventBaseThreadAlwaysEnqueue(
        [log = &log_, producer, sequence] { log->record(producer, sequence); });
  }

  bool waitFor(size_t producer, size_t count) {
    return log_.waitFor(producer, count);
  }

  bool waitForReplacement(
      const TcpKeys& baseline, const TcpKeys& additionallyReset = {}) {
    const auto deadline = Clock::now() + kWait;
    do {
      const auto current = connectedKeys();
      const auto replaced = [&](const TcpKeys& keys) {
        return std::none_of(keys.begin(), keys.end(), [&](const auto& key) {
          return current.count(key);
        });
      };
      // The baseline may include libevent's separate signal pair, which this
      // queue does not repair. Only the Folly notifier recreates a pair here;
      // these tests do not exercise libevent signal-event notification.
      const auto newPair =
          std::any_of(current.begin(), current.end(), [&](const TcpKey& key) {
            const auto& [localAddress, localPort, remoteAddress, remotePort] =
                key;
            return current.count(
                {remoteAddress, remotePort, localAddress, localPort});
          });
      if (replaced(baseline) && replaced(additionallyReset) &&
          current.size() >= 2 && newPair) {
        return true;
      }
      std::unique_lock<std::mutex> lock(log_.mutex);
      log_.changed.wait_until(lock, std::min(deadline, Clock::now() + 10ms));
    } while (Clock::now() < deadline);
    return false;
  }

  void finish() {
    if (worker_.joinable()) {
      eventBase_->terminateLoopSoon();
      worker_.join();
    }
    eventBase_.reset();
  }

  EventBase* base() { return eventBase_.get(); }
  DeliveryLog* log() { return &log_; }

 private:
  void doWorker() {
    // after-loop callbacks follow queue rearm; with no remaining work,
    // the next backend poll is blocking
    IdleMarker idle(*eventBase_, log_);
    eventBase_->runAfterLoop(&idle);
    eventBase_->loopForever();
  }

  DeliveryLog log_;
  std::unique_ptr<EventBase> eventBase_{std::make_unique<EventBase>()};
  std::thread worker_;
};

std::vector<int> sequence(int begin, int end) {
  std::vector<int> values;
  for (int value = begin; value < end; ++value) {
    values.push_back(value);
  }
  return values;
}

void runUntil(EventBase& base, const std::function<bool()>& completed) {
  bool expired = false;
  auto timeout = AsyncTimeout::make(base, [&]() noexcept {
    expired = true;
    base.terminateLoopSoon();
  });
  require(timeout->scheduleTimeout(kWait), "completion timeout scheduling");
  require(base.loop(), "EventBase::loop failed");
  require(
      !expired && completed(), "queue delivery did not complete within 10s");
}

void runFor(EventBase& base, std::chrono::milliseconds duration) {
  bool expired = false;
  auto timeout = AsyncTimeout::make(base, [&]() noexcept { expired = true; });
  require(timeout->scheduleTimeout(duration), "observation timeout scheduling");
  require(base.loop(), "EventBase::loop failed during detach observation");
  require(expired, "detach observation returned before its timeout");
}

TEST(WindowsNotificationQueueRecoveryTest, IdleSinglePostRepairsFollyNotifier) {
  IdleEventBase loop;
  ASSERT_TRUE(loop.ready());
  TcpKeys baseline;
  ASSERT_NO_THROW(baseline = resetTestProcessConnections());
  // No manual execute/drain, polling EventBase, or second post to rescue this
  // task: only the production queue's event/watchdog can deliver it.
  loop.post(0, 0);
  ASSERT_TRUE(loop.waitFor(0, 1));
  ASSERT_TRUE(loop.waitForReplacement(baseline));
  loop.post(0, 1);
  ASSERT_TRUE(loop.waitFor(0, 2));
  loop.finish();
  EXPECT_EQ(loop.log()->messages(0), sequence(0, 2));
}

TEST(
    WindowsNotificationQueueRecoveryTest, RepeatedResetsPreserveProducerOrder) {
  IdleEventBase loop;
  ASSERT_TRUE(loop.ready());
  constexpr int kRounds = 3;
  constexpr int kPerRound = 32;

  for (int round = 0; round < kRounds; ++round) {
    SCOPED_TRACE(round);
    TcpKeys baseline;
    ASSERT_NO_THROW(baseline = resetTestProcessConnections());
    std::vector<std::thread> posters;
    for (size_t producer = 0; producer < kProducers; ++producer) {
      posters.emplace_back(
          [base = loop.base(), log = loop.log(), producer, round] {
            for (int offset = 0; offset < kPerRound; ++offset) {
              const auto value = round * kPerRound + offset;
              base->runInEventBaseThreadAlwaysEnqueue([log, producer, value] {
                log->record(producer, value);
              });
            }
          });
    }
    // A second reset races active producers/replacement. The first reset is
    // deterministic even if a fast poster wins this particular race.
    TcpKeys additionallyReset;
    EXPECT_NO_THROW(additionallyReset = resetTestProcessConnections(false));
    for (auto& poster : posters) {
      poster.join();
    }
    for (size_t producer = 0; producer < kProducers; ++producer) {
      ASSERT_TRUE(loop.waitFor(producer, (round + 1) * kPerRound));
    }
    ASSERT_TRUE(loop.waitForReplacement(baseline, additionallyReset));
  }
  loop.finish();
  for (size_t producer = 0; producer < kProducers; ++producer) {
    EXPECT_EQ(loop.log()->messages(producer), sequence(0, kRounds * kPerRound));
  }
}

TEST(WindowsNotificationQueueRecoveryTest, DetachRetainsDataAndCancelsOldBase) {
  std::vector<int> received;
  size_t detachedSize = 0;
  std::vector<int> afterDetachedLoop;
  std::vector<int> afterOldLoop;
  bool wrongBase = false;
  {
    EventBase oldBase;
    EventBase newBase;
    EventBase* activeBase = &oldBase;
    size_t target = 1;
    auto consumer = [&](int&& value) noexcept {
      received.push_back(value);
      if (!activeBase || !activeBase->inRunningEventBaseThread()) {
        wrongBase = true;
      } else if (received.size() >= target) {
        activeBase->terminateLoopSoon();
      }
    };
    EventBaseAtomicNotificationQueue<int, decltype(consumer)> queue(
        std::move(consumer));
    queue.startConsuming(&oldBase);
    queue.putMessage(0);
    runUntil(oldBase, [&] { return received.size() == target; });

    resetTestProcessConnections();
    queue.putMessage(1);
    queue.stopConsuming();
    activeBase = nullptr;
    queue.putMessage(2);
    detachedSize = queue.size();
    // Longer than the prototype's maximum repair retry interval.
    runFor(oldBase, 1500ms);
    afterDetachedLoop = received;

    target = 3;
    activeBase = &newBase;
    queue.startConsuming(&newBase);
    runUntil(newBase, [&] { return received.size() == target; });
    target = 4;
    queue.putMessage(3);
    runUntil(newBase, [&] { return received.size() == target; });
    runFor(oldBase, 1500ms);
    afterOldLoop = received;
    queue.stopConsuming();
  }
  EXPECT_EQ(detachedSize, 2);
  EXPECT_EQ(afterDetachedLoop, sequence(0, 1));
  EXPECT_EQ(received, sequence(0, 4));
  EXPECT_EQ(afterOldLoop, sequence(0, 4));
  EXPECT_FALSE(wrongBase);
}

TEST(
    WindowsNotificationQueueRecoveryTest,
    InternalWatchdogDoesNotKeepLoopAlive) {
  EventBase base;
  EXPECT_TRUE(base.loop());
  resetTestProcessConnections();
  // A pending internal recovery must not turn loop() into loopForever().
  EXPECT_TRUE(base.loop());
  EXPECT_TRUE(base.loop());
}

TEST(WindowsNotificationQueueRecoveryTest, DestructionDropsResetSignalDebt) {
  std::vector<int> received;
  size_t pending = 0;
  {
    EventBase base;
    auto consumer = [&](int&& value) noexcept {
      received.push_back(value);
      base.terminateLoopSoon();
    };
    auto queue = std::make_unique<
        EventBaseAtomicNotificationQueue<int, decltype(consumer)>>(
        std::move(consumer));
    queue->startConsuming(&base);
    queue->putMessage(0);
    runUntil(base, [&] { return received.size() == 1; });

    constexpr int kMessages = 16;
    std::vector<std::thread> posters;
    for (size_t producer = 0; producer < kProducers; ++producer) {
      posters.emplace_back([queue = queue.get(), producer] {
        for (int value = 0; value < kMessages; ++value) {
          queue->putMessage(producer * kMessages + value + 1);
        }
      });
    }
    for (auto& poster : posters) {
      poster.join();
    }
    pending = queue->size();
    resetTestProcessConnections();
    // No loop iteration repairs the reset or observes the outstanding token.
    // All producers have joined before either queue or EventBase destruction.
    queue.reset();
  }
  EXPECT_EQ(pending, kProducers * 16);
  EXPECT_EQ(received, sequence(0, 1));
}

} // namespace
} // namespace folly

#endif
