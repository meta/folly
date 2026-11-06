#
# Copyright (c) Facebook, Inc. and its affiliates.
#
# Helper function for parsing arguments to a CMake function.
#
# This function is very similar to CMake's built-in cmake_parse_arguments()
# function, with some improvements:
# - This function correctly handles empty arguments.  (cmake_parse_arguments()
#   ignores empty arguments.)
# - If a multi-value argument is specified more than once, the subsequent
#   arguments are appended to the original list rather than replacing it.  e.g.
#   if "SOURCES" is a multi-value argument, and the argument list contains
#   "SOURCES a b c SOURCES x y z" then the resulting value for SOURCES will be
#   "a;b;c;x;y;z" rather than "x;y;z"
# - This function errors out by default on unrecognized arguments.  You can
#   pass in an extra "ALLOW_UNPARSED_ARGS" argument to make it behave like
#   cmake_parse_arguments(), and return the unparsed arguments in a
#   <prefix>_UNPARSED_ARGUMENTS variable instead.
#
# It does look like cmake_parse_arguments() handled empty arguments correctly
# from CMake 3.0 through 3.3, but it seems like this was probably broken when
# it was turned into a built-in function in CMake 3.4.  Here is discussion and
# patches that fixed this behavior prior to CMake 3.0:
# https://cmake.org/pipermail/cmake-developers/2013-November/020607.html
#
# The one downside to this function over the built-in cmake_parse_arguments()
# is that I don't think we can achieve the PARSE_ARGV behavior in a non-builtin
# function, so we can't properly handle arguments that contain ";".  CMake will
# treat the ";" characters as list element separators, and treat it as multiple
# separate arguments.
#
function(fb_cmake_parse_args PREFIX OPTIONS ONE_VALUE_ARGS MULTI_VALUE_ARGS ARGS)
  foreach(option IN LISTS ARGN)
    if ("${option}" STREQUAL "ALLOW_UNPARSED_ARGS")
      set(ALLOW_UNPARSED_ARGS TRUE)
    else()
      message(
        FATAL_ERROR
        "unknown optional argument for fb_cmake_parse_args(): ${option}"
      )
    endif()
  endforeach()

  # Define all options as FALSE in the parent scope to start with
  foreach(var_name IN LISTS OPTIONS)
    set("${PREFIX}_${var_name}" "FALSE" PARENT_SCOPE)
  endforeach()

  # TODO: We aren't extremely strict about error checking for one-value
  # arguments here.  e.g., we don't complain if a one-value argument is
  # followed by another option/one-value/multi-value name rather than an
  # argument.  We also don't complain if a one-value argument is the last
  # argument and isn't followed by a value.

  list(APPEND all_args ${ONE_VALUE_ARGS})
  list(APPEND all_args ${MULTI_VALUE_ARGS})
  set(current_variable)
  set(unparsed_args)
  foreach(arg IN LISTS ARGS)
    list(FIND OPTIONS "${arg}" opt_index)
    if("${opt_index}" EQUAL -1)
      list(FIND all_args "${arg}" arg_index)
      if("${arg_index}" EQUAL -1)
        # This argument does not match an argument name,
        # must be an argument value
        if("${current_variable}" STREQUAL "")
          list(APPEND unparsed_args "${arg}")
        else()
          # Ugh, CMake lists have a pretty fundamental flaw: they cannot
          # distinguish between an empty list and a list with a single empty
          # element.  We track our own SEEN_VALUES_arg setting to help
          # distinguish this and behave properly here.
          if ("${SEEN_${current_variable}}" AND "${${current_variable}}" STREQUAL "")
            set("${current_variable}" ";${arg}")
          else()
            list(APPEND "${current_variable}" "${arg}")
          endif()
          set("SEEN_${current_variable}" TRUE)
        endif()
      else()
        # We found a single- or multi-value argument name
        set(current_variable "VALUES_${arg}")
        set("SEEN_${arg}" TRUE)
      endif()
    else()
      # We found an option variable
      set("${PREFIX}_${arg}" "TRUE" PARENT_SCOPE)
      set(current_variable)
    endif()
  endforeach()

  foreach(arg_name IN LISTS ONE_VALUE_ARGS)
    if(NOT "${SEEN_${arg_name}}")
      unset("${PREFIX}_${arg_name}" PARENT_SCOPE)
    elseif(NOT "${SEEN_VALUES_${arg_name}}")
      # If the argument was seen but a value wasn't specified, error out.
      # We require exactly one value to be specified.
      message(
        FATAL_ERROR "argument ${arg_name} was specified without a value"
      )
    else()
      list(LENGTH "VALUES_${arg_name}" num_args)
      if("${num_args}" EQUAL 0)
        # We know an argument was specified and that we called list(APPEND).
        # If CMake thinks the list is empty that means there is really a single
        # empty element in the list.
        set("${PREFIX}_${arg_name}" "" PARENT_SCOPE)
      elseif("${num_args}" EQUAL 1)
        list(GET "VALUES_${arg_name}" 0 arg_value)
        set("${PREFIX}_${arg_name}" "${arg_value}" PARENT_SCOPE)
      else()
        message(
          FATAL_ERROR "too many arguments specified for ${arg_name}: "
          "${VALUES_${arg_name}}"
        )
      endif()
    endif()
  endforeach()

  foreach(arg_name IN LISTS MULTI_VALUE_ARGS)
    # If this argument name was never seen, then unset the parent scope
    if (NOT "${SEEN_${arg_name}}")
      unset("${PREFIX}_${arg_name}" PARENT_SCOPE)
    else()
      # TODO: Our caller still won't be able to distinguish between an empty
      # list and a list with a single empty element.  We can tell which is
      # which, but CMake lists don't make it easy to show this to our caller.
      set("${PREFIX}_${arg_name}" "${VALUES_${arg_name}}" PARENT_SCOPE)
    endif()
  endforeach()

  # By default we fatal out on unparsed arguments, but return them to the
  # caller if ALLOW_UNPARSED_ARGS was specified.
  if (DEFINED unparsed_args)
    if ("${ALLOW_UNPARSED_ARGS}")
      set("${PREFIX}_UNPARSED_ARGUMENTS" "${unparsed_args}" PARENT_SCOPE)
    else()
      message(FATAL_ERROR "unrecognized arguments: ${unparsed_args}")
    endif()
  endif()
endfunction()
