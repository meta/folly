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

include(CheckCXXSourceCompiles)
include(CheckCXXSymbolExists)
include(CheckIncludeFileCXX)
include(CheckFunctionExists)
include(CMakePushCheckState)

include(FBFetchFromManifest)

# Fetch the archive or commit `manifest` pins, so a fetched dependency cannot
# drift from the one getdeps builds. OVERRIDE_FIND_PACKAGE makes a later
# find_package() resolve to what was fetched instead of searching the system,
# which thrift/lib relies on for Boost. The fetched source and binary
# directories are not set in the caller's scope; read them back with
# FetchContent_GetProperties.
function (folly_fetch_from_manifest name manifest)
  # Some dependencies build a static library whatever BUILD_SHARED_LIBS says,
  # gflags and libevent among them, and folly links those into its shared
  # libraries. An ELF linker takes that only if the objects are position
  # independent.
  if (BUILD_SHARED_LIBS)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  endif ()
  fb_fetch_from_manifest(${name} ${manifest} OVERRIDE_FIND_PACKAGE)
  set_property(GLOBAL APPEND PROPERTY FOLLY_FETCHED_DEPS ${name})
endfunction ()

set(BOOST_LINK_STATIC
    "auto"
    CACHE STRING "Whether to link against boost statically or dynamically.")
if ("${BOOST_LINK_STATIC}" STREQUAL "auto")
  # Default to linking boost statically on Windows with MSVC
  if (MSVC)
    set(FOLLY_BOOST_LINK_STATIC ON)
  else ()
    set(FOLLY_BOOST_LINK_STATIC OFF)
  endif ()
else ()
  set(FOLLY_BOOST_LINK_STATIC "${BOOST_LINK_STATIC}")
endif ()
set(Boost_USE_STATIC_LIBS "${FOLLY_BOOST_LINK_STATIC}")

# Note: We find these components so the CMake targets exist, but we don't
# link them globally. Targets that need specific Boost libraries should
# add them to their EXTERNAL_DEPS (e.g., Boost::regex, Boost::context).
set(FOLLY_BOOST_COMPONENTS context regex)

# CMake 4 dropped FindBoost, so this config-only probe warns without QUIET.
# folly/cli's ProgramOptions is built only if Boost.ProgramOptions is found.
find_package(Boost 1.69.0 QUIET COMPONENTS ${FOLLY_BOOST_COMPONENTS}
             OPTIONAL_COMPONENTS program_options)
if (NOT Boost_FOUND)
  set(BOOST_ENABLE_CMAKE ON)
  # A parent project that builds folly can ask for more of Boost's libraries
  # in BOOST_INCLUDE_LIBRARIES, as eden does for filesystem.
  list(APPEND BOOST_INCLUDE_LIBRARIES ${FOLLY_BOOST_COMPONENTS})
  list(REMOVE_DUPLICATES BOOST_INCLUDE_LIBRARIES)
  # Boost 1.83's libs/predef and libs/filesystem still ask for
  # cmake_minimum_required 2.8 and 3.0. CMake 4 rejects anything below 3.5 and
  # warns below 3.10.
  set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
  folly_fetch_from_manifest(Boost boost)
  unset(CMAKE_POLICY_VERSION_MINIMUM)
  # In the modular layout Boost::headers points at libs/headers/include, which
  # is empty, so gather every module's include directory instead. folly reads
  # the variable below; thrift's lib/cpp links the target.
  FetchContent_GetProperties(Boost SOURCE_DIR folly_boost_source_dir)
  file(GLOB Boost_INCLUDE_DIRS "${folly_boost_source_dir}/libs/*/include"
       "${folly_boost_source_dir}/libs/numeric/*/include")
  target_include_directories(
    boost_headers INTERFACE "$<BUILD_INTERFACE:${Boost_INCLUDE_DIRS}>")
  # Both the alias and the underlying name get linked.
  set(folly_boost_local Boost::headers boost_headers)
  foreach (folly_boost_component IN LISTS BOOST_INCLUDE_LIBRARIES)
    list(APPEND folly_boost_local "Boost::${folly_boost_component}"
         "boost_${folly_boost_component}")
  endforeach ()
  set_property(GLOBAL APPEND PROPERTY FOLLY_BUILD_LOCAL_TARGETS
                                      ${folly_boost_local})
endif ()
# Only add include directories globally, not libraries
# Per-target Boost dependencies are specified via EXTERNAL_DEPS
list(APPEND FOLLY_INCLUDE_DIRECTORIES ${Boost_INCLUDE_DIRS})

find_package(FastFloat MODULE)
if (NOT FASTFLOAT_FOUND)
  # Conv.h checks the same macros. Conv.cpp calls the long double overload
  # only where __cpp_lib_to_chars promises it; libc++ has none.
  check_cxx_source_compiles(
    "
    #include <charconv>
    #if defined(__cpp_lib_to_chars)
    #define LONG_DOUBLE 1
    #elif _LIBCPP_VERSION >= 200000 && _LIBCPP_AVAILABILITY_HAS_FROM_CHARS_FLOATING_POINT
    #define LONG_DOUBLE 0
    #else
    #error no floating-point std::from_chars
    #endif
    int main() {
      const char s[] = \"1.5\";
      float f = 0;
      double d = 0;
      auto general = std::chars_format::general;
      int failures = 0;
      failures += std::from_chars(s, s + 3, f, general).ec != std::errc();
      failures += std::from_chars(s, s + 3, d, general).ec != std::errc();
    #if LONG_DOUBLE
      long double ld = 0;
      failures += std::from_chars(s, s + 3, ld, general).ec != std::errc();
    #endif
      return failures;
    }"
    FOLLY_HAVE_FLOAT_FROM_CHARS)
endif ()
set(FOLLY_HAVE_FAST_FLOAT ON)
if (FASTFLOAT_FOUND)
  list(APPEND FOLLY_INCLUDE_DIRECTORIES ${FASTFLOAT_INCLUDE_DIR})
elseif (FOLLY_HAVE_FLOAT_FROM_CHARS)
  set(FOLLY_HAVE_FAST_FLOAT OFF)
  message(STATUS "fast_float not found, parsing floating-point numbers with "
                 "std::from_chars, which is several times slower")
else ()
  # fast_float asks for cmake_minimum_required 3.9, which is deprecated and
  # leaves CMP0077 OLD, so its option() calls would clear the two settings
  # below. 3.13 is where that policy arrived.
  set(CMAKE_POLICY_VERSION_MINIMUM 3.13)
  set(FASTFLOAT_TEST OFF)
  set(FASTFLOAT_SANITIZE OFF)
  folly_fetch_from_manifest(FastFloat fast_float)
  unset(CMAKE_POLICY_VERSION_MINIMUM)
  # Header-only, and Conv.cpp selects it with __has_include, so only the
  # include directory matters.
  FetchContent_GetProperties(FastFloat SOURCE_DIR folly_fast_float_source_dir)
  if (NOT folly_fast_float_source_dir)
    message(FATAL_ERROR "fast_float was fetched but not populated")
  endif ()
  list(APPEND FOLLY_INCLUDE_DIRECTORIES
       "${folly_fast_float_source_dir}/include")
endif ()

# gflags is optional: without it, folly/portability/GFlags.h defines folly's
# flags as plain variables. The tests and benchmarks use gflags directly, so
# they still need it.
find_package(Gflags MODULE)
if (NOT LIBGFLAGS_FOUND AND (BUILD_TESTS OR BUILD_BENCHMARKS))
  # As a subproject gflags builds only its single-threaded library.
  set(GFLAGS_BUILD_gflags_LIB ON)
  set(GFLAGS_BUILD_gflags_nothreads_LIB OFF)
  # gflags builds a static library even for a shared build, and its `gflags`
  # aliases prefer it. Every shared library linking an alias then embeds its
  # own copy, and the second copy to register its flags aborts the process.
  if (BUILD_SHARED_LIBS)
    set(GFLAGS_BUILD_STATIC_LIBS OFF)
  endif ()
  folly_fetch_from_manifest(gflags gflags)
  # FindGflags reports through variables; the subproject alias already carries
  # the generated include directory, so only the library needs one. Clear the
  # failed find_path() result so it is not used as an include directory.
  set(LIBGFLAGS_LIBRARY gflags)
  set(LIBGFLAGS_INCLUDE_DIR "")
  set(LIBGFLAGS_FOUND ON)
  set_property(
    GLOBAL APPEND PROPERTY FOLLY_BUILD_LOCAL_TARGETS gflags gflags_static
                           gflags::gflags gflags::gflags_static)
endif ()
# folly-config.cmake.in substitutes this, so it has to be a literal ON or OFF.
if (LIBGFLAGS_FOUND)
  set(FOLLY_HAVE_LIBGFLAGS ON)
  list(APPEND FOLLY_LINK_LIBRARIES ${LIBGFLAGS_LIBRARY})
  list(APPEND FOLLY_INCLUDE_DIRECTORIES ${LIBGFLAGS_INCLUDE_DIR})
  set(FOLLY_LIBGFLAGS_LIBRARY ${LIBGFLAGS_LIBRARY})
  set(FOLLY_LIBGFLAGS_INCLUDE ${LIBGFLAGS_INCLUDE_DIR})
else ()
  set(FOLLY_HAVE_LIBGFLAGS OFF)
endif ()

find_package(Glog MODULE)
if (GLOG_FOUND)
  list(APPEND FOLLY_INCLUDE_DIRECTORIES ${GLOG_INCLUDE_DIR})
  # Glog 0.7+ requires GLOG_USE_GLOG_EXPORT to be defined so that headers
  # include glog/export.h which defines GLOG_EXPORT.
  if (EXISTS "${GLOG_INCLUDE_DIR}/glog/export.h")
    list(APPEND FOLLY_CXX_FLAGS -DGLOG_USE_GLOG_EXPORT)
  endif ()
else ()
  # glog runs include(CTest), which would turn testing on for the whole
  # superproject. CMP0077 makes the option() inside it defer to this.
  set(folly_saved_build_testing "${BUILD_TESTING}")
  set(BUILD_TESTING OFF)
  # glog asks for `gflags` in lower case, which on a case-insensitive
  # filesystem matches the FindGflags.cmake on our module path and warns.
  # Config mode resolves the gflags fetched above on either filesystem.
  set(folly_saved_prefer_config "${CMAKE_FIND_PACKAGE_PREFER_CONFIG}")
  set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)
  folly_fetch_from_manifest(glog glog)
  if (folly_saved_prefer_config STREQUAL "")
    unset(CMAKE_FIND_PACKAGE_PREFER_CONFIG)
  else()
    set(CMAKE_FIND_PACKAGE_PREFER_CONFIG "${folly_saved_prefer_config}")
  endif()
  if (folly_saved_build_testing STREQUAL "")
    unset(BUILD_TESTING)
  else ()
    set(BUILD_TESTING "${folly_saved_build_testing}")
  endif ()
  # glog exports its whole src/, which puts internal headers on the include
  # path of everything linking glog::glog -- folly/Demangle.cpp probes for
  # libiberty's <demangle.h> and finds glog's. Export only the glog/ directory,
  # completed with the public headers that are not generated into it.
  FetchContent_GetProperties(
    glog
    SOURCE_DIR folly_glog_source_dir
    BINARY_DIR folly_glog_binary_dir)
  file(GLOB folly_glog_headers "${folly_glog_source_dir}/src/glog/*.h")
  file(COPY ${folly_glog_headers} DESTINATION "${folly_glog_binary_dir}/glog")
  set_property(
    TARGET glog
    PROPERTY INTERFACE_INCLUDE_DIRECTORIES
             "$<BUILD_INTERFACE:${folly_glog_binary_dir}>"
             "$<INSTALL_INTERFACE:${INCLUDE_INSTALL_DIR}>")
  # folly's granular libraries name ${GLOG_LIBRARIES} in their EXPORTED_DEPS.
  set(GLOG_LIBRARIES glog::glog)
  # A project built alongside folly gets FindGlog's variables from here. The
  # redirect that OVERRIDE_FIND_PACKAGE writes sets none of them, and the search
  # above left GLOG_INCLUDE_DIR NOTFOUND in the cache.
  file(WRITE "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/glog-extra.cmake"
"set(GLOG_FOUND TRUE)
set(GLOG_INCLUDE_DIR \"${folly_glog_binary_dir}\")
set(GLOG_INCLUDE_DIRS \"${folly_glog_binary_dir}\")
set(GLOG_LIBRARY glog::glog)
set(GLOG_LIBRARIES glog::glog)
")
  set_property(GLOBAL APPEND PROPERTY FOLLY_BUILD_LOCAL_TARGETS glog glog::glog)
endif ()
set(FOLLY_HAVE_LIBGLOG ON)
list(APPEND FOLLY_LINK_LIBRARIES glog::glog)

find_package(LibEvent MODULE)
if (NOT LibEvent_FOUND)
  set(EVENT__DISABLE_TESTS ON)
  set(EVENT__DISABLE_BENCHMARK ON)
  set(EVENT__DISABLE_SAMPLES ON)
  set(EVENT__DISABLE_REGRESS ON)
  # libevent declares this with set(CACHE), not option(), so a plain variable
  # would be dropped on the configure that creates the cache entry.
  set(EVENT__LIBRARY_TYPE
      STATIC
      CACHE STRING "libevent library type")
  # libevent also FORCEs CMAKE_BUILD_TYPE to Release when it is unset, which
  # would choose the build type for the whole superproject.
  set(folly_saved_build_type "${CMAKE_BUILD_TYPE}")
  folly_fetch_from_manifest(LibEvent libevent)
  if (NOT folly_saved_build_type)
    set(CMAKE_BUILD_TYPE
        ""
        CACHE STRING "Choose the type of build." FORCE)
  endif ()
  # FindLibEvent reports through variables. These cover the same sources as the
  # combined `event` target, but unlike it they carry the include directories
  # and are exported, which install(EXPORT folly) requires. The cache entry
  # above is a default, so follow whichever variant libevent ended up building.
  if (TARGET event_core_static)
    set(LIBEVENT_LIB event_core_static event_extra_static)
  else ()
    set(LIBEVENT_LIB event_core_shared event_extra_shared)
  endif ()
  # The targets carry the include directories; clear the failed find_path()
  # result.
  set(LIBEVENT_INCLUDE_DIR "")
  # As for glog, a project built alongside folly gets FindLibEvent's variables
  # from here, or it reads the NOTFOUND results cached above.
  file(WRITE "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/libevent-extra.cmake"
"set(LIBEVENT_LIB ${LIBEVENT_LIB})
set(LIBEVENT_INCLUDE_DIR \"\")
")
  set_property(GLOBAL APPEND PROPERTY FOLLY_BUILD_LOCAL_TARGETS ${LIBEVENT_LIB})
endif ()
list(APPEND FOLLY_LINK_LIBRARIES ${LIBEVENT_LIB})
list(APPEND FOLLY_INCLUDE_DIRECTORIES ${LIBEVENT_INCLUDE_DIR})

find_package(ZLIB MODULE)
set(FOLLY_HAVE_LIBZ ${ZLIB_FOUND})
if (ZLIB_FOUND)
  list(APPEND FOLLY_INCLUDE_DIRECTORIES ${ZLIB_INCLUDE_DIRS})
  list(APPEND CMAKE_REQUIRED_LIBRARIES ${ZLIB_LIBRARIES})
endif ()
folly_add_external_library(zlib LIBRARIES ${ZLIB_LIBRARIES})

# OpenSSL ships no CMakeLists.txt, so it cannot go through
# folly_fetch_from_manifest. ExternalProject runs its own Configure script at
# build time instead, which leaves the paths below non-existent during
# configure; that is only workable because nothing here compiles against
# OpenSSL at configure time.
function (folly_build_openssl)
  fb_manifest_value(openssl download url url)
  fb_manifest_value(openssl download sha256 sha)
  # The trailing letter matters: 1.1.x releases are versioned 1.1.1w and the
  # like, and that is the string FindOpenSSL would have reported.
  if (NOT url MATCHES "openssl-([0-9]+\\.[0-9]+\\.[0-9]+[a-z]*)")
    message(FATAL_ERROR "no version in the openssl url ${url}")
  endif()
  set(version "${CMAKE_MATCH_1}")
  message(STATUS "OpenSSL not found, building ${url}")
  set(prefix "${CMAKE_CURRENT_BINARY_DIR}/openssl")
  # Configure is a Perl script on every platform.
  find_program(FOLLY_OPENSSL_PERL NAMES perl)
  if (NOT FOLLY_OPENSSL_PERL)
    message(FATAL_ERROR
      "OpenSSL not found, and building it needs perl on PATH. Install perl, "
      "or install OpenSSL and set OPENSSL_ROOT_DIR.")
  endif()
  set(extra "")
  set(jobs "")
  if (WIN32)
    # jom reads nmake's input and builds in parallel; nmake is the fallback.
    find_program(FOLLY_OPENSSL_MAKE NAMES jom nmake)
    set(make_hint
        "Run from a Visual Studio developer prompt, or install OpenSSL and "
        "set OPENSSL_ROOT_DIR.")
    # The target getdeps builds. /FS lets several cl.exe share one .pdb.
    set(extra VC-WIN64A-masm -utf-8 /FS)
    set(libs "${prefix}/lib/libssl.lib" "${prefix}/lib/libcrypto.lib")
  else()
    find_program(FOLLY_OPENSSL_MAKE NAMES make)
    set(make_hint "Install make, or install OpenSSL and set OPENSSL_ROOT_DIR.")
    set(jobs -j)
    set(libs "${prefix}/lib/libssl.a" "${prefix}/lib/libcrypto.a")
    # Configure does not find the SDK on its own the way the compiler CMake
    # drives does, and without it every header lookup fails.
    if (APPLE)
      set(sysroot "${CMAKE_OSX_SYSROOT}")
      if (NOT sysroot)
        execute_process(
        COMMAND xcrun --show-sdk-path
          OUTPUT_VARIABLE sysroot
        OUTPUT_STRIP_TRAILING_WHITESPACE)
      endif ()
      if (sysroot)
        set(extra "-isysroot" "${sysroot}")
      endif()
    endif ()
  endif ()
  if (NOT FOLLY_OPENSSL_MAKE)
    message(FATAL_ERROR "OpenSSL not found, and building it needs a make. "
                        ${make_hint})
  endif()
  include(ExternalProject)
  ExternalProject_Add(
    openssl
    URL "${url}"
    URL_HASH SHA256=${sha}
    # Timestamp the extracted tree, so a changed pin rebuilds it.
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    BUILD_IN_SOURCE ON
    CONFIGURE_COMMAND
      ${FOLLY_OPENSSL_PERL} <SOURCE_DIR>/Configure --prefix=${prefix}
      --libdir=lib no-shared ${extra}
    BUILD_COMMAND ${FOLLY_OPENSSL_MAKE} ${jobs}
    # install_sw leaves out the man pages, which dominate a full install.
    INSTALL_COMMAND ${FOLLY_OPENSSL_MAKE} install_sw
    # Without this Ninja has no rule to produce the libraries and refuses to
    # link them.
    BUILD_BYPRODUCTS ${libs}
  )
  # An include directory has to exist by generate time even when what it will
  # hold does not.
  file(MAKE_DIRECTORY "${prefix}/include")

  # Imported targets rather than bare paths, so dependents get the include
  # directory too and so folly_localize_deps() has a name to match on. GLOBAL
  # because a dependent that fetched folly refers to them from its own scope.
  # add_dependencies() is what orders compiling against the ExternalProject;
  # BUILD_BYPRODUCTS only orders linking.
  # ${libs} holds ssl then crypto, so it lines up with ${targets} index for
  # index. The library file name is not spelled here because its suffix is
  # .lib on Windows and .a elsewhere.
  set(targets OpenSSL::SSL OpenSSL::Crypto)
  foreach (i RANGE 1)
    list(GET targets ${i} target)
    list(GET libs ${i} location)
    add_library(${target} STATIC IMPORTED GLOBAL)
    set_target_properties(${target} PROPERTIES
      IMPORTED_LOCATION "${location}"
      INTERFACE_INCLUDE_DIRECTORIES "${prefix}/include")
    add_dependencies(${target} openssl)
  endforeach()
  set_property(TARGET OpenSSL::SSL APPEND PROPERTY
    INTERFACE_LINK_LIBRARIES OpenSSL::Crypto)

  # CMAKE_FIND_PACKAGE_REDIRECTS_DIR is searched before Find modules, not after
  # them, so writing this outside the not-found branch would hijack a working
  # system OpenSSL. Reached only when find_package(OpenSSL) already failed.
  file(WRITE "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/openssl-config.cmake"
"# Generated by folly for the OpenSSL it builds from source.
set(OPENSSL_FOUND TRUE)
set(OPENSSL_VERSION \"${version}\")
set(OPENSSL_INCLUDE_DIR \"${prefix}/include\")
set(OPENSSL_SSL_LIBRARY OpenSSL::SSL)
set(OPENSSL_CRYPTO_LIBRARY OpenSSL::Crypto)
set(OPENSSL_LIBRARIES OpenSSL::SSL OpenSSL::Crypto)
")
  file(WRITE "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/openssl-config-version.cmake"
"set(PACKAGE_VERSION \"${version}\")
if (PACKAGE_VERSION VERSION_LESS PACKAGE_FIND_VERSION)
  set(PACKAGE_VERSION_COMPATIBLE FALSE)
else()
  set(PACKAGE_VERSION_COMPATIBLE TRUE)
  if (PACKAGE_VERSION VERSION_EQUAL PACKAGE_FIND_VERSION)
    set(PACKAGE_VERSION_EXACT TRUE)
  endif()
endif()
")

  # libssl before libcrypto: the static link order matters.
  set(OPENSSL_LIBRARIES OpenSSL::SSL OpenSSL::Crypto PARENT_SCOPE)
  set(OPENSSL_INCLUDE_DIR "${prefix}/include" PARENT_SCOPE)
endfunction()

# OpenSSL is optional: without it, folly leaves out the libraries that need it,
# such as folly/ssl, AsyncSSLSocket and SSLContext. A project that builds folly
# as a subproject usually needs OpenSSL itself, so there folly builds it from
# source by default when it is not installed.
if (CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(folly_build_openssl_default OFF)
else ()
  set(folly_build_openssl_default ON)
endif ()
option(FOLLY_BUILD_OPENSSL "Build OpenSSL from source if it is not found"
       ${folly_build_openssl_default})

if (CMAKE_DISABLE_FIND_PACKAGE_OpenSSL)
  # A parent project that found OpenSSL leaves OPENSSL_FOUND set here.
  set(OPENSSL_FOUND OFF)
else ()
  find_package(OpenSSL 1.1.1 MODULE)
endif ()
# FOLLY_HAVE_OPENSSL decides which libraries are built. Only a system copy
# reaches folly's export set, so only then does a consumer have to find it.
# folly-config.cmake.in substitutes FOLLY_OPENSSL_SYSTEM, so both have to be a
# literal ON or OFF rather than an unset OPENSSL_FOUND.
if (OPENSSL_FOUND)
  set(FOLLY_HAVE_OPENSSL ON)
  set(FOLLY_OPENSSL_SYSTEM ON)
elseif (FOLLY_BUILD_OPENSSL AND NOT CMAKE_DISABLE_FIND_PACKAGE_OpenSSL)
  folly_build_openssl()
  set(FOLLY_HAVE_OPENSSL ON)
  set(FOLLY_OPENSSL_SYSTEM OFF)
  # BUILD_BYPRODUCTS orders the link step only, so compiles can reach
  # <openssl/*.h> before the headers are installed. Applied to folly_deps below.
  set(folly_openssl_external_project openssl)
  # Staged in the build tree and never installed, so these would dangle for a
  # consumer of an installed folly. Target names, not the archive paths, so
  # folly_localize_deps() matches them.
  set_property(GLOBAL APPEND PROPERTY FOLLY_BUILD_LOCAL_TARGETS
    OpenSSL::SSL OpenSSL::Crypto)
else ()
  message(STATUS "OpenSSL not found, so the folly libraries that need it are "
                 "left out. Set FOLLY_BUILD_OPENSSL to build OpenSSL instead.")
  set(FOLLY_HAVE_OPENSSL OFF)
  set(FOLLY_OPENSSL_SYSTEM OFF)
endif ()
if (FOLLY_HAVE_OPENSSL)
  list(APPEND FOLLY_LINK_LIBRARIES ${OPENSSL_LIBRARIES})
  list(APPEND FOLLY_INCLUDE_DIRECTORIES ${OPENSSL_INCLUDE_DIR})
endif ()
if (ZLIB_FOUND)
  list(REMOVE_ITEM CMAKE_REQUIRED_LIBRARIES ${ZLIB_LIBRARIES})
endif ()

find_package(BZip2 MODULE)
set(FOLLY_HAVE_LIBBZ2 ${BZIP2_FOUND})
if (BZIP2_FOUND)
  list(APPEND FOLLY_INCLUDE_DIRECTORIES ${BZIP2_INCLUDE_DIRS})
endif ()
folly_add_external_library(bz2 LIBRARIES ${BZIP2_LIBRARIES})

find_package(LibLZMA MODULE)
set(FOLLY_HAVE_LIBLZMA ${LIBLZMA_FOUND})
if (LIBLZMA_FOUND)
  list(APPEND FOLLY_INCLUDE_DIRECTORIES ${LIBLZMA_INCLUDE_DIRS})
endif ()
folly_add_external_library(lzma LIBRARIES ${LIBLZMA_LIBRARIES})

find_package(LZ4 MODULE)
set(FOLLY_HAVE_LIBLZ4 ${LZ4_FOUND})
if (LZ4_FOUND)
  list(APPEND FOLLY_INCLUDE_DIRECTORIES ${LZ4_INCLUDE_DIR})
endif ()
folly_add_external_library(lz4 LIBRARIES ${LZ4_LIBRARY})

find_package(Zstd MODULE)
set(FOLLY_HAVE_LIBZSTD ${ZSTD_FOUND})
if (ZSTD_FOUND)
  list(APPEND FOLLY_INCLUDE_DIRECTORIES ${ZSTD_INCLUDE_DIR})
endif ()
folly_add_external_library(zstd LIBRARIES ${ZSTD_LIBRARY})

find_package(Snappy MODULE)
set(FOLLY_HAVE_LIBSNAPPY ${SNAPPY_FOUND})
if (SNAPPY_FOUND)
  list(APPEND FOLLY_INCLUDE_DIRECTORIES ${SNAPPY_INCLUDE_DIR})
endif ()
folly_add_external_library(snappy LIBRARIES ${SNAPPY_LIBRARY})

find_package(Libiberty)
folly_add_external_library(iberty LIBRARIES ${LIBIBERTY_LIBRARIES})
list(APPEND FOLLY_INCLUDE_DIRECTORIES ${LIBIBERTY_INCLUDE_DIRS})

find_package(LibAIO)
folly_add_external_library(aio LIBRARIES ${LIBAIO_LIBRARIES})
list(APPEND FOLLY_INCLUDE_DIRECTORIES ${LIBAIO_INCLUDE_DIRS})

find_package(LibUring)
folly_add_external_library(uring LIBRARIES ${LIBURING_LIBRARIES})
list(APPEND FOLLY_INCLUDE_DIRECTORIES ${LIBURING_INCLUDE_DIRS})

find_package(Libsodium)
# LtHash's templates call into libsodium.
folly_add_external_library(sodium MONOLITH_PUBLIC
                           LIBRARIES ${LIBSODIUM_LIBRARIES})
list(APPEND FOLLY_INCLUDE_DIRECTORIES ${LIBSODIUM_INCLUDE_DIRS})

list(APPEND FOLLY_LINK_LIBRARIES ${CMAKE_DL_LIBS})
list(APPEND CMAKE_REQUIRED_LIBRARIES ${CMAKE_DL_LIBS})

if (PYTHON_EXTENSIONS)
  find_package(
    Python3
    COMPONENTS Interpreter Development
    REQUIRED)
  find_package(Cython 0.26 REQUIRED)
endif ()

find_package(LibUnwind)
folly_add_external_library(unwind LIBRARIES ${LIBUNWIND_LIBRARIES})
list(APPEND FOLLY_INCLUDE_DIRECTORIES ${LIBUNWIND_INCLUDE_DIRS})
if (LIBUNWIND_FOUND)
  set(FOLLY_HAVE_LIBUNWIND ON)
endif ()

cmake_push_check_state()
set(CMAKE_REQUIRED_DEFINITIONS -D_XOPEN_SOURCE)
check_cxx_symbol_exists(swapcontext ucontext.h FOLLY_HAVE_SWAPCONTEXT)
cmake_pop_check_state()

set(FOLLY_USE_SYMBOLIZER OFF)
check_include_file_cxx(elf.h FOLLY_HAVE_ELF)
find_package(Backtrace)
# backtrace() is in libc on glibc and macOS.
set(folly_backtrace_libraries ${Backtrace_LIBRARIES})
if (CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
  list(APPEND folly_backtrace_libraries execinfo)
endif ()
folly_add_external_library(backtrace LIBRARIES ${folly_backtrace_libraries})

set(FOLLY_HAVE_BACKTRACE ${Backtrace_FOUND})
# folly reads DWARF itself and needs no library for it, only ELF. It has not
# been validated on FreeBSD.
if (CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
  set(FOLLY_HAVE_DWARF OFF)
else ()
  set(FOLLY_HAVE_DWARF ${FOLLY_HAVE_ELF})
endif ()
if (NOT WIN32 AND NOT APPLE)
  set(FOLLY_USE_SYMBOLIZER ON)
endif ()
message(STATUS "Setting FOLLY_USE_SYMBOLIZER: ${FOLLY_USE_SYMBOLIZER}")
message(STATUS "Setting FOLLY_HAVE_ELF: ${FOLLY_HAVE_ELF}")
message(STATUS "Setting FOLLY_HAVE_DWARF: ${FOLLY_HAVE_DWARF}")

# Using clang with libstdc++ requires explicitly linking against libatomic
check_cxx_source_compiles(
  "
  #include <atomic>
  int main(int argc, char** argv) {
    std::atomic<uint8_t> a1;
    std::atomic<uint16_t> a2;
    std::atomic<uint32_t> a4;
    std::atomic<uint64_t> a8;
    struct Test { bool val; };
    std::atomic<Test> s;
    return a1++ + a2++ + a4++ + a8++ + unsigned(s.is_lock_free());
  }"
  FOLLY_CPP_ATOMIC_BUILTIN)
if (NOT FOLLY_CPP_ATOMIC_BUILTIN)
  list(APPEND CMAKE_REQUIRED_LIBRARIES atomic)
  list(APPEND FOLLY_LINK_LIBRARIES atomic)
  set(ATOMIC_LIBRARY "atomic")
  check_cxx_source_compiles(
    "
    #include <atomic>
    int main(int argc, char** argv) {
      std::atomic<uint8_t> a1;
      std::atomic<uint16_t> a2;
      std::atomic<uint32_t> a4;
      std::atomic<uint64_t> a8;
      struct Test { bool val; };
      std::atomic<Test> s;
      return a1++ + a2++ + a4++ + a8++ + unsigned(s.is_lock_free());
    }"
    FOLLY_CPP_ATOMIC_WITH_LIBATOMIC)
  if (NOT FOLLY_CPP_ATOMIC_WITH_LIBATOMIC)
    message(FATAL_ERROR "unable to link C++ std::atomic code: you may need \
      to install GNU libatomic")
  endif ()
endif ()

check_cxx_source_compiles(
  "
  #include <type_traits>
  #if _GLIBCXX_RELEASE
  int main() {}
  #endif"
  FOLLY_STDLIB_LIBSTDCXX)
check_cxx_source_compiles(
  "
  #include <type_traits>
  #if _GLIBCXX_RELEASE >= 9
  int main() {}
  #endif"
  FOLLY_STDLIB_LIBSTDCXX_GE_9)
check_cxx_source_compiles(
  "
  #include <type_traits>
  #if _LIBCPP_VERSION
  int main() {}
  #endif"
  FOLLY_STDLIB_LIBCXX)
check_cxx_source_compiles(
  "
  #include <type_traits>
  #if _LIBCPP_VERSION >= 9000
  int main() {}
  #endif"
  FOLLY_STDLIB_LIBCXX_GE_9)
check_cxx_source_compiles(
  "
  #include <type_traits>
  #if _CPPLIB_VER
  int main() {}
  #endif"
  FOLLY_STDLIB_LIBCPP)

if (APPLE)
  list(APPEND CMAKE_REQUIRED_LIBRARIES c++abi)
  list(APPEND FOLLY_LINK_LIBRARIES c++abi)
endif ()

if (FOLLY_STDLIB_LIBSTDCXX AND NOT FOLLY_STDLIB_LIBSTDCXX_GE_9)
  list(APPEND CMAKE_REQUIRED_LIBRARIES stdc++fs)
  list(APPEND FOLLY_LINK_LIBRARIES stdc++fs)
endif ()
if (FOLLY_STDLIB_LIBCXX AND NOT FOLLY_STDLIB_LIBCXX_GE_9)
  list(APPEND CMAKE_REQUIRED_LIBRARIES c++fs)
  list(APPEND FOLLY_LINK_LIBRARIES c++fs)
endif ()

option(FOLLY_LIBRARY_SANITIZE_ADDRESS
       "Build folly with Address Sanitizer enabled." OFF)

if ($ENV{WITH_ASAN})
  message(STATUS "ENV WITH_ASAN is set")
  set(FOLLY_LIBRARY_SANITIZE_ADDRESS ON)
endif ()

if (FOLLY_LIBRARY_SANITIZE_ADDRESS)
  if ("${CMAKE_CXX_COMPILER_ID}" MATCHES GNU)
    set(FOLLY_LIBRARY_SANITIZE_ADDRESS ON)
    set(FOLLY_ASAN_FLAGS -fsanitize=address,undefined)
    list(APPEND FOLLY_CXX_FLAGS ${FOLLY_ASAN_FLAGS})
    # All of the functions in folly/detail/Sse.cpp are intended to be compiled
    # with ASAN disabled.  They are marked with attributes to disable the
    # sanitizer, but even so, gcc fails to compile them for some reason when
    # sanitization is enabled on the compile line.
    set_source_files_properties(
      "${PROJECT_SOURCE_DIR}/folly/detail/Sse.cpp"
      PROPERTIES COMPILE_FLAGS -fno-sanitize=address,undefined)
  elseif ("${CMAKE_CXX_COMPILER_ID}" MATCHES Clang)
    set(FOLLY_LIBRARY_SANITIZE_ADDRESS ON)
    set(FOLLY_ASAN_FLAGS
        -fno-common -fsanitize=address,undefined,integer,nullability
        -fno-sanitize=unsigned-integer-overflow)
    list(APPEND FOLLY_CXX_FLAGS ${FOLLY_ASAN_FLAGS})
  endif ()
endif ()

# folly/cli's ProgramOptions needs gflags and Boost.ProgramOptions. The library,
# its installed headers and the components folly-config.cmake asks for all
# follow this.
if (TARGET Boost::program_options AND (LIBGFLAGS_FOUND OR gflags_FOUND))
  set(FOLLY_BUILD_CLI_PROGRAM_OPTIONS ON)
  set(FOLLY_BOOST_PROGRAM_OPTIONS program_options)
else ()
  set(FOLLY_BUILD_CLI_PROGRAM_OPTIONS OFF)
  set(FOLLY_BOOST_PROGRAM_OPTIONS "")
endif ()

add_library(folly_deps INTERFACE)

if (folly_openssl_external_project)
  add_dependencies(folly_deps ${folly_openssl_external_project})
endif ()

find_package(fmt CONFIG)
if (NOT DEFINED fmt_CONFIG)
  # Fallback on a normal search on the current system.
  find_package(Fmt MODULE)
endif ()
if (NOT TARGET fmt::fmt)
  folly_fetch_from_manifest(fmt fmt)
  set_property(GLOBAL APPEND PROPERTY FOLLY_BUILD_LOCAL_TARGETS fmt fmt::fmt)
endif ()
folly_localize_deps(folly_fmt_dep fmt::fmt)
target_link_libraries(folly_deps INTERFACE ${folly_fmt_dep})

list(REMOVE_DUPLICATES FOLLY_INCLUDE_DIRECTORIES)
# A failed find_path() leaves <VAR>-NOTFOUND behind, which would reach the
# compiler as a relative include directory.
list(FILTER FOLLY_INCLUDE_DIRECTORIES EXCLUDE REGEX "-NOTFOUND$")
foreach (_dir IN LISTS FOLLY_INCLUDE_DIRECTORIES)
  # Wrap build-tree paths in BUILD_INTERFACE so they do not leak into an
  # install interface, whether folly is the top project or a subproject. An
  # installed folly re-finds these through folly-config.cmake.
  string(FIND "${_dir}" "${CMAKE_BINARY_DIR}/" _dir_in_build_tree)
  if (NOT "${CMAKE_SOURCE_DIR}" STREQUAL "${PROJECT_SOURCE_DIR}"
      OR _dir_in_build_tree EQUAL 0)
    target_include_directories(folly_deps INTERFACE $<BUILD_INTERFACE:${_dir}>)
  else ()
    target_include_directories(folly_deps INTERFACE ${_dir})
  endif ()
endforeach ()
folly_localize_deps(folly_deps_link ${FOLLY_LINK_LIBRARIES}
                    ${FOLLY_SHINY_DEPENDENCIES} ${FOLLY_ASAN_FLAGS})
target_link_libraries(folly_deps INTERFACE ${folly_deps_link})
