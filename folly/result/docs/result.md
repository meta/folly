# `folly::result<T>`: error/value container & short-circuiting coroutine

This doc describes when, and how, to use the `result` class template for
error-handling in C++ programs.

This doc assumes familiarity with C++ exceptions.  It is helpful -- but
completely optional -- to have used other similar types, like `folly::Try` or
`Expected`, Rust's [`Result`](https://doc.rust-lang.org/std/result/), Haskell's
[MonadError](https://hackage.haskell.org/package/mtl-2.2.2/docs/Control-Monad-Except.html),
or Niall Douglas's C++ [`boost::outcome`
](https://www.boost.org/doc/libs/1_87_0/libs/outcome/doc/html/index.html).

Folly veterans can think of `result` as an improved `Try`, with a smooth enough
user experience to use as your main error-handling pattern in synchronous code.
For async `folly::coro` code, `value_or_error_or_stopped(fn())` /
`or_unwind(result)` / `co_result(result)` supersede `co_awaitTry(fn())` and
`co_result(Try)`.

The intent of `result` is most similar to `boost::outcome`, and indeed its
[introduction](https://www.boost.org/doc/libs/1_87_0/libs/outcome/doc/html/index.html
) describes many use-cases where `result` would also be appropriate. Unlike the
C++11 `boost::outcome`, `folly::result` is a C++20 design, making it simpler &
more usable:

  - `result` coroutines allow easy error propagation via `co_await or_unwind`,
    and provide automatic exception boundaries.  Credit: The original [C++
    short-circuiting coroutine](
    https://github.com/toby-allsopp/coroutine_monad), later reimplemented in
    [`folly::Expected`](
    https://github.com/facebook/folly/commit/0e8c7e1c97e27d96fddbdf552bc99faa22066d00).

  - `result` uses `std::exception_ptr` (with some folly-specific enhancements)
    to efficiently transport all exception types, which avoids the user-facing
    complexity of distinguishing `outcome::result` (`error_code` only) and
    `outcome::outcome` (code OR exception) -- more in `design_notes.md`.

  - `result` needs no macros, and has a much easier-to-learn API.

Although `boost::outcome` enumerates a litany of use-cases, we will focus on two
particular scenarios that are common in Meta's code.

## A tale of two use-cases

### Use-case 1: Explicit error propagation in synchronous code

Exceptions in C++ are common, unavoidable, useful, and a frequent source of bugs
(see "Exceptions are imperfect & inevitable" in `design_notes.md`). However,
some circumstances -- such as low-level systems programs, high-performance, or
high-reliability applications -- can benefit from policies along these lines:
  - never throw exceptions,
  - never let thrown exceptions escape a function,
  - handle thrown exceptions as locally as possible, etc.

`result` can reduce the effort of following such policies, and improve adherence.

An extreme of this approach is exemplified by the Linux kernel, a C-language
mega-project focused on reliability and performance.  Linux handles errors
primarily by one convention -- functions return an integer that encodes success
or error.  This approach is vulnerable to "forgetting to check the return
value", but in practice this **local** requirement is easily enforced (lint all
functions to be "nodiscard").  On the other hand, exception propagation is
inherently non-local, making it much easier to forget to draw appropriate error
boundaries.

Using `result` gives you the best of both worlds.  It does not eliminate
exceptions from C++ -- that ship has long sailed.  Instead, critical portions
of the code can be made into `result` coroutines.  These are safer & faster
than thrown exceptions, and more flexible than error codes:
  - `result` in an error state transports an `exception_ptr`, giving perfect
    interoperability with thrown exceptions and `folly::Try`.
  - You cannot forget to handle returned errors -- `result<T>` is nodiscard,
    and does not implicitly convert to `T`.  You **have** to unpack it.
  - The standard pattern for accessing the value is `co_await
    or_unwind(resultFn())`, which also **visibly and efficiently** propagates
    unhandled exceptions (and cancellation) to the caller.  To add epitaphs
    with source location tracking, use `co_await or_unwind_epitaph(
    resultFn(), "context {}", arg)` instead.
  - Handling specific exceptions via `if (auto ex = get_exception<Ex>(res))` is
    as clear as `try-catch`, and lacks the many gotchas of the
    exception-unwinding context.
  - You only need one error path -- `result` coroutines capture all internally
    thrown exceptions and pack them into the return value, so the caller can
    assume that the function won't throw (besides `std::bad_alloc` or move/copy
    constructors of its arguments).
  - Propagating errors via `result` costs O(1nsec), vs ~1µsec for `throw`.
  - `result` also propagates `folly::coro` cancellation -- and exposes it as a
    separate "stopped" state. The API is carefully designed not to add
    complexity in writing code that is transparent to cancellation. It is also
    forward-compatible with C++26 / P1677 / P2300.

This `result` coroutine showcases some common error-handling patterns:

```cpp
#include <folly/result/coro.h>

// This "result coro" works like a regular synchronous function returning
// `result`. But, on exception it acts as if its body is surrounded with:
//    try {} catch(...) { return error_or_stopped::from_current_exception(); }
result<size_t> countGrapefruitSeeds() {
  // If `getFruitBox()` returns an "error" or a "stopped" state, then
  // `co_await or_unwind` immediately propagates that result to the caller!
  auto box = co_await or_unwind(getFruitBox());
  auto boxRes = box.findFruit(FruitTypes::GRAPEFRUIT);
  if (auto ex = get_exception<RottenFruit>(boxRes)) {
    logDiscardedGrapefruit(ex); // `ex` quacks like `const RottenFruit*`
    return 0;
  }
  const auto& grapefruit = co_await or_unwind(boxRes);
  size_t numSeeds = 0;
  // `fetchSegments()` does **not** return `result<>` as a demo -- any
  // exceptions it throws are caught and captured in our return value.
  auto segments = grapefruit.fetchSegments();
  for (auto& segment : segments) {
    numSeeds += (co_await or_unwind(segment.seeds())).size();
  }
  co_return numSeeds;
}
```

Code returning `result` cleanly interoperates with async `folly::coro`
coroutines -- the syntax is exactly the same:

```cpp
auto val = co_await or_unwind(someResult());
```

Before diving in, you might review the `result` contract, best practices, and
"how-to"s below.

### Use-case 2: Exceptions without `throw` overhead in `folly::coro`

`result` brings some minor improvements over the `Try` solution to the same
problem (pre-2025). Most importantly, it helps us migrate to using
`has_stopped()` tests for cancellation, instead of the legacy pattern of
"cancellation is an exception".

Continue to use `co_nothrow` to pass **all** child exceptions to the parent
without re-throwing:

```cpp
co_await coro::co_nothrow(childTask())
```

Where handling exceptions is required, `result` can replace `Try`, with
slightly better ergonomics:

```cpp
result<int> intRes = co_await coro::value_or_error_or_stopped(
    taskReturningInt());
if (auto ex = get_exception<MyError>(intRes)) {
  // Handle `ex`, it quacks like `const MyError*`
} else {
  // `std::move()` is optional, makes error propagation cheaper
  sum += co_await or_unwind(std::move(intRes));
}
```

If you wanted to only handle specific errors, but not the value (like in a
generic retry functor), then `co_yield co_result(std::move(intRes))` will
propagate any value *or* unhandled error to the parent of the current coro.

## A summary of `result`'s contract

`result` is a bit opinionated, aiming to popularize several reliability &
efficiency best practices. Reading this contract will help you understand the
design principles.  If you want to know **why** `result` was designed this way,
check out `design_notes.md`.  For example, they offer comparisons with `folly`
prior art, like `Expected`, `Try`, and `coro::Task`.

In bullets, `result<T>`:

  - Contains one of:
    * `T` -- which can be a value or reference, or
    * `error_or_stopped` -- which either `has_stopped()`, or stores an error as
      `std::exception_ptr`, with folly-specific optimizations.  Access the
      latter via `folly::get_exception<Ex>(res)`.

    *NB*: Right now, `result` is "almost never empty, like `folly::Expected`",
    but when C++23 is widely available, it will be truly never empty.

  - Provides constructors optimized for usability. For example, unlike
    `folly::Try`, `result` is implicitly constructible from values & errors.

    *NB*: Storing an empty `std::exception_ptr` is a contract violation.  It is
    fatal in debug builds, but "works" in opt unless you hit a later assertion,
    like the `std::terminate` in `exception_wrapper::throw_exception`.

  - Is movable iff `T` is, and copyable iff `T` is.  `result<V&&>` is
    move-only (following `rvalue_reference_wrapper`).  `result<const V&>` is
    fully copyable, but `result<V&>` can only be copied from a mutable source
    (not from `const result<V&>&`) -- see `design_notes.md` for why.
    Moves save ~7ns on the error path (atomic `exception_ptr` copy).
      * For usability, copy conversion from cheap types (like `int`) is allowed.
      * WARNING: `co_await or_unwind(resOfV)` returns `V&` and so makes it easy
        to accidentally copy `V` (or the `exception_ptr`).  While unfortunate,
        this is the behavior of least surprise for C++ programmers.  Luckily,
        it is most common to await `co_await or_unwind(resFn())`, which avoids
        copies

  - Is `[[nodiscard]]`, meaning that in common usage you cannot forget to
    handle an error.  A line with just `resultFoo()` will not compile, you need
    `co_await or_unwind(resultFoo())`.

  - Integrates with `now_task`, `value_task` (or the legacy `Task`) via
      * `const auto& v = co_await or_unwind(res)` -- get `res`'s value.
        Use `std::move(res)` if the error path profiles hot.
      * `r = co_await value_or_error_or_stopped(someCoro())` -- await without
        throwing, though if you won't handle *any* errors, keep using
        `co_nothrow`.
      * `co_yield co_result(res)` propagate a value or error to your awaiter.

  - Supports migration from `Try` via `try_to_result` and `result_to_try`.

  - Uses automatic storage duration for `T`.  Allocation caveats:
      * `std::exception_ptr` stores the exception on the heap.
      * While `result` coroutines are set up to be HALO-friendly, a compiler is
        not *obligated* to allocate `result` coroutines on the stack. Profile
        first, then read "how to avoid coro frame allocations" below.

What to know about exceptions & `result`:

  - `result` coroutines (but **not** functions) are exception boundaries.
    Any uncaught exception is captured in `res.error_or_stopped()` & returned.

  - The `result` API avoids throwing, aside from:
      * `value_or_throw()`, which you should avoid in favor of `co_await`,
      * `std::bad_alloc`, which is unavoidable due to `std::exception_ptr`
        and coroutines.

  - If you need truly non-throwing code, wrap it with `result_catch_all`.

  - `result` coroutines (functions containing `co_await`, `co_return`, or
    `co_yield` ) should **NOT** be declared `noexcept` -- that will not do
    [what you expect](
    https://devblogs.microsoft.com/oldnewthing/20210426-00/?p=105153).
      * In particular, if you mark a `result` coro `noexcept`, calling it may
        `std::terminate` if an argument's copy/move constructors throws, or if
        HALO fails (it shouldn't, but compilers make no guarantees) and you hit
        `std::bad_alloc`. Those are not useful behaviors!
      * You may, of course, write a non-coroutine function that `return`s a
        `result` and is `noexcept`, but this should not be typical usage.

## `result` best practices

### Avoid `Task<result<T>>`

`coro::Task` coroutines (or safer `NowTask`, `value_task`, etc) should **not**
wrap `result`, because tasks are already capable of non-throwing error
propagation. Instead, read "how to interoperate with `Task`" below.

### Prefer `result` coroutines over non-coroutine `result` functions

This recommendation is *primarily* about consistency of expectations -- `result`
coroutines are more likely to follow a policy like "not letting exceptions fly",
since each one is an exception boundary. Secondarily, propagating errors via
`co_await or_unwind(subResultFn())` is just easier & cleaner. Compare:

```cpp
// coroutine
result<int> addFive1() { co_return 5 + co_await or_unwind(childFn()); }

// non-coroutine
result<int> addFive2() {
  auto res = childFn();
  if (auto* value = res.get_pointer()) {
    return 5 + *value;
  }
  return res.error_or_stopped(); // propagate "error" or "stopped"
}
```

### `value_or_throw()` is *only* for edge-cases

The recommended pattern for `result`-returning code is to **explicitly** return
errors, so the "throw" part is discouraged. It is also bad for performance and
readability:
  - In `result` coroutines, `res.value_or_throw()` is an inefficient & ugly way
    of writing `co_await or_unwind(res)`.
  - In `folly::coro` async coroutines, `auto v = co_await or_unwind(resultFn())`
    and `co_yield co_error(ew)` are much cheaper than throwing.

For lvalue `result`s, `get_pointer()` is the non-throwing replacement for
`has_value()` + `value_or_throw()`:

```cpp
if (auto* val = res.get_pointer()) { use(*val); }
```

Keep `value_or_throw()` for cases that intentionally cross into throwing code,
or for rvalue extraction when `co_await or_unwind(std::move(res))` does not fit.

### Avoid `co_return std::move(...)` or `return std::move(...)`

Both are implicitly movable contexts, so the `std::move` is just visual
noise, and can actually prevent NVRO for `return` (there's a linter against it).

You can directly return any of these types: `result<V>`, `V`,
convertible-to-`V`, convertible-to-`result<V>`, `error_or_stopped`, or
`stopped_result`.  None will incur unnecessary copies.

## How to...

### Handle specific errors

If you want to check for a single error, `folly::get_exception` is all you need:

```cpp
result<> handlesErrors() {
  auto r = propagatesErrors();
  if (auto* ex = get_exception<MyErr>(r)) {
    // Handle `ex`, it quacks like `const MyErr*`
  } else {
    auto v = co_await or_unwind(std::move(r)); // propagates error or stopped
  }
}
```

Chaining these via `} else if (...) {` should be fine for the vast majority of
usage. If not, "Future improvements" mentions the possibility of more efficient
multi-type dispatch.

### Store values in a `result`

The semantics of `result<Value>` are straightforward:
  - You can implicitly move values in & out -- `result r{fn()}`, then
    `co_await or_unwind(std::move(r))`.
  - You can copy `result` via `auto r2{r};` or `folly::copy(r)`.
  - To copy `Value v` into a `result`, use `result r{folly::copy(v)}`. Small
    trivially copyable `Value`s (like `int`) can be copied in implicitly.
  - Some `result<U>`s are implicitly convertible to `result<V>`. See the two
    cases in `result.h` -- "simple" and "fallible".
  - [CTAD](https://en.cppreference.com/w/cpp/language/class_template_argument_deduction
    ) works: `result r{42}` declares a `result<int>`.

### Store references in a `result`

`result` may store a reference type.  You must construct it via the
corresponding reference wrapper:

```cpp
V v{...};
result lres = std::ref(v); // result<V&>
result clres = std::cref(v); // <const V&>
result rres = folly::rref(v); // <V&&>
```

This API was chosen for two reasons:
  - It allows `result` to deduce the reference type name, reducing repetition.
  - It makes it obvious that a reference is being taken at the site that
    is returning or passing a reference, to help avoid aliasing bugs.

Future: We may later add support for `result<V&> lres = v`, if it we see that
it would significantly improve readability.

Note that it is **also** possible to construct result-of-reference wrapper, via
an explicit template parameter.  These types have worse ergonomics.  Also, you
will learn below that they don't propagate const, while `result<V&> does.

```cpp
result<std::reference_wrapper<V>> lres = std::ref(v);
result<folly::rvalue_reference_wrapper<V>> rres = folly::rref(std::move(v));
```

**WATCH OUT**: Both `rvalue_reference_wrapper<V>` and `result<V&&>` are
opinionated wrappers that strictly enforce "use-once" behavior --
use-after-move literally becomes a null pointer dereference.

Reference support allows "fallible accessors".  For example, a map may provide:

```cpp
result<Value&> at(const Key&);
```

For this use-case to make sense, `co_await or_unwind(m.at(k))` must return
`Value&`, even though the `result<Value&>` being accessed is an rvalue.

Note that `const result<V&>` only lets you access the value as `const V&`.

```cpp
std::as_const(rn).get_pointer() -> const int*
std::as_const(rn).value_or_throw() -> const int&
co_await or_unwind(std::as_const(rn)) -> const int&
```

This doesn't affect the reference-accessor use-case, but it results in safer
behavior when you write code like this:

```cpp
void logResult(const auto& res);

logResult(result<V&>{std::ref(v)});
```

If `const` didn't propagate inside `result<T&>`, then `logResult` could
accidentally mutate `v`, even though the signature looks like it shouldn't --
`design_notes.md` discusses this in-depth.

In rare scenarios, `const`-propagation may not be what you want.  Your
workaround is to store `result<V*>` or `result<std::reference_wrapper<V>>`.
One example is a read-locked map that references thread-safe values:

```cpp
Synchronized<map<K, result<std::reference_wrapper<std::atomic_int>>>> syncMap;
co_await or_unwind(syncMap.with_rlock([](auto& m) -> result<> {
  // Wouldn't compile with `result<std::atomic_int&>` due to `const` propagation
  (co_await or_unwind(m.at(key))).get().fetch_add(1);
}));
```

### `co_await or_unwind()` and value categories

Most of the time, you will await a prvalue, i.e. `co_await or_unwind(resFn())`.
For `result<Val>`, you get `Val&&`, and for `result<Ref>` you get `Ref`.

This default is good for hot error paths -- when the thing in `or_unwind()` is
an rvalue, `exception_ptr`s are propagated by move (~1ns), not by copy (~7ns).

With value `result<Val> res`, you can:
  - `co_await or_unwind(std::move(res))`: Returns `Val&&`, or moves to
    propagate the `exception_ptr` (or "stopped" state).
  - `co_await or_unwind(res)`: Returns `Val&`, or copies the `exception_ptr`.
  - `co_await or_unwind(std::as_const(res))`: Ditto, but `const Val&`.

With an lvalue ref `result<T&> res`, you can only:
  - `co_await or_unwind(res)`: Returns `T&`, or copies the `exception_ptr`.

With an rvalue ref `result<Val&&> res`, you can only:
  - `co_await or_unwind(std::move(res))`: Returns `Val&&`, or moves the
    `exception_ptr`.

In hot code, choose your approach via profiling or benchmarks.  Off the hot
path, those finer points of performance are negligible, so choose readability:
  - Prefer `[const] auto& v = co_await or_unwind(r)` for in-place access.
    This copies `exception_ptr` on error paths, but that's usually fine.
  - `co_await or_unwind(std::move(r))` if you need an r-value right away.

### Interoperate with `coro::now_task`, `value_task` (or legacy `Task`)

  - To pass all errors to the parent, use `co_await co_nothrow(childTask())`.
  - Get a task's `result` via `res = co_await
    value_or_error_or_stopped(childTask())`.
  - Get the value from a `result` via `co_await or_unwind` just as in `result`
    coros; error & stopped states propagate to the parent task.
  - In any `Task-like<T>`, `co_yield co_result(res)` cheaply forwards the
    value/error/stopped state of `result<T>` to the caller.

### Interoperate with `folly::Try`

New code should prefer `result`, since it is more flexible (e.g. reference
types), and more ergonomic (short-circuiting coroutines, usable conversions).

To interact with existing `Try` code, use the shims `result_to_try()` and
`try_to_result()`. The latter defaults to encoding empty `Try`s as
`UsingUninitializedTry` errors, though you can customize this by passing
`empty_try_with(fn)` as the second argument.

### Eliminating "coro frame overhead"

Before you read further, profile to ascertain that you **have** a coro frame
allocation problem. The heap allocations may not matter for your usage!

`result` coroutines are designed to be HALO-friendly, so they should be able to
compile to plain stack functions.  In some case, the existing "clang coro
elidable" attributes may help.  Also see LLVM PR 152623 for a more robust
solution.

If you have a compelling reason to avoid coroutines (old compiler?), you can
write plain functions that return `result<T>`, and the caller won't know the
difference, **as long as you follow the "mostly non-throwing" contract** of
`result` coros. This demonstrates non-coroutine error-handling patterns:

```cpp
#include <folly/result/result.h>

result<int> plantSeeds(int n) {
  return result_catch_all([&]() -> result<int> {
    if (n < 0) {
      return error_or_stopped{std::logic_error{"cannot plant < 0 seeds"}};
    }
    int seedsLeft = n;
    for (int i = 0; i < n; ++i) {
      auto rh = digHole(i);
      if (auto ex = get_exception<HitBigRock>(rh)) {
        continue; // skip planting this seed
      }
      if (auto* hole = rh.get_pointer()) {
        hole->plantSeed(i);
        --seedsLeft;
      } else {
        return rh.error_or_stopped(); // unhandled error or stopped
      }
    }
    return seedsLeft;
  });
}
```

## Known issue: `or_unwind(rvalue)` and dangling references

When storing the result of `co_await or_unwind(rvalue)` in a reference variable, there
is a **lifetime footgun** that current compilers do not catch:

```cpp
auto&& ref = co_await or_unwind(resFn());
use(ref); // BAD: use-after-free
```

Use one of these safe alternatives:

```cpp
// Store the result before it gets destroyed at the `;`.
auto val = co_await or_unwind(resFn()); //

// Store the result in a *movable* `or_unwind_owning`.
auto val = co_await or_unwind_owning(resFn());

// Store the result first, keeping the ref alive
auto res = resFn();
auto&& ref = co_await or_unwind(std::move(res));
```

The issue only affects `or_unwind(rvalue)` with reference binding. Using `auto`
(without `&&`) is always safe since it stores the value.

**Aside**: Why don't the `[[clang::lifetimebound]]` annotations in `or_unwind`
catch this? It turns out there's an [LLVM issue #177023](
https://github.com/llvm/llvm-project/issues/177023) with propagating
`lifetimebound` analysis through `operator co_await`. Once it is fixed, this
footgun will emit `-Wdangling`, failing sensibly-configured builds.
