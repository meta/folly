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
#include <cstdint>
#include <type_traits>
#include <utility>

#include <glog/logging.h>

struct counted_shared_tag {};
template <template <typename> class Atom = std::atomic>
struct intrusive_shared_count {
  using Dispose = void(intrusive_shared_count*);

  explicit intrusive_shared_count(Dispose* dispose) : dispose_(dispose) {
    counts.store(0);
  }
  void add_ref(uint64_t count = 1) { counts.fetch_add(count); }
  uint64_t release_ref(uint64_t count = 1) { return counts.fetch_sub(count); }
  void release(uint64_t count = 1) {
    if (release_ref(count) == count) {
      dispose_(this);
    }
  }
  Atom<uint64_t> counts;
  // Owned object address for the control-block-only reconstruction hooks.
  void* object_ = nullptr;

 private:
  Dispose* dispose_;
};

// Shared ownership and stored pointers are independent, as in shared_ptr.
// Owned objects are created by make_counted; aliasing constructors may point
// elsewhere, including nullptr or a pointer with no owner.
template <typename T, template <typename> class Atom = std::atomic>
class counted_ptr {
 public:
  using element_type = std::remove_extent_t<T>;
  using counted_base = intrusive_shared_count<Atom>;

  counted_base* owner_ = nullptr;
  element_type* p_ = nullptr;

  counted_ptr() = default;
  explicit counted_ptr(element_type* p) : p_(p) { CHECK(!p); }
  counted_ptr(
      counted_shared_tag, counted_base* owner, element_type* p, bool inc = true)
      : owner_(owner), p_(p) {
    if (owner_ && inc) {
      owner_->add_ref();
    }
  }
  counted_ptr(const counted_ptr& other)
      : counted_ptr(counted_shared_tag{}, other.owner_, other.p_) {}
  counted_ptr(counted_ptr&& other) noexcept
      : owner_(std::exchange(other.owner_, nullptr)),
        p_(std::exchange(other.p_, nullptr)) {}
  template <typename Y>
  counted_ptr(const counted_ptr<Y, Atom>& owner, element_type* p)
      : counted_ptr(counted_shared_tag{}, owner.owner_, p) {}
  template <typename Y>
  counted_ptr(counted_ptr<Y, Atom>&& owner, element_type* p) noexcept
      : owner_(std::exchange(owner.owner_, nullptr)), p_(p) {
    owner.p_ = nullptr;
  }
  counted_ptr& operator=(counted_ptr other) noexcept {
    swap(other);
    return *this;
  }
  ~counted_ptr() {
    if (owner_) {
      owner_->release();
    }
  }
  void swap(counted_ptr& other) noexcept {
    std::swap(owner_, other.owner_);
    std::swap(p_, other.p_);
  }
  void reset() noexcept { counted_ptr{}.swap(*this); }
  typename std::add_lvalue_reference<T>::type operator*() const { return *p_; }
  element_type* get() const { return p_; }
  element_type* operator->() const { return p_; }
  long use_count() const { return owner_ ? owner_->counts.load() : 0; }
  explicit operator bool() const { return p_ != nullptr; }
  bool operator==(const counted_ptr& other) const {
    return get() == other.get();
  }
};

template <typename T, template <typename> class Atom>
struct counted_storage : intrusive_shared_count<Atom> {
  template <typename... Args>
  explicit counted_storage(Args&&... args)
      : intrusive_shared_count<Atom>(&dispose),
        value(std::forward<Args>(args)...) {
    this->object_ = &value;
  }
  static void dispose(intrusive_shared_count<Atom>* base) {
    delete static_cast<counted_storage*>(base);
  }
  T value;
};

template <
    template <typename> class Atom = std::atomic,
    typename T,
    typename... Args>
counted_ptr<T, Atom> make_counted(Args&&... args) {
  auto* owner = new counted_storage<T, Atom>(std::forward<Args>(args)...);
  return counted_ptr<T, Atom>(counted_shared_tag{}, owner, &owner->value);
}

template <template <typename> class Atom = std::atomic>
class counted_ptr_internals {
 public:
  template <typename T, typename... Args>
  static counted_ptr<T, Atom> make_ptr(Args&&... args) {
    return make_counted<Atom, T>(std::forward<Args>(args)...);
  }
  template <typename T>
  using CountedPtr = counted_ptr<T, Atom>;
  using counted_base = intrusive_shared_count<Atom>;

  template <typename T>
  static counted_base* get_counted_base(const counted_ptr<T, Atom>& p) {
    return p.owner_;
  }
  template <typename T>
  static std::remove_extent_t<T>* release_ptr(counted_ptr<T, Atom>& p) {
    auto* value = std::exchange(p.p_, nullptr);
    p.owner_ = nullptr;
    return value;
  }
  template <typename T>
  static std::remove_extent_t<T>* get_shared_ptr(counted_base* base) {
    return base
        ? static_cast<std::remove_extent_t<T>*>(base->object_)
        : nullptr;
  }
  template <typename T>
  static counted_ptr<T, Atom> get_shared_ptr_from_counted_base(
      counted_base* base, bool inc = true) {
    return counted_ptr<T, Atom>(
        counted_shared_tag{}, base, get_shared_ptr<T>(base), inc);
  }
  static void inc_shared_count(counted_base* base, int64_t count) {
    base->add_ref(count);
  }
  template <typename T>
  static void release_shared(counted_base* base, uint64_t count) {
    base->release(count);
  }
};
