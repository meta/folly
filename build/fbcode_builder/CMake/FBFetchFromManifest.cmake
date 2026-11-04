# Copyright (c) Meta Platforms, Inc. and affiliates.

include(CMakePushCheckState)

# getdeps' manifests sit next to this module, in fbsource and in the
# build/fbcode_builder of each project that ships it.
get_filename_component(
  _FB_FETCH_FBCODE_BUILDER_DIR "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY
)

# For CI that builds against its dependencies' code on main rather than their
# pins. ShipIt exports the projects within minutes of each other, but
# build/deps/github_hashes is refreshed about once a day, so a change that
# spans projects can break a pinned build until then. ShipIt also dates each
# exported commit with the time its fbsource commit landed, the same in every
# project, so a project's commit time picks out the dependencies it was
# exported with.
set(
  FB_FETCH_AS_OF ""
  CACHE STRING
  "Fetch git dependencies' branches as they were at this time, such as git log -1 --format=%cI or now, ignoring build/deps/github_hashes"
)

# Sets <out> to <key> in the first [<section>] or [<section>.<condition>] of
# getdeps' <manifest> that has it, or to an empty string. The condition isn't
# evaluated: what getdeps takes from the system on this platform, such as
# OpenSSL on Linux, still has to be built here when the system doesn't have it.
function(fb_manifest_value manifest section key out)
  file(READ "${_FB_FETCH_FBCODE_BUILDER_DIR}/manifests/${manifest}" text)
  string(REPLACE ";" "\\;" text "${text}")
  string(REPLACE "\n" ";" lines "${text}")
  set(in_section FALSE)
  set(value "")
  foreach(line IN LISTS lines)
    if(line MATCHES "^\\[([^]]*)\\]")
      set(in_section FALSE)
      if(CMAKE_MATCH_1 STREQUAL section OR CMAKE_MATCH_1 MATCHES "^${section}\\.")
        set(in_section TRUE)
      endif()
    elseif(in_section AND line MATCHES "^${key}[ \t]*=(.*)")
      string(STRIP "${CMAKE_MATCH_1}" value)
      break()
    endif()
  endforeach()
  set(${out} "${value}" PARENT_SCOPE)
endfunction()

# Sets <out> to the last commit on <branch> of <repo_url> at or before
# FB_FETCH_AS_OF, which a clone of the branch's commits alone can tell. A later
# configure with the same time reuses the answer.
function(_fb_fetch_commit_as_of out name repo_url branch)
  string(TOLOWER "${name}" lower)
  set(key "${repo_url} ${branch} ${FB_FETCH_AS_OF}")
  if(NOT _FB_FETCH_AS_OF_${lower}_KEY STREQUAL key)
    find_package(Git REQUIRED)
    set(history "${CMAKE_BINARY_DIR}/CMakeFiles/fb_fetch_${lower}_history")
    file(REMOVE_RECURSE "${history}")
    execute_process(
      COMMAND
        "${GIT_EXECUTABLE}" clone --quiet --bare --filter=tree:0
        --single-branch --branch "${branch}" "${repo_url}" "${history}"
      RESULT_VARIABLE result
    )
    set(commit "")
    if(result EQUAL 0)
      execute_process(
        COMMAND
          "${GIT_EXECUTABLE}" -C "${history}" rev-list -1
          "--before=${FB_FETCH_AS_OF}" "${branch}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE commit
        OUTPUT_STRIP_TRAILING_WHITESPACE
      )
    endif()
    file(REMOVE_RECURSE "${history}")
    if(NOT result EQUAL 0)
      message(
        FATAL_ERROR "git couldn't read ${name}'s ${branch} from ${repo_url}."
      )
    endif()
    if(NOT commit)
      message(
        FATAL_ERROR
        "Can't find the commit on ${name}'s ${branch} at ${FB_FETCH_AS_OF} in "
        "${repo_url}."
      )
    endif()
    set(_FB_FETCH_AS_OF_${lower} "${commit}" CACHE INTERNAL "")
    set(_FB_FETCH_AS_OF_${lower}_KEY "${key}" CACHE INTERNAL "")
  endif()
  set(${out} "${_FB_FETCH_AS_OF_${lower}}" PARENT_SCOPE)
endfunction()

# Adds to the build, as <name>, the dependency that getdeps' <manifest> pins,
# so that it can't drift from the one getdeps builds: the [download] archive,
# or the [git] repo_url at the revision getdeps picks, built from the [build]
# subdir. Like getdeps, that's the manifest's rev, else the one that
# build/deps/github_hashes or build/project_hashes records, else the
# manifest's branch, else main, with a warning. FB_FETCH_AS_OF skips the
# recorded revision and the warning, and takes the branch's commit at that
# time. Further arguments go to FetchContent_Declare().
function(fb_fetch_from_manifest name manifest)
  fb_manifest_value(${manifest} git repo_url repo_url)
  fb_manifest_value(${manifest} build subdir subdir)
  set(branch "")
  if(repo_url)
    fb_manifest_value(${manifest} git rev rev)
    if(NOT rev AND NOT FB_FETCH_AS_OF)
      string(REGEX REPLACE "^[^:]+://[^/]*/" "" rev_file "${repo_url}")
      string(REGEX REPLACE "\\.git$" "-rev.txt" rev_file "${rev_file}")
      foreach(hashes deps/github_hashes project_hashes)
        set(hashes "${_FB_FETCH_FBCODE_BUILDER_DIR}/../${hashes}")
        if(IS_DIRECTORY "${hashes}")
          if(EXISTS "${hashes}/${rev_file}")
            file(READ "${hashes}/${rev_file}" rev_text)
            if(NOT rev_text MATCHES "Subproject commit ([0-9a-fA-F]+)")
              message(FATAL_ERROR "no ${name} revision in ${hashes}/${rev_file}")
            endif()
            set(rev "${CMAKE_MATCH_1}")
          endif()
          break()
        endif()
      endforeach()
    endif()
    if(NOT rev)
      fb_manifest_value(${manifest} git branch branch)
      if(NOT branch)
        if(NOT FB_FETCH_AS_OF)
          message(
            WARNING
            "getdeps pins no ${name} revision: its ${manifest} manifest has no "
            "rev or branch, and neither build/deps/github_hashes nor "
            "build/project_hashes has ${rev_file}. Fetching ${name}'s main "
            "instead."
          )
        endif()
        set(branch main)
      endif()
      set(rev "${branch}")
      if(FB_FETCH_AS_OF)
        _fb_fetch_commit_as_of(rev ${name} "${repo_url}" "${branch}")
      endif()
    endif()
    message(STATUS "${name} not found, fetching ${repo_url} ${rev}")
    set(source GIT_REPOSITORY "${repo_url}" GIT_TAG "${rev}")
  else()
    fb_manifest_value(${manifest} download url url)
    fb_manifest_value(${manifest} download sha256 sha256)
    if(NOT url)
      message(
        FATAL_ERROR
        "Can't fetch ${name}: getdeps' ${manifest} manifest has neither a "
        "[git] repo_url nor a [download] url."
      )
    endif()
    message(STATUS "${name} not found, fetching ${url}")
    set(source URL "${url}" URL_HASH "SHA256=${sha256}")
    # FetchContent strips the archive's top-level directory, where subdir
    # starts.
    if(subdir MATCHES "^[^/]+/(.+)")
      set(subdir "${CMAKE_MATCH_1}")
    else()
      set(subdir "")
    endif()
  endif()
  if(subdir AND NOT subdir STREQUAL ".")
    list(APPEND source SOURCE_SUBDIR "${subdir}")
  endif()
  # Older CMake passes these on to ExternalProject, whose error doesn't say why.
  set(min_version "")
  if("SOURCE_SUBDIR" IN_LIST source)
    set(min_version 3.18)
  endif()
  if("OVERRIDE_FIND_PACKAGE" IN_LIST ARGN)
    set(min_version 3.24)
  endif()
  if(min_version AND CMAKE_VERSION VERSION_LESS "${min_version}")
    message(
      FATAL_ERROR
      "Can't fetch ${name}: fetching it needs CMake ${min_version} or newer, "
      "and this is CMake ${CMAKE_VERSION}. Install ${name}, or use a newer "
      "CMake."
    )
  endif()
  # The dependency is for building this project, not part of what it
  # installs: its install(EXPORT) rules would fail as soon as it links another
  # fetched dependency, which no export set has, and its install rules needn't
  # work in a subproject at all.
  set(exclude "")
  if(CMAKE_VERSION VERSION_LESS 3.28)
    # A build that already skips its install rules has no install to break.
    if(NOT CMAKE_SKIP_INSTALL_RULES)
      message(
        WARNING
        "Fetching ${name} needs CMake 3.28 to keep it out of the install, so "
        "install will fail. Install ${name}, use CMake 3.28 or newer, or "
        "configure with -DCMAKE_SKIP_INSTALL_RULES=ON to build without "
        "installing."
      )
    endif()
  else()
    set(exclude EXCLUDE_FROM_ALL)
  endif()
  set(CMAKE_SKIP_INSTALL_RULES ON)
  # Nor may its export(PACKAGE), as in cpptoml, put its build tree in the
  # user's package registry, where later configures would find it.
  set(CMAKE_EXPORT_NO_PACKAGE_REGISTRY ON)
  # Its warnings are for its own project to fix, and an installed copy
  # wouldn't print them either.
  if(NOT MSVC)
    string(APPEND CMAKE_C_FLAGS " -w")
    string(APPEND CMAKE_CXX_FLAGS " -w")
  endif()
  include(FetchContent)
  FetchContent_Declare(${name} ${source} ${ARGN} ${exclude})
  # CMAKE_REQUIRED_* configure this project's check_*() calls, and a
  # subproject inherits them. An imported target such as Threads::Threads is
  # absent from the separate project a try_compile() generates, so the
  # dependency's own checks would fail there with "target was not found".
  cmake_push_check_state(RESET)
  FetchContent_MakeAvailable(${name})
  cmake_pop_check_state()
  string(TOLOWER "${name}" lower)
  # A branch moves, so name the commit this configure got.
  if(branch AND rev STREQUAL branch)
    find_package(Git QUIET)
    if(GIT_FOUND)
      execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${${lower}_SOURCE_DIR}"
        OUTPUT_VARIABLE head
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
      )
      if(head)
        message(STATUS "${name} is at ${head}")
      endif()
    endif()
  endif()
  # A project fetched after this one skips its find_package() for this one when
  # it sees <name>_SOURCE_DIR, which FetchContent_MakeAvailable() sets here.
  set(${lower}_SOURCE_DIR "${${lower}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

# Sets <out> to whether this configure has added <dir> with add_subdirectory(),
# as a parent project does when it builds a dependency from source.
function(_fb_directory_added dir out)
  set(added FALSE)
  set(dirs "${CMAKE_SOURCE_DIR}")
  while(dirs)
    list(GET dirs 0 current)
    list(REMOVE_AT dirs 0)
    if(current STREQUAL dir)
      set(added TRUE)
      break()
    endif()
    get_property(subdirs DIRECTORY "${current}" PROPERTY SUBDIRECTORIES)
    list(APPEND dirs ${subdirs})
  endwhile()
  set(${out} ${added} PARENT_SCOPE)
endfunction()

# Finds <package> with find_package(CONFIG), unless a parent project builds it
# from source, and otherwise fetches it with fb_fetch_from_manifest(), as the
# manifest named <package> in lower case, unless a parent project already did.
# Further arguments go to FetchContent_Declare().
macro(fb_find_or_fetch package)
  string(TOLOWER "${package}" _fb_find_or_fetch_name)
  # project() caches <name>_SOURCE_DIR, so it can also be left from an earlier
  # configure; it names a copy being built only if this configure added it.
  set(_fb_find_or_fetch_built FALSE)
  if(DEFINED ${_fb_find_or_fetch_name}_SOURCE_DIR)
    _fb_directory_added(
      "${${_fb_find_or_fetch_name}_SOURCE_DIR}" _fb_find_or_fetch_built
    )
  endif()
  include(FetchContent)
  FetchContent_GetProperties(${_fb_find_or_fetch_name})
  if(NOT _fb_find_or_fetch_built AND NOT ${_fb_find_or_fetch_name}_POPULATED)
    find_package(${package} CONFIG QUIET)
    if(NOT ${package}_FOUND)
      fb_fetch_from_manifest(
        ${_fb_find_or_fetch_name} ${_fb_find_or_fetch_name} ${ARGN}
      )
    endif()
  endif()
endmacro()

# Finds GMock, and if it isn't installed, fetches the googletest that getdeps
# pins. FindGMock's variables describe either one. Further arguments go to
# FetchContent_Declare().
macro(fb_find_or_fetch_gmock)
  find_package(GMock MODULE)
  if(NOT LIBGMOCK_FOUND)
    # Everything else uses the shared runtime; gtest would pick the static one.
    set(gtest_force_shared_crt ON)
    fb_fetch_from_manifest(GTest googletest ${ARGN})
    # FindGMock picks up the GTest:: targets that googletest declares.
    find_package(GMock MODULE REQUIRED)
  endif()
endmacro()
