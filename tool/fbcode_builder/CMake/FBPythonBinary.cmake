# Copyright (c) Facebook, Inc. and its affiliates.

include(FBCMakeParseArgs)

#
# This file contains helper functions for building self-executing Python
# binaries.
#
# This is somewhat different than typical python installation with
# distutils/pip/virtualenv/etc.  We primarily want to build a standalone
# executable, isolated from other Python packages on the system.  We don't want
# to install files into the standard library python paths.  This is more
# similar to PEX (https://github.com/pantsbuild/pex) and XAR
# (https://github.com/facebookincubator/xar).  (In the future it would be nice
# to update this code to also support directly generating XAR files if XAR is
# available.)
#
# We also want to be able to easily define "libraries" of python files that can
# be shared and re-used between these standalone python executables, and can be
# shared across projects in different repositories.  This means that we do need
# a way to "install" libraries so that they are visible to CMake builds in
# other repositories, without actually installing them in the standard python
# library paths.
#

# If the caller has not already found Python, do so now.
# If we fail to find python now we won't fail immediately, but
# add_fb_python_executable() or add_fb_python_library() will fatal out if they
# are used.
if(NOT TARGET Python3::Interpreter)
  # CMake 3.12+ ships with a FindPython3.cmake module.  Try using it first.
  # We find with QUIET here, since otherwise this generates some noisy warnings
  # on versions of CMake before 3.12
  if (WIN32)
    # On Windows we need both the Interpreter as well as the Development
    # libraries.
    find_package(Python3 COMPONENTS Interpreter Development QUIET)
  else()
    find_package(Python3 COMPONENTS Interpreter QUIET)
  endif()
  if(Python3_Interpreter_FOUND)
    message(STATUS "Found Python 3: ${Python3_EXECUTABLE}")
  else()
    # Try with the FindPythonInterp.cmake module available in older CMake
    # versions.  Check to see if the caller has already searched for this
    # themselves first.
    if(NOT PYTHONINTERP_FOUND)
      set(Python_ADDITIONAL_VERSIONS 3 3.6 3.5 3.4 3.3 3.2 3.1)
      find_package(PythonInterp)
      # TODO: On Windows we require the Python libraries as well.
      # We currently do not search for them on this code path.
      # For now we require building with CMake 3.12+ on Windows, so that the
      # FindPython3 code path above is available.
    endif()
    if(PYTHONINTERP_FOUND)
      if("${PYTHON_VERSION_MAJOR}" GREATER_EQUAL 3)
        set(Python3_EXECUTABLE "${PYTHON_EXECUTABLE}")
        add_custom_target(Python3::Interpreter)
      else()
        string(
          CONCAT FBPY_FIND_PYTHON_ERR
          "found Python ${PYTHON_VERSION_MAJOR}.${PYTHON_VERSION_MINOR}, "
          "but need Python 3"
        )
      endif()
    endif()
  endif()
endif()

# Find our helper program.
# We typically install this in the same directory as this .cmake file.
find_program(
  FB_MAKE_PYTHON_ARCHIVE "make_fbpy_archive.py"
  PATHS ${CMAKE_MODULE_PATH}
)
set(FB_PY_TEST_MAIN "${CMAKE_CURRENT_LIST_DIR}/fb_py_test_main.py")
set(
  FB_PY_TEST_DISCOVER_SCRIPT
  "${CMAKE_CURRENT_LIST_DIR}/FBPythonTestAddTests.cmake"
)
set(
  FB_PY_WIN_MAIN_C
  "${CMAKE_CURRENT_LIST_DIR}/fb_py_win_main.c"
)

# An option to control the default installation location for
# install_fb_python_library().  This is relative to ${CMAKE_INSTALL_PREFIX}
set(
  FBPY_LIB_INSTALL_DIR "lib/fb-py-libs" CACHE STRING
  "The subdirectory where FB python libraries should be installed"
)

#
# Build a self-executing python binary.
#
# This accepts the same arguments as add_fb_python_library().
#
# In addition, a MAIN_MODULE argument is accepted.  This argument specifies
# which module should be started as the __main__ module when the executable is
# run.  If left unspecified, a __main__.py script must be present in the
# manifest.
#
function(add_fb_python_executable TARGET)
  fb_py_check_available()

  # Parse the arguments
  set(one_value_args BASE_DIR NAMESPACE MAIN_MODULE TYPE)
  set(multi_value_args SOURCES DEPENDS)
  fb_cmake_parse_args(
    ARG "" "${one_value_args}" "${multi_value_args}" "${ARGN}"
  )
  fb_py_process_default_args(ARG_NAMESPACE ARG_BASE_DIR)

  # Use add_fb_python_library() to perform most of our source handling
  add_fb_python_library(
    "${TARGET}.main_lib"
    BASE_DIR "${ARG_BASE_DIR}"
    NAMESPACE "${ARG_NAMESPACE}"
    SOURCES ${ARG_SOURCES}
    DEPENDS ${ARG_DEPENDS}
  )

  set(
    manifest_files
    "$<TARGET_PROPERTY:${TARGET}.main_lib.py_lib,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  set(
    source_files
    "$<TARGET_PROPERTY:${TARGET}.main_lib.py_lib,INTERFACE_SOURCES>"
  )

  # The command to build the executable archive.
  #
  # If we are using CMake 3.8+ we can use COMMAND_EXPAND_LISTS.
  # CMP0067 isn't really the policy we care about, but seems like the best way
  # to check if we are running 3.8+.
  if (POLICY CMP0067)
    set(extra_cmd_params COMMAND_EXPAND_LISTS)
    set(make_py_args "${manifest_files}")
  else()
    set(extra_cmd_params)
    set(make_py_args --manifest-separator "::" "$<JOIN:${manifest_files},::>")
  endif()

  set(output_file "${TARGET}${CMAKE_EXECUTABLE_SUFFIX}")
  if(WIN32)
    set(zipapp_output "${TARGET}.py_zipapp")
  else()
    set(zipapp_output "${output_file}")
  endif()
  set(zipapp_output_file "${zipapp_output}")

  set(is_dir_output FALSE)
  if(DEFINED ARG_TYPE)
    list(APPEND make_py_args "--type" "${ARG_TYPE}")
    if ("${ARG_TYPE}" STREQUAL "dir")
      set(is_dir_output TRUE)
      # CMake doesn't really seem to like having a directory specified as an
      # output; specify the __main__.py file as the output instead.
      set(zipapp_output_file "${zipapp_output}/__main__.py")
      # Update output_file to match zipapp_output_file for dir type
      set(output_file "${zipapp_output_file}")
    endif()
  endif()

  if(DEFINED ARG_MAIN_MODULE)
    list(APPEND make_py_args "--main" "${ARG_MAIN_MODULE}")
  endif()

  # Rebuild when the type changes. Otherwise an output that an earlier build
  # made with another type looks up to date: a directory where a zipapp is
  # expected, for example.
  set(type_file "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.py_type")
  file(GENERATE OUTPUT "${type_file}" CONTENT "${ARG_TYPE}")

  add_custom_command(
    OUTPUT "${zipapp_output_file}"
    ${extra_cmd_params}
    COMMAND
      "${Python3_EXECUTABLE}" "${FB_MAKE_PYTHON_ARCHIVE}"
      -o "${zipapp_output}"
      ${make_py_args}
    DEPENDS
      ${source_files}
      "${TARGET}.main_lib.py_sources_built"
      "${FB_MAKE_PYTHON_ARCHIVE}"
      "${type_file}"
  )

  if(WIN32)
    if(is_dir_output)
      # TODO: generate a main executable that will invoke Python3
      # with the correct main module inside the output directory
    else()
      add_executable("${TARGET}.winmain" "${FB_PY_WIN_MAIN_C}")
      target_link_libraries("${TARGET}.winmain" Python3::Python)
      # The Python3::Python target doesn't seem to be set up completely
      # correctly on Windows for some reason, and we have to explicitly add
      # ${Python3_LIBRARY_DIRS} to the target link directories.
      target_link_directories(
        "${TARGET}.winmain"
        PUBLIC ${Python3_LIBRARY_DIRS}
      )
      add_custom_command(
        OUTPUT "${output_file}"
        DEPENDS "${TARGET}.winmain" "${zipapp_output_file}"
        COMMAND
          "cmd.exe" "/c" "copy" "/b"
          "${TARGET}.winmain${CMAKE_EXECUTABLE_SUFFIX}+${zipapp_output}"
          "${output_file}"
      )
    endif()
  endif()

  # Add an "ALL" target that depends on force ${TARGET},
  # so that ${TARGET} will be included in the default list of build targets.
  add_custom_target("${TARGET}.GEN_PY_EXE" ALL DEPENDS "${output_file}")

  # Allow resolving the executable path for the target that we generate
  # via a generator expression like:
  # "WATCHMAN_WAIT_PATH=$<TARGET_PROPERTY:watchman-wait.GEN_PY_EXE,EXECUTABLE>"
  set_property(TARGET "${TARGET}.GEN_PY_EXE"
      PROPERTY EXECUTABLE "${CMAKE_CURRENT_BINARY_DIR}/${output_file}")

  # Let install_fb_python_executable() install the whole directory.
  if(is_dir_output)
    set_property(TARGET "${TARGET}.GEN_PY_EXE"
        PROPERTY OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/${zipapp_output}")
  endif()
endfunction()

# Define a python unittest executable.
# The executable is built using add_fb_python_executable and has the
# following differences:
#
# Each of the source files specified in SOURCES will be imported
# and have unittest discovery performed upon them.
# Those sources will be imported in the top level namespace.
#
# The ENV argument allows specifying a list of "KEY=VALUE"
# pairs that will be used by the test runner to set up the environment
# in the child process prior to running the test.  This is useful for
# passing additional configuration to the test.
function(add_fb_python_unittest TARGET)
  # Parse the arguments
  set(multi_value_args SOURCES DEPENDS ENV PROPERTIES)
  set(
    one_value_args
    WORKING_DIRECTORY BASE_DIR NAMESPACE TEST_LIST DISCOVERY_TIMEOUT TYPE
  )
  fb_cmake_parse_args(
    ARG "" "${one_value_args}" "${multi_value_args}" "${ARGN}"
  )
  fb_py_process_default_args(ARG_NAMESPACE ARG_BASE_DIR)
  if(NOT ARG_WORKING_DIRECTORY)
    # Default the working directory to the current binary directory.
    # This matches the default behavior of add_test() and other standard
    # test functions like gtest_discover_tests()
    set(ARG_WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  endif()
  if(NOT ARG_TEST_LIST)
    set(ARG_TEST_LIST "${TARGET}_TESTS")
  endif()
  if(NOT ARG_DISCOVERY_TIMEOUT)
    set(ARG_DISCOVERY_TIMEOUT 5)
  endif()

  # Tell our test program the list of modules to scan for tests.
  # We scan all modules directly listed in our SOURCES argument, and skip
  # modules that came from dependencies in the DEPENDS list.
  #
  # This is written into a __test_modules__.py module that the test runner
  # will look at.
  set(
    test_modules_path
    "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_test_modules.py"
  )
  file(WRITE "${test_modules_path}" "TEST_MODULES = [\n")
  string(REPLACE "." "/" namespace_dir "${ARG_NAMESPACE}")
  if (NOT "${namespace_dir}" STREQUAL "")
    set(namespace_dir "${namespace_dir}/")
  endif()
  set(test_modules)
  foreach(src_path IN LISTS ARG_SOURCES)
    fb_py_compute_dest_path(
      abs_source dest_path
      "${src_path}" "${namespace_dir}" "${ARG_BASE_DIR}"
    )
    string(REPLACE "/" "." module_name "${dest_path}")
    string(REGEX REPLACE "\\.py$" "" module_name "${module_name}")
    list(APPEND test_modules "${module_name}")
    file(APPEND "${test_modules_path}" "  '${module_name}',\n")
  endforeach()
  file(APPEND "${test_modules_path}" "]\n")

  # The __main__ is provided by our runner wrapper/bootstrap
  list(APPEND ARG_SOURCES "${FB_PY_TEST_MAIN}=__main__.py")
  list(APPEND ARG_SOURCES "${test_modules_path}=__test_modules__.py")

  if(NOT DEFINED ARG_TYPE)
    set(ARG_TYPE "zipapp")
  endif()

  add_fb_python_executable(
    "${TARGET}"
    TYPE "${ARG_TYPE}"
    NAMESPACE "${ARG_NAMESPACE}"
    BASE_DIR "${ARG_BASE_DIR}"
    SOURCES ${ARG_SOURCES}
    DEPENDS ${ARG_DEPENDS}
  )

  # Run test discovery after the test executable is built.
  # This logic is based on the code for gtest_discover_tests()
  set(ctest_file_base "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}")
  set(ctest_include_file "${ctest_file_base}_include.cmake")
  set(ctest_tests_file "${ctest_file_base}_tests.cmake")
  add_custom_command(
    TARGET "${TARGET}.GEN_PY_EXE" POST_BUILD
    BYPRODUCTS "${ctest_tests_file}"
    COMMAND
      "${CMAKE_COMMAND}"
      -D "TEST_TARGET=${TARGET}"
      -D "TEST_INTERPRETER=${Python3_EXECUTABLE}"
      -D "TEST_ENV=${ARG_ENV}"
      -D "TEST_EXECUTABLE=$<TARGET_PROPERTY:${TARGET}.GEN_PY_EXE,EXECUTABLE>"
      -D "TEST_WORKING_DIR=${ARG_WORKING_DIRECTORY}"
      -D "TEST_LIST=${ARG_TEST_LIST}"
      -D "TEST_PREFIX=${TARGET}::"
      -D "TEST_PROPERTIES=${ARG_PROPERTIES}"
      -D "CTEST_FILE=${ctest_tests_file}"
      -P "${FB_PY_TEST_DISCOVER_SCRIPT}"
    VERBATIM
  )

  file(
    WRITE "${ctest_include_file}"
    "if(EXISTS \"${ctest_tests_file}\")\n"
    "  include(\"${ctest_tests_file}\")\n"
    "else()\n"
    "  add_test(\"${TARGET}_NOT_BUILT\" \"${TARGET}_NOT_BUILT\")\n"
    "endif()\n"
  )
  set_property(
    DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
    "${ctest_include_file}"
  )
endfunction()

#
# Define a python library.
#
# If you want to install a python library generated from this rule note that
# you need to use install_fb_python_library() rather than CMake's built-in
# install() function.  This will make it available for other downstream
# projects to use in their add_fb_python_executable() and
# add_fb_python_library() calls.  (You do still need to use `install(EXPORT)`
# later to install the CMake exports.)
#
# Parameters:
# - BASE_DIR <dir>:
#   The base directory path to strip off from each source path.  All source
#   files must be inside this directory.  If not specified it defaults to
#   ${CMAKE_CURRENT_SOURCE_DIR}.
# - NAMESPACE <namespace>:
#   The destination namespace where these files should be installed in python
#   binaries.  If not specified, this defaults to the current relative path of
#   ${CMAKE_CURRENT_SOURCE_DIR} inside ${CMAKE_SOURCE_DIR}.  e.g., a python
#   library defined in the directory repo_root/foo/bar will use a default
#   namespace of "foo.bar"
# - SOURCES <src1> <...>:
#   The python source files.
#   You may optionally specify as source using the form: PATH=ALIAS where
#   PATH is a relative path in the source tree and ALIAS is the relative
#   path into which PATH should be rewritten.  This is useful for mapping
#   an executable script to the main module in a python executable.
#   e.g.: `python/bin/watchman-wait=__main__.py`
# - DEPENDS <target1> <...>:
#   Other python libraries that this one depends on.
# - INSTALL_DIR <dir>:
#   The directory where this library should be installed.
#   install_fb_python_library() must still be called later to perform the
#   installation.  If a relative path is given it will be treated relative to
#   ${CMAKE_INSTALL_PREFIX}
#
# CMake is unfortunately pretty crappy at being able to define custom build
# rules & behaviors.  It doesn't support transitive property propagation
# between custom targets; only the built-in add_executable() and add_library()
# targets support transitive properties.
#
# We hack around this janky CMake behavior by (ab)using interface libraries to
# propagate some of the data we want between targets, without actually
# generating a C library.
#
# add_fb_python_library(SOMELIB) generates the following things:
# - An INTERFACE library rule named SOMELIB.py_lib which tracks some
#   information about transitive dependencies:
#   - the transitive set of source files in the INTERFACE_SOURCES property
#   - the transitive set of manifest files that this library depends on in
#     the INTERFACE_INCLUDE_DIRECTORIES property.
# - A custom command that generates a SOMELIB.manifest file.
#   This file contains the mapping of source files to desired destination
#   locations in executables that depend on this library.  This manifest file
#   will then be read at build-time in order to build executables.
#
function(add_fb_python_library LIB_NAME)
  fb_py_check_available()

  # Parse the arguments
  # We use fb_cmake_parse_args() rather than cmake_parse_arguments() since
  # cmake_parse_arguments() does not handle empty arguments, and it is common
  # for callers to want to specify an empty NAMESPACE parameter.
  set(one_value_args BASE_DIR NAMESPACE INSTALL_DIR)
  set(multi_value_args SOURCES DEPENDS)
  fb_cmake_parse_args(
    ARG "" "${one_value_args}" "${multi_value_args}" "${ARGN}"
  )
  fb_py_process_default_args(ARG_NAMESPACE ARG_BASE_DIR)

  string(REPLACE "." "/" namespace_dir "${ARG_NAMESPACE}")
  if (NOT "${namespace_dir}" STREQUAL "")
    set(namespace_dir "${namespace_dir}/")
  endif()

  if(NOT DEFINED ARG_INSTALL_DIR)
    set(install_dir "${FBPY_LIB_INSTALL_DIR}/")
  elseif("${ARG_INSTALL_DIR}" STREQUAL "")
    set(install_dir "")
  else()
    set(install_dir "${ARG_INSTALL_DIR}/")
  endif()

  # message(STATUS "fb py library ${LIB_NAME}: "
  #         "NS=${namespace_dir} BASE=${ARG_BASE_DIR}")

  # TODO: In the future it would be nice to support pre-compiling the source
  # files.  We could emit a rule to compile each source file and emit a
  # .pyc/.pyo file here, and then have the manifest reference the pyc/pyo
  # files.

  # Define a library target to help pass around information about the library,
  # and propagate dependency information.
  #
  # CMake make a lot of assumptions that libraries are C++ libraries.  To help
  # avoid confusion we name our target "${LIB_NAME}.py_lib" rather than just
  # "${LIB_NAME}".  This helps avoid confusion if callers try to use
  # "${LIB_NAME}" on their own as a target name.  (e.g., attempting to install
  # it directly with install(TARGETS) won't work.  Callers must use
  # install_fb_python_library() instead.)
  add_library("${LIB_NAME}.py_lib" INTERFACE)

  # Emit the manifest file.
  #
  # We write the manifest file to a temporary path first, then copy it with
  # configure_file(COPYONLY).  This is necessary to get CMake to understand
  # that "${manifest_path}" is generated by the CMake configure phase,
  # and allow using it as a dependency for add_custom_command().
  # (https://gitlab.kitware.com/cmake/cmake/issues/16367)
  set(manifest_path "${CMAKE_CURRENT_BINARY_DIR}/${LIB_NAME}.manifest") 
  set(tmp_manifest "${manifest_path}.tmp")
  file(WRITE "${tmp_manifest}" "FBPY_MANIFEST 1\n")
  set(abs_sources)
  foreach(src_path IN LISTS ARG_SOURCES)
    fb_py_compute_dest_path(
      abs_source dest_path
      "${src_path}" "${namespace_dir}" "${ARG_BASE_DIR}"
    )
    list(APPEND abs_sources "${abs_source}")
    target_sources(
      "${LIB_NAME}.py_lib" INTERFACE
      "$<BUILD_INTERFACE:${abs_source}>"
      "$<INSTALL_INTERFACE:${install_dir}${LIB_NAME}/${dest_path}>"
    )
    file(
      APPEND "${tmp_manifest}"
      "${abs_source} :: ${dest_path}\n"
    )
  endforeach()
  configure_file("${tmp_manifest}" "${manifest_path}" COPYONLY)

  target_include_directories(
    "${LIB_NAME}.py_lib" INTERFACE
    "$<BUILD_INTERFACE:${manifest_path}>"
    "$<INSTALL_INTERFACE:${install_dir}${LIB_NAME}.manifest>"
  )

  # Add a target that depends on all of the source files.
  # This is needed in case some of the source files are generated.  This will
  # ensure that these source files are brought up-to-date before we build
  # any python binaries that depend on this library.
  add_custom_target("${LIB_NAME}.py_sources_built" DEPENDS ${abs_sources})
  add_dependencies("${LIB_NAME}.py_lib" "${LIB_NAME}.py_sources_built")

  # Hook up library dependencies, and also make the *.py_sources_built target
  # depend on the sources for all of our dependencies also being up-to-date.
  foreach(dep IN LISTS ARG_DEPENDS)
    target_link_libraries("${LIB_NAME}.py_lib" INTERFACE "${dep}.py_lib")

    # Mark that our .py_sources_built target depends on each our our dependent
    # libraries.  This serves two functions:
    # - This causes CMake to generate an error message if one of the
    #   dependencies is never defined.  The target_link_libraries() call above
    #   won't complain if one of the dependencies doesn't exist (since it is
    #   intended to allow passing in file names for plain library files rather
    #   than just targets).
    # - It ensures that sources for our dependencies are built before any
    #   executable that depends on us.  Note that we depend on "${dep}.py_lib"
    #   rather than "${dep}.py_sources_built" for this purpose because the
    #   ".py_sources_built" target won't be available for imported targets.
    add_dependencies("${LIB_NAME}.py_sources_built" "${dep}.py_lib")
  endforeach()

  # Add a custom command to help with library installation, in case
  # install_fb_python_library() is called later for this library.
  # add_custom_command() only works with file dependencies defined in the same
  # CMakeLists.txt file, so we want to make sure this is defined here, rather
  # then where install_fb_python_library() is called.
  # This command won't be run by default, but will only be run if it is needed
  # by a subsequent install_fb_python_library() call.
  #
  # This command copies the library contents into the build directory.
  # It would be nicer if we could skip this intermediate copy, and just run
  # make_fbpy_archive.py at install time to copy them directly to the desired
  # installation directory.  Unfortunately this is difficult to do, and seems
  # to interfere with some of the CMake code that wants to generate a manifest
  # of installed files.
  set(build_install_dir "${CMAKE_CURRENT_BINARY_DIR}/${LIB_NAME}.lib_install")
  add_custom_command(
    OUTPUT
      "${build_install_dir}/${LIB_NAME}.manifest"
    COMMAND "${CMAKE_COMMAND}" -E remove_directory "${build_install_dir}"
    COMMAND
      "${Python3_EXECUTABLE}" "${FB_MAKE_PYTHON_ARCHIVE}" --type lib-install
      --install-dir "${LIB_NAME}"
      -o "${build_install_dir}/${LIB_NAME}" "${manifest_path}"
    DEPENDS
      "${abs_sources}"
      "${manifest_path}"
      "${FB_MAKE_PYTHON_ARCHIVE}"
  )
  add_custom_target(
    "${LIB_NAME}.py_lib_install"
    DEPENDS "${build_install_dir}/${LIB_NAME}.manifest"
  )

  # Set some properties to pass through the install paths to
  # install_fb_python_library()
  #
  # Passing through ${build_install_dir} allows install_fb_python_library()
  # to work even if used from a different CMakeLists.txt file than where
  # add_fb_python_library() was called (i.e. such that
  # ${CMAKE_CURRENT_BINARY_DIR} is different between the two calls).
  set(abs_install_dir "${install_dir}")
  if(NOT IS_ABSOLUTE "${abs_install_dir}")
    set(abs_install_dir "${CMAKE_INSTALL_PREFIX}/${abs_install_dir}")
  endif()
  string(REGEX REPLACE "/$" "" abs_install_dir "${abs_install_dir}")
  set_target_properties(
    "${LIB_NAME}.py_lib_install"
    PROPERTIES
    INSTALL_DIR "${abs_install_dir}"
    BUILD_INSTALL_DIR "${build_install_dir}"
  )
endfunction()

#
# Install an FB-style packaged python binary.
#
# - DESTINATION <dir>:
#   The directory to install the executable into, relative to
#   ${CMAKE_INSTALL_PREFIX}.  Defaults to bin.
# - LIB_DESTINATION <dir>:
#   For a TYPE dir executable, install its directory as <dir>/<TARGET>,
#   relative to ${CMAKE_INSTALL_PREFIX}, and make the executable in
#   DESTINATION a symlink to its __main__.py (except on Windows).  Defaults
#   to lib.
#
function(install_fb_python_executable TARGET)
  # Parse the arguments
  set(one_value_args DESTINATION LIB_DESTINATION)
  set(multi_value_args)
  fb_cmake_parse_args(
    ARG "" "${one_value_args}" "${multi_value_args}" "${ARGN}"
  )

  if(NOT DEFINED ARG_DESTINATION)
    set(ARG_DESTINATION bin)
  endif()
  if(NOT DEFINED ARG_LIB_DESTINATION)
    set(ARG_LIB_DESTINATION lib)
  endif()

  get_target_property(output_dir "${TARGET}.GEN_PY_EXE" OUTPUT_DIR)
  if(output_dir)
    set(lib_dir "${ARG_LIB_DESTINATION}/${TARGET}")
    install(
      DIRECTORY "${output_dir}/"
      DESTINATION "${lib_dir}"
      USE_SOURCE_PERMISSIONS
    )
    if(NOT WIN32)
      file(
        RELATIVE_PATH link_target
        "/${ARG_DESTINATION}" "/${lib_dir}/__main__.py"
      )
      set(bin_dir "\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${ARG_DESTINATION}")
      install(CODE "
        file(MAKE_DIRECTORY \"${bin_dir}\")
        file(CREATE_LINK \"${link_target}\" \"${bin_dir}/${TARGET}\" SYMBOLIC)
      ")
    endif()
  else()
    install(
      PROGRAMS "$<TARGET_PROPERTY:${TARGET}.GEN_PY_EXE,EXECUTABLE>"
      DESTINATION "${ARG_DESTINATION}"
    )
  endif()
endfunction()

#
# Install a python library.
#
# - EXPORT <export-name>:
#   Associate the installed target files with the given export-name.
#
# Note that unlike the built-in CMake install() function we do not accept a
# DESTINATION parameter.  Instead, use the INSTALL_DIR parameter to
# add_fb_python_library() to set the installation location.
#
function(install_fb_python_library LIB_NAME)
  set(one_value_args EXPORT)
  fb_cmake_parse_args(ARG "" "${one_value_args}" "" "${ARGN}")

  # Export our "${LIB_NAME}.py_lib" target so that it will be available to
  # downstream projects in our installed CMake config files.
  if(DEFINED ARG_EXPORT)
    install(TARGETS "${LIB_NAME}.py_lib" EXPORT "${ARG_EXPORT}")
  endif()

  # add_fb_python_library() emits a .py_lib_install target that will prepare
  # the installation directory.  However, it isn't part of the "ALL" target and
  # therefore isn't built by default.
  #
  # Make sure the ALL target depends on it now.  We have to do this by
  # introducing yet another custom target.
  # Add it as a dependency to the ALL target now.
  add_custom_target("${LIB_NAME}.py_lib_install_all" ALL)
  add_dependencies(
    "${LIB_NAME}.py_lib_install_all" "${LIB_NAME}.py_lib_install"
  )

  # Copy the intermediate install directory generated at build time into
  # the desired install location.
  get_target_property(dest_dir "${LIB_NAME}.py_lib_install" "INSTALL_DIR")
  get_target_property(
    build_install_dir "${LIB_NAME}.py_lib_install" "BUILD_INSTALL_DIR"
  )
  install(
    DIRECTORY "${build_install_dir}/${LIB_NAME}"
    DESTINATION "${dest_dir}"
  )
  install(
    FILES "${build_install_dir}/${LIB_NAME}.manifest"
    DESTINATION "${dest_dir}"
  )
endfunction()

# Helper macro to process the BASE_DIR and NAMESPACE arguments for
# add_fb_python_executable() and add_fb_python_executable()
macro(fb_py_process_default_args NAMESPACE_VAR BASE_DIR_VAR)
  # If the namespace was not specified, default to the relative path to the
  # current directory (starting from the repository root).
  if(NOT DEFINED "${NAMESPACE_VAR}")
    file(
      RELATIVE_PATH "${NAMESPACE_VAR}"
      "${CMAKE_SOURCE_DIR}"
      "${CMAKE_CURRENT_SOURCE_DIR}"
    )
  endif()

  if(NOT DEFINED "${BASE_DIR_VAR}")
    # If the base directory was not specified, default to the current directory
    set("${BASE_DIR_VAR}" "${CMAKE_CURRENT_SOURCE_DIR}")
  else()
    # If the base directory was specified, always convert it to an
    # absolute path.
    get_filename_component("${BASE_DIR_VAR}" "${${BASE_DIR_VAR}}" ABSOLUTE)
  endif()
endmacro()

function(fb_py_check_available)
  # Make sure that Python 3 and our make_fbpy_archive.py helper script are
  # available.
  if(NOT Python3_EXECUTABLE)
    if(FBPY_FIND_PYTHON_ERR)
      message(FATAL_ERROR "Unable to find Python 3: ${FBPY_FIND_PYTHON_ERR}")
    else()
      message(FATAL_ERROR "Unable to find Python 3")
    endif()
  endif()

  if (NOT FB_MAKE_PYTHON_ARCHIVE)
    message(
      FATAL_ERROR "unable to find make_fbpy_archive.py helper program (it "
      "should be located in the same directory as FBPythonBinary.cmake)"
    )
  endif()
endfunction()

function(
    fb_py_compute_dest_path
    src_path_output dest_path_output src_path namespace_dir base_dir
)
  if("${src_path}" MATCHES "=")
    # We want to split the string on the `=` sign, but cmake doesn't
    # provide much in the way of helpers for this, so we rewrite the
    # `=` sign to `;` so that we can treat it as a cmake list and
    # then index into the components
    string(REPLACE "=" ";" src_path_list "${src_path}")
    list(GET src_path_list 0 src_path)
    # Note that we ignore the `namespace_dir` in the alias case
    # in order to allow aliasing a source to the top level `__main__.py`
    # filename.
    list(GET src_path_list 1 dest_path)
  else()
    unset(dest_path)
  endif()

  get_filename_component(abs_source "${src_path}" ABSOLUTE)
  if(NOT DEFINED dest_path)
    file(RELATIVE_PATH rel_src "${ARG_BASE_DIR}" "${abs_source}")
    if("${rel_src}" MATCHES "^../")
      message(
        FATAL_ERROR "${LIB_NAME}: source file \"${abs_source}\" is not inside "
        "the base directory ${ARG_BASE_DIR}"
      )
    endif()
    set(dest_path "${namespace_dir}${rel_src}")
  endif()

  set("${src_path_output}" "${abs_source}" PARENT_SCOPE)
  set("${dest_path_output}" "${dest_path}" PARENT_SCOPE)
endfunction()
