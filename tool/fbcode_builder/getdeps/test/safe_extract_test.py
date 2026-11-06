# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import io
import os
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

from ..fetcher import _validate_archive_members, safe_extractall


class ValidateArchiveMembersTest(unittest.TestCase):
    def test_valid_paths(self) -> None:
        with tempfile.TemporaryDirectory() as dest:
            _validate_archive_members(["foo.txt", "subdir/bar.txt", "a/b/c.txt"], dest)

    def test_blocks_absolute_path(self) -> None:
        with tempfile.TemporaryDirectory() as dest:
            with self.assertRaises(ValueError) as ctx:
                _validate_archive_members(["/etc/passwd"], dest)
            self.assertIn("absolute path", str(ctx.exception))

    def test_blocks_path_traversal(self) -> None:
        with tempfile.TemporaryDirectory() as dest:
            with self.assertRaises(ValueError) as ctx:
                _validate_archive_members(["../../etc/passwd"], dest)
            self.assertIn("path traversal", str(ctx.exception))

    def test_allows_dotdot_that_stays_within_dest(self) -> None:
        with tempfile.TemporaryDirectory() as dest:
            _validate_archive_members(["a/../b.txt"], dest)


class SafeExtractallTarTest(unittest.TestCase):
    def _create_tar(self, members: dict[str, bytes]) -> str:
        fd, path = tempfile.mkstemp(suffix=".tar.gz")
        os.close(fd)
        with tarfile.open(path, "w:gz") as tar:
            for name, data in members.items():
                info = tarfile.TarInfo(name=name)
                info.size = len(data)
                tar.addfile(info, io.BytesIO(data))
        return path

    def test_extracts_valid_tar(self) -> None:
        archive = self._create_tar(
            {"hello.txt": b"hello world", "sub/nested.txt": b"nested"}
        )
        try:
            with tempfile.TemporaryDirectory() as dest:
                with tarfile.open(archive) as tar:
                    safe_extractall(tar, dest)
                self.assertTrue(Path(dest, "hello.txt").is_file())
                self.assertTrue(Path(dest, "sub/nested.txt").is_file())
                with open(os.fspath(Path(dest, "hello.txt"))) as f:
                    self.assertEqual(f.read(), "hello world")
        finally:
            os.unlink(archive)

    def test_blocks_traversal_tar(self) -> None:
        archive = self._create_tar({"../../evil.txt": b"pwned"})
        try:
            with tempfile.TemporaryDirectory() as dest:
                with tarfile.open(archive) as tar:
                    with self.assertRaises(ValueError):
                        safe_extractall(tar, dest)
        finally:
            os.unlink(archive)


class SafeExtractallZipTest(unittest.TestCase):
    def _create_zip(self, members: dict[str, bytes]) -> str:
        fd, path = tempfile.mkstemp(suffix=".zip")
        os.close(fd)
        with zipfile.ZipFile(path, "w") as zf:
            for name, data in members.items():
                zf.writestr(name, data)
        return path

    def test_extracts_valid_zip(self) -> None:
        archive = self._create_zip(
            {"hello.txt": b"hello world", "sub/nested.txt": b"nested"}
        )
        try:
            with tempfile.TemporaryDirectory() as dest:
                with zipfile.ZipFile(archive) as zf:
                    safe_extractall(zf, dest)
                self.assertTrue(Path(dest, "hello.txt").is_file())
                with open(os.fspath(Path(dest, "hello.txt"))) as f:
                    self.assertEqual(f.read(), "hello world")
        finally:
            os.unlink(archive)

    def test_blocks_traversal_zip(self) -> None:
        archive = self._create_zip({"../../evil.txt": b"pwned"})
        try:
            with tempfile.TemporaryDirectory() as dest:
                with zipfile.ZipFile(archive) as zf:
                    with self.assertRaises(ValueError):
                        safe_extractall(zf, dest)
        finally:
            os.unlink(archive)
