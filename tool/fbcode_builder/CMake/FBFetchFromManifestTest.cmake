# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Checks of FBFetchFromManifest.cmake that need no project:
#
#   cmake -P FBFetchFromManifestTest.cmake
#
# Each check reads a manifest this script writes to a scratch directory, so
# the real manifests can change without breaking it. The last check reads the
# real boost and openssl manifests when they sit next to this module.

cmake_minimum_required(VERSION 3.14)
include("${CMAKE_CURRENT_LIST_DIR}/FBFetchFromManifest.cmake")

set(checks 0)
function(check what actual expected)
  if(NOT "${actual}" STREQUAL "${expected}")
    file(REMOVE_RECURSE "${scratch}")
    message(FATAL_ERROR "${what}:\n  got      '${actual}'\n  expected '${expected}'")
  endif()
  math(EXPR checks "${checks} + 1")
  set(checks "${checks}" PARENT_SCOPE)
endfunction()

# A scratch fbcode_builder layout: the module reads <dir>/manifests/<name>.
set(tmp "$ENV{TMPDIR}")
if(NOT tmp)
  set(tmp "$ENV{TEMP}")
endif()
if(NOT tmp)
  set(tmp "/tmp")
endif()
string(RANDOM LENGTH 8 token)
set(scratch "${tmp}/fb_fetch_from_manifest_test_${token}")
file(MAKE_DIRECTORY "${scratch}/manifests")
set(_FB_FETCH_FBCODE_BUILDER_DIR "${scratch}")

set(hash "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")
file(WRITE "${scratch}/manifests/archived" "[manifest]
name = archived

[download]
url = https://example.com/releases/archived-1.2.3.tar.gz
sha256 = ${hash}

[build]
builder = cmake
subdir = archived-1.2.3
")
file(WRITE "${scratch}/manifests/by_os" "[manifest]
name = by_os

[download.not(os=windows)]
url = https://example.com/releases/by_os-2.0.tar.gz
sha256 = ${hash}

[download.os=windows]
url = https://example.com/releases/by_os-2.0.zip
sha256 = fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210
")
file(WRITE "${scratch}/manifests/unhashed" "[manifest]
name = unhashed

[download]
url = https://example.com/releases/unhashed-0.1.tar.gz?token=abc#top
")
file(WRITE "${scratch}/manifests/cloned" "[manifest]
name = cloned

[git]
repo_url = https://example.com/cloned.git
rev = 0123456789abcdef0123456789abcdef01234567
")

# With no mirror, the manifest's URL alone.
set(FB_FETCH_ARCHIVE_MIRRORS "")
fb_manifest_download(archived args urls)
check("archived, no mirror: urls" "${urls}"
      "https://example.com/releases/archived-1.2.3.tar.gz")
check("archived, no mirror: args" "${args}"
      "URL;https://example.com/releases/archived-1.2.3.tar.gz;URL_HASH;SHA256=${hash};DOWNLOAD_NAME;archived-1.2.3.tar.gz")

# With two mirrors, each prefix with the hash, in order, then the URL.
set(FB_FETCH_ARCHIVE_MIRRORS
    "https://mirror-a.example/flat;https://mirror-b.example/tree")
set(mirrored "https://mirror-a.example/flat/${hash};https://mirror-b.example/tree/${hash}")
fb_manifest_download(archived args urls)
check("archived, two mirrors: urls" "${urls}"
      "${mirrored};https://example.com/releases/archived-1.2.3.tar.gz")
check("archived, two mirrors: args" "${args}"
      "URL;${urls};URL_HASH;SHA256=${hash};DOWNLOAD_NAME;archived-1.2.3.tar.gz")

# The arguments parse as FetchContent_Declare() and ExternalProject_Add() read
# them: every URL under URL, one hash, one name.
cmake_parse_arguments(parsed "" "URL_HASH;DOWNLOAD_NAME" "URL" ${args})
check("archived, two mirrors: parsed URL" "${parsed_URL}" "${urls}")
check("archived, two mirrors: parsed URL_HASH" "${parsed_URL_HASH}" "SHA256=${hash}")
check("archived, two mirrors: parsed DOWNLOAD_NAME" "${parsed_DOWNLOAD_NAME}"
      "archived-1.2.3.tar.gz")
check("archived, two mirrors: nothing else" "${parsed_UNPARSED_ARGUMENTS}" "")

# A conditional [download.<condition>] section counts as [download]; the first
# one wins, as in fb_manifest_value.
fb_manifest_download(by_os args urls)
check("by_os: urls" "${urls}" "${mirrored};https://example.com/releases/by_os-2.0.tar.gz")
check("by_os: args" "${args}"
      "URL;${urls};URL_HASH;SHA256=${hash};DOWNLOAD_NAME;by_os-2.0.tar.gz")

# No sha256: no mirror can name the archive, so the URL alone. The file name
# stops at a query or a fragment.
fb_manifest_download(unhashed args urls)
check("unhashed: urls" "${urls}" "https://example.com/releases/unhashed-0.1.tar.gz?token=abc#top")
check("unhashed: args" "${args}"
      "URL;${urls};URL_HASH;SHA256=;DOWNLOAD_NAME;unhashed-0.1.tar.gz")

# A [git] manifest has no archive.
fb_manifest_download(cloned args urls)
check("cloned: urls" "${urls}" "")
check("cloned: args" "${args}" "")

# A url whose query holds a / : the archive's name is in the path, and the
# last / of the whole url is in the query.
file(WRITE "${scratch}/manifests/queried" "[manifest]
name = queried

[download]
url = https://example.com/releases/queried-3.4.tar.gz?path=/pub/x
sha256 = ${hash}
")

# Each prefix is read without its surrounding spaces and without a trailing /,
# and an empty one is skipped, so one / stands between the prefix and the hash.
set(FB_FETCH_ARCHIVE_MIRRORS
    " https://mirror-a.example/flat/ ;;  ;https://mirror-b.example/tree//  ")
fb_manifest_download(archived args urls)
check("untidy prefixes: urls" "${urls}"
      "${mirrored};https://example.com/releases/archived-1.2.3.tar.gz")
check("untidy prefixes: args" "${args}"
      "URL;${urls};URL_HASH;SHA256=${hash};DOWNLOAD_NAME;archived-1.2.3.tar.gz")

# Five prefixes are the most that keep the manifest's own url reachable: CMake
# stops after six URLs that answer an HTTP error.
set(five "")
foreach(n RANGE 1 5)
  list(APPEND five "https://mirror-${n}.example/flat")
endforeach()
set(FB_FETCH_ARCHIVE_MIRRORS "${five}")
fb_manifest_download(archived args urls)
list(LENGTH urls count)
check("five prefixes: six URLs" "${count}" "6")
list(GET urls 5 last)
check("five prefixes: the manifest's url is last" "${last}"
      "https://example.com/releases/archived-1.2.3.tar.gz")

# The name stops at the query: a / inside the query names no file.
set(FB_FETCH_ARCHIVE_MIRRORS "https://mirror-a.example/flat")
fb_manifest_download(queried args urls)
check("queried: urls" "${urls}"
      "https://mirror-a.example/flat/${hash};https://example.com/releases/queried-3.4.tar.gz?path=/pub/x")
check("queried: args" "${args}"
      "URL;${urls};URL_HASH;SHA256=${hash};DOWNLOAD_NAME;queried-3.4.tar.gz")

# A case whose verdict is the configure's own stop runs in its own cmake -P: a
# FATAL_ERROR ends the script that raises it, so this script cannot read it.
set(here "${CMAKE_CURRENT_LIST_DIR}")
function(check_child what body stops expected)
  set(file "${scratch}/child.cmake")
  file(WRITE "${file}" "${body}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -P "${file}"
    RESULT_VARIABLE code
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
  )
  if(stops AND code EQUAL 0)
    file(REMOVE_RECURSE "${scratch}")
    message(FATAL_ERROR "${what}: the child exited 0, and a stop was expected")
  endif()
  if(NOT stops AND NOT code EQUAL 0)
    file(REMOVE_RECURSE "${scratch}")
    message(FATAL_ERROR "${what}: the child exited ${code}:\n${err}")
  endif()
  if(NOT "${out}${err}" MATCHES "${expected}")
    file(REMOVE_RECURSE "${scratch}")
    message(FATAL_ERROR
            "${what}:\n  got      '${out}${err}'\n  expected a match of '${expected}'")
  endif()
  math(EXPR checks "${checks} + 1")
  set(checks "${checks}" PARENT_SCOPE)
endfunction()

set(six "${five};https://mirror-6.example/flat")
set(child_head "include(\"${here}/FBFetchFromManifest.cmake\")
set(_FB_FETCH_FBCODE_BUILDER_DIR \"${scratch}\")
set(FB_FETCH_ARCHIVE_MIRRORS \"${six}\")
")

# A sixth prefix stops the configure, and the message names the count and the
# reason. Without the stop the build reads the mirrors alone.
check_child("six prefixes: the configure stops"
            "${child_head}fb_manifest_download(archived args urls)\n"
            TRUE "names 6 prefixes, and five is the most")

# A [git] manifest reads no prefix, so six of them change nothing for it.
set(child_tail [[
fb_manifest_download(cloned args urls)
if(NOT args STREQUAL "" OR NOT urls STREQUAL "")
  message(FATAL_ERROR "cloned returned '${args}' and '${urls}'")
endif()
message(STATUS "CLONED-EMPTY")
]])
check_child("six prefixes: a [git] manifest is unchanged"
            "${child_head}${child_tail}" FALSE "CLONED-EMPTY")

set(FB_FETCH_ARCHIVE_MIRRORS "")

file(REMOVE_RECURSE "${scratch}")

# The real boost and openssl manifests, when they sit next to this module: one
# mirror gives two URLs, the mirror's first, with the manifest's hash and the
# archive's name. fb_fetch_from_manifest gives boost's arguments to
# FetchContent_Declare(), and folly gives openssl's to ExternalProject_Add().
get_filename_component(fbcode_builder "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)
foreach(manifest boost openssl)
  if(EXISTS "${fbcode_builder}/manifests/${manifest}")
    set(_FB_FETCH_FBCODE_BUILDER_DIR "${fbcode_builder}")
    set(FB_FETCH_ARCHIVE_MIRRORS "https://mirror.example/flat")
    fb_manifest_value(${manifest} download url expected_url)
    fb_manifest_value(${manifest} download sha256 expected_hash)
    string(REGEX REPLACE ".*/" "" expected_name "${expected_url}")
    fb_manifest_download(${manifest} args urls)
    check("${manifest}: urls" "${urls}"
          "https://mirror.example/flat/${expected_hash};${expected_url}")
    check("${manifest}: args" "${args}"
          "URL;${urls};URL_HASH;SHA256=${expected_hash};DOWNLOAD_NAME;${expected_name}")
  endif()
endforeach()

message(STATUS "FBFetchFromManifestTest: ${checks} checks passed")
