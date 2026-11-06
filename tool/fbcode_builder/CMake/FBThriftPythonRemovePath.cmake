# Copyright (c) Facebook, Inc. and its affiliates.

if(NOT DEFINED FB_REMOVE_PATH)
  message(FATAL_ERROR "FB_REMOVE_PATH must be specified")
endif()

# file(REMOVE) always removes the link itself. Avoid remove_directory here:
# old CMake versions have not consistently handled directory symlinks safely.
if(IS_SYMLINK "${FB_REMOVE_PATH}")
  file(REMOVE "${FB_REMOVE_PATH}")
elseif(EXISTS "${FB_REMOVE_PATH}")
  file(REMOVE_RECURSE "${FB_REMOVE_PATH}")
endif()
