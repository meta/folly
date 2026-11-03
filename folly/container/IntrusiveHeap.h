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
 * Skew heap [1] implementation using top-down meld.
 *
 * [1] D.D. Sleator, R.E. Tarjan, Self-Adjusting Heaps,
 * SIAM Journal of Computing, 15(1): 52-69, 1986.
 * http://www.cs.cmu.edu/~sleator/papers/adjusting-heaps.pdf
 */

#pragma once

#include <functional>
#include <utility>

#include <boost/intrusive/parent_from_member.hpp>
#include <glog/logging.h>
#include <folly/Portability.h>
#include <folly/lang/Builtin.h>

namespace folly {

/**
 * Base class for items to be inserted into IntrusiveHeap<..., Tag> storing
 * pointers for internal use.
 */
template <class Tag = void>
class IntrusiveHeapNode {
 public:
  IntrusiveHeapNode() = default;
  IntrusiveHeapNode(const IntrusiveHeapNode&) = delete;
  IntrusiveHeapNode(IntrusiveHeapNode&&) = delete;
  IntrusiveHeapNode& operator=(const IntrusiveHeapNode&) = delete;
  IntrusiveHeapNode& operator=(IntrusiveHeapNode&&) = delete;
  ~IntrusiveHeapNode() = default;

  bool isLinked() const { return parent_ != kUnlinked; }

 private:
  template <class, class, class, class>
  friend class IntrusiveHeap;
  friend class IntrusiveHeapTest;

  static IntrusiveHeapNode* const kUnlinked;

  /**
   * If this is in a heap, (parent_ == nullptr) <=> (this == heap_.root_).
   * Otherwise, parent_ is kUnlinked.
   */
  IntrusiveHeapNode* parent_ = kUnlinked;

  /**
   * If this is in a heap, left_ and right_ point to subheaps or nullptr.
   * Otherwise, these are undefined.
   */
  IntrusiveHeapNode* left_ = nullptr;
  IntrusiveHeapNode* right_ = nullptr;
};

template <class Tag>
IntrusiveHeapNode<Tag>* const IntrusiveHeapNode<Tag>::kUnlinked =
    reinterpret_cast<IntrusiveHeapNode*>(1);

template <class T, class Tag>
struct DerivedNodeTraits {
  static IntrusiveHeapNode<Tag>* asNode(T* x) { return x; }
  static T* asT(IntrusiveHeapNode<Tag>* n) { return static_cast<T*>(n); }
};

template <class T, class Tag, IntrusiveHeapNode<Tag> T::* PtrToMember>
struct MemberNodeTraits {
  static IntrusiveHeapNode<Tag>* asNode(T* x) { return &(x->*PtrToMember); }
  static T* asT(IntrusiveHeapNode<Tag>* n) {
    return boost::intrusive::get_parent_from_member(n, PtrToMember);
  }
};

/**
 * IntrusiveHeap implements a skew heap with intrusive pointers to provide
 * O(log(n)) operations on any node in the heap with no separately allocated
 * node type.
 *
 * - To be inserted into an IntrusiveHeap<T, Compare, Tag>, T must inherit from
 *   IntrusiveHeapNode<Tag>, or have a member of type IntrusiveHeapNode<Tag> and
 *   use MemberNodeTraits.
 *
 * - An instance of T may only be included in one IntrusiveHeap for each Tag
 *   type. It may be included in more than one IntrusiveHeap by inheriting from
 *   IntrusiveHeapNode again with a different tag type, or by using composition
 *   with different members.
 */
template <
    class T,
    class Compare = std::less<>,
    class Tag = void,
    class NodeTraitsType = DerivedNodeTraits<T, Tag>>
class IntrusiveHeap {
 public:
  using Node = IntrusiveHeapNode<Tag>;
  using NodeTraits = NodeTraitsType;
  using Value = T;

  IntrusiveHeap() {}

  IntrusiveHeap(const IntrusiveHeap&) = delete;
  IntrusiveHeap& operator=(const IntrusiveHeap&) = delete;

  IntrusiveHeap(IntrusiveHeap&& other) noexcept
      : root_(std::exchange(other.root_, nullptr)) {}
  IntrusiveHeap& operator=(IntrusiveHeap&&) = delete;

  /**
   * Returns a pointer to the maximum value in the heap, or nullptr if the heap
   * is empty.
   */
  T* top() const { return root_ != nullptr ? asT(root_) : nullptr; }

  bool empty() const { return root_ == nullptr; }

  /**
   * Removes the maximum value from the heap and returns it, or nullptr if the
   * heap is empty.
   */
  T* pop() {
    if (root_ == nullptr) {
      return nullptr;
    }
    Node* top = root_;
    merge(top->left_, top->right_, nullptr, &root_);
    top->parent_ = Node::kUnlinked;
    return asT(top);
  }

  /**
   * Visits all items in the heap.
   */
  template <class Visitor>
  void visit(const Visitor& visitor) const {
    visit(visitor, root_);
  }

  /**
   * Updates the heap to reflect a change in a given value.
   */
  void update(T* x) {
    erase(x);
    push(x);
  }

  void push(T* x) {
    DCHECK(x);
    auto n = asNode(x);
    DCHECK(!n->isLinked());
    n->parent_ = nullptr;
    n->left_ = nullptr;
    n->right_ = nullptr;
    merge(n, root_, nullptr, &root_);
  }

  void erase(T* x) {
    auto n = asNode(x);
    DCHECK(n->isLinked());
    DCHECK(contains(x));
    auto parent = n->parent_;
    Node** out;
    if (parent == nullptr) {
      out = &root_;
    } else if (parent->left_ == n) {
      out = &parent->left_;
    } else {
      DCHECK_EQ(parent->right_, n);
      out = &parent->right_;
    }

    merge(n->left_, n->right_, parent, out);
    n->parent_ = Node::kUnlinked;
  }

  /**
   * Check whether this node is included in this heap. Primarily meant for
   * assertions, as containment should be externally tracked.
   */
  bool contains(const T* x) const {
    DCHECK(x);
    auto n = asNode(x);
    while (n->parent_) {
      n = n->parent_;
    }
    return n == root_;
  }

  /**
   * Moves the contents of other into *this.
   */
  void merge(IntrusiveHeap other) {
    merge(root_, other.root_, nullptr, &root_);
  }

 private:
  friend class IntrusiveHeapTest;

  template <class Visitor>
  static void visit(const Visitor& visitor, Node* x) {
    for (; x != nullptr; x = x->right_) {
      visitor(asT(x));
      visit(visitor, x->left_);
    }
  }

  /**
   * Merges two subtrees, assigns a parent, populates *out with new subtree.
   */
  FOLLY_ALWAYS_INLINE static void merge(
      Node* a, Node* b, Node* parent, Node** out) {
    DCHECK(out);
    if (a == nullptr || b == nullptr) {
      *out = a ? a : b;
      if (*out) {
        (*out)->parent_ = parent;
      }
      return;
    }
    do {
      Node* grandparent = parent;
      if (FOLLY_BUILTIN_UNPREDICTABLE(compare(a, b))) {
        parent = b;
      } else {
        parent = a;
        a = b;
      }
      b = parent->right_;
      *out = parent;
      out = &parent->left_;
      parent->right_ = parent->left_;
      parent->parent_ = grandparent;
      DCHECK(a);
    } while (b != nullptr);
    *out = a;
    a->parent_ = parent;
  }

  static Node* asNode(T* x) { return NodeTraits::asNode(x); }

  static const Node* asNode(const T* x) {
    return NodeTraits::asNode(const_cast<T*>(x));
  }

  static T* asT(Node* n) { return NodeTraits::asT(n); }

  static const T* asT(const Node* n) {
    return NodeTraits::asT(const_cast<Node*>(n));
  }

  static bool compare(const Node* a, const Node* b) {
    return Compare()(*asT(a), *asT(b));
  }

  Node* root_ = nullptr;
};

} // namespace folly
