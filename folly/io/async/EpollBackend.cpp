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

#include <folly/io/async/Epoll.h> // @manual

#if FOLLY_HAS_EPOLL

#include <signal.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>

#include <array>
#include <utility>

#include <folly/IntrusiveList.h>
#include <folly/MapUtil.h>
#include <folly/String.h>
#include <folly/io/async/EpollBackend.h>

#include <folly/FileUtil.h>

extern "C" FOLLY_ATTR_WEAK void eb_poll_loop_pre_hook(uint64_t* call_time);
extern "C" FOLLY_ATTR_WEAK void eb_poll_loop_post_hook(
    uint64_t call_time, int ret);

namespace folly {
namespace {

// `epoll_event.data` of the entries that are not an Event's fd. As fds they
// would be -1 and -2.
constexpr uint64_t kTimerData = ~uint64_t{0};
constexpr uint64_t kSignalData = ~uint64_t{0} - 1;

// `epoll_event.data` of an Event's fd: a stale entry, one that outlived the
// fd's close through a dup of it, carries a generation the fd no longer has.
uint64_t entryData(int fd, uint32_t generation) {
  return (uint64_t{generation} << 32) | static_cast<uint32_t>(fd);
}

// The directions an fd entry holds an Event for, in FdEntry::owners order.
constexpr std::array<short, 2> kDirections{{EV_READ, EV_WRITE}};
constexpr std::array<uint32_t, 2> kDirectionFlags{{EPOLLIN, EPOLLOUT}};

// The fd was closed. Its entry went with it, unless a dup keeps the file open,
// and then no fd can reach the entry.
bool isGone(int err) {
  return err == ENOENT || err == EBADF;
}

struct SignalRegistry {
  struct SigInfo {
    struct sigaction sa_{};
    size_t refs_{0};
  };
  using SignalMap = std::map<int, SigInfo>;

  SignalRegistry() {}

  void notify(int sig);
  void setNotifyFd(int sig, int fd);

  // lock protecting the signal map
  // we use a spinlock because we need
  // it to async-signal-safe
  folly::MicroSpinLock mapLock_ = {0};
  SignalMap map_;
  std::atomic<int> notifyFd_{-1};
};

SignalRegistry& getSignalRegistry() {
  static auto& sInstance = *new SignalRegistry();
  return sInstance;
}

void evSigHandler(int sig) {
  getSignalRegistry().notify(sig);
}

void SignalRegistry::notify(int sig) {
  // use try_lock in case somebody already has the lock
  std::unique_lock lk(mapLock_, std::try_to_lock);
  if (!lk.owns_lock()) {
    return;
  }

  int fd = notifyFd_.load();
  if (fd >= 0) {
    uint8_t sigNum = static_cast<uint8_t>(sig);
    fileops::write(fd, &sigNum, 1);
  }
}

void SignalRegistry::setNotifyFd(int sig, int fd) {
  std::lock_guard g(mapLock_);
  if (fd >= 0) {
    // switch the fd
    notifyFd_.store(fd);

    auto& entry = map_[sig];

    if (entry.refs_++ == 0) {
      struct sigaction sa = {};
      sa.sa_handler = evSigHandler;
      sa.sa_flags |= SA_RESTART;
      ::sigfillset(&sa.sa_mask);

      if (::sigaction(sig, &sa, &entry.sa_) == -1) {
        map_.erase(sig);
      }
    }
  } else {
    notifyFd_.store(fd);
    auto iter = map_.find(sig);
    if ((iter != map_.end()) && (--iter->second.refs_ == 0)) {
      auto entry = iter->second;
      map_.erase(iter);
      // just restore
      ::sigaction(sig, &entry.sa_, nullptr);
    }
  }
}

} // namespace

struct EpollBackend::EventInfo {
  static void freeFunction(void* v) { delete static_cast<EventInfo*>(v); }

  void resetEvent() {
    listHook.unlink(); // Remove from the info list.
    ev = nullptr;
    res = 0;
  }

  folly::IntrusiveListHook listHook;
  EventBaseBackendBase::Event* ev{nullptr};
  // The directions that fired in the batch being collected.
  short res{0};
};

struct EpollBackend::TimerInfo : public IntrusiveHeapNode<> {
  bool operator<(const TimerInfo& other) const {
    // IntrusiveHeap is a max-heap.
    return expiration > other.expiration;
  }

  static void freeFunction(void* v) { delete static_cast<TimerInfo*>(v); }

  ~TimerInfo() { DCHECK(!isLinked()); }

  std::chrono::steady_clock::time_point expiration;
  Event* ev;
};

EpollBackend::SocketPair::SocketPair() {
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds_.data())) {
    throw std::runtime_error(folly::errnoStr(errno));
  }

  // set the sockets to non blocking mode
  for (auto fd : fds_) {
    auto flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

EpollBackend::SocketPair::~SocketPair() {
  for (auto fd : fds_) {
    if (fd >= 0) {
      fileops::close(fd);
    }
  }
}

EpollBackend::EpollBackend(Options options) : options_(options) {
  epollFd_ = ::epoll_create1(EPOLL_CLOEXEC);

  if (epollFd_ == -1) {
    throw std::runtime_error(folly::errnoStr(errno));
  }

  {
    timerFd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerFd_ == -1) {
      auto errnoCopy = errno;
      fileops::close(epollFd_);
      throw std::runtime_error(folly::errnoStr(errnoCopy));
    }
    struct epoll_event epev = {};
    epev.events = EPOLLIN;
    epev.data.u64 = kTimerData;
    PCHECK(::epoll_ctl(epollFd_, EPOLL_CTL_ADD, timerFd_, &epev) == 0);
  }

  {
    struct epoll_event epev = {};
    epev.events = EPOLLIN;
    epev.data.u64 = kSignalData;
    PCHECK(
        ::epoll_ctl(epollFd_, EPOLL_CTL_ADD, signalFds_.readFd(), &epev) == 0);
  }

  events_.resize(options_.numLoopEvents);
}

EpollBackend::~EpollBackend() {
  fileops::close(epollFd_);
  fileops::close(timerFd_);
}

int EpollBackend::eb_event_base_loop(int flags) {
  const bool waitForEvents = (flags & EVLOOP_NONBLOCK) == 0;
  while (true) {
    if (loopBreak_) {
      loopBreak_ = false;
      return 0;
    }

    if (numInternalEvents_ == numInsertedEvents_ && timers_.empty() &&
        signals_.empty()) {
      return 1;
    }

    uint64_t call_time = 0;
    if (eb_poll_loop_pre_hook) {
      eb_poll_loop_pre_hook(&call_time);
    }
    if (pollLoopHook_.preLoopHook) {
      pollLoopHook_.preLoopHook(pollLoopHook_.hookCtx);
    }

    int numEvents;
    do {
      numEvents = ::epoll_wait(
          epollFd_, events_.data(), events_.size(), waitForEvents ? -1 : 0);
    } while (numEvents == -1 && errno == EINTR);

    if (eb_poll_loop_post_hook) {
      eb_poll_loop_post_hook(call_time, numEvents);
    }
    if (pollLoopHook_.postLoopHook) {
      pollLoopHook_.postLoopHook(pollLoopHook_.hookCtx, numEvents);
    }

    if (numEvents < 0) {
      return -1;
    } else if (numEvents == 0) {
      CHECK(!waitForEvents);
      return 2;
    }

    bool shouldProcessTimers = false;
    bool shouldProcessSignals = false;
    // Callbacks may delete other active events, so we accumulate active events
    // first into an intrusive list that is updated if events in it are deleted.
    IntrusiveList<EventInfo, &EventInfo::listHook> infoList;
    for (int i = 0; i < numEvents; ++i) {
      uint64_t data = events_[i].data.u64;
      if (data == kTimerData) {
        shouldProcessTimers = true;
        continue;
      } else if (data == kSignalData) {
        shouldProcessSignals = true;
        continue;
      }

      int fd = static_cast<int>(static_cast<uint32_t>(data));
      auto it = fds_.find(fd);
      // The kernel cannot be told to drop a stale entry, so it is skipped for
      // as long as the dup keeps it alive.
      if (it == fds_.end() ||
          it->second.generation != static_cast<uint32_t>(data >> 32)) {
        continue;
      }
      uint32_t what = events_[i].events;
      // A hang-up or an error belongs to the fd, so it wakes every direction
      // its Events asked for.
      bool hangUp = (what & (EPOLLHUP | EPOLLERR)) != 0;
      // Copied: releasing a one-shot Event below changes the entry.
      auto owners = it->second.owners;
      for (size_t d = 0; d < kDirections.size(); ++d) {
        auto* info = owners[d];
        if (info == nullptr || (!hangUp && (what & kDirectionFlags[d]) == 0)) {
          continue;
        }
        auto* event = info->ev;
        info->res |= hangUp
            ? (event->eb_ev_events() & (EV_READ | EV_WRITE))
            : kDirections[d];
        // A READ|WRITE Event already collected through its other direction.
        if (info->listHook.is_linked()) {
          continue;
        }
        // if not persistent we need to remove it
        if (~event->eb_ev_events() & EV_PERSIST) {
          if (event->eb_registration() == Event::Registration::kInserted) {
            event->eb_set_registration(Event::Registration::kNone);

            DCHECK_GT(numInsertedEvents_, 0);
            numInsertedEvents_--;

            if (event->eb_internal()) {
              DCHECK_GT(numInternalEvents_, 0);
              numInternalEvents_--;
            }

            PCHECK(releaseFd(fd, info) == 0);
          }
        }

        event->eb_set_active(true);
        infoList.push_back(*info);
      }
    }

    // Process timers and signals first.
    if (shouldProcessTimers) {
      processTimers();
    }
    if (shouldProcessSignals) {
      processSignals();
    }

    while (!infoList.empty()) {
      auto* info = &infoList.front();
      infoList.pop_front();

      Event* event = info->ev;
      short res = std::exchange(info->res, 0);

      event->eb_set_active(false);
      event->eb_set_ev_res(res);
      if (res != 0) {
        event->eb_ev_invoke_callback();
      }
    }

    if (flags & EVLOOP_ONCE) {
      return 0;
    }
  }
}

int EpollBackend::eb_event_base_loopbreak() {
  loopBreak_ = true;
  return 0;
}

int EpollBackend::eb_event_add(Event& event, const struct timeval* timeout) {
  // we do not support read/write timeouts
  if (timeout) {
    event.eb_set_registration(Event::Registration::kTimeout);
    addTimerEvent(event, timeout);
    return 0;
  }

  if (event.eb_ev_events() & EV_SIGNAL) {
    event.eb_set_registration(Event::Registration::kInserted);
    addSignalEvent(event);
    return 0;
  }

  // Adding an Event that is already registered changes nothing.
  if (event.eb_registration() == Event::Registration::kInserted) {
    return 0;
  }

  short dirs = event.eb_ev_events() & (EV_READ | EV_WRITE);
  // An Event with neither READ nor WRITE takes no entry.
  if (dirs == 0) {
    return 0;
  }

  EventInfo* info = static_cast<EventInfo*>(event.getUserData());
  if (!info) {
    info = new EventInfo();
    event.setUserData(info, EventInfo::freeFunction);
  }

  int fd = event.eb_ev_fd();
  bool shared = false;
  bool conflict = false;
  bool edge = false;
  if (auto it = fds_.find(fd); it != fds_.end()) {
    for (size_t d = 0; d < kDirections.size(); ++d) {
      auto* owner = it->second.owners[d];
      if (owner != nullptr && owner != info) {
        shared = true;
        conflict = conflict || (dirs & kDirections[d]) != 0;
      }
    }
    edge = it->second.edge;
  }

  // A refused Event stays unregistered, so that the eb_event_del a caller
  // cleans up with does not take down the registration it collided with.
  bool join = shared && !conflict && !edge;
  if ((join ? joinFd(fd, info, dirs) : holdFdAlone(fd, info, dirs)) != 0) {
    // EPOLLET would apply to the other Event too.
    if (errno == EEXIST && shared && !conflict) {
      errno = EBUSY;
    }
    return -1;
  }
  info->ev = &event;
  event.eb_set_registration(Event::Registration::kInserted);
  numInsertedEvents_++;
  if (event.eb_internal()) {
    numInternalEvents_++;
  }
  return 0;
}

int EpollBackend::eb_event_del(Event& event) {
  if (!event.eb_ev_base()) {
    errno = EINVAL;
    return -1;
  }

  if (event.eb_registration() == Event::Registration::kTimeout) {
    event.eb_set_registration(Event::Registration::kNone);
    return removeTimerEvent(event);
  }

  if (!event.eb_active() &&
      event.eb_registration() != Event::Registration::kInserted) {
    errno = EINVAL;
    return -1;
  }

  if (event.eb_ev_events() & EV_SIGNAL) {
    event.eb_clear_registration();
    return removeSignalEvent(event);
  }

  auto* info = static_cast<EventInfo*>(event.getUserData());
  if (info) {
    info->resetEvent();
  }

  event.eb_set_active(false);

  if (event.eb_registration() == Event::Registration::kInserted) {
    event.eb_set_registration(Event::Registration::kNone);

    DCHECK_GT(numInsertedEvents_, 0);
    numInsertedEvents_--;

    if (event.eb_internal()) {
      DCHECK_GT(numInternalEvents_, 0);
      numInternalEvents_--;
    }

    return releaseFd(event.eb_ev_fd(), info);
  }

  errno = EINVAL;
  return -1;
}

uint32_t EpollBackend::interest(const FdEntry& entry) {
  uint32_t events = 0;
  for (size_t d = 0; d < kDirections.size(); ++d) {
    if (entry.owners[d] != nullptr) {
      events |= kDirectionFlags[d];
    }
  }
  if (events != 0 && entry.edge) {
    events |= EPOLLET;
  }
  return events;
}

int EpollBackend::holdFdAlone(int fd, EventInfo* info, short dirs) {
  FdEntry entry;
  entry.generation = ++nextGeneration_;
  for (size_t d = 0; d < kDirections.size(); ++d) {
    if (dirs & kDirections[d]) {
      entry.owners[d] = info;
    }
  }
  struct epoll_event epev = {};
  epev.events = interest(entry);
  epev.data.u64 = entryData(fd, entry.generation);
  if (::epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &epev) != 0) {
    return -1;
  }
  // The kernel had no entry for this fd's file, so any Event that held the
  // number before had its fd closed under it; it gives way.
  fds_[fd] = entry;
  return 0;
}

int EpollBackend::joinFd(int fd, EventInfo* info, short dirs) {
  FdEntry& entry = fds_.at(fd);
  FdEntry joined = entry;
  for (size_t d = 0; d < kDirections.size(); ++d) {
    if (dirs & kDirections[d]) {
      joined.owners[d] = info;
    }
  }
  struct epoll_event epev = {};
  epev.events = interest(joined);
  epev.data.u64 = entryData(fd, entry.generation);
  if (::epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &epev) == 0) {
    entry = joined;
    return 0;
  }
  if (!isGone(errno)) {
    return -1;
  }
  // The other Event's fd was closed under it.
  return holdFdAlone(fd, info, dirs);
}

int EpollBackend::releaseFd(int fd, EventInfo* info) {
  auto it = fds_.find(fd);
  if (it == fds_.end()) {
    return 0;
  }
  FdEntry& entry = it->second;
  bool held = false;
  for (auto& owner : entry.owners) {
    if (owner == info) {
      owner = nullptr;
      held = true;
    }
  }
  // Another Event took the number over after this one's fd was closed.
  if (!held) {
    return 0;
  }
  int rc = 0;
  if (uint32_t events = interest(entry); events == 0) {
    fds_.erase(it);
    rc = ::epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
  } else {
    struct epoll_event epev = {};
    epev.events = events;
    epev.data.u64 = entryData(fd, entry.generation);
    rc = ::epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &epev);
  }
  if (rc != 0 && !isGone(errno)) {
    return -1;
  }
  return 0;
}

bool EpollBackend::setEdgeTriggered(Event& event) {
  EventInfo* info = static_cast<EventInfo*>(event.getUserData());
  if (info == nullptr) {
    return false;
  }
  auto it = fds_.find(event.eb_ev_fd());
  if (it == fds_.end()) {
    return false;
  }
  FdEntry& entry = it->second;
  bool held = false;
  for (auto* owner : entry.owners) {
    // EPOLLET would apply to the other Event too.
    if (owner != nullptr && owner != info) {
      return false;
    }
    held = held || owner == info;
  }
  if (!held) {
    return false;
  }
  if (entry.edge) {
    return true;
  }

  entry.edge = true;
  struct epoll_event epev = {};
  epev.events = interest(entry);
  epev.data.u64 = entryData(event.eb_ev_fd(), entry.generation);
  if (::epoll_ctl(epollFd_, EPOLL_CTL_MOD, event.eb_ev_fd(), &epev) != 0) {
    entry.edge = false;
    return false;
  }
  return true;
}

void EpollBackend::addTimerEvent(Event& event, const struct timeval* timeout) {
  TimerInfo* info = static_cast<TimerInfo*>(event.getUserData());
  if (info == nullptr) {
    info = new TimerInfo;
    info->ev = &event;
    event.setUserData(info, TimerInfo::freeFunction);
  }

  info->expiration = std::chrono::steady_clock::now() +
      std::chrono::seconds{timeout->tv_sec} +
      std::chrono::microseconds{timeout->tv_usec};
  if (info->isLinked()) {
    timers_.update(info);
  } else {
    timers_.push(info);
  }

  updateTimerFd();
}

int EpollBackend::removeTimerEvent(Event& event) {
  auto* info = static_cast<TimerInfo*>(event.getUserData());
  if (info == nullptr || !info->isLinked()) {
    errno = EINVAL;
    return -1;
  }

  DCHECK_EQ(event.getFreeFunction(), TimerInfo::freeFunction);
  timers_.erase(info);
  updateTimerFd();
  return 0;
}

void EpollBackend::updateTimerFd() {
  std::optional<std::chrono::steady_clock::time_point> expiration;
  if (auto* earliest = timers_.top()) {
    expiration = earliest->expiration;
  }
  if (expiration == timerFdExpiration_) {
    return; // Nothing to do.
  }

  if (!expiration) {
    struct itimerspec val = {}; // Disable.
    PCHECK(::timerfd_settime(timerFd_, 0, &val, nullptr) == 0);
  } else {
    auto delta = std::chrono::duration_cast<std::chrono::microseconds>(
        *expiration - std::chrono::steady_clock::now());
    if (delta < std::chrono::microseconds(1000)) {
      delta = std::chrono::microseconds(1000);
    }

    struct itimerspec val;
    val.it_interval = {0, 0};
    val.it_value.tv_sec =
        std::chrono::duration_cast<std::chrono::seconds>(delta).count();
    val.it_value.tv_nsec =
        std::chrono::duration_cast<std::chrono::nanoseconds>(delta).count() %
        1'000'000'000LL;

    PCHECK(::timerfd_settime(timerFd_, 0, &val, nullptr) == 0);
  }

  timerFdExpiration_ = expiration;
}

void EpollBackend::processTimers() {
  // Consume the event.
  uint64_t data = 0;
  PCHECK(folly::readNoInt(timerFd_, &data, sizeof(data)) == sizeof(data));

  while (!timers_.empty() &&
         timers_.top()->expiration <= std::chrono::steady_clock::now()) {
    auto* info = timers_.pop();
    auto* ev = info->ev;
    ev->eb_set_ev_res(EV_TIMEOUT);
    ev->eb_clear_registration();
    // NOTE: The callback might change the set of registered timers.
    ev->eb_ev_invoke_callback();
  }

  updateTimerFd();
}

void EpollBackend::addSignalEvent(Event& event) {
  signals_[event.eb_ev_fd()].insert(&event);

  // we pass the write fd for notifications
  getSignalRegistry().setNotifyFd(event.eb_ev_fd(), signalFds_.writeFd());
}

int EpollBackend::removeSignalEvent(Event& event) {
  auto* set = get_ptr(signals_, event.eb_ev_fd());
  if (set == nullptr || set->erase(&event) == 0) {
    errno = EINVAL;
    return -1;
  }
  getSignalRegistry().setNotifyFd(event.eb_ev_fd(), -1);
  return 0;
}

void EpollBackend::processSignals() {
  static constexpr auto kNumEntries = NSIG * 2;
  static_assert(
      NSIG < std::numeric_limits<uint8_t>::max(),
      "Use a different data type to cover all the signal values");
  std::array<bool, NSIG> processed{};
  std::array<uint8_t, kNumEntries> signals;

  ssize_t num =
      folly::readNoInt(signalFds_.readFd(), signals.data(), signals.size());
  for (ssize_t i = 0; i < num; i++) {
    int signum = static_cast<int>(signals[i]);
    if (signum < 0 || signum >= NSIG || processed[signum]) {
      continue;
    }
    processed[signum] = true;
    auto* events = get_ptr(signals_, signum);
    if (events == nullptr) {
      continue;
    }
    for (auto* ev : *events) {
      ev->eb_set_ev_res(0);
      ev->eb_set_active(true);
      ev->eb_ev_invoke_callback();
      ev->eb_set_active(false);
    }
  }
}

} // namespace folly
#endif
