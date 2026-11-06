# Copyright (c) Facebook, Inc. and its affiliates.

include(FBCMakeParseArgs)

# Generate a C++ library from a thrift file
#
# Parameters:
# - SERVICES <svc1> [<svc2> ...]
#   The names of the services defined in the thrift file.
# - DEPENDS <dep1> [<dep2> ...]
#   A list of other thrift C++ libraries that this library depends on.
# - OPTIONS <opt1> [<opt2> ...]
#   A list of options to pass to the thrift compiler.
# - INCLUDE_DIR <path>
#   The sub-directory where generated headers will be installed.
#   Defaults to "include" if not specified.  The caller must still call
#   install() to install the thrift library if desired.
# - THRIFT_INCLUDE_DIR <path>
#   The sub-directory where generated headers will be installed.
#   Defaults to "${INCLUDE_DIR}/thrift-files" if not specified.
#   The caller must still call install() to install the thrift library if
#   desired.
function(add_fbthrift_cpp_library LIB_NAME THRIFT_FILE)
  # Parse the arguments
  set(one_value_args INCLUDE_DIR THRIFT_INCLUDE_DIR)
  set(multi_value_args SERVICES DEPENDS OPTIONS)
  fb_cmake_parse_args(
    ARG "" "${one_value_args}" "${multi_value_args}" "${ARGN}"
  )
  if(NOT DEFINED ARG_INCLUDE_DIR)
    set(ARG_INCLUDE_DIR "include")
  endif()
  if(NOT DEFINED ARG_THRIFT_INCLUDE_DIR)
    set(ARG_THRIFT_INCLUDE_DIR "${ARG_INCLUDE_DIR}/thrift-files")
  endif()

  get_filename_component(base ${THRIFT_FILE} NAME_WE)
  get_filename_component(
    output_dir
    ${CMAKE_CURRENT_BINARY_DIR}/${THRIFT_FILE}
    DIRECTORY
  )

  # Generate relative paths in #includes
  file(
    RELATIVE_PATH include_prefix
    "${PROJECT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/${THRIFT_FILE}"
  )
  get_filename_component(include_prefix ${include_prefix} DIRECTORY)

  if (NOT "${include_prefix}" STREQUAL "")
    list(APPEND ARG_OPTIONS "include_prefix=${include_prefix}")
  endif()
  # CMake 3.12 is finally getting a list(JOIN) function, but until then
  # treating the list as a string and replacing the semicolons is good enough.
  string(REPLACE ";" "," GEN_ARG_STR "${ARG_OPTIONS}")

  # Compute the list of generated files
  list(APPEND generated_headers
    "${output_dir}/gen-cpp2/${base}_constants.h"
    "${output_dir}/gen-cpp2/${base}_types.h"
    "${output_dir}/gen-cpp2/${base}_types.tcc"
    "${output_dir}/gen-cpp2/${base}_types_custom_protocol.h"
    "${output_dir}/gen-cpp2/${base}_metadata.h"
  )
  list(APPEND generated_sources
    "${output_dir}/gen-cpp2/${base}_constants.cpp"
    "${output_dir}/gen-cpp2/${base}_data.h"
    "${output_dir}/gen-cpp2/${base}_data.cpp"
    "${output_dir}/gen-cpp2/${base}_types.cpp"
    "${output_dir}/gen-cpp2/${base}_types_binary.cpp"
    "${output_dir}/gen-cpp2/${base}_types_compact.cpp"
    "${output_dir}/gen-cpp2/${base}_types_serialization.cpp"
    "${output_dir}/gen-cpp2/${base}_metadata.cpp"
  )
  foreach(service IN LISTS ARG_SERVICES)
    list(APPEND generated_headers
      "${output_dir}/gen-cpp2/${service}.h"
      "${output_dir}/gen-cpp2/${service}.tcc"
      "${output_dir}/gen-cpp2/${service}AsyncClient.h"
      "${output_dir}/gen-cpp2/${service}_custom_protocol.h"
    )
    list(APPEND generated_sources
      "${output_dir}/gen-cpp2/${service}.cpp"
      "${output_dir}/gen-cpp2/${service}AsyncClient.cpp"
      "${output_dir}/gen-cpp2/${service}_processmap_binary.cpp"
      "${output_dir}/gen-cpp2/${service}_processmap_compact.cpp"
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
    message(FATAL_ERROR "add_fbthrift_cpp_library() requires CMake 3.8+")
  endif()
  set(
    thrift_include_options
    "-I;$<JOIN:$<TARGET_PROPERTY:${LIB_NAME}.thrift_includes,INTERFACE_INCLUDE_DIRECTORIES>,;-I;>"
  )

  # Emit the rule to run the thrift compiler
  add_custom_command(
    OUTPUT
      ${generated_headers}
      ${generated_sources}
    COMMAND_EXPAND_LISTS
    COMMAND
      "${CMAKE_COMMAND}" -E make_directory "${output_dir}"
    COMMAND
      "${FBTHRIFT_COMPILER}"
      --legacy-strict
      --gen "mstch_cpp2:${GEN_ARG_STR}"
      "${thrift_include_options}"
      -I "${FBTHRIFT_INCLUDE_DIR}"
      -o "${output_dir}"
      "${CMAKE_CURRENT_SOURCE_DIR}/${THRIFT_FILE}"
    WORKING_DIRECTORY
      "${PROJECT_BINARY_DIR}"
    MAIN_DEPENDENCY
      "${THRIFT_FILE}"
    DEPENDS
      ${ARG_DEPENDS}
      "${FBTHRIFT_COMPILER}"
  )

  # Now emit the library rule to compile the sources
  if (BUILD_SHARED_LIBS)
    set(LIB_TYPE SHARED)
  else ()
    set(LIB_TYPE STATIC)
  endif ()

  add_library(
    "${LIB_NAME}" ${LIB_TYPE}
    ${generated_sources}
  )

  target_include_directories(
    "${LIB_NAME}"
    PUBLIC
      "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>"
      "$<INSTALL_INTERFACE:${ARG_INCLUDE_DIR}>"
      ${Xxhash_INCLUDE_DIR}
  )
  target_link_libraries(
    "${LIB_NAME}"
    PUBLIC
      ${ARG_DEPENDS}
      FBThrift::thriftcpp2
      Folly::folly
      ${Xxhash_LIBRARY}
  )

  # Add ${generated_headers} to the PUBLIC_HEADER property for ${LIB_NAME}
  #
  # This allows callers to install it using
  # "install(TARGETS ${LIB_NAME} PUBLIC_HEADER)"
  # However, note that CMake's PUBLIC_HEADER behavior is rather inflexible,
  # and does have any way to preserve header directory structure.  Callers
  # must be careful to use the correct PUBLIC_HEADER DESTINATION parameter
  # when doing this, to put the files the correct directory themselves.
  # We define a HEADER_INSTALL_DIR property with the include directory prefix,
  # so typically callers should specify the PUBLIC_HEADER DESTINATION as
  # "$<TARGET_PROPERTY:${LIB_NAME},HEADER_INSTALL_DIR>"
  set_property(
    TARGET "${LIB_NAME}"
    PROPERTY PUBLIC_HEADER ${generated_headers}
  )

  # Define a dummy interface library to help propagate the thrift include
  # directories between dependencies.
  add_library("${LIB_NAME}.thrift_includes" INTERFACE)
  target_include_directories(
    "${LIB_NAME}.thrift_includes"
    INTERFACE
      "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>"
      "$<INSTALL_INTERFACE:${ARG_THRIFT_INCLUDE_DIR}>"
  )
  foreach(dep IN LISTS ARG_DEPENDS)
    target_link_libraries(
      "${LIB_NAME}.thrift_includes"
      INTERFACE "${dep}.thrift_includes"
    )
  endforeach()

  set_target_properties(
    "${LIB_NAME}"
    PROPERTIES
      EXPORT_PROPERTIES "THRIFT_INSTALL_DIR"
      THRIFT_INSTALL_DIR "${ARG_THRIFT_INCLUDE_DIR}/${include_prefix}"
      HEADER_INSTALL_DIR "${ARG_INCLUDE_DIR}/${include_prefix}/gen-cpp2"
  )
endfunction()
