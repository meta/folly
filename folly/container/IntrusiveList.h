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

/*
 * Intrusive doubly linked lists. The list types provide the interface of
 * boost::intrusive::list that users need, with the same names and semantics.
 */

#include <folly/container/detail/IntrusiveList.h>

namespace folly {

/**
 * An auto-unlink intrusive list hook.
 */
using IntrusiveListHook =
    detail::IntrusiveListHookImpl<detail::IntrusiveLinkMode::auto_unlink>;

/**
 * An intrusive list.
 *
 * An IntrusiveList always uses an auto-unlink hook.
 * Beware that IntrusiveList::size() is an O(n) operation, since it has to walk
 * the entire list.
 *
 * Example usage:
 *
 *   class Foo {
 *     // Note that the listHook member variable needs to be visible
 *     // to the code that defines the IntrusiveList instantiation.
 *     // The list hook can be made public, or you can make the other class a
 *     // friend.
 *     IntrusiveListHook listHook;
 *   };
 *
 *   using FooList = IntrusiveList<Foo, &Foo::listHook>;
 *
 *   Foo *foo = new Foo();
 *   FooList myList;
 *   myList.push_back(*foo);
 *
 * Note that each IntrusiveListHook can only be part of a single list at any
 * given time.  If you need the same object to be stored in two lists at once,
 * you need to use two different IntrusiveListHook member variables.
 *
 * The elements stored in the list must contain an IntrusiveListHook member
 * variable.
 *
 * If Foo declares virtual functions, name FooList after the definition of Foo,
 * not inside it: MSVC and clang-cl give &Foo::listHook the wrong offset while
 * Foo is incomplete.
 */
template <typename T, IntrusiveListHook T::* PtrToMember>
using IntrusiveList = detail::IntrusiveListImpl<T, PtrToMember, false>;

/**
 * A safe-link intrusive list hook.
 */
using SafeIntrusiveListHook =
    detail::IntrusiveListHookImpl<detail::IntrusiveLinkMode::safe_link>;

/**
 * A safe intrusive list.
 *
 * This is like IntrusiveList but always uses the safe-link hook which ensures
 * that the hook is initialised to an unlinked state on construction and reset
 * an unlinked state upon removing it from a list.
 */
template <typename T, SafeIntrusiveListHook T::* PtrToMember>
using SafeIntrusiveList = detail::IntrusiveListImpl<T, PtrToMember, false>;

/**
 * An intrusive list with const-time size() method.
 *
 * A CountedIntrusiveList always uses a safe-link hook.
 * CountedIntrusiveList::size() is an O(1) operation. Users of this type
 * of lists need to remove a member from a list by calling one of the
 * methods on the list (e.g., erase(), pop_front(), etc.), rather than
 * calling unlink on the member's list hook. Given references to a
 * list and a member, a constant-time removal operation can be
 * accomplished by list.erase(list.iterator_to(member)). Also, when a
 * member is destroyed, it is NOT automatically removed from the list.
 *
 * Example usage:
 *
 *   class Foo {
 *     // Note that the listHook member variable needs to be visible
 *     // to the code that defines the CountedIntrusiveList instantiation.
 *     // The list hook can be made public, or you can make the other class a
 *     // friend.
 *     SafeIntrusiveListHook listHook;
 *   };
 *
 *   using FooList = CountedIntrusiveList<Foo, &Foo::listHook> FooList;
 *
 *   Foo *foo = new Foo();
 *   FooList myList;
 *   myList.push_back(*foo);
 *   myList.pop_front();
 *
 * Note that each SafeIntrusiveListHook can only be part of a single list at any
 * given time.  If you need the same object to be stored in two lists at once,
 * you need to use two different SafeIntrusiveListHook member variables.
 *
 * The elements stored in the list must contain an SafeIntrusiveListHook member
 * variable.
 */
template <typename T, SafeIntrusiveListHook T::* PtrToMember>
using CountedIntrusiveList = detail::IntrusiveListImpl<T, PtrToMember, true>;

} // namespace folly
