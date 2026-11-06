# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Leaf helpers accept str | Path (slice 1 of the pathlib migration)."""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path

from ..copytree import containing_repo_type, find_eden_root
from ..envfuncs import path_search
from ..fetcher import copy_if_different, LocalDirFetcher
from ..runcmd import run_cmd


class PathInputsTest(unittest.TestCase):
    def test_containing_repo_type_accepts_path(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, ".git").mkdir(parents=True, exist_ok=True)
            for path in (tmp, Path(tmp)):
                repo_type, repo_root = containing_repo_type(path)
                self.assertEqual(repo_type, "git")
                self.assertEqual(repo_root, tmp)
                self.assertIsInstance(repo_root, str)

    def test_find_eden_root_none_for_plain_dir(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            for path in (tmp, Path(tmp)):
                self.assertIsNone(find_eden_root(path))

    def test_path_search_returns_str(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            for name in ("mytool", "mytool.exe"):
                with open(os.fspath(Path(tmp, name)), "w") as f:
                    f.write("#!/bin/sh\n")
                os.chmod(os.fspath(Path(tmp, name)), 0o755)
            env = {"PATH": tmp}
            found = path_search(env, "mytool")
            self.assertIsNotNone(found)
            self.assertIsInstance(found, str)
            self.assertEqual(os.fspath(Path(str(found)).parent), tmp)

    def test_run_cmd_accepts_path_cwd_and_log_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp, "out.log")
            rc = run_cmd(
                [sys.executable, "-c", "pass"],
                cwd=Path(tmp),
                log_file=log,
            )
            self.assertEqual(rc, 0)
            self.assertTrue(log.is_file())

    def test_local_dir_fetcher_accepts_path(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            fetcher = LocalDirFetcher(Path(tmp))
            src_dir = fetcher.get_src_dir()
            self.assertIsInstance(src_dir, str)
            self.assertEqual(src_dir, os.fspath(Path(tmp).resolve()))

    def test_copy_if_different_accepts_path(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp, "src.txt")
            dest = Path(tmp, "sub", "dest.txt")
            src.write_text("data\n")
            self.assertTrue(copy_if_different(src, dest))
            self.assertEqual(dest.read_text(), "data\n")
            # Second copy is a no-op: dest mtime is preserved.
            mtime = dest.stat().st_mtime_ns
            self.assertFalse(copy_if_different(src, dest))
            self.assertEqual(dest.stat().st_mtime_ns, mtime)
