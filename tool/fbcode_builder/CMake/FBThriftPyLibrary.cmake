# Copyright (c) Facebook, Inc. and its affiliates.

include(FBCMakeParseArgs)
include(FBPythonBinary)

# Generate a Python library from a thrift file
function(add_fbthrift_py_library LIB_NAME THRIFT_FILE)
  # Parse the arguments
  set(one_value_args NAMESPACE THRIFT_INCLUDE_DIR)
  set(multi_value_args SERVICES DEPENDS OPTIONS)
  fb_cmake_parse_args(
    ARG "" "${one_value_args}" "${multi_value_args}" "${ARGN}"
  )

  if(NOT DEFINED ARG_THRIFT_INCLUDE_DIR)
    set(ARG_THRIFT_INCLUDE_DIR "include/thrift-files")
  endif()

  get_filename_component(base ${THRIFT_FILE} NAME_WE)
  set(output_dir "${CMAKE_CURRENT_BINARY_DIR}/${THRIFT_FILE}-py")

  # Parse the namespace value
  if (NOT DEFINED ARG_NAMESPACE)
    set(ARG_NAMESPACE "${base}")
  endif()

  string(REPLACE "." "/" namespace_dir "${ARG_NAMESPACE}")
  set(py_output_dir "${output_dir}/gen-py/${namespace_dir}")
  list(APPEND generated_sources
    "${py_output_dir}/__init__.py"
    "${py_output_dir}/ttypes.py"
    "${py_output_dir}/constants.py"
  )
  foreach(service IN LISTS ARG_SERVICES)
    list(APPEND generated_sources
      ${py_output_dir}/${service}.py
    )
  endforeach()

  # Define a dummy interface library to help propagate the thrift include
  # directories between dependencies.
  add_library("${LIB_NAME}.thrift_includes" INTERFACE)
  target_include_directories(
    "${LIB_NAME}.thrift_includes"
    INTERFACE
      "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>"
      "$<INSTALL_INTERFACE:${ARG_THRIFT_INCLUDE_DIR}>"
  )
  foreach(dep IN LISTS ARG_DEPENDS)
    target_link_libraries(
      "${LIB_NAME}.thrift_includes"
      INTERFACE "${dep}.thrift_includes"
    )
  endforeach()

  # This generator expression gets the list of include directories required
  # for all of our dependencies.
  # It requires using COMMAND_EXPAND_LISTS in the add_custom_command() call
  # below.  COMMAND_EXPAND_LISTS is only available in CMake 3.8+
  # If we really had to support older versions of CMake we would probably need
  # to use a wrapper script around the thrift compiler that could take the
  # include list as a single argument and split it up before invoking the
  # thrift compiler.
  if (NOT POLICY CMP0067)
    message(FATAL_ERROR "add_fbthrift_py_library() requires CMake 3.8+")
  endif()
  set(
    thrift_include_options
    "-I;$<JOIN:$<TARGET_PROPERTY:${LIB_NAME}.thrift_includes,INTERFACE_INCLUDE_DIRECTORIES>,;-I;>"
  )

  # Always force generation of "new-style" python classes for Python 2
  list(APPEND ARG_OPTIONS "new_style")
  # CMake 3.12 is finally getting a list(JOIN) function, but until then
  # treating the list as a string and replacing the semicolons is good enough.
  string(REPLACE ";" "," GEN_ARG_STR "${ARG_OPTIONS}")

  # Emit the rule to run the thrift compiler
  add_custom_command(
    OUTPUT
      ${generated_sources}
    COMMAND_EXPAND_LISTS
    COMMAND
      "${CMAKE_COMMAND}" -E make_directory "${output_dir}"
    COMMAND
      "${FBTHRIFT_COMPILER}"
      --legacy-strict
      --gen "py:${GEN_ARG_STR}"
      "${thrift_include_options}"
      -o "${output_dir}"
      "${CMAKE_CURRENT_SOURCE_DIR}/${THRIFT_FILE}"
    WORKING_DIRECTORY
      "${CMAKE_BINARY_DIR}"
    MAIN_DEPENDENCY
      "${THRIFT_FILE}"
    DEPENDS
      "${FBTHRIFT_COMPILER}"
  )

  # We always want to pass the namespace as "" to this call:
  # thrift will already emit the files with the desired namespace prefix under
  # gen-py.  We don't want add_fb_python_library() to prepend the namespace a
  # second time.
  add_fb_python_library(
    "${LIB_NAME}"
    BASE_DIR "${output_dir}/gen-py"
    NAMESPACE ""
    SOURCES ${generated_sources}
    DEPENDS ${ARG_DEPENDS} FBThrift::thrift_py
  )
endfunction()
