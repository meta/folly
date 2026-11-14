# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# NOTE: This file is manually maintained, not auto-generated.
# Update this file when the corresponding BUCK file changes.

# Picks the vendored assembly for the target. CMAKE_SYSTEM_PROCESSOR names the
# host on Windows and may differ from CMAKE_OSX_ARCHITECTURES on Apple
# platforms, so those check the architectures being compiled instead.
include(CheckCSourceCompiles)
include(CheckSymbolExists)
unset(abi)
set(asm_language ASM)
if (APPLE)
  # The wrappers include the Mach-O variant for each architecture compiled.
  set(archs ${CMAKE_OSX_ARCHITECTURES})
  if (NOT archs)
    set(archs ${CMAKE_SYSTEM_PROCESSOR})
  endif ()
  foreach (arch IN LISTS archs)
    if (NOT arch MATCHES "^(x86_64|arm64|aarch64)$")
      message(FATAL_ERROR "folly/fibers does not support ${arch} on Apple platforms")
    endif ()
  endforeach ()
  set(abi macho_gas.S)
elseif (MSVC)
  check_symbol_exists(_M_ARM64EC "" FOLLY_FCONTEXT_ARM64EC)
  check_symbol_exists(_M_ARM64 "" FOLLY_FCONTEXT_ARM64)
  check_symbol_exists(_M_X64 "" FOLLY_FCONTEXT_X64)
  check_symbol_exists(_M_IX86 "" FOLLY_FCONTEXT_X86)
  if (FOLLY_FCONTEXT_ARM64EC)
    message(FATAL_ERROR "folly/fibers does not support ARM64EC")
  elseif (FOLLY_FCONTEXT_ARM64)
    if (CMAKE_VERSION VERSION_LESS 3.26)
      message(FATAL_ERROR "Building folly for ARM64 Windows requires CMake 3.26")
    endif ()
    set(abi arm64_aapcs_pe_armasm.asm)
    set(asm_language ASM_MARMASM)
  elseif (FOLLY_FCONTEXT_X64)
    set(abi x86_64_ms_pe_masm.asm)
    set(asm_language ASM_MASM)
  elseif (FOLLY_FCONTEXT_X86)
    set(abi i386_ms_pe_masm.asm)
    set(asm_language ASM_MASM)
    set(asm_options /safeseh)
  else ()
    message(FATAL_ERROR "folly/fibers does not support this MSVC target")
  endif ()
  enable_language(${asm_language})
elseif (WIN32)
  # MinGW and other GNU-style toolchains.
  check_symbol_exists(__x86_64__ "" FOLLY_FCONTEXT_X86_64)
  if (NOT FOLLY_FCONTEXT_X86_64)
    message(FATAL_ERROR "folly/fibers supports only x86_64 with a GNU-style toolchain on Windows")
  endif ()
  set(abi x86_64_ms_pe_clang_gas.S)
else ()
  set(arch ${CMAKE_SYSTEM_PROCESSOR})
  if (arch MATCHES "^(x86_64|amd64|AMD64|i[3-6]86|x86)$")
    check_symbol_exists(__x86_64__ "" FOLLY_FCONTEXT_X86_64)
    check_symbol_exists(__ILP32__ "" FOLLY_FCONTEXT_ILP32)
    check_symbol_exists(__i386__ "" FOLLY_FCONTEXT_I386)
    if (FOLLY_FCONTEXT_X86_64 AND FOLLY_FCONTEXT_ILP32)
      message(FATAL_ERROR "folly/fibers does not support the x32 ABI")
    elseif (FOLLY_FCONTEXT_X86_64)
      set(abi x86_64_sysv_elf_gas.S)
    elseif (FOLLY_FCONTEXT_I386)
      set(abi i386_sysv_elf_gas.S)
    endif ()
  elseif (arch MATCHES "^(aarch64|arm64|ARM64)$")
    set(abi arm64_aapcs_elf_gas.S)
  elseif (arch MATCHES "^arm")
    set(abi arm_aapcs_elf_gas.S)
  elseif (arch STREQUAL "riscv64")
    # The assembly saves the floating-point registers with fsd and fld.
    check_symbol_exists(__riscv_d "" FOLLY_FCONTEXT_RISCV_D)
    if (NOT FOLLY_FCONTEXT_RISCV_D)
      message(FATAL_ERROR "folly/fibers requires the D extension on riscv64")
    endif ()
    set(abi riscv64_sysv_elf_gas.S)
  elseif (arch STREQUAL "s390x")
    set(abi s390x_sysv_elf_gas.S)
  elseif (arch MATCHES "^mips")
    check_c_source_compiles("
      #if _MIPS_SIM != _ABIO32
      #error
      #endif
      int main(void) { return 0; }" FOLLY_FCONTEXT_MIPS_O32)
    check_c_source_compiles("
      #if _MIPS_SIM != _ABI64
      #error
      #endif
      int main(void) { return 0; }" FOLLY_FCONTEXT_MIPS_N64)
    if (FOLLY_FCONTEXT_MIPS_O32)
      set(abi mips32_o32_elf_gas.S)
    elseif (FOLLY_FCONTEXT_MIPS_N64)
      set(abi mips64_n64_elf_gas.S)
    else ()
      message(FATAL_ERROR "folly/fibers supports only the o32 and n64 MIPS ABIs")
    endif ()
  endif ()
  if (NOT abi)
    message(FATAL_ERROR "folly/fibers does not support ${arch}")
  endif ()
endif ()

set_source_files_properties(make_${abi} jump_${abi}
  PROPERTIES LANGUAGE ${asm_language})
folly_add_library(
  NAME fcontext
  SRCS make_${abi} jump_${abi}
  HEADERS fcontext.h
)

if (MSVC)
  # Folly's compile options are for the C++ compiler, not the assembler.
  set_target_properties(folly_external_boost-fcontext_fcontext_obj PROPERTIES
    COMPILE_OPTIONS "${asm_options}"
    COMPILE_DEFINITIONS BOOST_CONTEXT_EXPORT=)
endif ()
