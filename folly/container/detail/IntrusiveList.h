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

#include <cstddef>
#include <cstring>
#include <iterator>
#include <type_traits>
#include <utility>

#include <folly/lang/SafeAssert.h>

// An intrusive circular doubly linked list with member hooks. It provides the
// subset of the boost::intrusive::list interface, with the same names and
// semantics, that users of the folly/container/IntrusiveList.h aliases need.

namespace folly::detail {

// A node of a circular list. An unlinked hook has null links, as in Boost, so
// zeroed memory is an unlinked hook. A list head links to itself when the list
// is empty.
struct IntrusiveListNode {
  IntrusiveListNode* next;
  IntrusiveListNode* prev;

  void init() noexcept { next = prev = nullptr; }

  void initHead() noexcept { next = prev = this; }

  bool unlinked() const noexcept { return next == nullptr; }

  void unlink() noexcept {
    next->prev = prev;
    prev->next = next;
  }

  void linkBefore(IntrusiveListNode* pos) noexcept {
    next = pos;
    prev = pos->prev;
    prev->next = this;
    pos->prev = this;
  }

  // Exchanges the positions of two hooks, which may be in different lists,
  // adjacent, or unlinked.
  static void swapHooks(IntrusiveListNode* a, IntrusiveListNode* b) noexcept {
    bool aUnlinked = a->unlinked();
    bool bUnlinked = b->unlinked();
    if (aUnlinked) {
      a->initHead();
    }
    if (bUnlinked) {
      b->initHead();
    }
    IntrusiveListNode* aNext = a->next;
    IntrusiveListNode* aPrev = a->prev;
    IntrusiveListNode* bNext = b->next;
    IntrusiveListNode* bPrev = b->prev;
    // The neighbors must be updated before the nodes themselves.
    std::swap(aNext->prev, bNext->prev);
    std::swap(aPrev->next, bPrev->next);
    std::swap(a->next, b->next);
    std::swap(a->prev, b->prev);
    if (aUnlinked) {
      b->init();
    }
    if (bUnlinked) {
      a->init();
    }
  }
};

enum class IntrusiveLinkMode {
  // The hook asserts in debug builds that it isn't linked when it's inserted
  // or destroyed.
  safe_link,
  // Like safe_link, but the hook destructor unlinks it and unlink() is public.
  auto_unlink,
};

template <typename T, auto Member, bool ConstantTimeSize>
class IntrusiveListImpl;

// A list hook. Copying or moving a hook gives an unlinked hook, and assigning
// a hook does nothing.
template <IntrusiveLinkMode Mode>
class IntrusiveListHookImpl {
 public:
  IntrusiveListHookImpl() noexcept { node_.init(); }
  IntrusiveListHookImpl(const IntrusiveListHookImpl&) noexcept { node_.init(); }
  IntrusiveListHookImpl& operator=(const IntrusiveListHookImpl&) noexcept {
    return *this;
  }
  IntrusiveListHookImpl(IntrusiveListHookImpl&&) noexcept { node_.init(); }
  IntrusiveListHookImpl& operator=(IntrusiveListHookImpl&&) noexcept {
    return *this;
  }

  ~IntrusiveListHookImpl() {
    if constexpr (Mode == IntrusiveLinkMode::auto_unlink) {
      if (is_linked()) {
        node_.unlink();
      }
    } else {
      FOLLY_SAFE_DCHECK(!is_linked(), "destroying a linked list hook");
    }
  }

  bool is_linked() const noexcept { return !node_.unlinked(); }

  void unlink() noexcept {
    static_assert(
        Mode == IntrusiveLinkMode::auto_unlink,
        "unlink() requires an auto_unlink hook");
    if (is_linked()) {
      node_.unlink();
      node_.init();
    }
  }

  void swap_nodes(IntrusiveListHookImpl& other) noexcept {
    IntrusiveListNode::swapHooks(&node_, &other.node_);
  }

 private:
  template <typename T, auto Member, bool ConstantTimeSize>
  friend class IntrusiveListImpl;

  IntrusiveListNode node_{};
};

template <typename Traits, typename Value>
class IntrusiveListIterator {
 public:
  using iterator_category = std::bidirectional_iterator_tag;
  using value_type = std::remove_const_t<Value>;
  using difference_type = std::ptrdiff_t;
  using pointer = Value*;
  using reference = Value&;

  IntrusiveListIterator() = default;

  explicit IntrusiveListIterator(IntrusiveListNode* node) noexcept
      : node_(node) {}

  // Converts an iterator to a const_iterator.
  template <typename V>
    requires std::is_same_v<const V, Value>
  /* implicit */ IntrusiveListIterator(
      const IntrusiveListIterator<Traits, V>& other) noexcept
      : node_(other.node()) {}

  IntrusiveListNode* node() const noexcept { return node_; }

  reference operator*() const noexcept { return *Traits::toValue(node_); }
  pointer operator->() const noexcept { return Traits::toValue(node_); }

  IntrusiveListIterator& operator++() noexcept {
    node_ = node_->next;
    return *this;
  }
  IntrusiveListIterator operator++(int) noexcept {
    auto result = *this;
    node_ = node_->next;
    return result;
  }
  IntrusiveListIterator& operator--() noexcept {
    node_ = node_->prev;
    return *this;
  }
  IntrusiveListIterator operator--(int) noexcept {
    auto result = *this;
    node_ = node_->prev;
    return result;
  }

  friend bool operator==(
      const IntrusiveListIterator& lhs,
      const IntrusiveListIterator& rhs) noexcept {
    return lhs.node_ == rhs.node_;
  }

 private:
  IntrusiveListNode* node_ = nullptr;
};

// Returns the offset of the data member that member points to within Class.
template <typename Member, typename Class>
std::ptrdiff_t memberOffset(Member Class::* member) noexcept {
  // The Itanium ABI represents a pointer to a data member as its offset. MSVC
  // does too, as an int, for a class without virtual bases; its larger forms
  // aren't plain offsets.
#ifdef _MSC_VER
  using Offset = int;
#else
  using Offset = std::ptrdiff_t;
#endif
  static_assert(
      sizeof(member) == sizeof(Offset),
      "unsupported pointer-to-member representation");
  Offset offset;
  std::memcpy(&offset, &member, sizeof(offset));
  return offset;
}

// Returns the object whose data member at ptr is *member, like
// boost::intrusive::get_parent_from_member. member must point to that data
// member of a live, non-const Class object, and ptr must have a form that
// memberOffset accepts.
template <typename Class, typename Member>
Class* parentFromMember(Member* member, Member Class::* ptr) noexcept {
  return reinterpret_cast<Class*>(
      reinterpret_cast<char*>(member) - memberOffset(ptr));
}

template <typename MemberPointer>
struct IntrusiveListMember;

template <typename Hook, typename Class>
struct IntrusiveListMember<Hook Class::*> {
  using hook_type = Hook;
  using class_type = Class;
};

template <bool ConstantTimeSize>
struct IntrusiveListSize {};

template <>
struct IntrusiveListSize<true> {
  std::size_t size_ = 0;
};

// An intrusive list of T linked through the hook at Member, a pointer to a
// hook member of T. With ConstantTimeSize, the list keeps a count, so size()
// is O(1); otherwise size() walks the list.
template <typename T, auto Member, bool ConstantTimeSize>
class IntrusiveListImpl : private IntrusiveListSize<ConstantTimeSize> {
  using MemberInfo = IntrusiveListMember<decltype(Member)>;
  // toValue() computes T's address from the member's offset, which is only
  // the offset within T if Member is a pointer to a member of T.
  static_assert(
      std::is_same_v<typename MemberInfo::class_type, T>,
      "Member must be a pointer to a member of T");
  static_assert(
      !(ConstantTimeSize &&
        std::is_same_v<
            typename MemberInfo::hook_type,
            IntrusiveListHookImpl<IntrusiveLinkMode::auto_unlink>>),
      "auto_unlink hooks can't be used with a constant-time size list");

  struct Traits {
    static IntrusiveListNode* toNode(T& value) noexcept {
      return &(value.*Member).node_;
    }
    static T* toValue(IntrusiveListNode* node) noexcept {
      // node_ is the only member of the hook, so the node and the hook share
      // an address.
      return reinterpret_cast<T*>(
          reinterpret_cast<char*>(node) - memberOffset(Member));
    }
  };

 public:
  using value_type = T;
  using pointer = T*;
  using const_pointer = const T*;
  using reference = T&;
  using const_reference = const T&;
  using difference_type = std::ptrdiff_t;
  using size_type = std::size_t;
  using iterator = IntrusiveListIterator<Traits, T>;
  using const_iterator = IntrusiveListIterator<Traits, const T>;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  IntrusiveListImpl() noexcept { head_.initHead(); }

  IntrusiveListImpl(const IntrusiveListImpl&) = delete;
  IntrusiveListImpl& operator=(const IntrusiveListImpl&) = delete;

  IntrusiveListImpl(IntrusiveListImpl&& other) noexcept : IntrusiveListImpl() {
    swap(other);
  }

  // Like Boost, move assignment swaps the contents.
  IntrusiveListImpl& operator=(IntrusiveListImpl&& other) noexcept {
    swap(other);
    return *this;
  }

  ~IntrusiveListImpl() { clear(); }

  iterator begin() noexcept { return iterator(head_.next); }
  const_iterator begin() const noexcept { return const_iterator(head_.next); }
  const_iterator cbegin() const noexcept { return begin(); }
  iterator end() noexcept { return iterator(&head_); }
  const_iterator end() const noexcept { return const_iterator(headPtr()); }
  const_iterator cend() const noexcept { return end(); }

  reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
  const_reverse_iterator rbegin() const noexcept {
    return const_reverse_iterator(end());
  }
  const_reverse_iterator crbegin() const noexcept { return rbegin(); }
  reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
  const_reverse_iterator rend() const noexcept {
    return const_reverse_iterator(begin());
  }
  const_reverse_iterator crend() const noexcept { return rend(); }

  bool empty() const noexcept { return head_.next == &head_; }

  size_type size() const noexcept {
    if constexpr (ConstantTimeSize) {
      return this->size_;
    } else {
      size_type n = 0;
      for (auto* node = head_.next; node != &head_; node = node->next) {
        ++n;
      }
      return n;
    }
  }

  reference front() noexcept { return *Traits::toValue(head_.next); }
  const_reference front() const noexcept {
    return *Traits::toValue(head_.next);
  }
  reference back() noexcept { return *Traits::toValue(head_.prev); }
  const_reference back() const noexcept { return *Traits::toValue(head_.prev); }

  void push_back(reference value) noexcept { insert(end(), value); }
  void push_front(reference value) noexcept { insert(begin(), value); }
  void pop_front() noexcept { erase(begin()); }
  void pop_back() noexcept { erase(--end()); }

  iterator insert(const_iterator pos, reference value) noexcept {
    IntrusiveListNode* node = Traits::toNode(value);
    FOLLY_SAFE_DCHECK(node->unlinked(), "inserting a linked list hook");
    node->linkBefore(pos.node());
    addSize(1);
    return iterator(node);
  }

  // Returns the iterator following the erased element.
  iterator erase(const_iterator pos) noexcept {
    IntrusiveListNode* node = pos.node();
    IntrusiveListNode* next = node->next;
    node->unlink();
    node->init();
    addSize(-1);
    return iterator(next);
  }

  void clear() noexcept {
    clear_and_dispose([](pointer) {});
  }

  // The *_and_dispose functions unlink elements and then call disposer(ptr)
  // on each of them. A disposer must not throw or modify the list.
  template <typename Disposer>
  void clear_and_dispose(Disposer disposer) {
    IntrusiveListNode* node = head_.next;
    head_.initHead();
    if constexpr (ConstantTimeSize) {
      this->size_ = 0;
    }
    while (node != &head_) {
      IntrusiveListNode* next = node->next;
      node->init();
      disposer(Traits::toValue(node));
      node = next;
    }
  }

  template <typename Disposer>
  iterator erase_and_dispose(const_iterator pos, Disposer disposer) {
    pointer value = Traits::toValue(pos.node());
    iterator next = erase(pos);
    disposer(value);
    return next;
  }

  template <typename Disposer>
  void pop_front_and_dispose(Disposer disposer) {
    erase_and_dispose(begin(), disposer);
  }

  iterator iterator_to(reference value) noexcept {
    return s_iterator_to(value);
  }
  const_iterator iterator_to(const_reference value) const noexcept {
    return s_iterator_to(value);
  }
  static iterator s_iterator_to(reference value) noexcept {
    return iterator(Traits::toNode(value));
  }
  static const_iterator s_iterator_to(const_reference value) noexcept {
    return const_iterator(Traits::toNode(const_cast<reference>(value)));
  }

  // Moves all elements of other before pos. other must be a different list.
  void splice(const_iterator pos, IntrusiveListImpl& other) noexcept {
    if (other.empty()) {
      return;
    }
    IntrusiveListNode* first = other.head_.next;
    IntrusiveListNode* last = other.head_.prev;
    other.head_.initHead();
    linkRange(pos.node(), first, last);
    if constexpr (ConstantTimeSize) {
      this->size_ += other.size_;
      other.size_ = 0;
    }
  }

  // Moves the element at it from other, which may be this list, before pos.
  void splice(
      const_iterator pos,
      IntrusiveListImpl& other,
      const_iterator it) noexcept {
    IntrusiveListNode* node = it.node();
    if (node == pos.node() || node->next == pos.node()) {
      return;
    }
    node->unlink();
    node->linkBefore(pos.node());
    other.addSize(-1);
    addSize(1);
  }

  // Moves [first, last) from other, which may be this list, before pos. pos
  // must not be in [first, last).
  void splice(
      const_iterator pos,
      IntrusiveListImpl& other,
      const_iterator first,
      const_iterator last) noexcept {
    if (first == last) {
      return;
    }
    if constexpr (ConstantTimeSize) {
      if (&other != this) {
        auto n = std::distance(first, last);
        other.addSize(-n);
        addSize(n);
      }
    }
    IntrusiveListNode* f = first.node();
    IntrusiveListNode* l = last.node()->prev;
    f->prev->next = last.node();
    last.node()->prev = f->prev;
    linkRange(pos.node(), f, l);
  }

  void swap(IntrusiveListImpl& other) noexcept {
    swapHeads(&head_, &other.head_);
    if constexpr (ConstantTimeSize) {
      std::swap(this->size_, other.size_);
    }
  }

  friend void swap(IntrusiveListImpl& lhs, IntrusiveListImpl& rhs) noexcept {
    lhs.swap(rhs);
  }

 private:
  IntrusiveListNode* headPtr() const noexcept {
    return const_cast<IntrusiveListNode*>(&head_);
  }

  void addSize([[maybe_unused]] difference_type n) noexcept {
    if constexpr (ConstantTimeSize) {
      this->size_ += static_cast<size_type>(n);
    }
  }

  // Exchanges the contents of two lists. This computes the new links before
  // storing them, so it needs no branches and avoids store-to-load forwarding
  // stalls when a list is empty.
  static void swapHeads(IntrusiveListNode* a, IntrusiveListNode* b) noexcept {
    IntrusiveListNode* aNext = a->next;
    IntrusiveListNode* aPrev = a->prev;
    IntrusiveListNode* bNext = b->next;
    IntrusiveListNode* bPrev = b->prev;
    bool aEmpty = aNext == a;
    bool bEmpty = bNext == b;
    a->next = bEmpty ? a : bNext;
    a->prev = bEmpty ? a : bPrev;
    b->next = aEmpty ? b : aNext;
    b->prev = aEmpty ? b : aPrev;
    a->next->prev = a;
    a->prev->next = a;
    b->next->prev = b;
    b->prev->next = b;
  }

  // Links the chain [first, last] before pos.
  static void linkRange(
      IntrusiveListNode* pos,
      IntrusiveListNode* first,
      IntrusiveListNode* last) noexcept {
    first->prev = pos->prev;
    pos->prev->next = first;
    last->next = pos;
    pos->prev = last;
  }

  IntrusiveListNode head_{};
};

} // namespace folly::detail
