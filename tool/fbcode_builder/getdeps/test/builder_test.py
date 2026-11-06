# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import os
import unittest
from unittest.mock import call, MagicMock, patch

from .. import builder as builder_module
from ..builder import CMakeBuilder
from ..envfuncs import Env
from ..manifest import ManifestContext, ManifestParser

MINIMAL_MANIFEST = """
[manifest]
name = test

[build]
builder = cmake
"""


def make_cmake_builder() -> CMakeBuilder:
    manifest = ManifestParser("test", MINIMAL_MANIFEST)
    loader = MagicMock()
    loader.get_project_install_dir.return_value = "/tmp/install"
    build_opts = MagicMock()
    build_opts.is_windows.return_value = False
    build_opts.is_darwin.return_value = False
    build_opts.shared_lib = False
    build_opts.build_type = "RelWithDebInfo"
    return CMakeBuilder(
        loader=loader,
        dep_manifests=[],
        build_opts=build_opts,
        ctx=ManifestContext(
            {
                "os": None,
                "distro": None,
                "distro_vers": None,
                "distro_family": None,
                "fb": "off",
                "fbsource": "off",
                "test": "off",
            }
        ),
        manifest=manifest,
        src_dir="/tmp/src",
        build_dir="/tmp/build",
        inst_dir="/tmp/inst",
        defines=None,
    )


class CMakeBuilderCompilerCacheTest(unittest.TestCase):
    def setUp(self) -> None:
        # Ensure SANDCASTLE is not set so compiler cache detection runs
        self._orig_sandcastle = os.environ.pop("SANDCASTLE", None)

    def tearDown(self) -> None:
        if self._orig_sandcastle is not None:
            os.environ["SANDCASTLE"] = self._orig_sandcastle
        else:
            os.environ.pop("SANDCASTLE", None)

    def _launcher_args(self, define_args: list) -> list:
        return [a for a in define_args if "CMAKE_CXX_COMPILER_LAUNCHER" in a]

    def test_sccache_preferred_over_ccache_when_both_available(self) -> None:
        builder = make_cmake_builder()
        env = Env()

        def fake_path_search(env, name):
            if name == "sccache":
                return "/usr/bin/sccache"
            if name == "ccache":
                return "/usr/bin/ccache"
            return None

        with patch.object(builder_module, "path_search", side_effect=fake_path_search):
            define_args = builder._compute_cmake_define_args(env)

        launcher_args = self._launcher_args(define_args)
        self.assertEqual(len(launcher_args), 1)
        self.assertIn("/usr/bin/sccache", launcher_args[0])

    def test_ccache_used_when_sccache_unavailable(self) -> None:
        builder = make_cmake_builder()
        env = Env()

        def fake_path_search(env, name):
            if name == "sccache":
                return None
            if name == "ccache":
                return "/usr/bin/ccache"
            return None

        with patch.object(builder_module, "path_search", side_effect=fake_path_search):
            define_args = builder._compute_cmake_define_args(env)

        launcher_args = self._launcher_args(define_args)
        self.assertEqual(len(launcher_args), 1)
        self.assertIn("/usr/bin/ccache", launcher_args[0])

    def test_no_compiler_cache_when_neither_available(self) -> None:
        builder = make_cmake_builder()
        env = Env()

        with patch.object(builder_module, "path_search", return_value=None):
            define_args = builder._compute_cmake_define_args(env)

        launcher_args = self._launcher_args(define_args)
        self.assertEqual(len(launcher_args), 0)

    def test_restart_sccache_server_starts_serially_under_memory_limit(self) -> None:
        builder = make_cmake_builder()
        env = Env()
        preexec_fn = MagicMock()

        with (
            patch.object(
                builder_module,
                "path_search",
                return_value="/usr/bin/sccache",
            ),
            patch.object(builder, "_run_cmd", return_value=0) as run_cmd,
        ):
            builder._restart_sccache_server(env, preexec_fn)

        self.assertEqual(
            run_cmd.call_args_list,
            [
                call(
                    ["/usr/bin/sccache", "--stop-server"],
                    env=env,
                    allow_fail=True,
                ),
                call(
                    ["/usr/bin/sccache", "--start-server"],
                    env=env,
                    preexec_fn=preexec_fn,
                ),
            ],
        )

    def test_restart_sccache_server_fails_when_start_fails(self) -> None:
        builder = make_cmake_builder()
        env = Env()

        with (
            patch.object(
                builder_module,
                "path_search",
                return_value="/usr/bin/sccache",
            ),
            patch.object(builder, "_run_cmd", side_effect=[0, 1]),
            self.assertRaisesRegex(RuntimeError, "Failure exit code 1"),
        ):
            builder._restart_sccache_server(env, MagicMock())

    def test_restart_sccache_server_skipped_in_sandcastle(self) -> None:
        builder = make_cmake_builder()
        env = Env()

        with (
            patch.dict(os.environ, {"SANDCASTLE": "1"}),
            patch.object(builder_module, "path_search") as path_search,
            patch.object(builder, "_run_cmd") as run_cmd,
        ):
            builder._restart_sccache_server(env, MagicMock())

        path_search.assert_not_called()
        run_cmd.assert_not_called()

    def test_restart_sccache_server_skipped_without_memory_limit(self) -> None:
        builder = make_cmake_builder()
        env = Env()

        with (
            patch.object(builder_module, "path_search") as path_search,
            patch.object(builder, "_run_cmd") as run_cmd,
        ):
            builder._restart_sccache_server(env, None)

        path_search.assert_not_called()
        run_cmd.assert_not_called()

    def test_restart_sccache_server_skipped_without_sccache(self) -> None:
        builder = make_cmake_builder()
        env = Env()

        with (
            patch.object(builder_module, "path_search", return_value=None),
            patch.object(builder, "_run_cmd") as run_cmd,
        ):
            builder._restart_sccache_server(env, MagicMock())

        run_cmd.assert_not_called()

    def test_z7_debug_info_on_windows(self) -> None:
        # On Windows, force MSVC embedded debug info (/Z7) so sccache can wrap
        # cl.exe without the shared-PDB C1041 race.
        builder = make_cmake_builder()
        # pyrefly: ignore [missing-attribute]
        builder.build_opts.is_windows.return_value = True
        env = Env()

        with patch.object(builder_module, "path_search", return_value=None):
            define_args = builder._compute_cmake_define_args(env)

        self.assertIn("-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded", define_args)
        self.assertIn("-DCMAKE_POLICY_DEFAULT_CMP0141=NEW", define_args)

    def test_openssl_root_dir_on_darwin(self) -> None:
        # On macOS, point OPENSSL_ROOT_DIR at the getdeps OpenSSL so CMake's
        # FindOpenSSL does not fall back to a wrong-arch Homebrew keg.
        builder = make_cmake_builder()
        # pyrefly: ignore [missing-attribute]
        builder.build_opts.is_darwin.return_value = True
        openssl = MagicMock()
        openssl.name = "openssl"
        builder.dep_manifests = [openssl]
        builder.install_dirs = ["/getdeps/installed/openssl-abc"]
        env = Env()

        with patch.object(builder_module, "path_search", return_value=None):
            define_args = builder._compute_cmake_define_args(env)

        self.assertIn("-DOPENSSL_ROOT_DIR=/getdeps/installed/openssl-abc", define_args)
