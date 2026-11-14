# boost-fcontext

The `make_fcontext` and `jump_fcontext` assembly from
[Boost.Context](https://github.com/boostorg/context) 1.77.0, and a header
declaring them, adapted from `boost/context/detail/fcontext.hpp`.

The assembly matches Boost.Context 1.77.0 with two exceptions: the Windows arm64
(armasm) files are backported from Boost.Context 1.80.0, which added them, and
the arm64 ELF files add BTI landing pads (a `bti c` and the GNU BTI property
note), functionally equivalent to those Boost.Context 1.90.0 later added.
Otherwise, the only change to the assembly is that the functions are renamed to
`folly_make_fcontext` and `folly_jump_fcontext`, so that they do not clash with
Boost.Context in a program that links both. `update.sh` regenerates the
assembly from a Boost source tree. `make_macho_gas.S` and `jump_macho_gas.S`
include the Mach-O variant for the target architecture, so that CMake builds
for any combination of arm64 and x86_64 in `CMAKE_OSX_ARCHITECTURES`, including
universal binaries, and uses Boost.Context for other architectures.

Distributed under the Boost Software License, Version 1.0 (see
`LICENSE_1_0.txt`).
