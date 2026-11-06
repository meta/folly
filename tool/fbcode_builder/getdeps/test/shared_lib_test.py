# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import unittest
from unittest.mock import MagicMock

from ..envfuncs import Env
from ..shared_lib import (
    apply_shared_lib_dep_env,
    apply_shared_lib_top_level_cmake_defines,
)


def make_build_opts(
    *,
    shared_lib: bool = True,
    is_linux: bool = True,
    is_darwin: bool = False,
    is_windows: bool = False,
    top_level_manifest_name: str | None = "consumer",
):
    build_opts = MagicMock()
    build_opts.shared_lib = shared_lib
    build_opts.is_linux.return_value = is_linux
    build_opts.is_darwin.return_value = is_darwin
    build_opts.is_windows.return_value = is_windows
    build_opts.top_level_manifest_name = top_level_manifest_name
    return build_opts


class ApplyTopLevelCmakeDefinesTest(unittest.TestCase):
    def test_no_op_when_shared_lib_off(self) -> None:
        defines = {}
        apply_shared_lib_top_level_cmake_defines(
            defines, make_build_opts(shared_lib=False)
        )
        self.assertEqual(defines, {})

    def test_linux_sets_build_shared_libs_and_exclude_libs(self) -> None:
        defines = {}
        apply_shared_lib_top_level_cmake_defines(
            defines, make_build_opts(is_linux=True)
        )
        self.assertEqual(defines["BUILD_SHARED_LIBS"], "ON")
        self.assertIn("-Wl,--exclude-libs=ALL", defines["CMAKE_SHARED_LINKER_FLAGS"])
        self.assertIn("-Wl,--exclude-libs=ALL", defines["CMAKE_MODULE_LINKER_FLAGS"])

    def test_darwin_sets_build_shared_libs_no_linker_flags(self) -> None:
        defines = {}
        apply_shared_lib_top_level_cmake_defines(
            defines, make_build_opts(is_linux=False, is_darwin=True)
        )
        self.assertEqual(defines["BUILD_SHARED_LIBS"], "ON")
        self.assertNotIn("CMAKE_SHARED_LINKER_FLAGS", defines)
        self.assertNotIn("CMAKE_MODULE_LINKER_FLAGS", defines)

    def test_caller_build_shared_libs_override_wins(self) -> None:
        defines = {"BUILD_SHARED_LIBS": "OFF"}
        apply_shared_lib_top_level_cmake_defines(defines, make_build_opts())
        self.assertEqual(defines["BUILD_SHARED_LIBS"], "OFF")

    def test_existing_linker_flags_are_preserved_and_appended(self) -> None:
        defines = {"CMAKE_SHARED_LINKER_FLAGS": "-Wl,--as-needed"}
        apply_shared_lib_top_level_cmake_defines(
            defines, make_build_opts(is_linux=True)
        )
        self.assertEqual(
            defines["CMAKE_SHARED_LINKER_FLAGS"],
            "-Wl,--as-needed -Wl,--exclude-libs=ALL",
        )

    def test_idempotent(self) -> None:
        defines = {}
        opts = make_build_opts(is_linux=True)
        apply_shared_lib_top_level_cmake_defines(defines, opts)
        apply_shared_lib_top_level_cmake_defines(defines, opts)
        self.assertEqual(defines["CMAKE_SHARED_LINKER_FLAGS"], "-Wl,--exclude-libs=ALL")


class ApplyDepEnvTest(unittest.TestCase):
    def test_no_op_when_shared_lib_off(self) -> None:
        env = Env()
        apply_shared_lib_dep_env(env, make_build_opts(shared_lib=False), "anydep")
        self.assertNotIn("CFLAGS", env)
        self.assertNotIn("CXXFLAGS", env)

    def test_no_op_on_windows(self) -> None:
        env = Env()
        apply_shared_lib_dep_env(
            env,
            make_build_opts(is_linux=False, is_windows=True),
            "anydep",
        )
        self.assertNotIn("CFLAGS", env)

    def test_linux_dep_gets_pic_only(self) -> None:
        env = Env()
        apply_shared_lib_dep_env(env, make_build_opts(is_linux=True), "boost")
        self.assertEqual(env.get("CFLAGS"), "-fPIC")
        self.assertEqual(env.get("CXXFLAGS"), "-fPIC")

    def test_linux_top_level_gets_pic_only(self) -> None:
        env = Env()
        apply_shared_lib_dep_env(env, make_build_opts(is_linux=True), "consumer")
        self.assertEqual(env.get("CFLAGS"), "-fPIC")
        self.assertEqual(env.get("CXXFLAGS"), "-fPIC")
        self.assertNotIn("-fvisibility=hidden", (env.get("CFLAGS") or "").split())
        self.assertNotIn("-fvisibility=hidden", (env.get("CXXFLAGS") or "").split())

    def test_darwin_dep_gets_pic_and_hidden_visibility(self) -> None:
        env = Env()
        apply_shared_lib_dep_env(
            env,
            make_build_opts(is_linux=False, is_darwin=True),
            "boost",
        )
        self.assertIn("-fPIC", env.get("CFLAGS", "").split())
        self.assertIn("-fvisibility=hidden", env.get("CFLAGS", "").split())
        self.assertIn("-fPIC", env.get("CXXFLAGS", "").split())
        self.assertIn("-fvisibility=hidden", env.get("CXXFLAGS", "").split())
        self.assertIn("-fvisibility-inlines-hidden", env.get("CXXFLAGS", "").split())

    def test_darwin_top_level_skips_hidden_visibility(self) -> None:
        env = Env()
        apply_shared_lib_dep_env(
            env,
            make_build_opts(is_linux=False, is_darwin=True),
            "consumer",
        )
        self.assertEqual(env.get("CFLAGS"), "-fPIC")
        self.assertEqual(env.get("CXXFLAGS"), "-fPIC")

    def test_existing_flags_preserved(self) -> None:
        env = Env()
        env.set("CXXFLAGS", "-O2")
        apply_shared_lib_dep_env(env, make_build_opts(is_linux=True), "boost")
        tokens = env.get("CXXFLAGS", "").split()
        self.assertIn("-O2", tokens)
        self.assertIn("-fPIC", tokens)

    def test_idempotent(self) -> None:
        env = Env()
        opts = make_build_opts(is_linux=False, is_darwin=True)
        apply_shared_lib_dep_env(env, opts, "boost")
        apply_shared_lib_dep_env(env, opts, "boost")
        # Each flag should appear exactly once
        cxx_tokens = env.get("CXXFLAGS", "").split()
        self.assertEqual(cxx_tokens.count("-fPIC"), 1)
        self.assertEqual(cxx_tokens.count("-fvisibility=hidden"), 1)
        self.assertEqual(cxx_tokens.count("-fvisibility-inlines-hidden"), 1)
