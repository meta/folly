# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Helpers that compute the build settings induced by --shared-lib.

Split out from builder.py so the gating logic
(linux/darwin/top-level-vs-dep) can be unit-tested in isolation.
"""


from __future__ import annotations

import typing

if typing.TYPE_CHECKING:
    from .buildopts import BuildOptions
    from .envfuncs import Env


def _append_token(existing: str, token: str) -> str:
    if token in existing.split():
        return existing
    return (existing + " " + token).strip()


def apply_shared_lib_top_level_cmake_defines(
    defines: dict[str, str], build_opts: BuildOptions
) -> None:
    """Inject the CMake variables --shared-lib needs on the top-level project.

    - BUILD_SHARED_LIBS=ON so the consumer is built as a shared library
      that statically links its PIC dep tree (setdefault, so an explicit
      caller-supplied value still wins).
    - On Linux: append -Wl,--exclude-libs=ALL to the shared/module linker
      flags so the deps' symbols stay out of the produced .so's dynamic
      export table (avoids ODR clashes if the host process loads another
      copy of any dep).
    """
    if not build_opts.shared_lib:
        return
    defines.setdefault("BUILD_SHARED_LIBS", "ON")
    if build_opts.is_linux():
        for var in ("CMAKE_SHARED_LINKER_FLAGS", "CMAKE_MODULE_LINKER_FLAGS"):
            defines[var] = _append_token(defines.get(var, ""), "-Wl,--exclude-libs=ALL")


def apply_shared_lib_dep_env(
    env: Env, build_opts: BuildOptions, manifest_name: str
) -> None:
    """Inject CFLAGS/CXXFLAGS that --shared-lib needs in a per-package build env.

    - Every cmake/autotools/make dep gets -fPIC so its archive can be
      linked into the consumer's shared library.
    - On macOS, non-top-level deps additionally get -fvisibility=hidden
      (and -fvisibility-inlines-hidden for C++) so their symbols become
      private_extern in the consumer dylib instead of leaking into its
      export table — Mach-O has no blanket --exclude-libs analogue.
      Skipped for the top-level project itself, which still needs its
      public API exported.
    """
    if not build_opts.shared_lib or build_opts.is_windows():
        return
    for var in ("CFLAGS", "CXXFLAGS"):
        env.set(var, _append_token(env.get(var, "") or "", "-fPIC"))
    if build_opts.is_darwin() and manifest_name != build_opts.top_level_manifest_name:
        for var, flag in (
            ("CFLAGS", "-fvisibility=hidden"),
            ("CXXFLAGS", "-fvisibility=hidden"),
            ("CXXFLAGS", "-fvisibility-inlines-hidden"),
        ):
            env.set(var, _append_token(env.get(var, "") or "", flag))
