# Copyright (c) Facebook, Inc. and its affiliates.

# This file applies common compiler settings that are shared across
# a number of Facebook opensource projects.
# Please use caution and your best judgement before making changes
# to these shared compiler settings in order to avoid accidentally
# breaking a build in another project!

# C and C++ only: ml64, which assembles the MASM sources of dependencies such as
# Boost.Context, reads /Zc:preprocessor as several options, /Zs among them, and
# then writes no object file. CMake before 3.15 takes one language per
# $<COMPILE_LANGUAGE>, hence the OR.
add_compile_options(
  # 'class1' : inherits 'class2::member' via dominance
  "$<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:/wd4250>"
  # Enable conforming preprocessor for __VA_OPT__ support
  "$<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:/Zc:preprocessor>"
)
