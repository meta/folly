# Copyright (c) Facebook, Inc. and its affiliates.

include(FBCMakeParseArgs)
include(FBThriftPyLibrary)
include(FBThriftCppLibrary)

#
# add_fbthrift_library()
#
# This is a convenience function that generates thrift libraries for multiple
# languages.
#
# For example:
#   add_fbthrift_library(
#     foo foo.thrift
#     LANGUAGES cpp py
#     SERVICES Foo
#     DEPENDS bar)
#
# will be expanded into two separate calls:
#
# add_fbthrift_cpp_library(foo_cpp foo.thrift SERVICES Foo DEPENDS bar_cpp)
# add_fbthrift_py_library(foo_py foo.thrift SERVICES Foo DEPENDS bar_py)
#
function(add_fbthrift_library LIB_NAME THRIFT_FILE)
  # Parse the arguments
  set(one_value_args PY_NAMESPACE INCLUDE_DIR THRIFT_INCLUDE_DIR)
  set(multi_value_args SERVICES DEPENDS LANGUAGES CPP_OPTIONS PY_OPTIONS)
  fb_cmake_parse_args(
    ARG "" "${one_value_args}" "${multi_value_args}" "${ARGN}"
  )

  if(NOT DEFINED ARG_INCLUDE_DIR)
    set(ARG_INCLUDE_DIR "include")
  endif()
  if(NOT DEFINED ARG_THRIFT_INCLUDE_DIR)
    set(ARG_THRIFT_INCLUDE_DIR "${ARG_INCLUDE_DIR}/thrift-files")
  endif()

  # CMake 3.12+ adds list(TRANSFORM) which would be nice to use here, but for
  # now we still want to support older versions of CMake.
  set(CPP_DEPENDS)
  set(PY_DEPENDS)
  foreach(dep IN LISTS ARG_DEPENDS)
    list(APPEND CPP_DEPENDS "${dep}_cpp")
    list(APPEND PY_DEPENDS "${dep}_py")
  endforeach()

  foreach(lang IN LISTS ARG_LANGUAGES)
    if ("${lang}" STREQUAL "cpp")
      add_fbthrift_cpp_library(
        "${LIB_NAME}_cpp" "${THRIFT_FILE}"
        SERVICES ${ARG_SERVICES}
        DEPENDS ${CPP_DEPENDS}
        OPTIONS ${ARG_CPP_OPTIONS}
        INCLUDE_DIR "${ARG_INCLUDE_DIR}"
        THRIFT_INCLUDE_DIR "${ARG_THRIFT_INCLUDE_DIR}"
      )
    elseif ("${lang}" STREQUAL "py" OR "${lang}" STREQUAL "python")
      if (DEFINED ARG_PY_NAMESPACE)
        set(namespace_args NAMESPACE "${ARG_PY_NAMESPACE}")
      endif()
      add_fbthrift_py_library(
        "${LIB_NAME}_py" "${THRIFT_FILE}"
        SERVICES ${ARG_SERVICES}
        ${namespace_args}
        DEPENDS ${PY_DEPENDS}
        OPTIONS ${ARG_PY_OPTIONS}
        THRIFT_INCLUDE_DIR "${ARG_THRIFT_INCLUDE_DIR}"
      )
    else()
      message(
        FATAL_ERROR "unknown language for thrift library ${LIB_NAME}: ${lang}"
      )
    endif()
  endforeach()
endfunction()
