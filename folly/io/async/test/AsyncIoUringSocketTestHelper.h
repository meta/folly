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
#include <chrono>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>
#include <folly/io/async/AsyncIoUringSocket.h>
#include <folly/io/async/AsyncServerSocket.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/IoUringBackend.h>
#include <folly/io/async/IoUringEvent.h>
#include <folly/io/async/test/AsyncSocketTest.h>
#include <folly/io/async/test/AsyncSocketTest2.h>
#include <folly/portability/GTest.h>
#include <folly/portability/Sockets.h>

namespace folly {

namespace {

static constexpr std::chrono::milliseconds kTimeout{30000};
static constexpr size_t kBufferSize{1024};

std::string toString(std::unique_ptr<IOBuf> const& buf) {
  if (!buf) {
    return std::string();
  }
  auto coalesced = buf->coalesce();
  return std::string((char const*)coalesced.data(), coalesced.size());
}

class NullWriteCallback : public AsyncWriter::WriteCallback {
 public:
  void writeSuccess() noexcept override {}

  void writeErr(
      size_t bytesWritten, const AsyncSocketException& ex) noexcept override {
    LOG(FATAL) << "writeErr wrote=" << (int)bytesWritten << " " << ex;
  }
};

static NullWriteCallback nullWriteCallback;

} // namespace

inline std::string randomString(size_t n) {
  std::random_device r;
  std::default_random_engine e1(r());

  std::uniform_int_distribution<int8_t> uniform_dist('A', 'Z');

  std::string ret;
  ret.reserve(n);
  for (size_t i = 0; i < n; i++) {
    ret.push_back(uniform_dist(e1));
  }
  return ret;
}

class EchoTransport
    : public AsyncReader::ReadCallback,
      public AsyncWriter::WriteCallback {
 public:
  explicit EchoTransport(AsyncSocketTransport::UniquePtr s, bool bm)
      : transport(std::move(s)), bufferMovable(bm) {}

  void start() { transport->setReadCB(this); }

  void getReadBuffer(void** bufReturn, size_t* lenReturn) override {
    *bufReturn = buff.data();
    *lenReturn = buff.size();
  }

  void writeSuccess() noexcept override {}

  void writeErr(
      size_t bytesWritten, const AsyncSocketException& ex) noexcept override {
    LOG_EVERY_N(ERROR, 10000) << "writeErr " << bytesWritten << " " << ex;
    transport->close();
  }

  void readEOF() noexcept override {
    VLOG(1) << "Closing transport!";
    transport->close();
  }

  void readErr(const AsyncSocketException& ex) noexcept override {
    LOG(ERROR) << "readErr " << ex;
    transport->close();
  }

  void readDataAvailable(size_t len) noexcept override {
    VLOG(1) << "readDataAvailable " << len;
    // have to copy as buff will be reused after
    transport->writeChain(this, IOBuf::copyBuffer(buff.data(), len));
  }

  bool isBufferMovable() noexcept override { return bufferMovable; }

  void readBufferAvailable(std::unique_ptr<IOBuf> readBuf) noexcept override {
    VLOG(1) << "readBuffer available " << readBuf->computeChainDataLength();
    transport->writeChain(this, std::move(readBuf));
  }

  AsyncSocketTransport::UniquePtr transport;
  bool bufferMovable;
  std::array<char, kBufferSize> buff;
};

class CollectCallback : public AsyncReader::ReadCallback {
 public:
  void getReadBuffer(void** bufReturn, size_t* lenReturn) override {
    *bufReturn = buff.data();
    *lenReturn = buff.size();
  }

  void readEOF() noexcept override { clear(); }

  void readErr(const AsyncSocketException& ex) noexcept override {
    if (promise) {
      promise->second.setException(ex);
    }
    clear();
  }

  void readDataAvailable(size_t len) noexcept override {
    VLOG(1) << "CollectCallback::readDataAvailable " << len;
    if (hadBuffers.has_value() && *hadBuffers) {
      LOG(FATAL) << "must either send buffers or use getReadBuffer";
    }
    hadBuffers = false;
    data += std::string(buff.data(), len);
    dataAvailable();
  }

  SemiFuture<std::string> waitFor(size_t n) {
    auto [p, f] = makePromiseContract<std::string>();
    promise = std::make_pair(n, std::move(p));
    return std::move(f).within(kTimeout);
  }

  void readBufferAvailable(std::unique_ptr<IOBuf> readBuf) noexcept override {
    if (hadBuffers.has_value() && !*hadBuffers) {
      LOG(FATAL) << "must either send buffers or use getReadBuffer";
    }
    hadBuffers = true;

    if (holdData) {
      auto cloned = readBuf->clone();
      if (bufData) {
        bufData->appendToChain(std::move(readBuf));
      } else {
        bufData = std::move(readBuf);
      }
      readBuf = std::move(cloned);
    }

    data += toString(readBuf);
    dataAvailable();
  }

  void clear() {
    data.clear();
    promise.reset();
    bufData.reset();
  }

  void dataAvailable() {
    VLOG(1) << "CollectCallback::dataAvailable have=" << data.size()
            << " want=" << (promise ? static_cast<int>(promise->first) : -1);
    if (!promise || promise->first > data.size()) {
      return;
    }
    promise->second.setValue(data.substr(0, promise->first));
    data.erase(0, promise->first);
    promise.reset();
  }

  bool isBufferMovable() noexcept override { return true; }

  void setHoldData(bool b) { holdData = b; }

  std::optional<std::pair<size_t, Promise<std::string>>> promise;
  std::shared_ptr<AsyncSocket> sock;
  std::string data;
  std::unique_ptr<IOBuf> bufData;
  std::optional<bool> hadBuffers;
  std::array<char, kBufferSize> buff;
  bool holdData = true;
};

struct TestParams {
  bool ioUringServer = false;
  bool ioUringClient = false;
  bool manySmallBuffers = false;
  bool supportBufferMovable = true;
  bool sendzc = false;
  bool registerFd = true;
  std::string testName() const {
    return folly::to<std::string>(
        ioUringServer ? "ioUringServer" : "oldServer",
        "_",
        ioUringClient ? "ioUringClient" : "oldClient",
        "_",
        manySmallBuffers ? "manySmallBuffers" : "oneBigBuffer",
        supportBufferMovable ? "" : "_noSupportBufferMovable",
        sendzc ? "_zerocopy" : "",
        "_",
        registerFd ? "" : "_noRegisterFd",
        "iouringBackend");
  }
};

struct ConnectedOptions {
  std::string fastOpenInitial;
  bool serverShouldRead = true;

  ConnectedOptions withNoServerShouldRead() {
    auto ret = *this;
    ret.serverShouldRead = false;
    return ret;
  }

  ConnectedOptions withFastOpen(std::string i) {
    auto ret = *this;
    ret.fastOpenInitial = std::move(i);
    return ret;
  }
};

class AsyncIoUringSocketTest
    : public ::testing::TestWithParam<TestParams>,
      public AsyncServerSocket::AcceptCallback,
      public AsyncSocket::ConnectCallback {
 public:
  static IoUringBackend::Options ioOptions(TestParams const& p) {
    IoUringBackend::Options options;
    options.setUseRegisteredFds(p.registerFd ? 64 : 0);
    if (p.manySmallBuffers) {
      options.setInitialProvidedBuffers(1024, 2000);
    } else {
      options.setInitialProvidedBuffers(2000000, 1);
    }
    options.setDeferTaskRun(true);
    return options;
  }

  EventBase::Options ioUringEbOptions() {
    return EventBase::Options{}.setBackendFactory(
        [p = GetParam()]() mutable -> std::unique_ptr<EventBaseBackendBase> {
          return std::make_unique<IoUringBackend>(ioOptions(p));
        });
  }

  EventBase::Options ebOptions() { return ioUringEbOptions(); }

  void maybeSkip() {
    if (unableToRun) {
      GTEST_SKIP();
    }
  }

  AsyncIoUringSocketTest() {
    try {
      base = std::make_unique<EventBase>(ebOptions());
    } catch (IoUringBackend::NotAvailable const&) {
      unableToRun = true;
      return;
    }

    backend = dynamic_cast<IoUringBackend*>(base->getBackend());

    backend->loopPoll(); // init delayed bits as this is the only thread

    serverSocket = AsyncServerSocket::newSocket(base.get());
    serverSocket->setTFOEnabled(true, 1);
    serverSocket->bind(0);
    serverSocket->listen(1024);
    serverSocket->addAcceptCallback(this, nullptr);
    serverSocket->startAccepting();
    serverSocket->getAddress(&serverAddress);
  }

  void connectionAccepted(
      NetworkSocket ns, const SocketAddress&, AcceptInfo) noexcept override {
    fdPromise.setValue(ns);
  }

  void connectSuccess() noexcept override {}
  void connectErr(const AsyncSocketException& ex) noexcept override {
    LOG(FATAL) << ex;
  }

  template <typename ServerReadCallback>
  struct Connected {
    std::unique_ptr<EchoTransport> client;
    AsyncTransport::UniquePtr server;
    std::unique_ptr<ServerReadCallback> callback;
    ~Connected() {
      if (server) {
        server->setReadCB(nullptr);
      }
      if (client && client->transport) {
        client->transport->closeNow();
      }
    }
  };

  AsyncIoUringSocket::Options ioUringSocketOptions() const {
    AsyncIoUringSocket::Options ret;
    if (GetParam().sendzc) {
      ret.zeroCopyEnable = [](auto&&) { return true; };
    }
    return ret;
  }

  template <typename ServerReadCallback = CollectCallback>
  Connected<ServerReadCallback> makeConnected(
      ConnectedOptions options = ConnectedOptions{}) {
    AsyncSocketTransport::UniquePtr client;
    if (GetParam().ioUringClient) {
      client = AsyncSocketTransport::UniquePtr(
          new AsyncIoUringSocket(base.get(), ioUringSocketOptions()));
    } else {
      client =
          AsyncSocketTransport::UniquePtr(AsyncSocket::newSocket(base.get()));
    }

    if (options.fastOpenInitial.size()) {
      client->enableTFO();
    }
    client->connect(this, serverAddress);
    if (options.fastOpenInitial.size()) {
      client->writeChain(
          &nullWriteCallback, IOBuf::copyBuffer(options.fastOpenInitial));
    }

    auto fd =
        fdPromise.getFuture()
            .within(kTimeout)
            .via(base.get())
            .getVia(base.get());
    fdPromise = {};
    auto c = std::make_unique<EchoTransport>(
        std::move(client), GetParam().supportBufferMovable);
    c->start();
    auto serverReadCallback = std::make_unique<ServerReadCallback>();
    AsyncTransport::UniquePtr server = GetParam().ioUringServer
        ? AsyncTransport::UniquePtr(new AsyncIoUringSocket(
              AsyncSocket::newSocket(base.get(), fd), ioUringSocketOptions()))
        : AsyncTransport::UniquePtr(AsyncSocket::newSocket(base.get(), fd));
    if (options.serverShouldRead) {
      server->setReadCB(serverReadCallback.get());
    }
    return Connected<ServerReadCallback>{
        std::move(c), std::move(server), std::move(serverReadCallback)};
  }

  bool unableToRun = false;
  std::unique_ptr<EventBase> base;
  std::unique_ptr<IoUringEvent> ioUringEvent;
  std::shared_ptr<AsyncServerSocket> serverSocket;
  IoUringBackend* backend = nullptr;
  SocketAddress serverAddress;

  Promise<NetworkSocket> fdPromise;
};

class AsyncIoUringSocketTestAll : public AsyncIoUringSocketTest {};

#define MAYBE_SKIP()                   \
  if (unableToRun) {                   \
    LOG(INFO) << "Unsupported kernel"; \
    return;                            \
  }

auto mkAllTestParams() {
  std::vector<TestParams> t;

  auto addFeatureCases = [&](TestParams const& base) {
    TestParams all = base;

    // add test cases where each feature is not the default, as well as one
    // where all the features are not the default
    auto add_flip_case = [&](auto ptr) {
      auto tc = base;
      tc.*ptr = all.*ptr = !(tc.*ptr);
      t.push_back(tc);
    };

    add_flip_case(&TestParams::registerFd);
    add_flip_case(&TestParams::sendzc);
    add_flip_case(&TestParams::supportBufferMovable);
    t.push_back(all);
  };

  for (bool server : {false, true}) {
    for (bool client : {false, true}) {
      for (bool manySmallBuffers : {false, true}) {
        TestParams base;
        base.ioUringServer = server;
        base.ioUringClient = client;
        base.manySmallBuffers = manySmallBuffers;
        t.push_back(base);

        // only expand feature flags in some cases to reduce the massive
        // explosion of tests
        if (server && client) {
          addFeatureCases(base);
        }
      }
    }
  }
  return t;
}

} // namespace folly
