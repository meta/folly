# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import os
import tempfile
import unittest
from pathlib import Path

from ..fetcher import filter_strip_marker, ShipitPathMap
from ..manifest import ManifestParser


class ManifestStripMarkerTest(unittest.TestCase):
    def test_default_strip_marker(self) -> None:
        p = ManifestParser(
            "test",
            """
[manifest]
name = test
""",
        )
        self.assertEqual(p.shipit_strip_marker, "@fb-only")

    def test_custom_strip_marker(self) -> None:
        p = ManifestParser(
            "test",
            """
[manifest]
name = test
shipit_strip_marker = @oss-disable
""",
        )
        self.assertEqual(p.shipit_strip_marker, "@oss-disable")


class FilterStripMarkerTest(unittest.TestCase):
    def _write_temp(self, content: str) -> str:
        fd, path = tempfile.mkstemp(suffix=".txt")
        os.close(fd)
        with open(path, "w") as f:
            f.write(content)
        return path

    def _read(self, path: str) -> str:
        with open(path, "r") as f:
            return f.read()

    def test_single_line_removal(self) -> None:
        path = self._write_temp("keep this\nremove this @fb-only\nkeep this too\n")
        try:
            filter_strip_marker(path, "@fb-only")
            self.assertEqual(self._read(path), "keep this\nkeep this too\n")
        finally:
            os.unlink(path)

    def test_block_removal(self) -> None:
        content = (
            "before\n"
            "// @fb-only-start\n"
            "secret stuff\n"
            "more secret\n"
            "// @fb-only-end\n"
            "after\n"
        )
        path = self._write_temp(content)
        try:
            filter_strip_marker(path, "@fb-only")
            self.assertEqual(self._read(path), "before\nafter\n")
        finally:
            os.unlink(path)

    def test_no_marker_present_no_change(self) -> None:
        original = "nothing special here\njust plain code\n"
        path = self._write_temp(original)
        try:
            filter_strip_marker(path, "@fb-only")
            self.assertEqual(self._read(path), original)
        finally:
            os.unlink(path)

    def test_custom_marker_single_line(self) -> None:
        content = "keep\nremove @oss-disable\nkeep too\n"
        path = self._write_temp(content)
        try:
            filter_strip_marker(path, "@oss-disable")
            self.assertEqual(self._read(path), "keep\nkeep too\n")
        finally:
            os.unlink(path)

    def test_custom_marker_block(self) -> None:
        content = (
            "before\n"
            "# @oss-disable-start\n"
            "internal only\n"
            "# @oss-disable-end\n"
            "after\n"
        )
        path = self._write_temp(content)
        try:
            filter_strip_marker(path, "@oss-disable")
            self.assertEqual(self._read(path), "before\nafter\n")
        finally:
            os.unlink(path)

    def test_custom_marker_ignores_default(self) -> None:
        """When using a custom marker, @fb-only lines should be kept."""
        content = "keep @fb-only\nremove @oss-disable\nplain\n"
        path = self._write_temp(content)
        try:
            filter_strip_marker(path, "@oss-disable")
            self.assertEqual(self._read(path), "keep @fb-only\nplain\n")
        finally:
            os.unlink(path)

    def test_mixed_single_and_block(self) -> None:
        content = (
            "line1\n"
            "line2 @fb-only\n"
            "line3\n"
            "// @fb-only-start\n"
            "block content\n"
            "// @fb-only-end\n"
            "line4\n"
        )
        path = self._write_temp(content)
        try:
            filter_strip_marker(path, "@fb-only")
            self.assertEqual(self._read(path), "line1\nline3\nline4\n")
        finally:
            os.unlink(path)

    def test_marker_with_regex_metacharacters(self) -> None:
        """Markers containing regex metacharacters should be escaped properly."""
        content = "keep\nremove @fb.only\nkeep too\n"
        path = self._write_temp(content)
        try:
            # With proper escaping, the dot is literal, not a wildcard
            filter_strip_marker(path, "@fb.only")
            self.assertEqual(self._read(path), "keep\nkeep too\n")
        finally:
            os.unlink(path)

    def test_binary_file_skipped(self) -> None:
        """Binary files that can't be decoded as UTF-8 should be skipped."""
        fd, path = tempfile.mkstemp(suffix=".bin")
        os.close(fd)
        binary_content = b"\x80\x81\x82\xff\xfe"
        with open(path, "wb") as f:
            f.write(binary_content)
        try:
            filter_strip_marker(path, "@fb-only")
            with open(path, "rb") as f:
                self.assertEqual(f.read(), binary_content)
        finally:
            os.unlink(path)


class ShipitMirrorSymlinkTest(unittest.TestCase):
    """Regression test for mcrouter OSS getdeps build (T262442347).

    mcrouter's scripts/order_centos-7.2/15_fbthrift is a relative symlink to
    ../recipes/fbthrift.sh. os.walk visits order_centos-7.2/ before recipes/
    alphabetically, so when mirror() copied the symlink first, filter_strip_marker
    followed the dangling link and crashed with FileNotFoundError. mirror() must
    skip filter_strip_marker for symlinks; the actual target file gets filtered
    when os.walk reaches it on its own.
    """

    def test_mirror_skips_filter_for_dangling_symlinks(self) -> None:
        with tempfile.TemporaryDirectory() as src_root, tempfile.TemporaryDirectory() as dest_root:
            project_dir = os.fspath(Path(src_root, "proj"))
            order_dir = os.fspath(Path(project_dir, "scripts", "order_centos-7.2"))
            recipes_dir = os.fspath(Path(project_dir, "scripts", "recipes"))
            Path(order_dir).mkdir(parents=True, exist_ok=True)
            Path(recipes_dir).mkdir(parents=True, exist_ok=True)

            target_file = os.fspath(Path(recipes_dir, "fbthrift.sh"))
            with open(target_file, "w") as f:
                f.write("#!/bin/bash\necho hello\n")

            symlink_path = os.fspath(Path(order_dir, "15_fbthrift"))
            os.symlink("../recipes/fbthrift.sh", symlink_path)

            mapping = ShipitPathMap()
            mapping.add_mapping("proj", "proj")
            mapping.mirror(src_root, dest_root)

            mirrored_symlink = os.fspath(
                Path(dest_root, "proj", "scripts", "order_centos-7.2", "15_fbthrift")
            )
            mirrored_target = os.fspath(
                Path(dest_root, "proj", "scripts", "recipes", "fbthrift.sh")
            )
            self.assertTrue(Path(mirrored_symlink).is_symlink())
            self.assertTrue(Path(mirrored_target).is_file())
