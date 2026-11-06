# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import os
import tempfile
import unittest
from pathlib import Path

from ..buildopts import BuildOptions
from ..fetcher import SimpleShipitTransformerFetcher
from ..getdeps_platform import HostType
from ..manifest import ManifestContext, ManifestParser


def _manifest(layout_line: str = "") -> ManifestParser:
    return ManifestParser(
        "test",
        f"""
[manifest]
name = test
shipit_fbcode_builder = true
{layout_line}

[shipit.pathmap]
fbcode/test = .
""",
    )


class FbcodeBuilderLayoutTest(unittest.TestCase):
    def test_default_layout_is_build(self) -> None:
        self.assertEqual(_manifest().shipit_fbcode_builder_dir, "build/fbcode_builder")

    def test_tool_layout(self) -> None:
        self.assertEqual(
            _manifest("shipit_fbcode_builder_layout = tool").shipit_fbcode_builder_dir,
            "tool/fbcode_builder",
        )

    def test_unknown_layout_is_rejected(self) -> None:
        with self.assertRaisesRegex(Exception, "shipit_fbcode_builder_layout=tools;"):
            _manifest("shipit_fbcode_builder_layout = tools")

    def test_shipit_mirror_uses_layout(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            fbsource = Path(tmp, "fbsource")
            Path(fbsource, "fbcode/test").mkdir(parents=True)
            Path(fbsource, "fbcode/test/CMakeLists.txt").write_text("")
            Path(fbsource, "fbcode/opensource/fbcode_builder").mkdir(parents=True)
            Path(fbsource, "fbcode/opensource/fbcode_builder/getdeps.py").write_text("")
            opts = BuildOptions(
                fbcode_builder_dir=tmp,
                scratch_dir=os.path.join(tmp, "scratch"),
                host_type=HostType(),
            )
            opts.fbsource_dir = os.fspath(fbsource)
            fetcher = SimpleShipitTransformerFetcher(
                opts,
                _manifest("shipit_fbcode_builder_layout = tool"),
                ManifestContext(
                    {
                        "os": "linux",
                        "distro": None,
                        "distro_vers": None,
                        "distro_family": None,
                        "fb": "on",
                        "fbsource": "on",
                        "test": "off",
                    }
                ),
            )

            fetcher.update()

            src = Path(fetcher.get_src_dir())
            self.assertTrue(Path(src, "tool/fbcode_builder/getdeps.py").is_file())
            self.assertFalse(Path(src, "build").exists())

    def test_pins_found_next_to_tool_fbcode_builder(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            fbcode_builder = Path(tmp, "tool/fbcode_builder")
            fbcode_builder.mkdir(parents=True)
            Path(tmp, "tool/deps/github_hashes").mkdir(parents=True)

            opts = BuildOptions(
                fbcode_builder_dir=os.fspath(fbcode_builder),
                scratch_dir=os.path.join(tmp, "scratch"),
                host_type=HostType(),
            )

            self.assertEqual(
                Path(opts.project_hashes or "").resolve(),
                Path(tmp, "tool/deps/github_hashes").resolve(),
            )
