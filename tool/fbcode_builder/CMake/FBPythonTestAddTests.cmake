# Copyright (c) Facebook, Inc. and its affiliates.

# Add a command to be emitted to the CTest file
set(ctest_script)
function(add_command CMD)
  set(escaped_args "")
  foreach(arg ${ARGN})
    # Escape all arguments using "Bracket Argument" syntax
    # We could skip this for argument that don't contain any special
    # characters if we wanted to make the output slightly more human-friendly.
    set(escaped_args "${escaped_args} [==[${arg}]==]")
  endforeach()
  set(ctest_script "${ctest_script}${CMD}(${escaped_args})\n" PARENT_SCOPE)
endfunction()

if(NOT EXISTS "${TEST_EXECUTABLE}")
  message(FATAL_ERROR "Test executable does not exist: ${TEST_EXECUTABLE}")
endif()
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env ${TEST_ENV} "${TEST_INTERPRETER}" "${TEST_EXECUTABLE}" --list-tests
  WORKING_DIRECTORY "${TEST_WORKING_DIR}"
  OUTPUT_VARIABLE output
  RESULT_VARIABLE result
)
if(NOT "${result}" EQUAL 0)
  string(REPLACE "\n" "\n  " output "${output}")
  message(
    FATAL_ERROR
    "Error running test executable: ${TEST_EXECUTABLE}\n"
    "Output:\n"
    "  ${output}\n"
  )
endif()

# Parse output
string(REPLACE "\n" ";" tests_list "${output}")
foreach(test_name ${tests_list})
  add_command(
    add_test
    "${TEST_PREFIX}${test_name}"
    ${CMAKE_COMMAND} -E env ${TEST_ENV}
    "${TEST_INTERPRETER}" "${TEST_EXECUTABLE}" "${test_name}"
  )
  add_command(
    set_tests_properties
    "${TEST_PREFIX}${test_name}"
    PROPERTIES
    WORKING_DIRECTORY "${TEST_WORKING_DIR}"
    ${TEST_PROPERTIES}
  )
endforeach()

# Set a list of discovered tests in the parent scope, in case users
# want access to this list as a CMake variable
if(TEST_LIST)
  add_command(set ${TEST_LIST} ${tests_list})
endif()

file(WRITE "${CTEST_FILE}" "${ctest_script}")
