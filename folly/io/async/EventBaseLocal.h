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

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include <folly/Likely.h>
#include <folly/Synchronized.h>
#include <folly/container/F14Set.h>
#include <folly/io/async/EventBase.h>
#include <folly/lang/Thunk.h>

namespace folly {

namespace detail {

class EventBaseLocalBase {
 public:
  friend class folly::EventBase;
  EventBaseLocalBase() = default;
  EventBaseLocalBase(const EventBaseLocalBase&) = delete;
  EventBaseLocalBase& operator=(const EventBaseLocalBase&) = delete;
  ~EventBaseLocalBase();
  void erase(EventBase& evb);

 protected:
  void setVoid(EventBase& evb, void* ptr, void (*dtor)(void*));
  void* getVoid(EventBase& evb);

 private:
  bool tryDeregister(EventBase& evb);

  static std::atomic<std::size_t> keyCounter_;

  const std::size_t key_{keyCounter_++};
  const folly::F14HashToken keyToken_{
      folly::EventBase::LocalStorageMap{}.prehash(key_)};
  folly::Synchronized<folly::F14FastSet<EventBase*>> eventBases_;
};

} // namespace detail

/**
 * A storage abstraction for data that should be tied to an EventBase.
 *
 *   struct Foo { Foo(int a, int b); };
 *   EventBaseLocal<Foo> myFoo;
 *   ...
 *   EventBase evb;
 *   myFoo.emplace(evb, Foo(1, 2));
 *   myFoo.emplace(evb, 1, 2);
 *   Foo* foo = myFoo.get(evb);
 *   myFoo.erase(evb);
 *   Foo& foo = myFoo.try_emplace(evb, 1, 2); // ctor if missing
 *   Foo& foo = myFoo.try_emplace(evb, 1, 2); // noop if present
 *   myFoo.erase(evb);
 *   Foo& foo = myFoo.try_emplace_with(evb, [] { return Foo(3, 4); })
 *
 * The objects will be deleted when the EventBaseLocal or the EventBase is
 * destructed (whichever comes first). All methods must be called from the
 * EventBase thread.
 *
 * The user is responsible for throwing away invalid references/ptrs returned
 * by the get() method after emplace/erase is called.  If shared ownership is
 * needed, use a EventBaseLocal<shared_ptr<...>>.
 */
template <typename T>
class EventBaseLocal : public detail::EventBaseLocalBase {
 private:
  T& store(EventBase& evb, T* const ptr) {
    setVoid(evb, ptr, detail::thunk::ruin<T>);
    return *ptr;
  }

 public:
  EventBaseLocal() = default;

  T* get(EventBase& evb) { return static_cast<T*>(getVoid(evb)); }

  template <typename... A>
  T& emplace(EventBase& evb, A&&... a) {
    return store(evb, new T(static_cast<A&&>(a)...));
  }

  template <typename U, typename... A>
    requires std::is_constructible_v<T, std::initializer_list<U>, A...>
  T& emplace(EventBase& evb, std::initializer_list<U> i, A&&... a) {
    return store(evb, new T(i, static_cast<A&&>(a)...));
  }

  template <typename F>
  T& emplace_with(EventBase& evb, F f) {
    return store(evb, new T(f()));
  }

  template <typename... A>
  T& try_emplace(EventBase& evb, A&&... a) {
    auto const ptr = get(evb);
    return FOLLY_LIKELY(!!ptr) ? *ptr : emplace(evb, static_cast<A&&>(a)...);
  }

  template <typename U, typename... A>
    requires std::is_constructible_v<T, std::initializer_list<U>, A...>
  T& try_emplace(EventBase& evb, std::initializer_list<U> i, A&&... a) {
    auto const ptr = get(evb);
    return FOLLY_LIKELY(!!ptr) ? *ptr : emplace(evb, i, static_cast<A&&>(a)...);
  }

  template <typename F>
  T& try_emplace_with(EventBase& evb, F f) {
    auto const ptr = get(evb);
    return FOLLY_LIKELY(!!ptr) ? *ptr : emplace_with(evb, std::ref(f));
  }
};

} // namespace folly
