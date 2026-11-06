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

folly supports gcc, clang, or MSVC with C++20 support. It should run on Linux
(x86-32, x86-64, and ARM), iOS, macOS, and Windows (x86-64). The CMake build is
only tested on some of these platforms; at a minimum, we aim to support macOS
and Linux (on the latest Ubuntu LTS release or newer.)

## Build with CMake

The CMake build fetches any required dependency it cannot find installed, so a
clone and two commands are enough:

    git clone https://github.com/meta/folly
    cd folly
    cmake -B _build
    cmake --build _build

Anything already on the system is used as-is. Alternatively, install the system
packages first:

    sudo tool/fbcode_builder/getdeps.py install-system-deps --recursive

Tests are not part of the `all` target. Configure with `-DBUILD_TESTS=ON` to
build them, which also fetches googletest and gflags if they are not installed
(the library itself uses gflags only if it is installed):

    cmake -B _build -DBUILD_TESTS=ON
    cmake --build _build
    (cd _build && ctest)

Whether folly uses gflags is decided when folly is built and recorded in the
installed `folly/folly-config.h`; linking gflags into a program later does not
turn it on. Without gflags, `folly::Init` does not parse command-line flags,
`folly::gflags::FlagSaver` and `folly::gflags::SetCommandLineOption` do
nothing, and the `cli` libraries are not built. Declare folly's flags with the
`FOLLY_GFLAGS_DECLARE_*` macros from `folly/portability/GFlags.h`, not with
gflags' `DECLARE_*`, which would not match how folly defined them: without
gflags, for example, a string flag is a `std::string` rather than a reference.

OpenSSL is optional too. Without it, the libraries that need it are not built
or installed: `folly/ssl`, `AsyncSSLSocket`, `SSLContext`, `SSLOptions` and the
rest of `folly/io/async/ssl`, as well as `FmtUtility`, `hash/UniqueHashKey` and
`container/RegexMatchCache`, which hash with OpenSSL. `FOLLY_HAVE_OPENSSL` in
the installed `folly/folly-config.h` says which way folly was built. To build
OpenSSL from source when it is not installed, configure with
`-DFOLLY_BUILD_OPENSSL=ON`, which is the default when folly is built as part
of another project.

fast_float is optional with a standard library that has floating-point
`std::from_chars`, such as libstdc++ 11 or newer and libc++ 20 or newer.

`cmake --install` needs CMake 3.28 or newer once anything has been fetched,
because fetched dependencies are deliberately left out of what folly installs.
An installed folly expects its dependencies on the consumer's system instead;
`folly-config.cmake` looks them up with `find_package`.

## `getdeps.py`

`getdeps.py` also builds folly's sibling projects. Prefer the CMake build above
unless you need that.

This script is used by many of Meta's OSS tools.  It will download and build all of the necessary dependencies first, and will then invoke cmake etc to build folly.  This will help ensure that you build with relevant versions of all of the dependent libraries, taking into account what versions are installed locally on your system.

It's written in python so you'll need python3.6 or later on your PATH.  It works on Linux, macOS and Windows.

The settings for folly's cmake build are held in its getdeps manifest `tool/fbcode_builder/manifests/folly`, which you can edit locally if desired.

### Dependencies

If on Linux or MacOS (with homebrew installed) you can install system dependencies to save building them:

    # Clone the repo
    git clone https://github.com/meta/folly
    # Install dependencies
    cd folly
    sudo tool/fbcode_builder/getdeps.py install-system-deps --recursive

If you'd like to see the packages before installing them:

    tool/fbcode_builder/getdeps.py install-system-deps --dry-run --recursive

On other platforms or if on Linux and without system dependencies `getdeps.py` will mostly download and build them for you during the build step.

Some of the dependencies `getdeps.py` uses and installs are:

  * boost.
  * googletest is required to build and run folly's tests.

### Build

    # Clone the repo
    git clone https://github.com/meta/folly
    cd folly
    # Build, using system dependencies if available
    python3 tool/fbcode_builder/getdeps.py --allow-system-packages build

It puts output in its scratch area:

  * `installed/folly/lib/libfolly.a`: Library

You can also specify a `--scratch-path` argument to control
the location of the scratch directory used for the build. You can find the default scratch install location from logs or with `python3 tool/fbcode_builder/getdeps.py show-inst-dir`.

There are also
`--install-dir` and `--install-prefix` arguments to provide some more
fine-grained control of the installation directories. However, given that
folly provides no compatibility guarantees between commits we generally
recommend building and installing the libraries to a temporary location, and
then pointing your project's build at this temporary location, rather than
installing folly in the traditional system installation directories. e.g., if you are building with CMake you can use the `CMAKE_PREFIX_PATH` variable to allow CMake to find folly in this temporary installation directory when
building your project.

### Run tests

By default `getdeps.py` will build the tests for folly. To run them:

    cd folly
    python3 tool/fbcode_builder/getdeps.py --allow-system-packages test

### Iterating on a `getdeps.py` build

To invoke `cmake` again against a build getdeps made, there is a helpful `run_cmake.py` script output in the scratch-path build directory. You can find the scratch build directory from logs or with `python3 tool/fbcode_builder/getdeps.py show-build-dir`.

Running tests with ctests also works if you cd to the build dir, e.g.
`(cd $(python3 tool/fbcode_builder/getdeps.py show-build-dir) && ctest)`

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

This will create a build directory `_build` in the top-level.
