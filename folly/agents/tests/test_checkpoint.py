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

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from . import checkpoint, directory_snapshot


CHECKPOINT_KEY = bytes.fromhex("00112233445566778899aabbccddeeff")


class CheckpointTest(unittest.TestCase):
    def run_checkpoint(
        self, index: int, run_root: Path, workdir: Path
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [checkpoint.__file__, str(index)],
            env={
                **os.environ,
                "FOLLY_BACKTEST_RUN_DIR": str(run_root),
                "FOLLY_BACKTEST_WORKDIR": str(workdir),
            },
            text=True,
            capture_output=True,
            check=False,
        )

    def test_captures_sequence_and_reports_errors(self) -> None:
        # Save every output file, not just main.md. The second checkpoint must
        # capture the revision while retaining nested companions and empty files.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            workdir = root / "workdir"
            output = workdir / "output"
            output.mkdir(parents=True)
            (output / "main.md").write_text("initial")
            (output / "nested").mkdir()
            # Ordinary UTF-8/CRLF text must stay byte-exact through the save.
            (output / "nested/notes.md").write_text(
                "Notes — café\r\n", encoding="utf-8"
            )
            (output / "empty-file").touch()
            (root / "run.json").write_text(
                json.dumps({"checkpoint_key": CHECKPOINT_KEY.hex()})
            )

            first = self.run_checkpoint(0, root, workdir)
            (output / "main.md").write_text("author")
            second = self.run_checkpoint(1, root, workdir)

            self.assertEqual(first.returncode, 0)
            self.assertEqual(first.stderr, "")
            self.assertEqual(
                first.stdout.splitlines(),
                [
                    "@@FOLLY_BACKTEST_CHECKPOINT:5fdd93@@",
                    checkpoint.next_instruction(0),
                ],
            )
            self.assertEqual(second.returncode, 0)
            self.assertEqual(second.stderr, "")
            self.assertEqual(
                second.stdout.splitlines()[0],
                "@@FOLLY_BACKTEST_CHECKPOINT:233243@@",
            )
            for phrase in (
                "complete one round",
                "follow-up checks",
                "`backtest-checkpoint 2`",
                "follow its stdout",
            ):
                self.assertIn(phrase, second.stdout)
            self.assertEqual((root / "checkpoints/0/main.md").read_text(), "initial")
            self.assertEqual((root / "checkpoints/1/main.md").read_text(), "author")
            self.assertEqual(
                (root / "checkpoints/0/nested/notes.md").read_bytes(),
                "Notes — café\r\n".encode("utf-8"),
            )
            self.assertEqual((root / "checkpoints/0/empty-file").read_bytes(), b"")
            self.assertEqual(
                directory_snapshot.read(root / "checkpoints/1"),
                directory_snapshot.read(output),
            )

            invalid = self.run_checkpoint(1, root, workdir)

            self.assertNotEqual(invalid.returncode, 0)
            self.assertEqual(
                invalid.stdout.strip(),
                "This run is invalid. Stop and explain the error in your final response.",
            )

    def test_invalid_tree_does_not_publish_or_advance_a_checkpoint(self) -> None:
        # A failed save must not consume checkpoint 0 or print its success marker;
        # after fixing the draft, the author can retry the same checkpoint.
        for kind in ("empty", "link"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                output = root / "workdir/output"
                output.mkdir(parents=True)
                if kind == "link":
                    (output / "main.md").write_text("valid draft")
                    (output / "bad-link").symlink_to(root / "missing")
                (root / "run.json").write_text(
                    json.dumps({"checkpoint_key": CHECKPOINT_KEY.hex()})
                )

                failed = self.run_checkpoint(0, root, output.parent)

                self.assertNotEqual(failed.returncode, 0)
                self.assertNotIn("@@FOLLY_BACKTEST_CHECKPOINT:", failed.stdout)
                self.assertEqual(list((root / "checkpoints").iterdir()), [])
                if kind == "link":
                    self.assertIn("bad-link", failed.stderr)
                    (output / "bad-link").unlink()

                (output / "main.md").write_text("valid draft")
                retried = self.run_checkpoint(0, root, output.parent)

                self.assertEqual(retried.returncode, 0, retried.stderr)
                self.assertEqual(
                    directory_snapshot.read(root / "checkpoints/0"),
                    directory_snapshot.read(output),
                )
