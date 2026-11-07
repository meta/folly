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

from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from . import directory_snapshot


def make_tree(
    root: Path, paths_to_contents: directory_snapshot.Snapshot
) -> directory_snapshot.Snapshot:
    """Build fixtures independently of the reader and writer under test."""
    root.mkdir(parents=True)
    for relative, contents in paths_to_contents.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
    return paths_to_contents


class DirectorySnapshotTest(unittest.TestCase):
    def test_round_trip_preserves_paths_bytes_and_empty_files(self) -> None:
        for paths_to_contents in (
            {},
            {
                Path("main.md"): b"Document.\r\n",
                Path("nested/deeper/data.bin"): b"\0\xff",
                Path("empty-file"): b"",
            },
        ):
            with (
                self.subTest(paths=paths_to_contents),
                tempfile.TemporaryDirectory() as temporary,
            ):
                root = Path(temporary)
                expected = make_tree(root / "source", paths_to_contents)
                (root / "source/unused-empty").mkdir()

                entries = directory_snapshot.read(root / "source")
                directory_snapshot.write(dict(reversed(entries.items())), root / "copy")

                self.assertEqual(entries, expected)
                self.assertEqual(directory_snapshot.read(root / "copy"), expected)

    def test_rejects_root_and_child_links(self) -> None:
        for kind in ("root", "directory", "file"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                make_tree(root / "outside", {Path("private"): b"not output"})
                source = root / "source"
                if kind == "root":
                    source.symlink_to(root / "outside")
                else:
                    make_tree(source, {Path("main.md"): b"valid"})
                    target = {
                        "directory": root / "outside",
                        "file": root / "outside/private",
                    }[kind]
                    (source / "link").symlink_to(target)

                with self.assertRaises(ValueError):
                    directory_snapshot.read(source)

    def test_rejects_special_files_without_reading_them(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            os.mkfifo(source / "pipe")
            # A regressed reader must fail this test, not block waiting for a writer.
            with patch.object(
                Path, "read_bytes", return_value=b"not a regular file"
            ) as read_bytes:
                with self.assertRaises(ValueError):
                    directory_snapshot.read(source)
                read_bytes.assert_not_called()

    def test_write_rejects_paths_outside_destination(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for relative in (
                Path("../escape"),
                root / "absolute",
                Path("nested/../../escape"),
            ):
                with self.subTest(relative=relative):
                    with self.assertRaises(ValueError):
                        directory_snapshot.write({relative: b"bad"}, root / "copy")
                    self.assertFalse((root / "copy").exists())
                    self.assertFalse((root / "escape").exists())
                    self.assertFalse((root / "absolute").exists())

    def test_write_does_not_change_existing_destinations(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            expected = make_tree(root / "existing", {Path("main.md"): b"keep"})
            with self.assertRaises(FileExistsError):
                directory_snapshot.write({Path("new.md"): b"new"}, root / "existing")
            self.assertEqual(directory_snapshot.read(root / "existing"), expected)
