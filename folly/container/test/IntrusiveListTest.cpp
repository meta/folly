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

#include <folly/container/IntrusiveList.h>

#include <iterator>
#include <utility>
#include <vector>

#include <folly/portability/GTest.h>

namespace {

using AutoUnlinkHook = folly::IntrusiveListHook;
using SafeHook = folly::SafeIntrusiveListHook;

struct Item {
  explicit Item(int v) : value(v) {}

  int value = 0;
  AutoUnlinkHook hook;
  SafeHook safeHook;
};

using List = folly::IntrusiveList<Item, &Item::hook>;
using CountedList = folly::CountedIntrusiveList<Item, &Item::safeHook>;

static_assert(sizeof(AutoUnlinkHook) == 2 * sizeof(void*));
static_assert(sizeof(SafeHook) == 2 * sizeof(void*));
static_assert(sizeof(List) == 2 * sizeof(void*));
static_assert(sizeof(CountedList) == 2 * sizeof(void*) + sizeof(size_t));

template <typename Range>
std::vector<int> values(const Range& range) {
  std::vector<int> result;
  for (const auto& item : range) {
    result.push_back(item.value);
  }
  return result;
}

} // namespace

TEST(IntrusiveList, basicOps) {
  Item a(1), b(2), c(3);
  List list;
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(list.size(), 0);

  list.push_back(b);
  list.push_back(c);
  list.push_front(a);
  EXPECT_EQ(list.size(), 3);
  EXPECT_EQ(list.front().value, 1);
  EXPECT_EQ(list.back().value, 3);
  EXPECT_EQ(values(list), (std::vector<int>{1, 2, 3}));
  std::vector<int> reversed;
  for (auto it = list.crbegin(); it != list.crend(); ++it) {
    reversed.push_back(it->value);
  }
  EXPECT_EQ(reversed, (std::vector<int>{3, 2, 1}));

  list.pop_front();
  list.pop_back();
  EXPECT_FALSE(a.hook.is_linked());
  EXPECT_FALSE(c.hook.is_linked());
  EXPECT_EQ(values(list), (std::vector<int>{2}));

  List other(std::move(list));
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(values(other), (std::vector<int>{2}));
  other.clear();
  EXPECT_TRUE(other.empty());
  EXPECT_FALSE(b.hook.is_linked());
}

TEST(IntrusiveList, eraseIteratorToAndAutoUnlink) {
  Item a(1), b(2);
  List list;
  list.push_back(a);
  list.push_back(b);
  {
    Item c(3);
    list.push_back(c);
    EXPECT_EQ(&*list.iterator_to(c), &c);
    EXPECT_EQ(&*List::s_iterator_to(std::as_const(c)), &c);
  }
  EXPECT_EQ(values(list), (std::vector<int>{1, 2}));

  auto next = list.erase(list.iterator_to(a));
  EXPECT_EQ(&*next, &b);
  EXPECT_FALSE(a.hook.is_linked());

  b.hook.unlink();
  EXPECT_FALSE(b.hook.is_linked());
  EXPECT_TRUE(list.empty());
}

TEST(IntrusiveList, hookCopyAndSwap) {
  Item a(1), b(2), c(3);
  List list;
  list.push_back(a);
  list.push_back(b);

  Item copy(a);
  Item moved(std::move(copy));
  EXPECT_FALSE(copy.hook.is_linked());
  EXPECT_FALSE(moved.hook.is_linked());
  b.hook = c.hook;
  EXPECT_TRUE(b.hook.is_linked());

  a.hook.swap_nodes(b.hook);
  EXPECT_EQ(values(list), (std::vector<int>{2, 1}));
  c.hook.swap_nodes(b.hook);
  EXPECT_FALSE(b.hook.is_linked());
  EXPECT_EQ(values(list), (std::vector<int>{3, 1}));
}

TEST(IntrusiveList, splice) {
  std::vector<Item> items;
  items.reserve(6);
  for (int i = 0; i < 6; ++i) {
    items.emplace_back(i);
  }
  CountedList list1, list2;
  for (int i = 0; i < 3; ++i) {
    list1.push_back(items[i]);
    list2.push_back(items[i + 3]);
  }

  list1.splice(list1.end(), list2);
  EXPECT_EQ(values(list1), (std::vector<int>{0, 1, 2, 3, 4, 5}));
  EXPECT_TRUE(list2.empty());

  list1.splice(list1.begin(), list1, list1.iterator_to(items[4]));
  EXPECT_EQ(values(list1), (std::vector<int>{4, 0, 1, 2, 3, 5}));

  list2.splice(
      list2.end(),
      list1,
      list1.iterator_to(items[0]),
      list1.iterator_to(items[3]));
  EXPECT_EQ(values(list1), (std::vector<int>{4, 3, 5}));
  EXPECT_EQ(values(list2), (std::vector<int>{0, 1, 2}));
  list1.clear();
  list2.clear();
}

TEST(IntrusiveList, countedSize) {
  std::vector<Item> items;
  items.reserve(4);
  for (int i = 0; i < 4; ++i) {
    items.emplace_back(i);
  }
  CountedList list1, list2;
  for (auto& item : items) {
    list1.push_back(item);
  }
  EXPECT_EQ(list1.size(), 4);

  std::vector<int> disposed;
  auto disposer = [&](Item* item) { disposed.push_back(item->value); };
  list1.pop_front_and_dispose(disposer);
  list1.erase_and_dispose(list1.iterator_to(items[2]), disposer);
  EXPECT_EQ(list1.size(), 2);

  list2.splice(list2.end(), list1, list1.begin());
  list2.splice(list2.end(), list1, list1.begin(), list1.end());
  EXPECT_EQ(list1.size(), 0);
  EXPECT_EQ(list2.size(), 2);

  list1.swap(list2);
  EXPECT_EQ(list1.size(), 2);
  list1.clear_and_dispose(disposer);
  EXPECT_EQ(list1.size(), 0);
  EXPECT_EQ(disposed, (std::vector<int>{0, 2, 1, 3}));
  EXPECT_FALSE(items[1].safeHook.is_linked());
}

TEST(IntrusiveList, parentFromMember) {
  struct Base {
    virtual ~Base() = default;
    int x = 0;
  };
  struct Derived : Base {
    int y = 0;
  };
  Item item(1);
  Derived derived;
  EXPECT_EQ(
      folly::detail::parentFromMember(&item.safeHook, &Item::safeHook), &item);
  EXPECT_EQ(folly::detail::parentFromMember(&derived.y, &Derived::y), &derived);
}

namespace {

struct Polymorphic {
  virtual ~Polymorphic() = default;
  AutoUnlinkHook hook;
};

using PolymorphicList = folly::IntrusiveList<Polymorphic, &Polymorphic::hook>;

} // namespace

TEST(IntrusiveList, polymorphic) {
  Polymorphic a;
  PolymorphicList list;
  list.push_back(a);
  EXPECT_EQ(
      static_cast<void*>(PolymorphicList::s_iterator_to(a).node()),
      static_cast<void*>(&a.hook));
  EXPECT_EQ(&list.front(), &a);
}
