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

#include <folly/channels/ConsumeChannel.h>
#include <folly/coro/DetachOnCancel.h>
#include <folly/coro/Sleep.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/executors/SequencedExecutor.h>
#include <folly/futures/SharedPromise.h>
#include <folly/portability/GMock.h>

namespace folly {
namespace channels {

template <typename T, typename... Others>
std::vector<T> toVector(T firstItem, Others... items) {
  std::vector<T> itemsVector;
  itemsVector.push_back(std::move(firstItem));
  (void(itemsVector.push_back(std::move(items))), ...);
  return itemsVector;
}

template <typename Key, typename Mapped, typename... Others>
folly::F14FastMap<std::remove_const_t<Key>, Mapped> toMap(
    std::pair<Key, Mapped> firstPair, Others... items) {
  folly::F14FastMap<std::remove_const_t<Key>, Mapped> itemsMap;
  itemsMap.insert(std::move(firstPair));
  (void(itemsMap.insert(std::move(items))), ...);
  return itemsMap;
}

template <typename TValue>
class MockNextCallback {
 public:
  void operator()(Try<TValue> result) {
    if (result.hasValue()) {
      onValue(result.value());
    } else if (result.template hasException<folly::OperationCancelled>()) {
      onCancelled();
    } else if (result.template hasException<std::runtime_error>()) {
      onRuntimeError(result.exception().what().toStdString());
    } else if (result.hasException()) {
      LOG(FATAL) << "Unexpected exception: " << result.exception().what();
    } else {
      onClosed();
    }
  }

  MOCK_METHOD(void, onValue, (TValue));
  MOCK_METHOD(void, onClosed, ());
  MOCK_METHOD(void, onCancelled, ());
  MOCK_METHOD(void, onRuntimeError, (std::string));
};

enum class ConsumptionMode {
  CoroWithTry,
  CoroWithoutTry,
  CallbackWithHandle,
};

template <typename TValue>
class ChannelConsumerBase {
 public:
  explicit ChannelConsumerBase(ConsumptionMode mode) : mode_(mode) {
    continueConsuming_.setValue(true);
  }

  ChannelConsumerBase(ChannelConsumerBase&&) = default;
  ChannelConsumerBase& operator=(ChannelConsumerBase&&) = default;

  virtual ~ChannelConsumerBase() = default;

  virtual folly::Executor::KeepAlive<folly::SequencedExecutor>
  getExecutor() = 0;

  virtual void onNext(Try<TValue> result) = 0;

  void startConsuming(Receiver<TValue> receiver) {
    co_withExecutor(
        getExecutor(),
        folly::coro::co_withCancellation(
            cancellationSource_.getToken(),
            processValuesCoro(std::move(receiver))))
        .start();
  }

  folly::coro::Task<void> processValuesCoro(Receiver<TValue> receiver) {
    if (mode_ == ConsumptionMode::CoroWithTry ||
        mode_ == ConsumptionMode::CoroWithoutTry) {
      do {
        Try<TValue> resultTry;
        if (mode_ == ConsumptionMode::CoroWithTry) {
          resultTry = co_await folly::coro::co_awaitTry(receiver.next());
        } else if (mode_ == ConsumptionMode::CoroWithoutTry) {
          try {
            auto result = co_await receiver.next();
            if (result.has_value()) {
              resultTry = Try<TValue>(result.value());
            } else {
              resultTry = Try<TValue>();
            }
          } catch (...) {
            resultTry = Try<TValue>(exception_wrapper(current_exception()));
          }
        } else {
          LOG(FATAL) << "Unknown consumption mode";
        }
        bool hasValue = resultTry.hasValue();
        onNext(std::move(resultTry));
        if (!hasValue) {
          co_return;
        }
      } while (co_await folly::coro::detachOnCancel(
          continueConsuming_.getSemiFuture()));
    } else if (mode_ == ConsumptionMode::CallbackWithHandle) {
      auto callbackHandle = consumeChannelWithCallback(
          std::move(receiver),
          getExecutor(),
          [=, this](Try<TValue> resultTry) -> folly::coro::Task<bool> {
            onNext(std::move(resultTry));
            co_return co_await folly::coro::detachOnCancel(
                continueConsuming_.getSemiFuture());
          });
      cancelCallback_ = std::make_unique<folly::CancellationCallback>(
          co_await folly::coro::co_current_cancellation_token,
          [=, handle = std::move(callbackHandle)]() mutable {
            handle.reset();
          });
    } else {
      LOG(FATAL) << "Unknown consumption mode";
    }
  }

 protected:
  ConsumptionMode mode_;
  folly::CancellationSource cancellationSource_;
  folly::SharedPromise<bool> continueConsuming_;
  std::unique_ptr<folly::CancellationCallback> cancelCallback_;
};

enum class CloseType { NoException, Exception, Cancelled };

template <typename TValue>
class StressTestConsumer : public ChannelConsumerBase<TValue> {
 public:
  StressTestConsumer(
      ConsumptionMode mode, folly::Function<void(TValue)> onValue)
      : ChannelConsumerBase<TValue>(mode),
        executor_(std::make_unique<folly::IOThreadPoolExecutor>(1)),
        onValue_(std::move(onValue)) {}

  StressTestConsumer(StressTestConsumer&&) = delete;
  StressTestConsumer&& operator=(StressTestConsumer&&) = delete;

  ~StressTestConsumer() override {
    this->cancellationSource_.requestCancellation();
    if (!this->continueConsuming_.isFulfilled()) {
      this->continueConsuming_.setValue(false);
    }
    executor_.reset();
  }

  folly::Executor::KeepAlive<folly::SequencedExecutor> getExecutor() override {
    return executor_->getEventBase();
  }

  void onNext(Try<TValue> result) override {
    if (result.hasValue()) {
      onValue_(std::move(result.value()));
    } else if (result.template hasException<folly::OperationCancelled>()) {
      closedType_.setValue(CloseType::Cancelled);
    } else if (result.hasException()) {
      EXPECT_TRUE(result.template hasException<std::runtime_error>());
      closedType_.setValue(CloseType::Exception);
    } else {
      closedType_.setValue(CloseType::NoException);
    }
  }

  void cancel() { this->cancellationSource_.requestCancellation(); }

  folly::SemiFuture<CloseType> waitForClose() {
    return closedType_.getSemiFuture();
  }

 private:
  std::unique_ptr<folly::IOThreadPoolExecutor> executor_;
  folly::Function<void(TValue)> onValue_;
  folly::Promise<CloseType> closedType_;
};

template <typename TValue>
class StressTestProducer {
 public:
  explicit StressTestProducer(folly::Function<TValue()> getNextValue)
      : executor_(std::make_unique<folly::CPUThreadPoolExecutor>(1)),
        getNextValue_(std::move(getNextValue)) {}

  StressTestProducer(StressTestProducer&&) = delete;
  StressTestProducer&& operator=(StressTestProducer&&) = delete;

  ~StressTestProducer() {
    if (executor_) {
      stopProducing();
      executor_.reset();
    }
  }

  void startProducing(
      Sender<TValue> sender, std::optional<exception_wrapper> closeException) {
    auto produceTask = folly::coro::co_invoke(
        [=,
         this,
         sender = std::move(sender),
         ex = std::move(closeException)]() mutable -> folly::coro::Task<void> {
          for (int i = 1; !stopped_.load(std::memory_order_relaxed); i++) {
            if (i % 1000 == 0) {
              co_await folly::coro::sleep(std::chrono::milliseconds(100));
            }
            sender.write(getNextValue_());
          }
          if (ex.has_value()) {
            std::move(sender).close(std::move(ex.value()));
          } else {
            std::move(sender).close();
          }
          co_return;
        });
    co_withExecutor(executor_.get(), std::move(produceTask)).start();
  }

  void stopProducing() { stopped_.store(true); }

 private:
  std::unique_ptr<folly::CPUThreadPoolExecutor> executor_;
  folly::Function<TValue()> getNextValue_;
  std::atomic<bool> stopped_{false};
};
} // namespace channels
} // namespace folly
