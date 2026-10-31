# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.14)
# We use the GoogleTest module if it is available (only in CMake 3.9+)
# It requires CMP0054 and CMP0057 to be enabled.
if (POLICY CMP0054)
  cmake_policy(SET CMP0054 NEW)
endif ()
if (POLICY CMP0057)
  cmake_policy(SET CMP0057 NEW)
endif ()

# CMP0075 Include file check macros honor CMAKE_REQUIRED_LIBRARIES
if (POLICY CMP0075)
  cmake_policy(SET CMP0075 NEW)
endif ()

# CMP0135: URL downloads timestamp extracted files at extraction time.
if (POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif ()

# CMP0167 The FindBoost module is removed
if (POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif ()

# includes
set(CMAKE_MODULE_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/CMake" # for in-fbsource builds
    "${CMAKE_CURRENT_SOURCE_DIR}/../opensource/fbcode_builder/CMake"
    # For shipit-transformed builds
    "${CMAKE_CURRENT_SOURCE_DIR}/build/fbcode_builder/CMake"
    ${CMAKE_MODULE_PATH}
)

# package information
set(PACKAGE_NAME "folly")
if (NOT DEFINED PACKAGE_VERSION)
  set(PACKAGE_VERSION "0.58.0-dev")
endif ()
set(PACKAGE_STRING "${PACKAGE_NAME} ${PACKAGE_VERSION}")
set(PACKAGE_TARNAME "${PACKAGE_NAME}-${PACKAGE_VERSION}")
set(PACKAGE_BUGREPORT "https://github.com/facebook/folly/issues")

# 150+ tests in the root folder anyone? No? I didn't think so.
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

project(${PACKAGE_NAME} CXX C ASM)

# Set for FetchContent to skip find_package(folly)
set(folly_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    CACHE
      INTERNAL
      "folly source directory"
)

set(INCLUDE_INSTALL_DIR include
    CACHE
      STRING
      "The subdirectory where header files should be installed"
)
set(LIB_INSTALL_DIR lib
    CACHE
      STRING
      "The subdirectory where libraries should be installed"
)
set(BIN_INSTALL_DIR bin
    CACHE
      STRING
      "The subdirectory where binaries should be installed"
)
set(CMAKE_INSTALL_DIR lib/cmake/folly
    CACHE
      STRING
      "The subdirectory where CMake package config files should be installed"
)

option(
  BUILD_SHARED_LIBS
  "If enabled, build folly as a shared library.  \
  This is generally discouraged, since folly does not commit to having \
  a stable ABI."
  OFF
)
option(
  PYTHON_EXTENSIONS
  "Build Python Bindings for Folly, requires Cython and (BUILD_SHARED_LIBS=ON)"
  OFF
)
# Python extensions require shared libraries for proper symbol resolution
if (PYTHON_EXTENSIONS)
  set(BUILD_SHARED_LIBS ON
      CACHE
        BOOL
        "Forced ON because PYTHON_EXTENSIONS requires shared libraries"
        FORCE
  )
endif ()
set(PYTHON_PACKAGE_INSTALL_DIR ""
    CACHE
      STRING
      "Installation directory for folly Python packages. \
  If empty, CMAKE_INSTALL_PREFIX will be used. \
  No effect if PYTHON_EXTENSIONS=OFF."
)
# Mark BUILD_SHARED_LIBS as an "advanced" option, since enabling it
# is generally discouraged.
mark_as_advanced(BUILD_SHARED_LIBS)
set(FOLLY_SUPPORT_SHARED_LIBRARY "${BUILD_SHARED_LIBS}")

include(FBBuildOptions)
fb_activate_static_library_option()

if (NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  message(STATUS "setting C++ standard to C++${CMAKE_CXX_STANDARD}")
endif ()

if (NOT DEFINED IS_X86_64_ARCH
    AND ${CMAKE_SYSTEM_PROCESSOR} MATCHES "x86_64|AMD64"
)
  set(IS_X86_64_ARCH TRUE)
else ()
  set(IS_X86_64_ARCH FALSE)
endif ()

if (NOT DEFINED IS_AARCH64_ARCH AND ${CMAKE_SYSTEM_PROCESSOR} MATCHES "aarch64")
  set(IS_AARCH64_ARCH TRUE)
else ()
  set(IS_AARCH64_ARCH FALSE)
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
  # Check target architecture
  if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Folly requires a 64bit target architecture.")
  endif ()

  if (MSVC_VERSION LESS 1900)
    message(
      FATAL_ERROR
        "This build script only supports building Folly on 64-bit Windows with "
        "at least Visual Studio 2017. "
        "MSVC version '${MSVC_VERSION}' is not supported."
    )
  endif ()
endif ()

if (NOT DEFINED FOLLY_HAVE_INT128_T)
  include(CheckTypeSize)
  check_type_size("__int128_t" FOLLY_INT128_T_SIZE)
  if (HAVE_FOLLY_INT128_T_SIZE)
    set(FOLLY_HAVE_INT128_T TRUE)
  endif ()
endif ()

set(TOP_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(FOLLY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/folly")
set(FOLLY_DIR_PREFIXES "${CMAKE_CURRENT_SOURCE_DIR}:${CMAKE_CURRENT_BINARY_DIR}"
)
# https://gitlab.kitware.com/cmake/cmake/-/issues/18580#note_1405108
string(REGEX REPLACE "(.)" "\\\\\\1" FOLLY_DIR_REGEX_ESCAPED "${FOLLY_DIR}")

include(GNUInstallDirs)

set(CMAKE_THREAD_PREFER_PTHREAD ON)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
set(FOLLY_HAVE_PTHREAD "${CMAKE_USE_PTHREADS_INIT}")
list(APPEND CMAKE_REQUIRED_LIBRARIES Threads::Threads)
list(APPEND FOLLY_LINK_LIBRARIES Threads::Threads)

if (MSVC)
  include(FollyCompilerMSVC)
else ()
  include(FollyCompilerUnix)
endif ()
include(FollyFunctions)

# A parent project's BUILD_TESTS is for its own tests. Set before option(),
# which then adds no BUILD_TESTS to the parent's cache.
if (NOT CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(BUILD_TESTS OFF)
endif ()
# Defined before folly-deps, which requires gflags only for these.
option(BUILD_TESTS "If enabled, compile the tests." OFF)
option(BUILD_BENCHMARKS "If enabled, compile the benchmarks." OFF)

include(folly-deps) # Find the required packages

# A shared folly links what it was built against, and a fetched dependency
# exists only in this build tree, so the installed libraries would point at
# files that are not there. Build and test such a tree, but do not offer to
# install it. A static folly links none of them, so it installs as before and
# folly-config.cmake finds the consumer's own copies.
get_property(folly_fetched_deps GLOBAL PROPERTY FOLLY_FETCHED_DEPS)
if (BUILD_SHARED_LIBS
    AND folly_fetched_deps AND NOT DEFINED CMAKE_SKIP_INSTALL_RULES
)
  list(JOIN folly_fetched_deps ", " folly_fetched_deps_text)
  message(
    STATUS
      "Not installable, so install rules are off. Install these and reconfigure "
      "for an installable shared folly: ${folly_fetched_deps_text}"
  )
  set(CMAKE_SKIP_INSTALL_RULES ON)
endif ()

include(FollyConfigChecks)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/CMake/folly-config.h.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/folly/folly-config.h
)

# Define FOLLY_XLOG_STRIP_PREFIXES when compiling our sources so that
# folly/logging will automatically choose the correct log category names,
# using only the relative portion of the source file name inside the
# folly repository.
set_property(
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  APPEND
  PROPERTY
    COMPILE_DEFINITIONS
    "FOLLY_XLOG_STRIP_PREFIXES=\"${CMAKE_SOURCE_DIR}:${CMAKE_BINARY_DIR}\""
)

# Collect headers for installation (still needed for install step)
auto_sources(hfiles "*.h" "RECURSE" "${FOLLY_DIR}")
remove_matches_from_lists(
  hfiles
  MATCHES
  "^${FOLLY_DIR_REGEX_ESCAPED}/build/"
  "^${FOLLY_DIR_REGEX_ESCAPED}/docs/examples/"
  "^${FOLLY_DIR_REGEX_ESCAPED}/logging/example/"
  "^${FOLLY_DIR_REGEX_ESCAPED}/(.*/)?test/"
  "^${FOLLY_DIR_REGEX_ESCAPED}/(.*/)?tool/"
  "^${FOLLY_DIR_REGEX_ESCAPED}/facebook/"
  "^${FOLLY_DIR_REGEX_ESCAPED}/rust/"
  "^${FOLLY_DIR_REGEX_ESCAPED}/ext/buck2/"
)
if (NOT FOLLY_BUILD_CLI_PROGRAM_OPTIONS)
  list(REMOVE_ITEM hfiles ${FOLLY_DIR}/cli/NestedCommandLineApp.h
       ${FOLLY_DIR}/cli/ProgramOptions.h)
endif ()
# Add test utility headers that are part of public API
list(
  APPEND
  hfiles
  ${FOLLY_DIR}/container/test/F14TestUtil.h
  ${FOLLY_DIR}/container/test/TrackingTypes.h
  ${FOLLY_DIR}/io/async/test/AsyncSSLSocketTest.h
  ${FOLLY_DIR}/io/async/test/AsyncSocketTest.h
  ${FOLLY_DIR}/io/async/test/AsyncSocketTest2.h
  ${FOLLY_DIR}/io/async/test/BlockingSocket.h
  ${FOLLY_DIR}/io/async/test/CallbackStateEnum.h
  ${FOLLY_DIR}/io/async/test/ConnCallback.h
  ${FOLLY_DIR}/io/async/test/MockAsyncSocket.h
  ${FOLLY_DIR}/io/async/test/MockAsyncServerSocket.h
  ${FOLLY_DIR}/io/async/test/MockAsyncSSLSocket.h
  ${FOLLY_DIR}/io/async/test/MockAsyncTransport.h
  ${FOLLY_DIR}/io/async/test/MockAsyncUDPSocket.h
  ${FOLLY_DIR}/io/async/test/MockTimeoutManager.h
  ${FOLLY_DIR}/io/async/test/ScopedBoundPort.h
  ${FOLLY_DIR}/io/async/test/SocketPair.h
  ${FOLLY_DIR}/io/async/test/TFOUtil.h
  ${FOLLY_DIR}/io/async/test/TestSSLServer.h
  ${FOLLY_DIR}/io/async/test/TimeUtil.h
  ${FOLLY_DIR}/io/async/test/UndelayedDestruction.h
  ${FOLLY_DIR}/io/async/test/Util.h
  ${FOLLY_DIR}/logging/test/TestLogHandler.h
  ${FOLLY_DIR}/synchronization/test/Barrier.h
  ${FOLLY_DIR}/synchronization/test/Semaphore.h
  ${FOLLY_DIR}/test/DeterministicSchedule.h
  ${FOLLY_DIR}/test/TestUtils.h
)

if (FOLLY_USE_JEMALLOC)
  # Consumers may include jemalloc before a Folly header.
  target_compile_definitions(folly_deps INTERFACE JEMALLOC_NO_DEMANGLE)
endif ()

# Generate pkg-config variables from folly_deps before we add our own
# build/install-time include directory generator expressions
include(GenPkgConfig)
gen_pkgconfig_vars(FOLLY_PKGCONFIG folly_deps)

target_include_directories(
  folly_deps
  BEFORE
  INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)
target_include_directories(folly_deps INTERFACE $<INSTALL_INTERFACE:include>)

# =============================================================================
# Build granular libraries from subdirectory CMakeLists.txt files
# Sources are compiled ONCE via OBJECT libraries, then reused for both
# granular .a files and the monolithic libfolly.a
# =============================================================================

# Check for architecture-specific compiler flags needed by subdirectories
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-mpclmul COMPILER_HAS_M_PCLMUL)

# GCC coroutines support
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  check_cxx_compiler_flag(-fcoroutines COMPILER_HAS_F_COROUTINES)
  if (COMPILER_HAS_F_COROUTINES)
    message(STATUS "GCC has C++ coroutines support, enabling for Folly.")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fcoroutines>)
  else ()
    message(STATUS "GCC lacks C++ coroutines, disabling Folly coroutines.")
  endif ()
endif ()

add_subdirectory(folly)

# Create monolithic folly library from all component OBJECT libraries
folly_create_monolithic_library()

# Resolve deferred dependencies (must be after monolithic library is created)
folly_resolve_deferred_dependencies()

# Define FOLLY_CERTS_DIR for the granular test_util target
# This points to the SSL test certificates used by TestSSLServer and related tests
if (TARGET folly_testing_test_util)
  target_compile_definitions(
    folly_testing_test_util
    INTERFACE FOLLY_CERTS_DIR="${FOLLY_DIR}/io/async/test/certs"
  )
endif ()

set(FOLLY_INSTALL_TARGETS folly folly_deps)

# Test utilities exported for use by downstream projects
add_library(
  folly_test_util
  ${FOLLY_DIR}/test/DeterministicSchedule.cpp
  ${FOLLY_DIR}/json/JsonTestUtil.cpp
  ${FOLLY_DIR}/io/async/test/TimeUtil.cpp
  ${FOLLY_DIR}/logging/test/TestLogHandler.cpp
)
add_library(Folly::folly_test_util ALIAS folly_test_util)
target_compile_definitions(
  folly_test_util
  PUBLIC FOLLY_CERTS_DIR="${FOLLY_DIR}/io/async/test/certs"
)
set_property(TARGET folly_test_util PROPERTY VERSION ${PACKAGE_VERSION})
target_link_libraries(folly_test_util PUBLIC folly ${LIBGMOCK_LIBRARIES})
apply_folly_compile_options_to_target(folly_test_util)
list(APPEND FOLLY_INSTALL_TARGETS folly_test_util)

# The main() of folly's tests, for downstream test executables. Each target
# that links this compiles TestMain.cpp itself, against its own GoogleTest.
add_library(folly_test_main INTERFACE)
add_library(Folly::folly_test_main ALIAS folly_test_main)
target_sources(
  folly_test_main
  INTERFACE
    $<BUILD_INTERFACE:${FOLLY_DIR}/test/common/TestMain.cpp>
    $<INSTALL_INTERFACE:${INCLUDE_INSTALL_DIR}/folly/test/common/TestMain.cpp>
)
target_link_libraries(folly_test_main INTERFACE folly)
list(APPEND FOLLY_INSTALL_TARGETS folly_test_main)
install(
  FILES
  ${FOLLY_DIR}/test/common/TestMain.cpp
  DESTINATION ${INCLUDE_INSTALL_DIR}/folly/test/common
)

install(
  TARGETS
  ${FOLLY_INSTALL_TARGETS}
  EXPORT folly
  RUNTIME
    DESTINATION bin
  LIBRARY
    DESTINATION ${LIB_INSTALL_DIR}
  ARCHIVE
    DESTINATION ${LIB_INSTALL_DIR}
)

auto_install_files(folly ${FOLLY_DIR} ${hfiles})
install(
  FILES
  ${CMAKE_CURRENT_BINARY_DIR}/folly/folly-config.h
  DESTINATION ${INCLUDE_INSTALL_DIR}/folly
  COMPONENT dev
)

# Generate the folly-config.cmake file for installation so that
# downstream projects that use on folly can easily depend on it in their CMake
# files using "find_package(folly CONFIG)"
include(CMakePackageConfigHelpers)
configure_package_config_file(
  CMake/folly-config.cmake.in
  folly-config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_DIR}
  PATH_VARS
    INCLUDE_INSTALL_DIR
    CMAKE_INSTALL_DIR
)
install(
  FILES
  ${CMAKE_CURRENT_BINARY_DIR}/folly-config.cmake
  DESTINATION ${CMAKE_INSTALL_DIR}
  COMPONENT dev
)
install(
  EXPORT
  folly
  DESTINATION ${CMAKE_INSTALL_DIR}
  NAMESPACE Folly::
  FILE folly-targets.cmake
  COMPONENT dev
)

# Generate a pkg-config file so that downstream projects that don't use
# CMake can depend on folly using pkg-config.
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/CMake/libfolly.pc.in
  ${CMAKE_CURRENT_BINARY_DIR}/libfolly.pc.gen
  @ONLY
)

# Specify target to allow resolving generator expressions requiring
# a target for CMake >=3.19. See #1414.
# VERSION_GREATER_EQUAL isn't available before CMake 3.7.
if (NOT CMAKE_VERSION VERSION_LESS 3.19)
  set(target_arg TARGET folly_deps)
endif ()

file(
  GENERATE
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/libfolly.pc
  INPUT
    ${CMAKE_CURRENT_BINARY_DIR}/libfolly.pc.gen
    ${target_arg}
)
install(
  FILES
  ${CMAKE_CURRENT_BINARY_DIR}/libfolly.pc
  DESTINATION ${LIB_INSTALL_DIR}/pkgconfig
  COMPONENT dev
)

option(
  BUILD_BROKEN_TESTS
  "If enabled, compile tests that are known to be broken."
  OFF
)
option(
  BUILD_HANGING_TESTS
  "If enabled, compile tests that are known to hang."
  OFF
)
option(
  BUILD_SLOW_TESTS
  "If enabled, compile tests that take a while to run in debug mode."
  OFF
)
if (BUILD_TESTS OR BUILD_BENCHMARKS)
  option(
    USE_CMAKE_GOOGLE_TEST_INTEGRATION
    "If enabled, use the google test integration included in CMake."
    ON
  )
  # So that find_package(GTest) finds a fetched googletest too.
  fb_find_or_fetch_gmock(OVERRIDE_FIND_PACKAGE)
  find_package(GTest MODULE REQUIRED)
  if (USE_CMAKE_GOOGLE_TEST_INTEGRATION)
    include(GoogleTest OPTIONAL RESULT_VARIABLE HAVE_CMAKE_GTEST)
    enable_testing()
  else ()
    set(HAVE_CMAKE_GTEST OFF)
  endif ()

  add_library(
    folly_test_support
    ${FOLLY_DIR}/test/common/TestMain.cpp
    ${FOLLY_DIR}/test/FBVectorTestUtil.cpp
    ${FOLLY_DIR}/test/DeterministicSchedule.cpp
    ${FOLLY_DIR}/test/DeterministicSchedule.h
    ${FOLLY_DIR}/test/SingletonTestStructs.cpp
    ${FOLLY_DIR}/test/SocketAddressTestHelper.cpp
    ${FOLLY_DIR}/test/SocketAddressTestHelper.h
    ${FOLLY_DIR}/compression/test/CodingTestUtils.cpp
    ${FOLLY_DIR}/container/test/F14TestUtil.h
    ${FOLLY_DIR}/container/test/TrackingTypes.h
    ${FOLLY_DIR}/futures/test/TestExecutor.cpp
    ${FOLLY_DIR}/futures/test/TestExecutor.h
    ${FOLLY_DIR}/io/async/test/BlockingSocket.h
    ${FOLLY_DIR}/io/async/test/CallbackStateEnum.h
    ${FOLLY_DIR}/io/async/test/ConnCallback.h
    ${FOLLY_DIR}/io/async/test/MockAsyncServerSocket.h
    ${FOLLY_DIR}/io/async/test/MockAsyncSocket.h
    ${FOLLY_DIR}/io/async/test/MockAsyncSSLSocket.h
    ${FOLLY_DIR}/io/async/test/MockAsyncTransport.h
    ${FOLLY_DIR}/io/async/test/MockAsyncUDPSocket.h
    ${FOLLY_DIR}/io/async/test/MockTimeoutManager.h
    ${FOLLY_DIR}/io/async/test/ScopedBoundPort.cpp
    ${FOLLY_DIR}/io/async/test/ScopedBoundPort.h
    ${FOLLY_DIR}/io/async/test/SocketPair.cpp
    ${FOLLY_DIR}/io/async/test/SocketPair.h
    ${FOLLY_DIR}/io/async/test/SSLUtil.cpp
    ${FOLLY_DIR}/io/async/test/SSLUtil.h
    ${FOLLY_DIR}/io/async/test/TFOUtil.cpp
    ${FOLLY_DIR}/io/async/test/TFOUtil.h
    ${FOLLY_DIR}/io/async/test/TestSSLServer.cpp
    ${FOLLY_DIR}/io/async/test/TestSSLServer.h
    ${FOLLY_DIR}/io/async/test/TimeUtil.cpp
    ${FOLLY_DIR}/io/async/test/TimeUtil.h
    ${FOLLY_DIR}/io/async/test/UndelayedDestruction.h
    ${FOLLY_DIR}/io/async/test/Util.h
    ${FOLLY_DIR}/logging/test/ConfigHelpers.cpp
    ${FOLLY_DIR}/logging/test/ConfigHelpers.h
    ${FOLLY_DIR}/logging/test/TestLogHandler.cpp
    ${FOLLY_DIR}/logging/test/TestLogHandler.h
  )
  target_compile_definitions(folly_test_support PUBLIC ${LIBGMOCK_DEFINES})
  target_include_directories(
    folly_test_support
    SYSTEM
    PUBLIC
      ${LIBGMOCK_INCLUDE_DIR}
      ${GTEST_INCLUDE_DIRS}
  )
  target_link_libraries(
    folly_test_support
    PUBLIC
      follybenchmark
      folly
      ${LIBGMOCK_LIBRARIES}
      glog::glog
  )
  apply_folly_compile_options_to_target(folly_test_support)

  folly_define_tests(
    DIRECTORY
    algorithm/simd/detail/test/
    TEST
    algorithm_simd_detail_simd_any_of_test
    SOURCES
    SimdAnyOfTest.cpp
    TEST
    algorithm_simd_detail_simd_for_each_test
    SOURCES
    SimdForEachTest.cpp
    TEST
    algorithm_simd_detail_simd_traits_test
    SOURCES
    TraitsTest.cpp
    TEST
    algorithm_simd_detail_unroll_utils_test
    SOURCES
    UnrollUtilsTest.cpp
    DIRECTORY
    algorithm/simd/test/
    TEST
    algorithm_simd_contains_test
    SOURCES
    ContainsTest.cpp
    TEST
    algorithm_simd_find_first_of_test
    SOURCES
    find_first_of_test.cpp
    TEST
    algorithm_simd_find_fixed_test
    SOURCES
    FindFixedTest.cpp
    TEST
    algorithm_simd_movemask_test
    SOURCES
    MovemaskTest.cpp
    DIRECTORY
    chrono/test/
    TEST
    chrono_conv_test
    WINDOWS_DISABLED
    SOURCES
    ConvTest.cpp
    DIRECTORY
    cli/test/
    TEST
    cli_args_test
    SOURCES
    ArgsTest.cpp
    DIRECTORY
    codec/test/
    TEST
    codec_hex_test
    SOURCES
    hex_test.cpp
    TEST
    codec_uuid_test
    SOURCES
    UuidTest.cpp
    DIRECTORY
    compression/test/
    TEST
    compression_compression_test
    SLOW
    SOURCES
    CompressionTest.cpp
    TEST
    compression_quotient_multiset_test
    SOURCES
    QuotientMultiSetTest.cpp
    TEST
    compression_select64_test
    SOURCES
    Select64Test.cpp
    DIRECTORY
    compression/elias_fano/test/
    TEST
    compression_alias_fano_bit_vector_coding_test
    SOURCES
    BitVectorCodingTest.cpp
    TEST
    compression_alias_fano_elias_fano_test
    SOURCES
    EliasFanoCodingTest.cpp
    DIRECTORY
    container/test/
    TEST
    container_access_test
    SOURCES
    AccessTest.cpp
    TEST
    container_array_test
    SOURCES
    ArrayTest.cpp
    BENCHMARK
    container_bit_iterator_bench
    SOURCES
    BitIteratorBench.cpp
    TEST
    container_bit_iterator_test
    SOURCES
    BitIteratorTest.cpp
    TEST
    container_enumerate_test
    SOURCES
    EnumerateTest.cpp
    BENCHMARK
    container_evicting_cache_map_bench
    SOURCES
    EvictingCacheMapBench.cpp
    TEST
    container_evicting_cache_map_test
    SOURCES
    EvictingCacheMapTest.cpp
    TEST
    container_f14_fwd_test
    SOURCES
    F14FwdTest.cpp
    TEST
    container_f14_map_test
    SOURCES
    F14MapTest.cpp
    TEST
    container_f14_set_test
    SOURCES
    F14SetTest.cpp
    BENCHMARK
    container_fbvector_benchmark
    SOURCES
    FBVectorBenchmark.cpp
    HEADERS
    FBVectorBenchmarks.cpp.h
    TEST
    container_fbvector_test
    SOURCES
    FBVectorTest.cpp
    BENCHMARK
    container_foreach_benchmark
    SOURCES
    ForeachBenchmark.cpp
    TEST
    container_foreach_test
    SOURCES
    ForeachTest.cpp
    BENCHMARK
    container_hash_maps_bench
    SOURCES
    HashMapsBench.cpp
    TEST
    container_heap_vector_types_test
    SOURCES
    heap_vector_types_test.cpp
    TEST
    container_map_util_test
    WINDOWS_DISABLED
    SOURCES
    MapUtilTest.cpp
    TEST
    container_merge_test
    SOURCES
    MergeTest.cpp
    TEST
    container_regex_match_cache_test
    SOURCES
    RegexMatchCacheTest.cpp
    TEST
    container_small_vector_test
    WINDOWS_DISABLED
    SOURCES
    small_vector_test.cpp
    TEST
    container_sorted_vector_types_test
    SOURCES
    sorted_vector_test.cpp
    TEST
    container_std_bitset_test
    SOURCES
    StdBitsetTest.cpp
    BENCHMARK
    container_sparse_byte_set_benchmark
    SOURCES
    SparseByteSetBenchmark.cpp
    TEST
    container_sparse_byte_set_test
    SOURCES
    SparseByteSetTest.cpp
    TEST
    container_util_test
    SOURCES
    UtilTest.cpp
    DIRECTORY
    concurrency/container/test/
    TEST
    concurrency_container_lock_free_ring_buffer_test
    SOURCES
    LockFreeRingBufferTest.cpp
    DIRECTORY
    concurrency/test/
    TEST
    concurrency_atomic_shared_ptr_test
    SOURCES
    AtomicSharedPtrTest.cpp
    TEST
    concurrency_cache_locality_test
    WINDOWS_DISABLED
    SOURCES
    CacheLocalityTest.cpp
    TEST
    concurrency_core_cached_shared_ptr_test
    SOURCES
    CoreCachedSharedPtrTest.cpp
    BENCHMARK
    concurrency_concurrent_hash_map_bench
    WINDOWS_DISABLED
    SOURCES
    ConcurrentHashMapBench.cpp
    TEST
    concurrency_concurrent_hash_map_stress_test
    SLOW
    WINDOWS_DISABLED
    SOURCES
    ConcurrentHashMapStressTest.cpp
    TEST
    concurrency_concurrent_hash_map_test
    WINDOWS_DISABLED
    SOURCES
    ConcurrentHashMapTest.cpp
    TEST
    concurrency_dynamic_bounded_queue_test
    WINDOWS_DISABLED
    SOURCES
    DynamicBoundedQueueTest.cpp
    TEST
    concurrency_priority_unbounded_queue_set_test
    SOURCES
    PriorityUnboundedQueueSetTest.cpp
    BENCHMARK
    concurrency_thread_cached_synchronized_bench
    SOURCES
    ThreadCachedSynchronizedBench.cpp
    TEST
    concurrency_thread_cached_synchronized_test
    SOURCES
    ThreadCachedSynchronizedTest.cpp
    TEST
    concurrency_unbounded_queue_test
    SOURCES
    UnboundedQueueTest.cpp
    DIRECTORY
    detail/test/
    TEST
    detail_simple_simd_string_utils_test
    SOURCES
    SimpleSimdStringUtilsTest.cpp
    TEST
    detail_split_string_simd_test
    WINDOWS_DISABLED
    SOURCES
    SplitStringSimdTest.cpp
    TEST
    detail_static_singleton_manager_test
    SOURCES
    StaticSingletonManagerTest.cpp
    TEST
    detail_type_list_test
    SOURCES
    TypeListTest.cpp
    DIRECTORY
    detail/base64_detail/test/
    TEST
    detail_base64_detail_test
    WINDOWS_DISABLED
    SOURCES
    Base64AgainstScalarTest.cpp
    Base64PlatformTest.cpp
    Base64SpecialCasesTest.cpp
    DIRECTORY
    executors/test/
    TEST
    executors_async_helpers_test
    SOURCES
    AsyncTest.cpp
    TEST
    executors_codel_test
    WINDOWS_DISABLED
    SOURCES
    CodelTest.cpp
    TEST
    executors_executor_test
    SOURCES
    ExecutorTest.cpp
    TEST
    executors_fiber_io_executor_test
    SOURCES
    FiberIOExecutorTest.cpp
    # FunctionSchedulerTest has a lot of timing-dependent checks,
    # and tends to fail on heavily loaded systems.
    TEST
    executors_function_scheduler_test
    BROKEN
    SOURCES
    FunctionSchedulerTest.cpp
    TEST
    executors_global_executor_test
    SOURCES
    GlobalExecutorTest.cpp
    TEST
    executors_serial_executor_test
    SOURCES
    SerialExecutorTest.cpp
    # Fails in ThreadPoolExecutorTest.RequestContext:719 data2 != nullptr
    TEST
    executors_thread_pool_executor_test
    BROKEN
    WINDOWS_DISABLED
    SOURCES
    ThreadPoolExecutorTest.cpp
    TEST
    executors_threaded_executor_test
    SOURCES
    ThreadedExecutorTest.cpp
    TEST
    executors_timed_drivable_executor_test
    SOURCES
    TimedDrivableExecutorTest.cpp
    DIRECTORY
    executors/task_queue/test/
    TEST
    executors_task_queue_priority_unbounded_blocking_queue_test
    SOURCES
    PriorityUnboundedBlockingQueueTest.cpp
    BENCHMARK
    executors_task_queue_unbounded_blocking_queue_bench
    SOURCES
    UnboundedBlockingQueueBench.cpp
    TEST
    executors_task_queue_unbounded_blocking_queue_test
    SOURCES
    UnboundedBlockingQueueTest.cpp #DIRECTORY experimental/test/
    #TEST nested_command_line_app_test SOURCES NestedCommandLineAppTest.cpp
    #TEST program_options_test SOURCES ProgramOptionsTest.cpp
    # Depends on liburcu
    #TEST read_mostly_shared_ptr_test SOURCES ReadMostlySharedPtrTest.cpp
    #TEST ref_count_test SOURCES RefCountTest.cpp
    #DIRECTORY experimental/io/test/
    DIRECTORY
    external/farmhash/test/
    TEST
    external_farmhash_farmhash_test
    SOURCES
    farmhash_test.cpp
    DIRECTORY
    logging/test/
    TEST
    logging_async_file_writer_test
    WINDOWS_DISABLED
    SOURCES
    AsyncFileWriterTest.cpp
    TEST
    logging_autotimer_test
    SOURCES
    AutoTimerTest.cpp
    TEST
    logging_config_parser_test
    SOURCES
    ConfigParserTest.cpp
    TEST
    logging_config_update_test
    SOURCES
    ConfigUpdateTest.cpp
    TEST
    logging_file_handler_factory_test
    WINDOWS_DISABLED
    SOURCES
    FileHandlerFactoryTest.cpp
    TEST
    logging_glog_formatter_test
    SOURCES
    GlogFormatterTest.cpp
    TEST
    logging_immediate_file_writer_test
    SOURCES
    ImmediateFileWriterTest.cpp
    TEST
    logging_log_category_test
    SOURCES
    LogCategoryTest.cpp
    TEST
    logging_logger_db_test
    SOURCES
    LoggerDBTest.cpp
    TEST
    logging_logger_test
    WINDOWS_DISABLED
    SOURCES
    LoggerTest.cpp
    TEST
    logging_log_level_test
    SOURCES
    LogLevelTest.cpp
    TEST
    logging_log_message_test
    SOURCES
    LogMessageTest.cpp
    TEST
    logging_log_name_test
    SOURCES
    LogNameTest.cpp
    TEST
    logging_log_stream_test
    SOURCES
    LogStreamTest.cpp
    TEST
    logging_rate_limiter_test
    SOURCES
    RateLimiterTest.cpp
    TEST
    logging_standard_log_handler_test
    SOURCES
    StandardLogHandlerTest.cpp
    TEST
    logging_xlog_test
    WINDOWS_DISABLED
    HEADERS
    XlogHeader1.h
    XlogHeader2.h
    SOURCES
    XlogFile1.cpp
    XlogFile2.cpp
    XlogTest.cpp
    DIRECTORY
    fibers/test/ # FiberManager swapWithException fails with segfault
    BENCHMARK
    fibers_fibers_benchmark
    WINDOWS_DISABLED
    SOURCES
    FibersBenchmark.cpp
    TEST
    fibers_fibers_test
    BROKEN
    SOURCES
    FibersTest.cpp
    DIRECTORY
    functional/test/
    TEST
    functional_apply_tuple_test
    WINDOWS_DISABLED
    SOURCES
    ApplyTupleTest.cpp
    TEST
    functional_invoke_test
    WINDOWS_DISABLED
    SOURCES
    InvokeTest.cpp
    TEST
    functional_protocol_test
    SOURCES
    protocol_test.cpp
    TEST
    functional_traits_test
    SOURCES
    traits_test.cpp
    DIRECTORY
    futures/test/
    TEST
    futures_barrier_test
    SOURCES
    BarrierTest.cpp
    TEST
    futures_callback_lifetime_test
    SOURCES
    CallbackLifetimeTest.cpp
    TEST
    futures_collect_test
    SOURCES
    CollectTest.cpp
    TEST
    futures_context_test
    SOURCES
    ContextTest.cpp
    TEST
    futures_core_test
    SOURCES
    CoreTest.cpp
    TEST
    futures_ensure_test
    SOURCES
    EnsureTest.cpp
    TEST
    futures_filter_test
    SOURCES
    FilterTest.cpp
    TEST
    futures_future_splitter_test
    SOURCES
    FutureSplitterTest.cpp
    BENCHMARK
    futures_benchmark
    WINDOWS_DISABLED
    SOURCES
    Benchmark.cpp
    TEST
    futures_future_test
    WINDOWS_DISABLED
    SOURCES
    FutureTest.cpp
    TEST
    futures_header_compile_test
    SOURCES
    HeaderCompileTest.cpp
    TEST
    futures_interrupt_test
    SOURCES
    InterruptTest.cpp
    TEST
    futures_map_test
    SOURCES
    MapTest.cpp
    TEST
    futures_non_copyable_lambda_test
    SOURCES
    NonCopyableLambdaTest.cpp
    TEST
    futures_poll_test
    SOURCES
    PollTest.cpp
    TEST
    futures_promise_test
    SOURCES
    PromiseTest.cpp
    TEST
    futures_reduce_test
    SOURCES
    ReduceTest.cpp
    TEST
    futures_retrying_test
    SOURCES
    RetryingTest.cpp
    TEST
    futures_self_destruct_test
    SOURCES
    SelfDestructTest.cpp
    TEST
    futures_shared_promise_test
    SOURCES
    SharedPromiseTest.cpp
    TEST
    futures_test_executor_test
    SOURCES
    TestExecutorTest.cpp
    TEST
    futures_then_compile_test
    HEADERS
    ThenCompileTest.h
    SOURCES
    ThenCompileTest.cpp
    TEST
    futures_then_test
    SOURCES
    ThenTest.cpp
    TEST
    futures_timekeeper_test
    WINDOWS_DISABLED
    SOURCES
    TimekeeperTest.cpp
    TEST
    futures_times_test
    SOURCES
    TimesTest.cpp
    TEST
    futures_unwrap_test
    SOURCES
    UnwrapTest.cpp
    TEST
    futures_via_test
    SOURCES
    ViaTest.cpp
    TEST
    futures_wait_test
    SOURCES
    WaitTest.cpp
    TEST
    futures_when_test
    SOURCES
    WhenTest.cpp
    TEST
    futures_while_do_test
    SOURCES
    WhileDoTest.cpp
    TEST
    futures_will_equal_test
    SOURCES
    WillEqualTest.cpp
    TEST
    futures_window_test
    WINDOWS_DISABLED
    SOURCES
    WindowTest.cpp
    DIRECTORY
    gen/test/ # MSVC bug can't resolve initializer_list constructor properly
    TEST
    gen_base_test
    WINDOWS_DISABLED
    SOURCES
    BaseTest.cpp
    TEST
    gen_combine_test
    SOURCES
    CombineTest.cpp
    BENCHMARK
    gen_parallel_map_benchmark
    SOURCES
    ParallelMapBenchmark.cpp
    TEST
    gen_parallel_map_test
    SOURCES
    ParallelMapTest.cpp
    BENCHMARK
    gen_parallel_benchmark
    SOURCES
    ParallelBenchmark.cpp
    TEST
    gen_parallel_test
    SOURCES
    ParallelTest.cpp
    DIRECTORY
    hash/test/
    BENCHMARK
    hash_checksum_benchmark
    SOURCES
    ChecksumBenchmark.cpp # builds, but tests fail on MSVC cmake build
    TEST
    hash_checksum_test
    WINDOWS_DISABLED
    SOURCES
    ChecksumTest.cpp
    TEST
    hash_farm_hash_test
    SOURCES
    FarmHashTest.cpp
    BENCHMARK
    hash_hash_benchmark
    WINDOWS_DISABLED
    SOURCES
    HashBenchmark.cpp
    TEST
    hash_hash_test
    WINDOWS_DISABLED
    SOURCES
    HashTest.cpp
    TEST
    hash_spooky_hash_v1_test
    SOURCES
    SpookyHashV1Test.cpp
    TEST
    hash_spooky_hash_v2_test
    SOURCES
    SpookyHashV2Test.cpp
    TEST
    hash_traits_test
    SOURCES
    traits_test.cpp
    TEST
    hash_unique_hash_key_test
    SOURCES
    UniqueHashKeyTest.cpp
    DIRECTORY
    io/test/
    TEST
    io_fs_util_test
    SOURCES
    FsUtilTest.cpp
    BENCHMARK
    io_iobuf_benchmark
    WINDOWS_DISABLED
    SOURCES
    IOBufBenchmark.cpp
    TEST
    io_iobuf_test
    WINDOWS_DISABLED
    SOURCES
    IOBufTest.cpp
    TEST
    io_iobuf_cursor_test
    SOURCES
    IOBufCursorTest.cpp
    TEST
    io_iobuf_queue_test
    SOURCES
    IOBufQueueTest.cpp
    TEST
    io_record_io_test
    WINDOWS_DISABLED
    SOURCES
    RecordIOTest.cpp
    TEST
    io_shutdown_socket_set_test
    HANGING
    SOURCES
    ShutdownSocketSetTest.cpp
    TEST
    io_socket_option_value_test
    HANGING
    SOURCES
    SocketOptionValueTest.cpp
    DIRECTORY
    io/async/test/
    # A number of tests in the async_test binary are unfortunately flaky.
    # When run under Travis CI a number of the tests also hang (it looks
    # like they do not get expected socket accept events, causing them
    # to never break out of their event loops).
    TEST
    io_async_async_test
    BROKEN
    CONTENT_DIR
    certs/
    HEADERS
    AsyncSocketTest.h
    AsyncSSLSocketTest.h
    SOURCES
    AsyncPipeTest.cpp
    AsyncSocketExceptionTest.cpp
    AsyncSocketTest.cpp
    AsyncSocketTest2.cpp
    AsyncSSLSocketTest.cpp
    AsyncSSLSocketTest2.cpp
    AsyncSSLSocketWriteTest.cpp
    AsyncTransportTest.cpp
    # This is disabled because it depends on things that don't exist
    # on Windows.
    # TODO: Refactor EventHandlerTest to not use eventfd so it can work on Mac OS X.
    #TEST io_async_event_handler_test WINDOWS_DISABLED SOURCES EventHandlerTest.cpp
    TEST
    io_async_async_timeout_test
    SOURCES
    AsyncTimeoutTest.cpp
    TEST
    io_async_async_udp_socket_test
    APPLE_DISABLED
    WINDOWS_DISABLED
    SOURCES
    AsyncUDPSocketTest.cpp
    TEST
    io_async_delayed_destruction_test
    SOURCES
    DelayedDestructionTest.cpp
    TEST
    io_async_delayed_destruction_base_test
    SOURCES
    DelayedDestructionBaseTest.cpp
    TEST
    io_async_destructor_check_test
    SOURCES
    DestructorCheckTest.cpp # Fails with gtest macro error
    BENCHMARK
    io_async_event_base_benchmark
    SOURCES
    EventBaseBenchmark.cpp
    TEST
    io_async_event_base_test
    BROKEN
    SOURCES
    EventBaseTest.cpp
    TEST
    io_async_event_base_local_test
    WINDOWS_DISABLED
    SOURCES
    EventBaseLocalTest.cpp
    TEST
    io_async_hh_wheel_timer_test
    SOURCES
    HHWheelTimerTest.cpp
    TEST
    io_async_hh_wheel_timer_slow_tests
    SLOW
    SOURCES
    HHWheelTimerSlowTests.cpp
    TEST
    io_async_notification_queue_test
    WINDOWS_DISABLED
    SOURCES
    NotificationQueueTest.cpp
    BENCHMARK
    io_async_request_context_benchmark
    WINDOWS_DISABLED
    SOURCES
    RequestContextBenchmark.cpp
    HEADERS
    RequestContextHelper.h
    TEST
    io_async_request_context_test
    WINDOWS_DISABLED
    SOURCES
    RequestContextTest.cpp
    TEST
    io_async_scoped_event_base_thread_test
    WINDOWS_DISABLED
    SOURCES
    ScopedEventBaseThreadTest.cpp
    TEST
    io_async_ssl_session_test
    CONTENT_DIR
    certs/
    SOURCES
    SSLSessionTest.cpp
    TEST
    io_async_write_chain_async_transport_wrapper_test
    SOURCES
    WriteChainAsyncTransportWrapperTest.cpp
    DIRECTORY
    io/async/ssl/test/
    TEST
    io_async_ssl_ssl_errors_test
    SOURCES
    SSLErrorsTest.cpp
    DIRECTORY
    lang/test/
    TEST
    lang_align_test
    SOURCES
    AlignTest.cpp
    TEST
    lang_aligned_test
    SOURCES
    AlignedTest.cpp
    TEST
    lang_badge_test
    SOURCES
    BadgeTest.cpp
    TEST
    lang_bits_class_test
    SOURCES
    BitsClassTest.cpp
    TEST
    lang_bits_test
    SOURCES
    BitsTest.cpp
    TEST
    lang_c_string_test
    SOURCES
    CStringTest.cpp
    TEST
    lang_cast_test
    SOURCES
    CastTest.cpp
    TEST
    lang_checked_math_test
    SOURCES
    CheckedMathTest.cpp
    TEST
    lang_exception_test
    SOURCES
    ExceptionTest.cpp
    TEST
    lang_extern_test
    SOURCES
    ExternTest.cpp
    TEST
    lang_ordering_test
    SOURCES
    OrderingTest.cpp
    TEST
    lang_pretty_test
    SOURCES
    PrettyTest.cpp
    TEST
    lang_propagate_const_test
    SOURCES
    PropagateConstTest.cpp
    TEST
    lang_r_value_reference_wrapper_test
    WINDOWS_DISABLED
    SOURCES
    RValueReferenceWrapperTest.cpp
    TEST
    lang_safe_assert_test
    SOURCES
    SafeAssertTest.cpp
    TEST
    lang_switch_test
    SOURCES
    SwitchTest.cpp
    BENCHMARK
    lang_to_ascii_benchmark
    SOURCES
    ToAsciiBench.cpp
    TEST
    lang_to_ascii_test
    SOURCES
    ToAsciiTest.cpp
    TEST
    lang_type_info_test
    SOURCES
    TypeInfoTest.cpp
    DIRECTORY
    math/test/
    BENCHMARK
    math_division_bench
    SOURCES
    DivisionBench.cpp
    TEST
    math_division_test
    SOURCES
    DivisionTest.cpp
    DIRECTORY
    memory/test/
    TEST
    memory_arena_test
    WINDOWS_DISABLED
    SOURCES
    ArenaTest.cpp
    TEST
    memory_reentrant_allocator_test
    WINDOWS_DISABLED
    SOURCES
    ReentrantAllocatorTest.cpp
    TEST
    memory_shared_from_this_ptr_test
    SOURCES
    shared_from_this_ptr_test.cpp
    TEST
    memory_thread_cached_arena_test
    WINDOWS_DISABLED
    SOURCES
    ThreadCachedArenaTest.cpp
    TEST
    memory_mallctl_helper_test
    SOURCES
    MallctlHelperTest.cpp
    TEST
    memory_malloc_test
    SOURCES
    MallocTest.cpp
    TEST
    memory_uninitialized_memory_hacks_test
    SOURCES
    UninitializedMemoryHacksTest.cpp
    DIRECTORY
    net/detail/test/
    TEST
    net_detail_socket_file_descriptor_map_test
    SOURCES
    SocketFileDescriptorMapTest.cpp
    DIRECTORY
    portability/test/
    TEST
    portability_constexpr_test
    SOURCES
    ConstexprTest.cpp
    TEST
    portability_filesystem_test
    SOURCES
    FilesystemTest.cpp
    TEST
    portability_libgen_test
    SOURCES
    LibgenTest.cpp
    TEST
    portability_openssl_portability_test
    SOURCES
    OpenSSLPortabilityTest.cpp
    TEST
    portability_pthread_test
    SOURCES
    PThreadTest.cpp
    TEST
    portability_time_test
    WINDOWS_DISABLED
    SOURCES
    TimeTest.cpp
    DIRECTORY
    ssl/test/
    TEST
    ssl_openssl_hash_test
    SOURCES
    OpenSSLHashTest.cpp
    DIRECTORY
    stats/test/
    TEST
    stats_buffered_stat_test
    SOURCES
    BufferedStatTest.cpp
    BENCHMARK
    stats_digest_builder_benchmark
    SOURCES
    DigestBuilderBenchmark.cpp
    TEST
    stats_digest_builder_test
    SOURCES
    DigestBuilderTest.cpp
    BENCHMARK
    stats_histogram_benchmark
    SOURCES
    HistogramBenchmark.cpp
    TEST
    stats_histogram_test
    SOURCES
    HistogramTest.cpp
    BENCHMARK
    stats_quantile_histogram_benchmark
    SOURCES
    QuantileHistogramBenchmark.cpp
    TEST
    stats_quantile_estimator_test
    SOURCES
    QuantileEstimatorTest.cpp
    TEST
    stats_sliding_window_test
    SOURCES
    SlidingWindowTest.cpp
    BENCHMARK
    stats_tdigest_benchmark
    SOURCES
    TDigestBenchmark.cpp
    TEST
    stats_tdigest_test
    SOURCES
    TDigestTest.cpp
    TEST
    stats_timeseries_histogram_test
    SOURCES
    TimeseriesHistogramTest.cpp
    TEST
    stats_timeseries_test
    SOURCES
    TimeSeriesTest.cpp
    DIRECTORY
    synchronization/test/
    TEST
    synchronization_atomic_util_test
    SOURCES
    AtomicUtilTest.cpp
    TEST
    synchronization_atomic_struct_test
    SOURCES
    AtomicStructTest.cpp
    BENCHMARK
    synchronization_baton_benchmark
    SOURCES
    BatonBenchmark.cpp
    TEST
    synchronization_baton_test
    SOURCES
    BatonTest.cpp
    TEST
    synchronization_call_once_test
    SOURCES
    CallOnceTest.cpp
    TEST
    synchronization_event_count_test
    SOURCES
    EventCountTest.cpp
    BENCHMARK
    synchronization_hazptr_bench
    SOURCES
    HazptrBench.cpp
    TEST
    synchronization_hazptr_test
    SOURCES
    HazptrTest.cpp
    BENCHMARK
    synchronization_lifo_sem_bench
    WINDOWS_DISABLED
    SOURCES
    LifoSemBench.cpp
    TEST
    synchronization_lifo_sem_test
    WINDOWS_DISABLED
    SOURCES
    LifoSemTests.cpp
    TEST
    synchronization_relaxed_atomic_test
    WINDOWS_DISABLED
    SOURCES
    RelaxedAtomicTest.cpp
    TEST
    synchronization_rw_spin_lock_test
    SOURCES
    RWSpinLockTest.cpp
    TEST
    synchronization_semaphore_test
    WINDOWS_DISABLED
    SOURCES
    SemaphoreTest.cpp
    BENCHMARK
    synchronization_small_locks_benchmark
    SOURCES
    SmallLocksBenchmark.cpp
    TEST
    synchronization_small_locks_test
    SOURCES
    SmallLocksTest.cpp
    DIRECTORY
    synchronization/detail/test/
    TEST
    synchronization_detail_inline_function_ref_test
    SOURCES
    InlineFunctionRefTest.cpp
    DIRECTORY
    system/test/
    TEST
    system_at_fork_test
    WINDOWS_DISABLED
    SOURCES
    AtForkTest.cpp
    TEST
    system_memory_mapping_test
    SOURCES
    MemoryMappingTest.cpp
    TEST
    system_shell_test
    SOURCES
    ShellTest.cpp #TEST system_subprocess_test SOURCES SubprocessTest.cpp
    TEST
    system_thread_id_test
    SOURCES
    ThreadIdTest.cpp
    TEST
    system_thread_name_test
    WINDOWS_DISABLED
    SOURCES
    ThreadNameTest.cpp
    DIRECTORY
    test/
    TEST
    ahm_int_stress_test
    SOURCES
    AHMIntStressTest.cpp
    TEST
    arena_smartptr_test
    SOURCES
    ArenaSmartPtrTest.cpp
    BENCHMARK
    ascii_case_insensitive_benchmark
    SOURCES
    AsciiCaseInsensitiveBenchmark.cpp
    TEST
    ascii_check_test
    SOURCES
    AsciiCaseInsensitiveTest.cpp
    TEST
    atomic_hash_array_test
    SOURCES
    AtomicHashArrayTest.cpp
    TEST
    atomic_hash_map_test
    HANGING
    SOURCES
    AtomicHashMapTest.cpp
    TEST
    atomic_linked_list_test
    SOURCES
    AtomicLinkedListTest.cpp
    TEST
    atomic_unordered_map_test
    SOURCES
    AtomicUnorderedMapTest.cpp
    TEST
    base64_test
    SOURCES
    base64_test.cpp
    TEST
    buffered_atomic_test
    SOURCES
    BufferedAtomicTest.cpp
    TEST
    cancellation_token_test
    SOURCES
    CancellationTokenTest.cpp
    TEST
    chrono_test
    SOURCES
    ChronoTest.cpp
    TEST
    clock_gettime_wrappers_test
    SOURCES
    ClockGettimeWrappersTest.cpp
    TEST
    concurrent_bit_set_test
    SOURCES
    ConcurrentBitSetTest.cpp
    BENCHMARK
    concurrent_skip_list_benchmark
    SOURCES
    ConcurrentSkipListBenchmark.cpp
    TEST
    concurrent_skip_list_test
    SOURCES
    ConcurrentSkipListTest.cpp
    # Builds but fails test on constexpr_exp_floating std::exp(471.L)
    TEST
    constexpr_math_test
    WINDOWS_DISABLED
    SOURCES
    ConstexprMathTest.cpp
    TEST
    conv_test
    SOURCES
    ConvTest.cpp
    TEST
    cpu_id_test
    SOURCES
    CpuIdTest.cpp
    TEST
    demangle_test
    SOURCES
    DemangleTest.cpp
    TEST
    deterministic_schedule_test
    SOURCES
    DeterministicScheduleTest.cpp
    TEST
    discriminated_ptr_test
    SOURCES
    DiscriminatedPtrTest.cpp
    TEST
    endian_test
    SOURCES
    EndianTest.cpp
    TEST
    exception_string_test
    BROKEN
    SOURCES
    ExceptionStringTest.cpp
    TEST
    exception_test
    SOURCES
    ExceptionTest.cpp
    BENCHMARK
    exception_wrapper_benchmark
    WINDOWS_DISABLED
    SOURCES
    ExceptionWrapperBenchmark.cpp
    TEST
    exception_wrapper_test
    WINDOWS_DISABLED
    SOURCES
    ExceptionWrapperTest.cpp
    TEST
    expected_coroutines_test
    SOURCES
    ExpectedCoroutinesTest.cpp
    TEST
    expected_test
    WINDOWS_DISABLED
    SOURCES
    ExpectedTest.cpp
    BENCHMARK
    fbstring_benchmark
    WINDOWS_DISABLED
    SOURCES
    FBStringBenchmark.cpp
    HEADERS
    FBStringTestBenchmarks.cpp.h
    TEST
    fbstring_test
    WINDOWS_DISABLED
    SOURCES
    FBStringTest.cpp
    TEST
    file_test
    SOURCES
    FileTest.cpp # Open-source linux build can't handle running this.
    #TEST file_lock_test SOURCES FileLockTest.cpp
    TEST
    file_util_test
    SOURCES
    FileUtilTest.cpp
    BENCHMARK
    fingerprint_benchmark
    SOURCES
    FingerprintBenchmark.cpp
    TEST
    fingerprint_test
    SOURCES
    FingerprintTest.cpp
    TEST
    fixed_string_test
    SOURCES
    FixedStringTest.cpp
    TEST
    fmt_utility_test
    SOURCES
    FmtUtilityTest.cpp
    TEST
    function_test
    BROKEN
    SOURCES
    FunctionTest.cpp
    BENCHMARK
    function_ref_benchmark
    SOURCES
    FunctionRefBenchmark.cpp
    TEST
    function_ref_test
    SOURCES
    FunctionRefTest.cpp
    TEST
    futex_test
    SOURCES
    FutexTest.cpp
    TEST
    glog_test
    SOURCES
    GLogTest.cpp
    TEST
    group_varint_test
    SOURCES
    GroupVarintTest.cpp
    TEST
    group_varint_test_ssse3
    SOURCES
    GroupVarintTest.cpp
    TEST
    indestructible_test
    SOURCES
    IndestructibleTest.cpp
    TEST
    indexed_mem_pool_test
    BROKEN
    SOURCES
    IndexedMemPoolTest.cpp
    TEST
    iterators_test
    SOURCES
    IteratorsTest.cpp
    TEST
    lazy_test
    SOURCES
    LazyTest.cpp
    TEST
    locks_test
    SOURCES
    SpinLockTest.cpp
    TEST
    math_test
    SOURCES
    MathTest.cpp
    BENCHMARK
    memcpy_benchmark
    WINDOWS_DISABLED
    SOURCES
    MemcpyBenchmark.cpp
    TEST
    memcpy_test
    SOURCES
    MemcpyTest.cpp
    TEST
    memory_idler_test
    SOURCES
    MemoryIdlerTest.cpp
    TEST
    memory_test
    WINDOWS_DISABLED
    SOURCES
    MemoryTest.cpp
    BENCHMARK
    memset_benchmark
    WINDOWS_DISABLED
    SOURCES
    MemsetBenchmark.cpp
    TEST
    move_wrapper_test
    SOURCES
    MoveWrapperTest.cpp
    TEST
    mpmc_pipeline_test
    SOURCES
    MPMCPipelineTest.cpp
    TEST
    mpmc_queue_test
    SLOW
    SOURCES
    MPMCQueueTest.cpp
    TEST
    network_address_test
    HANGING
    SOURCES
    IPAddressTest.cpp
    MacAddressTest.cpp
    SocketAddressTest.cpp
    TEST
    optional_coroutines_test
    SOURCES
    OptionalCoroutinesTest.cpp
    TEST
    optional_test
    SOURCES
    OptionalTest.cpp
    TEST
    packed_sync_ptr_test
    HANGING
    SOURCES
    PackedSyncPtrTest.cpp
    TEST
    padded_test
    SOURCES
    PaddedTest.cpp #TEST poly_test SOURCES PolyTest.cpp
    TEST
    portability_test
    SOURCES
    PortabilityTest.cpp
    TEST
    preprocessor_test
    SOURCES
    PreprocessorTest.cpp
    # Turns out this benchmark uses Linux only API for setting CPU affinity i.e. cpu_set_t.
    BENCHMARK
    producer_consumer_queue_benchmark
    APPLE_DISABLED
    WINDOWS_DISABLED
    SOURCES
    ProducerConsumerQueueBenchmark.cpp
    TEST
    producer_consumer_queue_test
    SLOW
    SOURCES
    ProducerConsumerQueueTest.cpp
    BENCHMARK
    range_find_benchmark
    SOURCES
    RangeFindBenchmark.cpp
    BENCHMARK
    random_benchmark
    SOURCES
    RandomBenchmark.cpp
    TEST
    random_test
    SOURCES
    RandomTest.cpp
    TEST
    range_test
    SOURCES
    RangeTest.cpp
    TEST
    replaceable_test
    WINDOWS_DISABLED
    SOURCES
    ReplaceableTest.cpp
    TEST
    scope_guard_test
    WINDOWS_DISABLED
    SOURCES
    ScopeGuardTest.cpp # Heavily dependent on drand and srand48
    #TEST shared_mutex_test SOURCES SharedMutexTest.cpp
    # SingletonTest requires Subprocess
    #TEST singleton_test SOURCES SingletonTest.cpp
    TEST
    singleton_test_global
    SOURCES
    SingletonTestGlobal.cpp
    TEST
    singleton_thread_local_test
    BROKEN
    SOURCES
    SingletonThreadLocalTest.cpp
    SingletonThreadLocalTestOverload.cpp
    TEST
    spin_lock_test
    SOURCES
    SpinLockTest.cpp
    BENCHMARK
    string_benchmark
    WINDOWS_DISABLED
    SOURCES
    StringBenchmark.cpp
    TEST
    string_test
    WINDOWS_DISABLED
    SOURCES
    StringTest.cpp
    BENCHMARK
    string_to_float_benchmark
    SOURCES
    StringToFloatBenchmark.cpp
    BENCHMARK
    synchronized_benchmark
    WINDOWS_DISABLED
    SOURCES
    SynchronizedBenchmark.cpp
    TEST
    synchronized_test
    WINDOWS_DISABLED
    SOURCES
    SynchronizedTest.cpp
    TEST
    thread_cached_int_test
    WINDOWS_DISABLED
    SOURCES
    ThreadCachedIntTest.cpp
    TEST
    thread_local_test
    WINDOWS_DISABLED
    SOURCES
    ThreadLocalTest.cpp
    TEST
    timeout_queue_test
    SOURCES
    TimeoutQueueTest.cpp
    TEST
    token_bucket_test
    SOURCES
    TokenBucketTest.cpp
    TEST
    traits_test
    SOURCES
    TraitsTest.cpp
    TEST
    try_test
    WINDOWS_DISABLED
    SOURCES
    TryTest.cpp
    TEST
    unicode_test
    SOURCES
    UnicodeTest.cpp
    TEST
    unit_test
    SOURCES
    UnitTest.cpp
    BENCHMARK
    uri_benchmark
    SOURCES
    UriBenchmark.cpp
    TEST
    uri_test
    SOURCES
    UriTest.cpp
    TEST
    utf8_string_test
    SOURCES
    UTF8StringTest.cpp
    TEST
    utility_test
    SOURCES
    UtilityTest.cpp
    TEST
    varint_test
    SOURCES
    VarintTest.cpp
    DIRECTORY
    test/function_benchmark/
    BENCHMARK
    function_benchmark
    SOURCES
    main.cpp
    benchmark_impl.cpp
    test_functions.cpp
    DIRECTORY
    testing/test/
    TEST
    testing_test_util_test
    SOURCES
    TestUtilTest.cpp
    DIRECTORY
    tracing/test/
    TEST
    static_tracepoint_section_test
    SOURCES
    StaticTracepointSectionTest.cpp
    DIRECTORY
    json/test/
    TEST
    json_dynamic_converter_test
    SOURCES
    DynamicConverterTest.cpp
    TEST
    json_dynamic_other_test
    SOURCES
    DynamicOtherTest.cpp
    TEST
    json_dynamic_parser_test
    SOURCES
    DynamicParserTest.cpp
    TEST
    json_dynamic_test
    SOURCES
    DynamicTest.cpp # MSVC Preprocessor stringizing raw string literals bug
    TEST
    json_json_test
    WINDOWS_DISABLED
    SOURCES
    JsonTest.cpp
    BENCHMARK
    json_json_benchmark
    SOURCES
    JsonBenchmark.cpp
    TEST
    json_json_other_test
    SOURCES
    JsonOtherTest.cpp
    TEST
    json_json_patch_test
    SOURCES
    json_patch_test.cpp
    TEST
    json_json_pointer_test
    SOURCES
    json_pointer_test.cpp
    TEST
    json_json_schema_test
    SOURCES
    JSONSchemaTest.cpp
  )

  folly_add_test_module(
    NAME thread_local_test_lib
    HOST thread_local_test
    SOURCES ${FOLLY_DIR}/test/ThreadLocalTestLib.cpp
  )

  if (${LIBSODIUM_FOUND})
    folly_define_tests(
      DIRECTORY
      crypto/test/
      TEST
      crypto_blake2xb_test
      SOURCES
      Blake2xbTest.cpp
      BENCHMARK
      crypto_lt_hash_benchmark
      SOURCES
      LtHashBenchmark.cpp
      TEST
      crypto_lt_hash_test
      SOURCES
      LtHashTest.cpp
    )
  endif ()

  if (${LIBAIO_FOUND})
    folly_define_tests(
      DIRECTORY
      io/async/test/
      TEST
      io_async_async_io_test
      SOURCES
      AsyncIOTest.cpp
      AsyncBaseTestLib.cpp
      IoTestTempFileUtil.cpp
    )
  endif ()

  get_target_property(pic folly POSITION_INDEPENDENT_CODE)
  if (pic)
    add_library(
      singleton_thread_local_overload
      SHARED ${FOLLY_DIR}/test/SingletonThreadLocalTestOverload.cpp
    )
    apply_folly_compile_options_to_target(singleton_thread_local_overload)
    set_target_properties(
      singleton_thread_local_overload
      PROPERTIES
        PREFIX
        ""
        LIBRARY_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/folly/test
    )
    target_link_libraries(singleton_thread_local_overload PRIVATE folly)
    folly_define_tests(
      DIRECTORY
      test/
      TEST
      singleton_thread_local_test
      SOURCES
      SingletonThreadLocalTest.cpp
    )
  endif ()
endif ()
