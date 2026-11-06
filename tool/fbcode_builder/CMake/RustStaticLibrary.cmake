# Copyright (c) Meta Platforms, Inc. and affiliates.

include(FBCMakeParseArgs)

set(
  USE_CARGO_VENDOR AUTO CACHE STRING
  "Download Rust Crates from an internally vendored location"
)
set_property(CACHE USE_CARGO_VENDOR PROPERTY STRINGS AUTO ON OFF)

set(
  GENERATE_CARGO_VENDOR_CONFIG AUTO CACHE STRING
  "Whether to generate Rust cargo vendor config or use existing"
)
set_property(CACHE GENERATE_CARGO_VENDOR_CONFIG PROPERTY STRINGS AUTO ON OFF)

set(RUST_VENDORED_CRATES_DIR "$ENV{RUST_VENDORED_CRATES_DIR}")

if("${USE_CARGO_VENDOR}" STREQUAL "AUTO")
  if(EXISTS "${RUST_VENDORED_CRATES_DIR}")
    set(USE_CARGO_VENDOR ON)
  else()
    set(USE_CARGO_VENDOR OFF)
  endif()
endif()

if("${GENERATE_CARGO_VENDOR_CONFIG}" STREQUAL "AUTO")
  set(GENERATE_CARGO_VENDOR_CONFIG "${USE_CARGO_VENDOR}")
endif()

if(GENERATE_CARGO_VENDOR_CONFIG)
  if(NOT EXISTS "${RUST_VENDORED_CRATES_DIR}")
    message(
      FATAL "vendored rust crates not present: "
      "${RUST_VENDORED_CRATES_DIR}"
    )
  endif()

  set(RUST_CARGO_HOME "${CMAKE_BINARY_DIR}/_cargo_home")
  file(MAKE_DIRECTORY "${RUST_CARGO_HOME}")

  file(
    TO_NATIVE_PATH "${RUST_VENDORED_CRATES_DIR}"
    ESCAPED_RUST_VENDORED_CRATES_DIR
  )
  string(
    REPLACE "\\" "\\\\"
    ESCAPED_RUST_VENDORED_CRATES_DIR
    "${ESCAPED_RUST_VENDORED_CRATES_DIR}"
  )
  file(
    WRITE "${RUST_CARGO_HOME}/config"
    "[source.crates-io]\n"
    "replace-with = \"vendored-sources\"\n"
    "\n"
    "[source.vendored-sources]\n"
    "directory = \"${ESCAPED_RUST_VENDORED_CRATES_DIR}\"\n"
  )
endif()

find_program(CARGO_COMMAND cargo REQUIRED)

# Cargo is a build system in itself, and thus will try to take advantage of all
# the cores on the system. Unfortunately, this conflicts with Ninja, since it
# also tries to utilize all the cores. This can lead to a system that is
# completely overloaded with compile jobs to the point where nothing else can
# be achieved on the system.
#
# Let's inform Ninja of this fact so it won't try to spawn other jobs while
# Rust being compiled.
set_property(GLOBAL APPEND PROPERTY JOB_POOLS rust_job_pool=1)

# This function creates an interface library target based on the static library
# built by Cargo. It will call Cargo to build a staticlib and generate a CMake
# interface library with it.
#
# This function requires `find_package(Python COMPONENTS Interpreter)`.
#
# You need to set `lib:crate-type = ["staticlib"]` in your Cargo.toml to make
# Cargo build static library.
#
# ```cmake
# rust_static_library(<TARGET> [CRATE <CRATE_NAME>] [PACKAGE <PACKAGE_NAME>]
#   [FEATURES <FEATURE_NAME>] [USE_CXX_INCLUDE] [ENV <NAME=VALUE>...]
#   [LINK_LIBRARIES <library>...])
# ```
#
# Parameters:
# - TARGET:
#   Name of the target name. This function will create an interface library
#   target with this name.
# - CRATE_NAME:
#   Name of the crate. This parameter is optional. If unspecified, it will
#   fallback to `${TARGET}`.
# - PACKAGE_NAME:
#   Name of the Cargo package that contains the crate, when it differs from
#   the crate name. This parameter is optional. If unspecified, it will
#   fallback to `${CRATE_NAME}`.
# - FEATURE_NAME:
#   Name of the Rust feature to enable.
# - USE_CXX_INCLUDE:
#   Include cxx.rs include path in `${TARGET}` INTERFACE.
# - ENV:
#   A list of `NAME=VALUE` pairs to set in the environment of `cargo build`.
# - LINK_LIBRARIES:
#   Libraries that have to come after the static library on a link line, such
#   as the C++ side of its cxx bridges.
#
# This function creates three targets:
# - "${TARGET}": an interface library target contains the static library built
#   from Cargo.
# - "${TARGET}.cargo": an internal custom target that invokes Cargo.
# - "${TARGET}.archive": an internal imported target for the static library,
#   which orders LINK_LIBRARIES after it.
#
# If you are going to use this static library from C/C++, you will need to
# write header files for the library (or generate with cbindgen) and bind these
# headers with the interface library.
#
function(rust_static_library TARGET)
  fb_cmake_parse_args(
    ARG "USE_CXX_INCLUDE" "CRATE;PACKAGE;FEATURES" "ENV;LINK_LIBRARIES" "${ARGN}"
  )

  if(DEFINED ARG_CRATE)
    set(crate_name "${ARG_CRATE}")
  else()
    set(crate_name "${TARGET}")
  endif()
  if(DEFINED ARG_PACKAGE)
    set(package_name "${ARG_PACKAGE}")
  else()
    set(package_name "${crate_name}")
  endif()
  if(DEFINED ARG_FEATURES)
    set(features --features ${ARG_FEATURES})
  else()
    set(features )
  endif()

  set(cargo_target "${TARGET}.cargo")
  set(target_dir $<IF:$<CONFIG:Debug>,debug,release>)
  set(staticlib_name "${CMAKE_STATIC_LIBRARY_PREFIX}${crate_name}${CMAKE_STATIC_LIBRARY_SUFFIX}")
  set(rust_staticlib "${CMAKE_CURRENT_BINARY_DIR}/${target_dir}/${staticlib_name}")

  if(DEFINED ARG_FEATURES)
    set(cargo_flags build $<IF:$<CONFIG:Debug>,,--release> -p ${package_name} --features ${ARG_FEATURES} --config fbcode_build=false)
  else()
    set(cargo_flags build $<IF:$<CONFIG:Debug>,,--release> -p ${package_name} --config fbcode_build=false)
  endif()
  if(USE_CARGO_VENDOR)
    set(extra_cargo_env "CARGO_HOME=${RUST_CARGO_HOME}")
    set(cargo_flags ${cargo_flags})
  endif()

  add_custom_target(
    ${cargo_target}
    COMMAND
      "${CMAKE_COMMAND}" -E remove -f "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock"
    COMMAND
      "${CMAKE_COMMAND}" -E env
      "CARGO_TARGET_DIR=${CMAKE_CURRENT_BINARY_DIR}"
      ${extra_cargo_env}
      ${ARG_ENV}
      ${CARGO_COMMAND}
      ${cargo_flags}
    COMMENT "Building Rust crate '${crate_name}'..."
    JOB_POOL rust_job_pool
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    BYPRODUCTS
      "${CMAKE_CURRENT_BINARY_DIR}/debug/${staticlib_name}"
      "${CMAKE_CURRENT_BINARY_DIR}/release/${staticlib_name}"
  )

  add_library(${TARGET} INTERFACE)
  add_dependencies(${TARGET} ${cargo_target})
  set_target_properties(
    ${TARGET}
    PROPERTIES
      INTERFACE_STATICLIB_OUTPUT_PATH "${rust_staticlib}"
      INTERFACE_INSTALL_LIBNAME
        "${CMAKE_STATIC_LIBRARY_PREFIX}${crate_name}_rs${CMAKE_STATIC_LIBRARY_SUFFIX}"
  )

  if(DEFINED ARG_USE_CXX_INCLUDE)
    target_include_directories(
      ${TARGET}
      INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/cxxbridge/
    )
  endif()

  # CMake orders a target's link dependencies after it, but not a plain path's.
  add_library(${TARGET}.archive STATIC IMPORTED GLOBAL)
  set_target_properties(
    ${TARGET}.archive
    PROPERTIES
      IMPORTED_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/release/${staticlib_name}"
      IMPORTED_LOCATION_DEBUG "${CMAKE_CURRENT_BINARY_DIR}/debug/${staticlib_name}"
  )
  target_link_libraries(${TARGET}.archive INTERFACE ${ARG_LINK_LIBRARIES})
  target_link_libraries(
    ${TARGET}
    INTERFACE "$<BUILD_INTERFACE:${TARGET}.archive>"
  )
endfunction()

# This function instructs CMake to define a target that will use `cargo build`
# to build a bin crate referenced by the Cargo.toml file in the current source
# directory.
# It accepts a single `TARGET` parameter which will be passed as the package
# name to `cargo build -p TARGET`. If binary has different name as package,
# use optional flag BINARY_NAME to override it.
# It also accepts a `FEATURES` parameter if you want to enable certain features
# in your Rust binary, and an `ENV` list of `NAME=VALUE` pairs to set in the
# environment of `cargo build`.
# The CMake target will be registered to build by default as part of the
# ALL target.
function(rust_executable TARGET)
  fb_cmake_parse_args(ARG "" "BINARY_NAME;FEATURES" "ENV" "${ARGN}")

  set(crate_name "${TARGET}")
  set(cargo_target "${TARGET}.cargo")
  set(target_dir $<IF:$<CONFIG:Debug>,debug,release>)

  if(DEFINED ARG_BINARY_NAME)
    set(executable_name "${ARG_BINARY_NAME}${CMAKE_EXECUTABLE_SUFFIX}")
  else()
    set(executable_name "${crate_name}${CMAKE_EXECUTABLE_SUFFIX}")
  endif()
  if(DEFINED ARG_FEATURES)
    set(features --features ${ARG_FEATURES})
  else()
    set(features )
  endif()

  if(DEFINED ARG_FEATURES)
    set(cargo_flags build $<IF:$<CONFIG:Debug>,,--release> -p ${crate_name} --features ${ARG_FEATURES})
  else()
    set(cargo_flags build $<IF:$<CONFIG:Debug>,,--release> -p ${crate_name})
  endif()
  if(USE_CARGO_VENDOR)
    set(extra_cargo_env "CARGO_HOME=${RUST_CARGO_HOME}")
    set(cargo_flags ${cargo_flags})
  endif()

  add_custom_target(
    ${cargo_target}
    ALL
    COMMAND
      "${CMAKE_COMMAND}" -E remove -f "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock"
    COMMAND
      "${CMAKE_COMMAND}" -E env
      "CARGO_TARGET_DIR=${CMAKE_CURRENT_BINARY_DIR}"
      ${extra_cargo_env}
      ${ARG_ENV}
      ${CARGO_COMMAND}
      ${cargo_flags}
    COMMENT "Building Rust executable '${crate_name}'..."
    JOB_POOL rust_job_pool
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    BYPRODUCTS
      "${CMAKE_CURRENT_BINARY_DIR}/debug/${executable_name}"
      "${CMAKE_CURRENT_BINARY_DIR}/release/${executable_name}"
  )

  set_property(TARGET "${cargo_target}"
      PROPERTY EXECUTABLE "${CMAKE_CURRENT_BINARY_DIR}/${target_dir}/${executable_name}")
endfunction()

# This function can be used to install the executable generated by a prior
# call to the `rust_executable` function.
# It requires a `TARGET` parameter to identify the target to be installed,
# and an optional `DESTINATION` parameter to specify the installation
# directory.  If DESTINATION is not specified then the `bin` directory
# will be assumed.
function(install_rust_executable TARGET)
  # Parse the arguments
  set(one_value_args DESTINATION)
  set(multi_value_args)
  fb_cmake_parse_args(
    ARG "" "${one_value_args}" "${multi_value_args}" "${ARGN}"
  )

  if(NOT DEFINED ARG_DESTINATION)
    set(ARG_DESTINATION bin)
  endif()

  get_target_property(foo "${TARGET}.cargo" EXECUTABLE)

  install(
    PROGRAMS "${foo}"
    DESTINATION "${ARG_DESTINATION}"
  )
endfunction()

# This function installs the interface target generated from the function
# `rust_static_library`. Use this function if you want to export your Rust
# target to external CMake targets.
#
# ```cmake
# install_rust_static_library(
#   <TARGET>
#   INSTALL_DIR <INSTALL_DIR>
#   [EXPORT <EXPORT_NAME>]
# )
# ```
#
# Parameters:
# - TARGET: Name of the Rust static library target.
# - EXPORT_NAME: Name of the exported target.
# - INSTALL_DIR: Path to the directory where this library will be installed.
#
function(install_rust_static_library TARGET)
  fb_cmake_parse_args(ARG "" "EXPORT;INSTALL_DIR" "" "${ARGN}")

  get_property(
    staticlib_output_path
    TARGET "${TARGET}"
    PROPERTY INTERFACE_STATICLIB_OUTPUT_PATH
  )
  get_property(
    staticlib_output_name
    TARGET "${TARGET}"
    PROPERTY INTERFACE_INSTALL_LIBNAME
  )

  if(NOT DEFINED staticlib_output_path)
    message(FATAL_ERROR "Not a rust_static_library target.")
  endif()

  if(NOT DEFINED ARG_INSTALL_DIR)
    message(FATAL_ERROR "Missing required argument.")
  endif()

  if(DEFINED ARG_EXPORT)
    set(install_export_args EXPORT "${ARG_EXPORT}")
  endif()

  set(install_interface_dir "${ARG_INSTALL_DIR}")
  if(NOT IS_ABSOLUTE "${install_interface_dir}")
    set(install_interface_dir "\${_IMPORT_PREFIX}/${install_interface_dir}")
  endif()

  target_link_libraries(
    ${TARGET} INTERFACE
    "$<INSTALL_INTERFACE:${install_interface_dir}/${staticlib_output_name}>"
  )
  install(
    TARGETS ${TARGET}
    ${install_export_args}
    LIBRARY DESTINATION ${ARG_INSTALL_DIR}
  )
  install(
    FILES ${staticlib_output_path}
    RENAME ${staticlib_output_name}
    DESTINATION ${ARG_INSTALL_DIR}
  )
endfunction()

# This function creates C++ bindings using the [cxx] crate.
#
# Original function found here: https://github.com/corrosion-rs/corrosion/blob/master/cmake/Corrosion.cmake#L1390
# Simplified for use as part of RustStaticLibrary module. License below.
#
# MIT License
#
# Copyright (c) 2018 Andrew Gaspar
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#
# The rules approximately do the following:
# - Check which version of `cxx` the Rust crate depends on.
# - Check if the exact same version of `cxxbridge-cmd` is installed
# - If not, create a rule to build the exact same version of `cxxbridge-cmd`.
# - Create rules to run `cxxbridge` and generate
#   - The `rust/cxx.h` header
#   - A header and source file for the specified CXX_BRIDGE_FILE.
# - The generated sources (and header include directories) are added to the
#   `${TARGET}` CMake library target.
#
# ```cmake
# rust_cxx_bridge(<TARGET> <CXX_BRIDGE_FILE> [CRATE <CRATE_NAME>] [LIBS <LIBNAMES>]
#                 [CXX_VERSION_DIR <DIR>])
# ```
#
# Parameters:
# - TARGET:
#   Name of the target name. The target that the bridge will be included with.
# - CXX_BRIDGE_FILE:
#   Name of the file that include the cxxbridge (e.g., "src/ffi.rs").
# - CRATE_NAME:
#   Name of the crate. This parameter is optional. If unspecified, it will
#   fallback to `${TARGET}`.
# - LIBS <lib1> [<lib2> ...]:
#   A list of libraries that this library depends on.
# - CXX_VERSION_DIR:
#   Directory of the crate whose version of cxx to match. This parameter is
#   optional, and defaults to the current source directory. A crate built into
#   the static library of another Cargo workspace links that workspace's cxx,
#   which can be another version.
#
function(rust_cxx_bridge TARGET CXX_BRIDGE_FILE)
  fb_cmake_parse_args(ARG "" "CRATE;CXX_VERSION_DIR" "LIBS" "${ARGN}")

  if(DEFINED ARG_CRATE)
    set(crate_name "${ARG_CRATE}")
  else()
    set(crate_name "${TARGET}")
  endif()

  _rust_cxxbridge(cxxbridge cxxbridge_target "${ARG_CXX_VERSION_DIR}")

  add_library(${crate_name} STATIC)
  target_include_directories(
    ${crate_name}
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
  )
  target_link_libraries(
    ${crate_name}
    PUBLIC
    ${ARG_LIBS}
  )

  get_filename_component(filename_component ${CXX_BRIDGE_FILE} NAME)
  get_filename_component(directory_component ${CXX_BRIDGE_FILE} DIRECTORY)
  set(directory "")
  if(directory_component)
    set(directory "${directory_component}")
  endif()

  set(cxx_header ${directory}/${filename_component}.h)
  set(cxx_source ${directory}/${filename_component}.cc)
  set(rust_source_path "${CMAKE_CURRENT_SOURCE_DIR}/${CXX_BRIDGE_FILE}")

  file(
    MAKE_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/${directory_component}"
  )

  add_custom_command(
    OUTPUT
      "${CMAKE_CURRENT_BINARY_DIR}/${cxx_header}"
      "${CMAKE_CURRENT_BINARY_DIR}/${cxx_source}"
    COMMAND
      ${cxxbridge} ${rust_source_path}
        --cfg fbcode_build=false
        --header
        --output "${CMAKE_CURRENT_BINARY_DIR}/${cxx_header}"
    COMMAND
      ${cxxbridge} ${rust_source_path}
        --cfg fbcode_build=false
        --output "${CMAKE_CURRENT_BINARY_DIR}/${cxx_source}"
        --include "${cxx_header}"
    DEPENDS "${cxxbridge_target}" "${rust_source_path}"
    COMMENT "Generating cxx bindings for crate ${crate_name}"
  )

  target_sources(
    ${crate_name}
    PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/${cxx_header}"
    "${CMAKE_CURRENT_BINARY_DIR}/rust/cxx.h"
    "${CMAKE_CURRENT_BINARY_DIR}/${cxx_source}"
  )
endfunction()

# This function generates the `rust/cxx.h` header of the [cxx] crate for C++
# code that uses cxx types, such as `rust::String`, without a bridge of its
# own. The header matches the version of cxx that the crate in the current
# source directory, or in CXX_VERSION_DIR, depends on.
#
# ```cmake
# rust_cxx_header(<TARGET> [LIBS <LIBNAMES>] [CXX_VERSION_DIR <DIR>])
# ```
#
# Parameters:
# - TARGET:
#   Name of the interface library target to create, which puts the header on
#   the include path. An internal custom target, `${TARGET}.header`, generates
#   it.
# - LIBS <lib1> [<lib2> ...]:
#   A list of libraries that this library depends on, such as the Rust static
#   library that contains the cxx runtime.
# - CXX_VERSION_DIR:
#   As for rust_cxx_bridge().
#
function(rust_cxx_header TARGET)
  fb_cmake_parse_args(ARG "" "CXX_VERSION_DIR" "LIBS" "${ARGN}")

  _rust_cxxbridge(cxxbridge cxxbridge_target "${ARG_CXX_VERSION_DIR}")
  add_custom_target(
    ${TARGET}.header
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/rust/cxx.h"
  )
  add_library(${TARGET} INTERFACE)
  target_include_directories(${TARGET} INTERFACE "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(${TARGET} INTERFACE ${ARG_LIBS})
  add_dependencies(${TARGET} ${TARGET}.header)
endfunction()

# Sets <CXXBRIDGE_VAR> to a cxxbridge with the version of cxx that the crate in
# <CXX_VERSION_DIR>, or else the current source directory, depends on, and
# <DEPENDS_VAR> to the target that commands running it depend on, which runs
# `cargo install cxxbridge-cmd` when no installed cxxbridge has that version.
# Also adds the rule that generates `rust/cxx.h` in the current binary
# directory.
function(_rust_cxxbridge CXXBRIDGE_VAR DEPENDS_VAR CXX_VERSION_DIR)
  if(NOT CXX_VERSION_DIR)
    set(CXX_VERSION_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
  if(USE_CARGO_VENDOR)
    set(extra_cargo_env "CARGO_HOME=${RUST_CARGO_HOME}")
  endif()

  execute_process(
    COMMAND
      "${CMAKE_COMMAND}" -E env
      ${extra_cargo_env}
      "${CARGO_COMMAND}" tree -i cxx --depth=0
    WORKING_DIRECTORY "${CXX_VERSION_DIR}"
    RESULT_VARIABLE cxx_version_result
    OUTPUT_VARIABLE cxx_version_output
    ERROR_VARIABLE cxx_version_error
  )

  if(NOT "${cxx_version_result}" EQUAL "0")
    if(cxx_version_error MATCHES "did not match any packages")
      message(
        FATAL_ERROR
        "The crate in ${CXX_VERSION_DIR} does not depend on cxx."
      )
    endif()
    message(
      FATAL_ERROR
      "`cargo tree -i cxx` failed in ${CXX_VERSION_DIR}:\n${cxx_version_error}"
    )
  endif()
  if(cxx_version_output MATCHES "cxx v([0-9]+.[0-9]+.[0-9]+)")
    set(cxx_required_version "${CMAKE_MATCH_1}")
  else()
    message(
      FATAL_ERROR
      "Failed to parse cxx version from cargo tree output: `cxx_version_output`")
  endif()

  # First check if a suitable version of cxxbridge is installed
  find_program(INSTALLED_CXXBRIDGE cxxbridge PATHS "$ENV{HOME}/.cargo/bin/")
  mark_as_advanced(INSTALLED_CXXBRIDGE)
  if(INSTALLED_CXXBRIDGE)
    execute_process(
      COMMAND "${INSTALLED_CXXBRIDGE}" --version
      OUTPUT_VARIABLE cxxbridge_version_output
    )
    if(cxxbridge_version_output MATCHES "cxxbridge ([0-9]+.[0-9]+.[0-9]+)")
      set(cxxbridge_version "${CMAKE_MATCH_1}")
    else()
      set(cxxbridge_version "")
    endif()
  endif()

  set(cxxbridge "")
  if(cxxbridge_version)
    if(cxxbridge_version VERSION_EQUAL cxx_required_version)
      set(cxxbridge "${INSTALLED_CXXBRIDGE}")
      if(NOT TARGET "cxxbridge_v${cxx_required_version}")
        # Add an empty target.
        add_custom_target("cxxbridge_v${cxx_required_version}")
      endif()
    endif()
  endif()

  # No suitable version of cxxbridge was installed,
  # so use custom target to install correct version.
  if(NOT cxxbridge)
    if(NOT TARGET "cxxbridge_v${cxx_required_version}")
      add_custom_command(
        OUTPUT
          "${CMAKE_BINARY_DIR}/cxxbridge_v${cxx_required_version}/bin/cxxbridge"
        COMMAND
          "${CMAKE_COMMAND}" -E make_directory
          "${CMAKE_BINARY_DIR}/cxxbridge_v${cxx_required_version}"
        COMMAND
          "${CMAKE_COMMAND}" -E remove -f "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock"
        COMMAND
          "${CMAKE_COMMAND}" -E env
          ${extra_cargo_env}
          "${CARGO_COMMAND}" install cxxbridge-cmd
          --version "${cxx_required_version}"
          --root "${CMAKE_BINARY_DIR}/cxxbridge_v${cxx_required_version}"
          --quiet
        COMMAND
          "${CMAKE_COMMAND}" -E remove -f "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock"
        COMMENT "Installing cxxbridge (version ${cxx_required_version})"
      )
      add_custom_target(
        "cxxbridge_v${cxx_required_version}"
        DEPENDS "${CMAKE_BINARY_DIR}/cxxbridge_v${cxx_required_version}/bin/cxxbridge"
      )
    endif()
    set(
      cxxbridge
      "${CMAKE_BINARY_DIR}/cxxbridge_v${cxx_required_version}/bin/cxxbridge"
    )
  endif()

  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/rust")
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/rust/cxx.h"
    COMMAND
      "${cxxbridge}" --header --output "${CMAKE_CURRENT_BINARY_DIR}/rust/cxx.h"
    DEPENDS "cxxbridge_v${cxx_required_version}"
    COMMENT "Generating rust/cxx.h header"
  )

  set("${CXXBRIDGE_VAR}" "${cxxbridge}" PARENT_SCOPE)
  set("${DEPENDS_VAR}" "cxxbridge_v${cxx_required_version}" PARENT_SCOPE)
endfunction()
