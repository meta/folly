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

function (auto_sources RETURN_VALUE PATTERN SOURCE_SUBDIRS)
  if ("${SOURCE_SUBDIRS}" STREQUAL "RECURSE")
    set(PATH ".")
    if (${ARGC} EQUAL 4)
      list(GET ARGV 3 PATH)
    endif ()
  endif ()

  if ("${SOURCE_SUBDIRS}" STREQUAL "RECURSE")
    unset(${RETURN_VALUE})
    file(GLOB SUBDIR_FILES "${PATH}/${PATTERN}")
    list(APPEND ${RETURN_VALUE} ${SUBDIR_FILES})

    file(
      GLOB subdirs
      RELATIVE ${PATH}
      ${PATH}/*)

    foreach (DIR ${subdirs})
      if (IS_DIRECTORY ${PATH}/${DIR})
        if (NOT "${DIR}" STREQUAL "CMakeFiles")
          file(GLOB_RECURSE SUBDIR_FILES "${PATH}/${DIR}/${PATTERN}")
          list(APPEND ${RETURN_VALUE} ${SUBDIR_FILES})
        endif ()
      endif ()
    endforeach ()
  else ()
    file(GLOB ${RETURN_VALUE} "${PATTERN}")

    foreach (PATH ${SOURCE_SUBDIRS})
      file(GLOB SUBDIR_FILES "${PATH}/${PATTERN}")
      list(APPEND ${RETURN_VALUE} ${SUBDIR_FILES})
    endforeach ()
  endif ()

  set(${RETURN_VALUE}
      ${${RETURN_VALUE}}
      PARENT_SCOPE)
endfunction (auto_sources)

# Remove all files matching a set of patterns, and,
# optionally, not matching a second set of patterns,
# from a set of lists.
#
# Example:
# This will remove all files in the CPP_SOURCES list
# matching "/test/" or "Test.cpp$", but not matching
# "BobTest.cpp$".
# REMOVE_MATCHES_FROM_LISTS(CPP_SOURCES MATCHES "/test/" "Test.cpp$" IGNORE_MATCHES "BobTest.cpp$")
#
# Parameters:
#
# [...]:
# The names of the lists to remove matches from.
#
# [MATCHES ...]:
# The matches to remove from the lists.
#
# [IGNORE_MATCHES ...]:
# The matches not to remove, even if they match
# the main set of matches to remove.
function (REMOVE_MATCHES_FROM_LISTS)
  set(LISTS_TO_SEARCH)
  set(MATCHES_TO_REMOVE)
  set(MATCHES_TO_IGNORE)
  set(argumentState 0)
  foreach (arg ${ARGN})
    if ("x${arg}" STREQUAL "xMATCHES")
      set(argumentState 1)
    elseif ("x${arg}" STREQUAL "xIGNORE_MATCHES")
      set(argumentState 2)
    elseif (argumentState EQUAL 0)
      list(APPEND LISTS_TO_SEARCH ${arg})
    elseif (argumentState EQUAL 1)
      list(APPEND MATCHES_TO_REMOVE ${arg})
    elseif (argumentState EQUAL 2)
      list(APPEND MATCHES_TO_IGNORE ${arg})
    else ()
      message(FATAL_ERROR "Unknown argument state!")
    endif ()
  endforeach ()

  foreach (theList ${LISTS_TO_SEARCH})
    foreach (entry ${${theList}})
      foreach (match ${MATCHES_TO_REMOVE})
        if (${entry} MATCHES ${match})
          set(SHOULD_IGNORE OFF)
          foreach (ign ${MATCHES_TO_IGNORE})
            if (${entry} MATCHES ${ign})
              set(SHOULD_IGNORE ON)
              break()
            endif ()
          endforeach ()

          if (NOT SHOULD_IGNORE)
            list(REMOVE_ITEM ${theList} ${entry})
          endif ()
        endif ()
      endforeach ()
    endforeach ()
    set(${theList}
        ${${theList}}
        PARENT_SCOPE)
  endforeach ()
endfunction ()

# Automatically create source_group directives for the sources passed in.
function (auto_source_group rootName rootDir)
  file(TO_CMAKE_PATH "${rootDir}" rootDir)
  string(LENGTH "${rootDir}" rootDirLength)
  set(sourceGroups)
  foreach (fil ${ARGN})
    file(TO_CMAKE_PATH "${fil}" filePath)
    string(FIND "${filePath}" "/" rIdx REVERSE)
    if (rIdx EQUAL -1)
      message(
        FATAL_ERROR "Unable to locate the final forward slash in '${filePath}'!"
      )
    endif ()
    string(SUBSTRING "${filePath}" 0 ${rIdx} filePath)

    string(LENGTH "${filePath}" filePathLength)
    string(FIND "${filePath}" "${rootDir}" rIdx)
    if (rIdx EQUAL 0)
      math(EXPR filePathLength "${filePathLength} - ${rootDirLength}")
      string(SUBSTRING "${filePath}" ${rootDirLength} ${filePathLength}
                       fileGroup)

      string(REPLACE "/" "\\" fileGroup "${fileGroup}")
      set(fileGroup "\\${rootName}${fileGroup}")

      list(FIND sourceGroups "${fileGroup}" rIdx)
      if (rIdx EQUAL -1)
        list(APPEND sourceGroups "${fileGroup}")
        source_group("${fileGroup}"
                     REGULAR_EXPRESSION "${filePath}/[^/.]+.(cpp|h)$")
      endif ()
    endif ()
  endforeach ()
endfunction ()

# CMake is a pain and doesn't have an easy way to install only the files
# we actually included in our build :(
function (auto_install_files rootName rootDir)
  file(TO_CMAKE_PATH "${rootDir}" rootDir)
  string(LENGTH "${rootDir}" rootDirLength)
  set(sourceGroups)
  foreach (fil ${ARGN})
    file(TO_CMAKE_PATH "${fil}" filePath)
    string(FIND "${filePath}" "/" rIdx REVERSE)
    if (rIdx EQUAL -1)
      message(
        FATAL_ERROR "Unable to locate the final forward slash in '${filePath}'!"
      )
    endif ()
    string(SUBSTRING "${filePath}" 0 ${rIdx} filePath)

    string(LENGTH "${filePath}" filePathLength)
    string(FIND "${filePath}" "${rootDir}" rIdx)
    if (rIdx EQUAL 0)
      math(EXPR filePathLength "${filePathLength} - ${rootDirLength}")
      string(SUBSTRING "${filePath}" ${rootDirLength} ${filePathLength}
                       fileGroup)
      install(FILES ${fil}
              DESTINATION ${INCLUDE_INSTALL_DIR}/${rootName}${fileGroup})
    endif ()
  endforeach ()
endfunction ()

function (folly_define_tests)
  set(directory_count 0)
  set(test_count 0)
  set(currentArg 0)
  while (currentArg LESS ${ARGC})
    if ("x${ARGV${currentArg}}" STREQUAL "xDIRECTORY")
      math(EXPR currentArg "${currentArg} + 1")
      if (NOT currentArg LESS ${ARGC})
        message(FATAL_ERROR "Expected base directory!")
      endif ()

      set(cur_dir ${directory_count})
      math(EXPR directory_count "${directory_count} + 1")
      set(directory_${cur_dir}_name "${ARGV${currentArg}}")
      # We need a single list of sources to get source_group to work nicely.
      set(directory_${cur_dir}_source_list)

      math(EXPR currentArg "${currentArg} + 1")
      while (currentArg LESS ${ARGC})
        if ("x${ARGV${currentArg}}" STREQUAL "xDIRECTORY")
          break()
        elseif ("x${ARGV${currentArg}}" STREQUAL "xTEST"
                OR "x${ARGV${currentArg}}" STREQUAL "xBENCHMARK")
          set(cur_test ${test_count})
          math(EXPR test_count "${test_count} + 1")

          set(test_${cur_test}_is_benchmark
              $<STREQUAL:"x${ARGV${currentArg}}","xBENCHMARK">)

          math(EXPR currentArg "${currentArg} + 1")
          if (NOT currentArg LESS ${ARGC})
            message(FATAL_ERROR "Expected test name!")
          endif ()

          set(test_${cur_test}_name "${ARGV${currentArg}}")
          math(EXPR currentArg "${currentArg} + 1")
          set(test_${cur_test}_directory ${cur_dir})
          set(test_${cur_test}_content_dir)
          set(test_${cur_test}_headers)
          set(test_${cur_test}_sources)
          set(test_${cur_test}_tag)

          set(argumentState 0)
          while (currentArg LESS ${ARGC})
            if ("x${ARGV${currentArg}}" STREQUAL "xHEADERS")
              set(argumentState 1)
            elseif ("x${ARGV${currentArg}}" STREQUAL "xSOURCES")
              set(argumentState 2)
            elseif ("x${ARGV${currentArg}}" STREQUAL "xCONTENT_DIR")
              math(EXPR currentArg "${currentArg} + 1")
              if (NOT currentArg LESS ${ARGC})
                message(FATAL_ERROR "Expected content directory name!")
              endif ()
              set(test_${cur_test}_content_dir "${ARGV${currentArg}}")
            elseif (
              "x${ARGV${currentArg}}" STREQUAL "xTEST"
              OR "x${ARGV${currentArg}}" STREQUAL "xBENCHMARK"
              OR "x${ARGV${currentArg}}" STREQUAL "xDIRECTORY")
              break()
            elseif (argumentState EQUAL 0)
              if ("x${ARGV${currentArg}}" STREQUAL "xBROKEN")
                list(APPEND test_${cur_test}_tag "BROKEN")
              elseif ("x${ARGV${currentArg}}" STREQUAL "xHANGING")
                list(APPEND test_${cur_test}_tag "HANGING")
              elseif ("x${ARGV${currentArg}}" STREQUAL "xSLOW")
                list(APPEND test_${cur_test}_tag "SLOW")
              elseif ("x${ARGV${currentArg}}" STREQUAL "xWINDOWS_DISABLED")
                list(APPEND test_${cur_test}_tag "WINDOWS_DISABLED")
              elseif ("x${ARGV${currentArg}}" STREQUAL "xAPPLE_DISABLED")
                list(APPEND test_${cur_test}_tag "APPLE_DISABLED")
              else ()
                message(FATAL_ERROR "Unknown test tag '${ARGV${currentArg}}'!")
              endif ()
            elseif (argumentState EQUAL 1)
              list(
                APPEND test_${cur_test}_headers
                "${FOLLY_DIR}/${directory_${cur_dir}_name}${ARGV${currentArg}}")
            elseif (argumentState EQUAL 2)
              list(
                APPEND test_${cur_test}_sources
                "${FOLLY_DIR}/${directory_${cur_dir}_name}${ARGV${currentArg}}")
            else ()
              message(FATAL_ERROR "Unknown argument state!")
            endif ()
            math(EXPR currentArg "${currentArg} + 1")
          endwhile ()

          list(APPEND directory_${cur_dir}_source_list
               ${test_${cur_test}_sources} ${test_${cur_test}_headers})
        else ()
          message(
            FATAL_ERROR
              "Unknown argument inside directory '${ARGV${currentArg}}'!")
        endif ()
      endwhile ()
    else ()
      message(FATAL_ERROR "Unknown argument '${ARGV${currentArg}}'!")
    endif ()
  endwhile ()

  set(cur_dir 0)
  while (cur_dir LESS directory_count)
    source_group("" FILES ${directory_${cur_dir}_source_list})
    math(EXPR cur_dir "${cur_dir} + 1")
  endwhile ()

  set(cur_test 0)
  while (cur_test LESS test_count)
    set(cur_test_name ${test_${cur_test}_name})
    set(cur_dir_name ${directory_${test_${cur_test}_directory}_name})
    if ("BROKEN" IN_LIST test_${cur_test}_tag AND NOT BUILD_BROKEN_TESTS)
      message(
        "Skipping broken test ${cur_dir_name}${cur_test_name}, enable with BUILD_BROKEN_TESTS"
      )
    elseif ("SLOW" IN_LIST test_${cur_test}_tag AND NOT BUILD_SLOW_TESTS)
      message(
        "Skipping slow test ${cur_dir_name}${cur_test_name}, enable with BUILD_SLOW_TESTS"
      )
    elseif ("HANGING" IN_LIST test_${cur_test}_tag AND NOT BUILD_HANGING_TESTS)
      message(
        "Skipping hanging test ${cur_dir_name}${cur_test_name}, enable with BUILD_HANGING_TESTS"
      )
    elseif (
      "WINDOWS_DISABLED" IN_LIST test_${cur_test}_tag
      AND WIN32
      AND NOT BUILD_WINDOWS_DISABLED)
      message(
        "Skipping windows disabled test ${cur_dir_name}${cur_test_name}, enable with BUILD_WINDOWS_DISABLED"
      )
    elseif (
      "APPLE_DISABLED" IN_LIST test_${cur_test}_tag
      AND APPLE
      AND NOT BUILD_APPLE_DISABLED)
      message(
        "Skipping apple disabled test ${cur_dir_name}${cur_test_name}, enable with BUILD_APPLE_DISABLED"
      )
    elseif (${test_${cur_test}_is_benchmark} AND NOT BUILD_BENCHMARKS)
      message(
        "Skipping benchmark ${cur_dir_name}${cur_test_name}, enable with BUILD_BENCHMARKS"
      )
    else ()
      add_executable(${cur_test_name} ${test_${cur_test}_headers}
                                      ${test_${cur_test}_sources})
      if (NOT ${test_${cur_test}_is_benchmark})
        if (HAVE_CMAKE_GTEST)
          # If we have CMake's built-in gtest support use it to add each test
          # function as a separate test.
          gtest_add_tests(
            TARGET ${cur_test_name}
            WORKING_DIRECTORY "${TOP_DIR}"
            TEST_PREFIX "${cur_test_name}."
            TEST_LIST test_cases)
          set_tests_properties(${test_cases} PROPERTIES TIMEOUT 120)
        else ()
          # Otherwise add each test executable as a single test.
          add_test(
            NAME ${cur_test_name}
            COMMAND ${cur_test_name}
            WORKING_DIRECTORY "${TOP_DIR}")
          set_tests_properties(${cur_test_name} PROPERTIES TIMEOUT 120)
        endif ()
      endif ()
      if (NOT "x${test_${cur_test}_content_dir}" STREQUAL "x")
        # Copy the content directory to the output directory tree so that
        # tests can be run easily from Visual Studio without having to change
        # the working directory for each test individually.
        file(
          COPY "${FOLLY_DIR}/${cur_dir_name}${test_${cur_test}_content_dir}"
          DESTINATION
            "${CMAKE_CURRENT_BINARY_DIR}/folly/${cur_dir_name}${test_${cur_test}_content_dir}"
        )
        add_custom_command(
          TARGET ${cur_test_name}
          POST_BUILD
          COMMAND
            ${CMAKE_COMMAND} ARGS -E copy_directory
            "${FOLLY_DIR}/${cur_dir_name}${test_${cur_test}_content_dir}"
            "$<TARGET_FILE_DIR:${cur_test_name}>/folly/${cur_dir_name}${test_${cur_test}_content_dir}"
          COMMENT "Copying test content for ${cur_test_name}"
          VERBATIM)
      endif ()
      # Strip the tailing test directory name for the folder name.
      string(REPLACE "test/" "" test_dir_name "${cur_dir_name}")
      set_property(TARGET ${cur_test_name} PROPERTY FOLDER
                                                    "Tests/${test_dir_name}")
      target_link_libraries(${cur_test_name} PRIVATE folly_test_support)
      apply_folly_compile_options_to_target(${cur_test_name})
    endif ()
    math(EXPR cur_test "${cur_test} + 1")
  endwhile ()
endfunction ()

# Defines a module whose Folly and sanitizer symbols are provided by HOST.
# RESOURCE_SUBDIR defaults to folly/test. COMPILE_DEPS add compile usage
# requirements without adding link dependencies.
function (folly_add_test_module)
  cmake_parse_arguments(
    FOLLY_TEST_MODULE
    ""
    "NAME;HOST;RESOURCE_SUBDIR"
    "SOURCES;COMPILE_DEPS"
    ${ARGN}
  )

  if (FOLLY_TEST_MODULE_UNPARSED_ARGUMENTS)
    message(
      FATAL_ERROR
        "Unknown folly_add_test_module arguments: "
        "${FOLLY_TEST_MODULE_UNPARSED_ARGUMENTS}"
    )
  endif ()
  foreach (required_arg NAME HOST SOURCES)
    if (NOT FOLLY_TEST_MODULE_${required_arg})
      message(
        FATAL_ERROR "folly_add_test_module requires ${required_arg}"
      )
    endif ()
  endforeach ()

  if (WIN32)
    return()
  endif ()
  if (NOT TARGET ${FOLLY_TEST_MODULE_HOST})
    message(
      FATAL_ERROR
        "folly_add_test_module host does not exist: "
        "${FOLLY_TEST_MODULE_HOST}"
    )
  endif ()

  if (NOT FOLLY_TEST_MODULE_RESOURCE_SUBDIR)
    set(FOLLY_TEST_MODULE_RESOURCE_SUBDIR "folly/test")
  elseif (IS_ABSOLUTE "${FOLLY_TEST_MODULE_RESOURCE_SUBDIR}")
    message(
      FATAL_ERROR "folly_add_test_module RESOURCE_SUBDIR must be relative"
    )
  endif ()

  set(resource_dir
      "${CMAKE_BINARY_DIR}/${FOLLY_TEST_MODULE_RESOURCE_SUBDIR}")
  get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
  if (is_multi_config)
    set(resource_dir
        "${CMAKE_BINARY_DIR}/$<CONFIG>/${FOLLY_TEST_MODULE_RESOURCE_SUBDIR}")
  endif ()

  add_library(
    ${FOLLY_TEST_MODULE_NAME}
    MODULE ${FOLLY_TEST_MODULE_SOURCES}
  )
  set_target_properties(
    ${FOLLY_TEST_MODULE_NAME}
    PROPERTIES
      PREFIX
      ""
      LIBRARY_OUTPUT_DIRECTORY
      "${resource_dir}"
  )
  apply_folly_compile_options_to_target(${FOLLY_TEST_MODULE_NAME})
  target_include_directories(
    ${FOLLY_TEST_MODULE_NAME}
    PRIVATE ${TOP_DIR} ${CMAKE_CURRENT_BINARY_DIR}
            ${FOLLY_INCLUDE_DIRECTORIES}
  )

  set(compile_deps fmt::fmt glog::glog ${FOLLY_TEST_MODULE_COMPILE_DEPS})
  list(REMOVE_DUPLICATES compile_deps)
  foreach (compile_dep IN LISTS compile_deps)
    if (NOT TARGET ${compile_dep})
      message(
        FATAL_ERROR
          "folly_add_test_module compile dependency does not exist: "
          "${compile_dep}"
      )
    endif ()
    target_include_directories(
      ${FOLLY_TEST_MODULE_NAME}
      PRIVATE $<TARGET_PROPERTY:${compile_dep},INTERFACE_INCLUDE_DIRECTORIES>
    )
    target_compile_definitions(
      ${FOLLY_TEST_MODULE_NAME}
      PRIVATE $<TARGET_PROPERTY:${compile_dep},INTERFACE_COMPILE_DEFINITIONS>
    )
    target_compile_options(
      ${FOLLY_TEST_MODULE_NAME}
      PRIVATE $<TARGET_PROPERTY:${compile_dep},INTERFACE_COMPILE_OPTIONS>
    )
  endforeach ()

  if (APPLE)
    target_link_options(
      ${FOLLY_TEST_MODULE_NAME}
      PRIVATE "LINKER:-undefined,dynamic_lookup"
    )
  endif ()

  set_target_properties(
    ${FOLLY_TEST_MODULE_HOST}
    PROPERTIES ENABLE_EXPORTS ON
  )
  add_dependencies(
    ${FOLLY_TEST_MODULE_HOST}
    ${FOLLY_TEST_MODULE_NAME}
  )
endfunction ()

# Initialize global property to track all granular component targets
define_property(
  GLOBAL
  PROPERTY FOLLY_COMPONENT_TARGETS
  BRIEF_DOCS "List of all folly component OBJECT targets"
  FULL_DOCS
    "Used to aggregate all component targets into the monolithic folly library")
set_property(GLOBAL PROPERTY FOLLY_COMPONENT_TARGETS "")

# Dependencies that exist only in this build tree, recorded by folly-deps.cmake
# when it fetches one whose own build installs nothing. install(EXPORT folly)
# refuses to export a target that links a target in no export set.
define_property(
  GLOBAL
  PROPERTY FOLLY_BUILD_LOCAL_TARGETS
  BRIEF_DOCS "Dependencies that must stay out of folly's install interface"
  FULL_DOCS "Target names, both alias and underlying, to wrap in
    $<BUILD_LOCAL_INTERFACE:> wherever folly links them")

# Dependencies folly fetched because the system did not have them. They are
# built here and never installed, so folly cannot be installed either.
define_property(GLOBAL PROPERTY FOLLY_FETCHED_DEPS
  BRIEF_DOCS "Dependencies fetched into this build tree"
  FULL_DOCS "Names passed to folly_fetch_from_manifest, reported once the
    dependency search is done"
)

# Wrap any dependency in ARGN that must not reach folly's install interface,
# leaving the rest untouched, and return the list in `out`.
function (folly_localize_deps out)
  get_property(_local GLOBAL PROPERTY FOLLY_BUILD_LOCAL_TARGETS)
  # $<BUILD_LOCAL_INTERFACE:> arrived in CMake 3.26; folly-deps.cmake warns
  # that installing is not possible when an older CMake needs it.
  if (NOT _local OR CMAKE_VERSION VERSION_LESS 3.26)
    set(${out}
        "${ARGN}"
        PARENT_SCOPE)
    return()
  endif ()
  set(_result "")
  foreach (_dep IN LISTS ARGN)
    if ("${_dep}" IN_LIST _local)
      list(APPEND _result "$<BUILD_LOCAL_INTERFACE:${_dep}>")
    else ()
      list(APPEND _result "${_dep}")
    endif ()
  endforeach ()
  set(${out}
      "${_result}"
      PARENT_SCOPE)
endfunction ()

# Track deferred dependencies to be linked after all targets are created
# Each entry is: "target|visibility|dep1,dep2,dep3"
define_property(
  GLOBAL
  PROPERTY FOLLY_DEFERRED_DEPS
  BRIEF_DOCS "List of deferred dependency specifications"
  FULL_DOCS "Format: target|PUBLIC|PRIVATE|dep1,dep2,...")
set_property(GLOBAL PROPERTY FOLLY_DEFERRED_DEPS "")

# Track INTERFACE targets that need to link to monolithic folly (for shared builds)
define_property(
  GLOBAL
  PROPERTY FOLLY_GRANULAR_INTERFACE_TARGETS
  BRIEF_DOCS "List of granular INTERFACE targets for shared builds"
  FULL_DOCS "These targets will be linked to the monolithic folly library")
set_property(GLOBAL PROPERTY FOLLY_GRANULAR_INTERFACE_TARGETS "")

# Headers of libraries this configuration does not build, such as the OpenSSL
# ones without OpenSSL. They are left out of the install.
define_property(
  GLOBAL
  PROPERTY FOLLY_EXCLUDED_HEADERS
  BRIEF_DOCS "Headers not to install"
  FULL_DOCS "Absolute paths, recorded by folly_exclude_headers()")
set_property(GLOBAL PROPERTY FOLLY_EXCLUDED_HEADERS "")

# Records headers in the current source directory that must not be installed,
# because the library they belong to is not built.
function (folly_exclude_headers)
  foreach (_header IN LISTS ARGN)
    set_property(GLOBAL APPEND PROPERTY FOLLY_EXCLUDED_HEADERS
                                        "${CMAKE_CURRENT_SOURCE_DIR}/${_header}")
  endforeach ()
endfunction ()

# Helper to add a folly library target
# Creates:
#   1. OBJECT library (${name}_obj) - for composition into monolithic target
#   2. STATIC library (${name}) - individual .a file for granular linking
# Usage:
#   folly_add_library(
#     NAME iobuf                    # Target suffix (full name: folly_io_iobuf)
#     TARGET_NAME follybenchmark    # Optional: override computed target name
#     SRCS Cursor.cpp IOBuf.cpp     # Source files (optional if using AUTO_SOURCES)
#     HEADERS Cursor.h IOBuf.h      # Header files
#     AUTO_SOURCES                  # Use auto_sources() instead of explicit SRCS
#     DEPS folly_lang_bits          # Private dependencies (internal folly targets)
#     EXPORTED_DEPS folly_range     # Public dependencies (propagated to users)
#     EXTERNAL_DEPS ${LIBSODIUM_LIBRARIES}  # External library dependencies
#     EXTERNAL_INCLUDE_DIRS ${LIBSODIUM_INCLUDE_DIRS}  # External include directories
#     COMPILE_OPTIONS -mpclmul      # Optional compile options for source files
#     EXCLUDE_FROM_MONOLITH         # Don't include in monolithic folly library
#   )
function (folly_add_library)
  cmake_parse_arguments(
    FOLLY_LIB
    "AUTO_SOURCES;EXCLUDE_FROM_MONOLITH" # Options
    "NAME;TARGET_NAME" # Single-value args
    "SRCS;HEADERS;DEPS;EXPORTED_DEPS;EXTERNAL_DEPS;EXTERNAL_INCLUDE_DIRS;COMPILE_OPTIONS" # Multi-value args
    ${ARGN})

  # Use explicit TARGET_NAME if provided, otherwise compute from directory
  if (FOLLY_LIB_TARGET_NAME)
    set(_target_name "${FOLLY_LIB_TARGET_NAME}")
  else ()
    # Compute target name from current directory relative to FOLLY_DIR
    # e.g., folly/io → folly_io, folly/io/async → folly_io_async
    file(RELATIVE_PATH _rel_path "${FOLLY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
    if (_rel_path STREQUAL "")
      set(_target_name "folly_${FOLLY_LIB_NAME}")
    else ()
      string(REPLACE "/" "_" _prefix "${_rel_path}")
      set(_target_name "folly_${_prefix}_${FOLLY_LIB_NAME}")
    endif ()
  endif ()

  # Handle source collection
  if (FOLLY_LIB_AUTO_SOURCES)
    auto_sources(_srcs "*.cpp" "" "${CMAKE_CURRENT_SOURCE_DIR}")
    remove_matches_from_lists(_srcs MATCHES "test/" "Test.cpp$"
                              "Benchmark.cpp$")
  else ()
    set(_srcs ${FOLLY_LIB_SRCS})
  endif ()

  # Skip if no sources (header-only library)
  list(LENGTH _srcs _src_count)
  if (_src_count EQUAL 0)
    # Header-only: create INTERFACE library
    add_library(${_target_name} INTERFACE)
    target_include_directories(
      ${_target_name}
      INTERFACE $<BUILD_INTERFACE:${TOP_DIR}>
                $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}>
                $<INSTALL_INTERFACE:${INCLUDE_INSTALL_DIR}>)
    folly_localize_deps(_interface_deps ${FOLLY_LIB_EXPORTED_DEPS}
                        ${FOLLY_LIB_EXTERNAL_DEPS})
    target_link_libraries(${_target_name} INTERFACE folly_deps
                                                    ${_interface_deps})
    # With no sources of its own, its users link what it uses.
    if (FOLLY_LIB_DEPS)
      list(JOIN FOLLY_LIB_DEPS "," _deps_str)
      set_property(GLOBAL APPEND PROPERTY FOLLY_DEFERRED_DEPS
                                          "${_target_name}|INTERFACE|${_deps_str}")
    endif ()
    # Track external deps for monolithic library (header-only deps are used transitively)
    if (FOLLY_LIB_EXTERNAL_DEPS)
      set_property(GLOBAL APPEND PROPERTY FOLLY_MONOLITHIC_EXTERNAL_DEPS
                                          ${FOLLY_LIB_EXTERNAL_DEPS})
    endif ()
    install(TARGETS ${_target_name} EXPORT folly)
    add_library(Folly::${_target_name} ALIAS ${_target_name})
    return()
  endif ()

  # 1. Create OBJECT library (for composition into monolithic target)
  set(_obj_target "${_target_name}_obj")
  add_library(${_obj_target} OBJECT ${_srcs} ${FOLLY_LIB_HEADERS})
  set_property(TARGET ${_obj_target} PROPERTY VERSION ${PACKAGE_VERSION})
  if (BUILD_SHARED_LIBS)
    set_property(TARGET ${_obj_target} PROPERTY POSITION_INDEPENDENT_CODE ON)
  endif ()

  target_include_directories(
    ${_obj_target}
    PUBLIC $<BUILD_INTERFACE:${TOP_DIR}> $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}>
           $<INSTALL_INTERFACE:${INCLUDE_INSTALL_DIR}>)
  apply_folly_compile_options_to_target(${_obj_target})

  # Apply optional compile options
  if (FOLLY_LIB_COMPILE_OPTIONS)
    target_compile_options(${_obj_target} PRIVATE ${FOLLY_LIB_COMPILE_OPTIONS})
  endif ()

  # Link dependencies on OBJECT library
  # External deps via folly_deps are always available
  # Internal folly deps are deferred until all targets exist
  target_link_libraries(${_obj_target} PUBLIC folly_deps)

  # Link external dependencies (e.g., libsodium, openssl) directly
  if (FOLLY_LIB_EXTERNAL_DEPS)
    target_link_libraries(${_obj_target} PUBLIC ${FOLLY_LIB_EXTERNAL_DEPS})
  endif ()

  # Add external include directories (e.g., libsodium include dirs)
  if (FOLLY_LIB_EXTERNAL_INCLUDE_DIRS)
    target_include_directories(${_obj_target}
                               PUBLIC ${FOLLY_LIB_EXTERNAL_INCLUDE_DIRS})
  endif ()

  # Defer internal folly dependencies until all targets are created
  # (Only for static builds - for shared builds, OBJECT targets are bundled into monolithic folly)
  if (NOT BUILD_SHARED_LIBS)
    if (FOLLY_LIB_EXPORTED_DEPS)
      # Join deps with comma, store as "target|PUBLIC|dep1,dep2,..."
      list(JOIN FOLLY_LIB_EXPORTED_DEPS "," _deps_str)
      set_property(GLOBAL APPEND PROPERTY FOLLY_DEFERRED_DEPS
                                          "${_obj_target}|PUBLIC|${_deps_str}")
    endif ()
    if (FOLLY_LIB_DEPS)
      list(JOIN FOLLY_LIB_DEPS "," _deps_str)
      set_property(GLOBAL APPEND PROPERTY FOLLY_DEFERRED_DEPS
                                          "${_obj_target}|PRIVATE|${_deps_str}")
    endif ()
  endif ()

  # Track OBJECT target for monolithic aggregation (unless excluded)
  if (NOT FOLLY_LIB_EXCLUDE_FROM_MONOLITH)
    set_property(GLOBAL APPEND PROPERTY FOLLY_COMPONENT_TARGETS ${_obj_target})
    # Track external deps for the monolithic library
    if (FOLLY_LIB_EXTERNAL_DEPS)
      set_property(GLOBAL APPEND PROPERTY FOLLY_MONOLITHIC_EXTERNAL_DEPS
                                          ${FOLLY_LIB_EXTERNAL_DEPS})
    endif ()
  endif ()

  # 2. Create the granular library target
  if (BUILD_SHARED_LIBS AND NOT FOLLY_LIB_EXCLUDE_FROM_MONOLITH)
    # For shared builds: create INTERFACE library that will link to monolithic folly
    # This avoids duplicating symbols between granular and monolithic libraries
    add_library(${_target_name} INTERFACE)

    target_include_directories(
      ${_target_name}
      INTERFACE $<BUILD_INTERFACE:${TOP_DIR}>
                $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}>
                $<INSTALL_INTERFACE:${INCLUDE_INSTALL_DIR}>)

    # Add external include directories for INTERFACE target
    if (FOLLY_LIB_EXTERNAL_INCLUDE_DIRS)
      target_include_directories(${_target_name}
                                 INTERFACE ${FOLLY_LIB_EXTERNAL_INCLUDE_DIRS})
    endif ()

    # Track this target to link to folly after monolithic library is created
    set_property(GLOBAL APPEND PROPERTY FOLLY_GRANULAR_INTERFACE_TARGETS
                                        ${_target_name})

    # Install the INTERFACE library
    install(TARGETS ${_target_name} EXPORT folly)
  elseif (BUILD_SHARED_LIBS AND FOLLY_LIB_EXCLUDE_FROM_MONOLITH)
    # For excluded targets in shared builds: create SHARED library with actual code
    # These are NOT in the monolithic folly, so they need their own implementation
    add_library(${_target_name} SHARED $<TARGET_OBJECTS:${_obj_target}>)
    set_property(TARGET ${_target_name} PROPERTY VERSION ${PACKAGE_VERSION})

    target_include_directories(
      ${_target_name}
      PUBLIC $<BUILD_INTERFACE:${TOP_DIR}>
             $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}>
             $<INSTALL_INTERFACE:${INCLUDE_INSTALL_DIR}>)

    # Link to folly_deps (external dependencies)
    target_link_libraries(${_target_name} PUBLIC folly_deps)

    # Link external dependencies
    if (FOLLY_LIB_EXTERNAL_DEPS)
      folly_localize_deps(_external_deps ${FOLLY_LIB_EXTERNAL_DEPS})
      target_link_libraries(${_target_name} PUBLIC ${_external_deps})
    endif ()

    # Add external include directories
    if (FOLLY_LIB_EXTERNAL_INCLUDE_DIRS)
      target_include_directories(${_target_name}
                                 PUBLIC ${FOLLY_LIB_EXTERNAL_INCLUDE_DIRS})
    endif ()

    # Defer linking to folly (created later by folly_create_monolithic_library)
    # Also defer internal folly dependencies
    set(_all_deps "folly")
    if (FOLLY_LIB_EXPORTED_DEPS)
      list(APPEND _all_deps ${FOLLY_LIB_EXPORTED_DEPS})
    endif ()
    list(JOIN _all_deps "," _deps_str)
    set_property(GLOBAL APPEND PROPERTY FOLLY_DEFERRED_DEPS
                                        "${_target_name}|PUBLIC|${_deps_str}")
    if (FOLLY_LIB_DEPS)
      list(JOIN FOLLY_LIB_DEPS "," _deps_str)
      set_property(
        GLOBAL APPEND PROPERTY FOLLY_DEFERRED_DEPS
                               "${_target_name}|PRIVATE|${_deps_str}")
    endif ()

    # Install the SHARED library
    install(
      TARGETS ${_target_name}
      EXPORT folly
      LIBRARY DESTINATION ${LIB_INSTALL_DIR}
      ARCHIVE DESTINATION ${LIB_INSTALL_DIR})
  else ()
    # For static builds: create STATIC library (individual .a file for granular linking)
    add_library(${_target_name} STATIC $<TARGET_OBJECTS:${_obj_target}>)
    set_property(TARGET ${_target_name} PROPERTY VERSION ${PACKAGE_VERSION})

    target_include_directories(
      ${_target_name}
      PUBLIC $<BUILD_INTERFACE:${TOP_DIR}>
             $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}>
             $<INSTALL_INTERFACE:${INCLUDE_INSTALL_DIR}>)

    # Link external dependencies on STATIC library
    # Internal folly deps are deferred (see above)
    target_link_libraries(${_target_name} PUBLIC folly_deps)

    # Link external dependencies (e.g., libsodium, openssl) directly
    if (FOLLY_LIB_EXTERNAL_DEPS)
      folly_localize_deps(_external_deps ${FOLLY_LIB_EXTERNAL_DEPS})
      target_link_libraries(${_target_name} PUBLIC ${_external_deps})
    endif ()

    # Add external include directories for STATIC target
    if (FOLLY_LIB_EXTERNAL_INCLUDE_DIRS)
      target_include_directories(${_target_name}
                                 PUBLIC ${FOLLY_LIB_EXTERNAL_INCLUDE_DIRS})
    endif ()

    # Defer internal folly dependencies for STATIC library too
    if (FOLLY_LIB_EXPORTED_DEPS)
      list(JOIN FOLLY_LIB_EXPORTED_DEPS "," _deps_str)
      set_property(GLOBAL APPEND PROPERTY FOLLY_DEFERRED_DEPS
                                          "${_target_name}|PUBLIC|${_deps_str}")
    endif ()
    if (FOLLY_LIB_DEPS)
      list(JOIN FOLLY_LIB_DEPS "," _deps_str)
      set_property(
        GLOBAL APPEND PROPERTY FOLLY_DEFERRED_DEPS
                               "${_target_name}|PRIVATE|${_deps_str}")
    endif ()

    # Install the STATIC library
    install(
      TARGETS ${_target_name}
      EXPORT folly
      LIBRARY DESTINATION ${LIB_INSTALL_DIR}
      ARCHIVE DESTINATION ${LIB_INSTALL_DIR})
  endif ()

  # Create alias for the library
  add_library(Folly::${_target_name} ALIAS ${_target_name})
endfunction ()

# Resolve all deferred dependencies after all targets have been created
# Call this after all add_subdirectory() calls
function (folly_resolve_deferred_dependencies)
  # Allow linking targets defined in other directories
  cmake_policy(SET CMP0079 NEW)

  get_property(_deferred_deps GLOBAL PROPERTY FOLLY_DEFERRED_DEPS)
  set(_remaining "")

  foreach (_spec IN LISTS _deferred_deps)
    # Parse the spec: "target|visibility|dep1,dep2,..."
    string(REPLACE "|" ";" _parts "${_spec}")
    list(LENGTH _parts _len)
    if (_len LESS 3)
      continue()
    endif ()

    list(GET _parts 0 _target)
    list(GET _parts 1 _visibility)
    list(GET _parts 2 _deps_str)

    # Split deps by comma
    string(REPLACE "," ";" _deps "${_deps_str}")

    # Filter to only existing targets (skip deps that weren't generated)
    set(_valid_deps "")
    set(_missing_deps "")
    foreach (_dep IN LISTS _deps)
      if (TARGET ${_dep})
        list(APPEND _valid_deps ${_dep})
      else ()
        list(APPEND _missing_deps ${_dep})
      endif ()
    endforeach ()

    if (_valid_deps)
      folly_localize_deps(_valid_deps ${_valid_deps})
      target_link_libraries(${_target} ${_visibility} ${_valid_deps})
    endif ()
    # folly/CMakeLists.txt and the top-level CMakeLists.txt both call this, the
    # first time before the monolithic folly exists, so keep only what is still
    # missing to link each dep once.
    if (_missing_deps)
      list(JOIN _missing_deps "," _missing_str)
      list(APPEND _remaining "${_target}|${_visibility}|${_missing_str}")
    endif ()
  endforeach ()
  set_property(GLOBAL PROPERTY FOLLY_DEFERRED_DEPS "${_remaining}")
endfunction ()

# Create the monolithic folly library from all component OBJECT libraries
# Call this after all add_subdirectory() calls and folly_resolve_deferred_dependencies()
function (folly_create_monolithic_library)
  get_property(_component_targets GLOBAL PROPERTY FOLLY_COMPONENT_TARGETS)

  # Collect all object files from component targets
  set(_all_objects)
  foreach (_target IN LISTS _component_targets)
    list(APPEND _all_objects $<TARGET_OBJECTS:${_target}>)
  endforeach ()

  # Create the monolithic library
  add_library(folly ${_all_objects})
  if (BUILD_SHARED_LIBS)
    set_property(TARGET folly PROPERTY POSITION_INDEPENDENT_CODE ON)
  endif ()
  set_property(TARGET folly PROPERTY VERSION ${PACKAGE_VERSION})

  apply_folly_compile_options_to_target(folly)
  target_compile_features(folly INTERFACE cxx_generic_lambdas)

  target_include_directories(
    folly
    PUBLIC $<BUILD_INTERFACE:${TOP_DIR}> $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}>
           $<INSTALL_INTERFACE:${INCLUDE_INSTALL_DIR}>)

  target_link_libraries(folly PUBLIC folly_deps)

  # Link all external dependencies that were tracked from component targets
  get_property(_external_deps GLOBAL PROPERTY FOLLY_MONOLITHIC_EXTERNAL_DEPS)
  if (_external_deps)
    list(REMOVE_DUPLICATES _external_deps)
    folly_localize_deps(_external_deps ${_external_deps})
    target_link_libraries(folly PUBLIC ${_external_deps})
  endif ()

  # Create alias for consistency
  add_library(Folly::folly ALIAS folly)

  # For shared builds: link all granular INTERFACE targets to the monolithic library
  if (BUILD_SHARED_LIBS)
    # CMP0079: target_link_libraries allows use with targets in other directories
    cmake_policy(SET CMP0079 NEW)
    get_property(_interface_targets GLOBAL
                 PROPERTY FOLLY_GRANULAR_INTERFACE_TARGETS)
    foreach (_target IN LISTS _interface_targets)
      target_link_libraries(${_target} INTERFACE folly)
    endforeach ()
  endif ()
endfunction ()
