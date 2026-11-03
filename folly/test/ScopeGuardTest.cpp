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

#include <folly/ScopeGuard.h>

#include <condition_variable>
#include <functional>
#include <stdexcept>
#include <thread>

#include <glog/logging.h>

#include <folly/lang/Keep.h>
#include <folly/portability/GTest.h>

using folly::makeDismissedGuard;
using folly::makeGuard;
using std::vector;

struct in_scope {};
struct in_guard {};

extern "C" FOLLY_KEEP void check_folly_scope_exit_opaque() {
  SCOPE_EXIT {
    folly::detail::keep_sink(in_guard{});
  };
  folly::detail::keep_sink(in_scope{});
}

extern "C" FOLLY_KEEP void check_folly_scope_exit_opaque_noexcept() {
  SCOPE_EXIT {
    folly::detail::keep_sink(in_guard{});
  };
  folly::detail::keep_sink_nx(in_scope{});
}

extern "C" FOLLY_KEEP [[noreturn]] void check_folly_scope_exit_visible_throw() {
  SCOPE_EXIT {
    folly::detail::keep_sink(in_guard{});
  };
  throw 0;
}

extern "C" FOLLY_KEEP void check_folly_scope_exit_visible_throw_cond(bool b) {
  SCOPE_EXIT {
    folly::detail::keep_sink(in_guard{});
  };
  b ? void(throw 0) : void();
}

extern "C" FOLLY_KEEP void check_folly_scope_success_opaque() {
  SCOPE_SUCCESS {
    folly::detail::keep_sink(in_guard{});
  };
  folly::detail::keep_sink(in_scope{});
}

extern "C" FOLLY_KEEP void check_folly_scope_success_opaque_noexcept() {
  SCOPE_SUCCESS {
    folly::detail::keep_sink(in_guard{});
  };
  folly::detail::keep_sink_nx(in_scope{});
}

extern "C" FOLLY_KEEP [[noreturn]] void
check_folly_scope_success_visible_throw() {
  SCOPE_SUCCESS {
    folly::detail::keep_sink(in_guard{});
  };
  throw 0;
}

extern "C" FOLLY_KEEP void check_folly_scope_success_visible_throw_cond(
    bool b) {
  SCOPE_SUCCESS {
    folly::detail::keep_sink(in_guard{});
  };
  b ? void(throw 0) : void();
}

extern "C" FOLLY_KEEP void check_folly_scope_fail_opaque() {
  SCOPE_FAIL {
    folly::detail::keep_sink(in_guard{});
  };
  folly::detail::keep_sink(in_scope{});
}

extern "C" FOLLY_KEEP void check_folly_scope_fail_opaque_noexcept() {
  SCOPE_FAIL {
    folly::detail::keep_sink(in_guard{});
  };
  folly::detail::keep_sink_nx(in_scope{});
}

extern "C" FOLLY_KEEP [[noreturn]] void check_folly_scope_fail_visible_throw() {
  SCOPE_FAIL {
    folly::detail::keep_sink(in_guard{});
  };
  throw 0;
}

extern "C" FOLLY_KEEP void check_folly_scope_fail_visible_throw_cond(bool b) {
  SCOPE_FAIL {
    folly::detail::keep_sink(in_guard{});
  };
  b ? void(throw 0) : void();
}

double returnsDouble() {
  return 0.0;
}

class MyFunctor {
 public:
  explicit MyFunctor(int* ptr) : ptr_(ptr) {}

  void operator()() { ++*ptr_; }

 private:
  int* ptr_;
};

TEST(ScopeGuard, DifferentWaysToBind) {
  vector<int> v;
  void (vector<int>::*push_back)(int const&) = &vector<int>::push_back;

  v.push_back(1);
  {
    // binding to member function.
    auto g = makeGuard(std::bind(&vector<int>::pop_back, &v));
    (void)g;
  }
  EXPECT_EQ(0, v.size());

  {
    // bind member function with args. v is passed-by-value!
    auto g = makeGuard(std::bind(push_back, v, 2));
    (void)g;
  }
  EXPECT_EQ(0, v.size()); // push_back happened on a copy of v... fail!

  // pass in an argument by pointer so to avoid copy.
  {
    auto g = makeGuard(std::bind(push_back, &v, 4));
    (void)g;
  }
  EXPECT_EQ(1, v.size());

  {
    // pass in an argument by reference so to avoid copy.
    auto g = makeGuard(std::bind(push_back, std::ref(v), 4));
    (void)g;
  }
  EXPECT_EQ(2, v.size());

  // lambda with a reference to v
  {
    auto g = makeGuard([&] { v.push_back(5); });
    (void)g;
  }
  EXPECT_EQ(3, v.size());

  // lambda with a copy of v
  {
    auto g = makeGuard([v]() mutable { v.push_back(6); });
    (void)g;
  }
  EXPECT_EQ(3, v.size());

  // functor object
  int n = 0;
  {
    MyFunctor f(&n);
    auto g = makeGuard(f);
    (void)g;
  }
  EXPECT_EQ(1, n);

  // temporary functor object
  n = 0;
  {
    auto g = makeGuard(MyFunctor(&n));
    (void)g;
  }
  EXPECT_EQ(1, n);

  // Use auto instead of ScopeGuard
  n = 2;
  {
    auto g = makeGuard(MyFunctor(&n));
    (void)g;
  }
  EXPECT_EQ(3, n);

  // Use const auto& instead of ScopeGuard
  n = 10;
  {
    const auto& g = makeGuard(MyFunctor(&n));
    (void)g;
  }
  EXPECT_EQ(11, n);
}

TEST(ScopeGuard, GuardException) {
  EXPECT_DEATH(
      (void)makeGuard([] {
        throw std::runtime_error("dtors should never throw!");
      }),
      "dtors should never throw!");
}

/**
 * Add an integer to a vector iff it was inserted into the
 * db successfuly. Here is a schematic of how you would accomplish
 * this with scope guard.
 */
void testUndoAction(bool failure) {
  vector<int64_t> v;
  { // defines a "mini" scope

    // be optimistic and insert this into memory
    v.push_back(1);

    // The guard is triggered to undo the insertion unless dismiss() is called.
    auto guard = makeGuard([&] { v.pop_back(); });

    // Do some action; Use the failure argument to pretend
    // if it failed or succeeded.

    // if there was no failure, dismiss the undo guard action.
    if (!failure) {
      guard.dismiss();
    }
  } // all stack allocated in the mini-scope will be destroyed here.

  if (failure) {
    EXPECT_EQ(0, v.size()); // the action failed => undo insertion
  } else {
    EXPECT_EQ(1, v.size()); // the action succeeded => keep insertion
  }
}

TEST(ScopeGuard, UndoAction) {
  testUndoAction(true);
  testUndoAction(false);
}

TEST(ScopeGuard, MakeDismissedGuard) {
  auto test = [](bool shouldFire) {
    bool fired = false;
    {
      auto guard = makeDismissedGuard([&] { fired = true; });
      if (shouldFire) {
        guard.rehire();
      }
    }
    EXPECT_EQ(shouldFire, fired);
  };

  test(true);
  test(false);
}

/**
 * Sometimes in a try catch block we want to execute a piece of code
 * regardless if an exception happened or not. For example, you want
 * to close a db connection regardless if an exception was thrown during
 * insertion. In Java and other languages there is a finally clause that
 * helps accomplish this:
 *
 *   try {
 *     dbConn.doInsert(sql);
 *   } catch (const DbException& dbe) {
 *     dbConn.recordFailure(dbe);
 *   } catch (const CriticalException& e) {
 *     throw e; // re-throw the exception
 *   } finally {
 *     dbConn.closeConnection(); // executes no matter what!
 *   }
 *
 * We can approximate this behavior in C++ with ScopeGuard.
 */
enum class ErrorBehavior {
  SUCCESS,
  HANDLED_ERROR,
  UNHANDLED_ERROR,
};

void testFinally(ErrorBehavior error) {
  bool cleanupOccurred = false;

  try {
    auto guard = makeGuard([&] { cleanupOccurred = true; });
    (void)guard;

    try {
      if (error == ErrorBehavior::HANDLED_ERROR) {
        throw std::runtime_error("throwing an expected error");
      } else if (error == ErrorBehavior::UNHANDLED_ERROR) {
        throw "never throw raw strings";
      }
    } catch (const std::runtime_error&) {
    }
  } catch (...) {
    // Outer catch to swallow the error for the UNHANDLED_ERROR behavior
  }

  EXPECT_TRUE(cleanupOccurred);
}

TEST(ScopeGuard, TryCatchFinally) {
  testFinally(ErrorBehavior::SUCCESS);
  testFinally(ErrorBehavior::HANDLED_ERROR);
  testFinally(ErrorBehavior::UNHANDLED_ERROR);
}

TEST(ScopeGuard, TESTScopeExit) {
  int x = 0;
  {
    SCOPE_EXIT {
      ++x;
    };
    EXPECT_EQ(0, x);
  }
  EXPECT_EQ(1, x);
}

class Foo {
 public:
  Foo() {}
  ~Foo() {
    try {
      auto e = std::current_exception();
      int test = 0;
      {
        SCOPE_EXIT {
          ++test;
        };
        EXPECT_EQ(0, test);
      }
      EXPECT_EQ(1, test);
    } catch (const std::exception& ex) {
      LOG(FATAL) << "Unexpected exception: " << ex.what();
    }
  }
};

TEST(ScopeGuard, TESTScopeFailure2) {
  try {
    Foo f;
    throw std::runtime_error("test");
  } catch (...) {
  }
}

void testScopeFailAndScopeSuccess(ErrorBehavior error, bool expectFail) {
  bool scopeFailExecuted = false;
  bool scopeSuccessExecuted = false;

  try {
    SCOPE_FAIL {
      scopeFailExecuted = true;
    };
    SCOPE_SUCCESS {
      scopeSuccessExecuted = true;
    };

    try {
      if (error == ErrorBehavior::HANDLED_ERROR) {
        throw std::runtime_error("throwing an expected error");
      } else if (error == ErrorBehavior::UNHANDLED_ERROR) {
        throw "never throw raw strings";
      }
    } catch (const std::runtime_error&) {
    }
  } catch (...) {
    // Outer catch to swallow the error for the UNHANDLED_ERROR behavior
  }

  EXPECT_EQ(expectFail, scopeFailExecuted);
  EXPECT_EQ(!expectFail, scopeSuccessExecuted);
}

TEST(ScopeGuard, TESTScopeFailExceptionPtr) {
  bool catchExecuted = false;
  bool failExecuted = false;

  try {
    SCOPE_FAIL {
      failExecuted = true;
    };

    std::exception_ptr ep;
    try {
      throw std::runtime_error("test");
    } catch (...) {
      ep = std::current_exception();
    }
    std::rethrow_exception(ep);
  } catch (const std::exception&) {
    catchExecuted = true;
  }

  EXPECT_TRUE(catchExecuted);
  EXPECT_TRUE(failExecuted);
}

TEST(ScopeGuard, TESTScopeFailAndScopeSuccess) {
  testScopeFailAndScopeSuccess(ErrorBehavior::SUCCESS, false);
  testScopeFailAndScopeSuccess(ErrorBehavior::HANDLED_ERROR, false);
  testScopeFailAndScopeSuccess(ErrorBehavior::UNHANDLED_ERROR, true);
}

TEST(ScopeGuard, TESTScopeSuccessThrow) {
  auto lambda = []() {
    SCOPE_SUCCESS {
      throw std::runtime_error("ehm");
    };
  };
  EXPECT_THROW(lambda(), std::runtime_error);
}

TEST(ScopeGuard, TESTThrowingCleanupAction) {
  struct ThrowingCleanupAction {
    // clang-format off
    explicit ThrowingCleanupAction(int& scopeExitExecuted)
        : scopeExitExecuted_(scopeExitExecuted) {}
    [[noreturn]] ThrowingCleanupAction(const ThrowingCleanupAction& other)
        : scopeExitExecuted_(other.scopeExitExecuted_) {
      throw std::runtime_error("whoa");
    }
    // clang-format on
    void operator()() { ++scopeExitExecuted_; }

   private:
    int& scopeExitExecuted_;
  };
  int scopeExitExecuted = 0;
  ThrowingCleanupAction onExit(scopeExitExecuted);
  EXPECT_THROW((void)makeGuard(onExit), std::runtime_error);
  EXPECT_EQ(scopeExitExecuted, 1);
}

TEST(ScopeGuard, ConstexprFires) {
  constexpr auto fired = [] {
    int v = 0;
    {
      auto guard = makeGuard([&] { v = 1; });
      (void)guard;
    }
    return v;
  }();
  static_assert(fired == 1, "guard must run at end of scope");
  EXPECT_EQ(fired, 1);
}

TEST(ScopeGuard, ConstexprDismiss) {
  constexpr auto dismissed = [] {
    int v = 0;
    {
      auto guard = makeGuard([&] { v = 1; });
      guard.dismiss();
    }
    return v;
  }();
  static_assert(dismissed == 0, "dismissed guard must not run");
  EXPECT_EQ(dismissed, 0);
}

TEST(ScopeGuard, ConstexprRehire) {
  constexpr auto rehired = [] {
    int v = 0;
    {
      auto guard = makeDismissedGuard([&] { v = 1; });
      guard.rehire();
    }
    return v;
  }();
  static_assert(rehired == 1, "rehired guard must run");
  EXPECT_EQ(rehired, 1);
}

TEST(ScopeGuard, ConstexprScopeExit) {
  constexpr auto exited = [] {
    int v = 0;
    {
      SCOPE_EXIT {
        v = 1;
      };
    }
    return v;
  }();
  static_assert(exited == 1, "SCOPE_EXIT must run at end of scope");
  EXPECT_EQ(exited, 1);
}
