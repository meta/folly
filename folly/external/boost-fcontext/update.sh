#!/bin/sh
#
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

# Regenerates the vendored Boost.Context assembly from a Boost source tree,
# renaming the symbols outside lines that start with a comment:
#   folly/external/boost-fcontext/update.sh path/to/boost

set -eu

if [ $# -ne 1 ]; then
  echo "usage: $0 <boost-source-dir>" >&2
  exit 1
fi
src="$1/libs/context/src/asm"
dst=$(dirname "$0")

abis="
  arm64_aapcs_elf_gas.S
  arm64_aapcs_macho_gas.S
  arm64_aapcs_pe_armasm.asm
  arm_aapcs_elf_gas.S
  i386_ms_pe_masm.asm
  i386_sysv_elf_gas.S
  mips32_o32_elf_gas.S
  mips64_n64_elf_gas.S
  riscv64_sysv_elf_gas.S
  s390x_sysv_elf_gas.S
  x86_64_ms_pe_clang_gas.S
  x86_64_ms_pe_masm.asm
  x86_64_sysv_elf_gas.S
  x86_64_sysv_macho_gas.S
"

for abi in $abis; do
  for fn in make jump; do
    if [ ! -r "$src/${fn}_$abi" ]; then
      echo "$0: cannot read $src/${fn}_$abi" >&2
      exit 1
    fi
  done
done

for abi in $abis; do
  for fn in make jump; do
    sed "/^[[:space:]]*[#/;@]/!s/${fn}_fcontext/folly_${fn}_fcontext/g" \
      "$src/${fn}_$abi" > "$dst/${fn}_$abi"
  done
done

echo "Check that the context layout that make_*.S saves still matches what"
echo "folly/fibers/BoostContextCompatibility.h reads for frame pointers."
