Folly: Foundational Open-source Library
=======================================

[![linux](https://github.com/meta/folly/actions/workflows/linux.yml/badge.svg)](https://github.com/meta/folly/actions/workflows/linux.yml)
[![macos](https://github.com/meta/folly/actions/workflows/macos.yml/badge.svg)](https://github.com/meta/folly/actions/workflows/macos.yml)
[![windows](https://github.com/meta/folly/actions/workflows/windows.yml/badge.svg)](https://github.com/meta/folly/actions/workflows/windows.yml)

# What is `folly`?

<img src="static/logo.svg" alt="Logo Folly" width="15%" align="right" />

Folly (originally an acronym for Facebook Open-source Library) is a
library of C++20 components designed with practicality and efficiency
in mind. **Folly contains a variety of core library components used extensively
at Meta**. In particular, it's often a dependency of Meta's other
open source C++ efforts and place where those projects can share code.

It builds on the C++ standard library and fills its gaps. We add a
component when the standard library doesn't provide it, or doesn't meet
the performance, scalability or interface Meta's code needs. When the
standard library provides an equivalent, we move folly and its users to
the standard one and keep the folly name only as long as users need it.

Performance concerns permeate much of Folly, sometimes leading to
designs that are more idiosyncratic than they would otherwise be (see
e.g. `PackedSyncPtr.h`, `SmallLocks.h`). Good performance at large
scale is a unifying theme in all of Folly.

## Check it out in the intro video
[![Explain Like I’m 5: Folly](https://img.youtube.com/vi/Wr_IfOICYSs/0.jpg)](https://www.youtube.com/watch?v=Wr_IfOICYSs)

# Logical Design

Folly is a collection of relatively independent components, some as
simple as a few symbols. There is no restriction on internal
dependencies, meaning that a given folly module may use any other
folly components.

All symbols are defined in the top-level namespace `folly`, except of
course macros. Macro names are ALL_UPPERCASE and should be prefixed
with `FOLLY_`. Namespace `folly` defines other internal namespaces
such as `internal` or `detail`. User code should not depend on symbols
in those namespaces.

# Physical Design

The sources live in the `folly/` directory, so headers are included
with the library name as a prefix, e.g. `#include <folly/Conv.h>`.

The directory structure is flat (mimicking the namespace structure),
i.e. we don't have an elaborate directory hierarchy (it is possible
this will change in future versions). The subdirectory `experimental`
contains files that are used inside folly and possibly at Meta but
not considered stable enough for client use. Your code should not use
files in `folly/experimental` lest it may break when you update Folly.

The `folly/test` subdirectory includes the unittests for all
components, usually named `ComponentXyzTest.cpp` for each
`ComponentXyz.*`. The `folly/docs` directory contains
documentation.

# What's in it?

Notable components include:

- Containers
  - [F14](folly/container/F14.md): fast hash maps and sets
  - [`small_vector`](folly/docs/small_vector.md): a vector that stores a few elements inline
  - [`sorted_vector_map` and `sorted_vector_set`](folly/container/sorted_vector_types.h): maps and sets backed by a sorted vector
  - [`ConcurrentHashMap`](folly/concurrency/ConcurrentHashMap.h): a concurrent hash map
- Concurrency
  - [`Synchronized`](folly/docs/Synchronized.md): data protected by a mutex
  - [`SharedMutex`](folly/SharedMutex.h): a fast reader-writer lock
  - [`ThreadLocal`](folly/docs/ThreadLocal.md): thread-local storage for non-trivial types
  - [Hazard pointers](folly/docs/Hazptr.md): safe memory reclamation for concurrent data structures
  - [Executors](folly/docs/Executors.md): thread pools and other executors
- Async
  - [Futures](folly/docs/Futures.md): composable futures and promises
  - [`folly::coro`](folly/coro/README.md): C++20 coroutine tasks and utilities
  - [`EventBase` and async sockets](folly/io/async/README.md): an event loop and asynchronous I/O
- I/O and data
  - [`IOBuf`](folly/io/IOBuf.h): chained, reference-counted byte buffers
  - [`dynamic` and JSON](folly/docs/Dynamic.md): a dynamically typed value with JSON support
- Utilities
  - [`Conv`](folly/docs/Conv.md): `to<T>` conversions between strings and numbers
  - [`ScopeGuard`](folly/ScopeGuard.h): code that runs when a scope exits
  - [`Function`](folly/docs/Function.md): a move-only, type-erased callable
  - [`Benchmark`](folly/docs/Benchmark.md): a microbenchmark framework

For everything else, see the [overview](folly/docs/Overview.md) and the
[`docs` folder](folly/docs).

Folly is published on GitHub at https://github.com/meta/folly.

# Build Notes

Because folly does not provide any ABI compatibility guarantees from commit to
commit, we generally recommend building folly as a static library.

folly requires C++20 and GCC 10, Clang 15 (or Apple clang 15), MSVC from Visual
Studio 2019 16.11, or newer. It should run on Linux (x86-32, x86-64, and ARM),
iOS, macOS, and Windows (x86-64). The CMake build is only tested on some of
these platforms; at a minimum, we aim to support macOS and Linux (on the latest
Ubuntu LTS release or newer.)

## Build with CMake

The CMake build fetches any required dependency it cannot find installed, so a
clone and two commands are enough:

    git clone https://github.com/meta/folly
    cd folly
    cmake -B build
    cmake --build build

Anything already on the system is used as-is. Alternatively, install the system
packages first:

    sudo tool/fbcode_builder/getdeps.py install-system-deps --recursive

Tests are not part of the `all` target. Configure with `-DBUILD_TESTS=ON` to
build them, which also fetches googletest and gflags if they are not installed
(the library itself uses gflags only if it is installed):

    cmake -B build -DBUILD_TESTS=ON
    cmake --build build
    (cd build && ctest)

### Dependencies

folly has minimal required dependencies: [fmt](https://github.com/fmtlib/fmt),
[glog](https://github.com/google/glog), [libevent](https://libevent.org/) and,
without floating-point `std::from_chars`,
[fast_float](https://github.com/fastfloat/fast_float). The CMake build fetches
any that are missing.

Optional dependencies enable more features:

  * gflags: flag parsing in `folly::Init` and `cli`.
  * OpenSSL: `folly/ssl` and `folly/io/async/ssl` (`-DFOLLY_BUILD_OPENSSL=ON`
    builds it).
  * Boost: `JSONSchema`, `RegexMatchCache`, `EXPECT_PCRE_MATCH` and
    `cli/ProgramOptions` (`-DFOLLY_FETCH_BOOST=ON` fetches it;
    `ProgramOptions` needs an installed Boost).
  * Compression codecs (zlib, bzip2, LZMA, LZ4, zstd, Snappy), libsodium,
    libunwind, libiberty, libaio and liburing.

`cmake --install` needs CMake 3.28 or newer once anything has been fetched,
because fetched dependencies are deliberately left out of what folly installs.
An installed folly expects its dependencies on the consumer's system instead;
`folly-config.cmake` looks them up with `find_package`.

## `getdeps.py`

`getdeps.py` builds folly together with its dependencies at the versions
Meta's OSS projects use, and also builds folly's sibling projects. Prefer the
CMake build above unless you need that. It needs Python 3.10 or newer and works
on Linux, macOS and Windows. folly's build settings are in its manifest,
`tool/fbcode_builder/manifests/folly`.

On Linux, or on macOS with Homebrew, install the system packages first to avoid
building them (`--dry-run` lists them without installing):

    sudo tool/fbcode_builder/getdeps.py install-system-deps --recursive

Then build and test:

    python3 tool/fbcode_builder/getdeps.py --allow-system-packages build
    python3 tool/fbcode_builder/getdeps.py --allow-system-packages test

The library ends up in `installed/folly/lib/libfolly.a` under the scratch
directory, which `--scratch-path` sets and `getdeps.py show-inst-dir` prints.
`--install-dir` and `--install-prefix` give finer control. Since folly makes no
compatibility guarantees between commits, install it to a temporary location
and point your project at it, for example with `CMAKE_PREFIX_PATH`, rather than
installing it system-wide.

To iterate, use the build directory that `getdeps.py show-build-dir` prints:
its `run_cmake.py` reruns CMake, and `ctest` there runs the tests.

## Ubuntu LTS, CentOS Stream, Fedora

Either build approach works. We test in CI on Ubuntu LTS, and occasionally on other distros.

If you find the set of system packages is not quite right for your chosen distro, you can specify distro version specific overrides in the dependency manifests (e.g. https://github.com/meta/folly/blob/main/tool/fbcode_builder/manifests/boost ). You could probably make it work on most recent Ubuntu/Debian or Fedora/Redhat derived distributions.

## Windows (Vcpkg)

Note that many tests are disabled for folly Windows builds, you can see them in the log from the cmake configure step, or by looking for WINDOWS_DISABLED in `CMakeLists.txt`

That said, Windows is tested in CI with the CMake build above, and `getdeps.py` builds work there as well.

If you prefer, you can try Vcpkg. folly is available in [Vcpkg](https://github.com/Microsoft/vcpkg#vcpkg) and releases may be built via `vcpkg install folly:x64-windows`.

You may also use `vcpkg install folly:x64-windows --head` to build against `main`.

## macOS

The CMake build is tested on macOS in CI, and `getdeps.py` builds work there too. If you prefer, you can try Homebrew.

### Homebrew

folly is available as a Formula and releases may be built via `brew install folly`.

You may also use `folly/build/bootstrap-osx-homebrew.sh` to build against `main`:

```
  folly/build/bootstrap-osx-homebrew.sh
```

This will create a build directory `build` in the top-level.
