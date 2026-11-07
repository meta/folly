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

import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from . import compress_checkpoint_outputs as compressor, directory_snapshot


def make_sample(
    root: Path, phases: list[tuple[str, directory_snapshot.Snapshot]]
) -> None:
    root.mkdir()
    records = []
    for index, (name, entries) in enumerate(phases):
        directory = "output" if index == len(phases) - 1 else f"output-{name}"
        directory_snapshot.write(entries, root / directory)
        records.append({"artifact": directory, "phase": name, "wall_seconds": 1.0})
    (root / "checkpoints.json").write_text(json.dumps(records))


def compress(root: Path) -> list[dict]:
    compressor.compress(root, io.StringIO())
    return json.loads((root / "checkpoints.json").read_text())


def text_phases() -> list[tuple[str, directory_snapshot.Snapshot]]:
    # Enough unchanged text makes a one-line edit smaller as a diff than a file.
    initial = b"".join(f"line {index}\n".encode() for index in range(100))
    author = initial.replace(b"line 50\n", b"author edit\n")
    return [
        ("initial", {Path("main.md"): initial}),
        ("author", {Path("main.md"): author}),
        ("review1", {Path("main.md"): b"final\n"}),
    ]


class CompressCheckpointOutputsTest(unittest.TestCase):
    def test_deduplicates_files_and_keeps_final_tree_readable(self) -> None:
        # review1 returns to the initial draft; review3 repeats the final draft.
        # Both reuse full copies. Equal-content companions must keep both names
        # in the readable final output, even when their storage shares one base.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sample = root / "sample"
            initial, author, final = [entries for _, entries in text_phases()]
            phases = [
                ("initial", initial),
                ("author", author),
                ("review1", dict(initial)),
                ("review2", final),
                ("review3", dict(final)),
            ]
            for _, entries in phases:
                entries[Path("shared-copy.md")] = b"shared notes\n"
                entries[Path("shared.md")] = b"shared notes\n"
            make_sample(sample, phases)
            (sample / "unrelated.diff").write_bytes(b"Keep.\n")
            final = directory_snapshot.read(sample / "output")

            records = compress(sample)

            self.assertEqual(directory_snapshot.read(sample / "output"), final)
            self.assertTrue((sample / "output-initial/main.md").is_file())
            self.assertFalse((sample / "output-author/main.md").exists())
            shared = records[0]["storage"]["files"]["shared.md"]
            self.assertIn(shared["base"], ("output/shared.md", "output/shared-copy.md"))
            self.assertEqual(shared, records[0]["storage"]["files"]["shared-copy.md"])
            self.assertEqual(
                records[-1]["storage"]["files"]["main.md"]["base"], "output/main.md"
            )
            self.assertEqual((sample / "unrelated.diff").read_bytes(), b"Keep.\n")
            self.assertEqual(records[0]["wall_seconds"], 1.0)
            for record, (name, expected) in zip(records, phases):
                self.assertNotIn("artifact", record)
                destination = root / f"restore-{name}"
                compressor.restore(sample, name, destination)
                self.assertEqual(directory_snapshot.read(destination), expected)

    def test_restores_mixed_full_files_and_diff_chains(
        self,
    ) -> None:
        # main.md uses a diff; the guide's author rewrite stays whole. Review1
        # patches both from their different bases. Added and deleted companions
        # must also round-trip, rather than leak in from the previous phase.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sample = root / "sample"
            # GNU diff counts LF, not CR, as a line boundary. A stray encoding
            # byte also must survive storage rather than disable the text diff.
            initial = text_phases()[0][1][Path("main.md")].replace(
                b"line 40\n", b"line 40\rsegment\xff\n"
            )
            # Two distant deletions require applying diff hunks bottom to top.
            author = initial.replace(b"line 20\n", b"").replace(b"line 80\n", b"")
            review = author.replace(b"line 90\n", b"review edit\n")
            guide = initial.replace(b"line", b"guide")
            author_guide = b"".join(
                f"author guide {index}\n".encode() for index in range(100)
            )
            phases = [
                (
                    "initial",
                    {
                        Path("main.md"): initial,
                        Path("removed.txt"): b"Deleted next.\n",
                        Path("docs/guide.md"): guide,
                    },
                ),
                (
                    "author",
                    {
                        Path("main.md"): author,
                        Path("nested/added.txt"): b"Added.\n",
                        Path("docs/guide.md"): author_guide,
                    },
                ),
                (
                    "review1",
                    {
                        Path("main.md"): review,
                        Path("nested/added.txt"): b"Added.\n",
                        Path("docs/guide.md"): author_guide.replace(
                            b"author guide 60\n", b"review guide\n"
                        ),
                    },
                ),
                (
                    "review2",
                    {
                        Path("main.md"): review.replace(b"line 70\n", b"final edit\n"),
                        Path("docs/guide.md"): b"final guide\n",
                    },
                ),
            ]
            make_sample(sample, phases)

            records = compress(sample)

            for filename, base, author_patches in (
                (
                    "main.md",
                    "output-initial/main.md",
                    ["output-initial-to-author/main.md.diff"],
                ),
                ("docs/guide.md", "output-initial-to-author/docs/guide.md", []),
            ):
                with self.subTest(filename=filename):
                    self.assertEqual(
                        records[1]["storage"]["files"][filename],
                        {"base": base, "diffs": author_patches},
                    )
                    self.assertEqual(
                        records[2]["storage"]["files"][filename],
                        {
                            "base": base,
                            "diffs": [
                                *author_patches,
                                f"output-author-to-review1/{filename}.diff",
                            ],
                        },
                    )
            self.assertFalse((sample / "output-author/docs/guide.md").exists())
            for name, expected in phases:
                destination = root / name
                compressor.restore(sample, name, destination)
                self.assertEqual(directory_snapshot.read(destination), expected)

    def test_patch_name_collision_keeps_the_conflicting_file_whole(self) -> None:
        # A replacement file named main.md.diff, or a directory with that name,
        # shares storage with main.md's patch. Keep main.md whole to avoid loss.
        for companion in ("main.md.diff", "main.md.diff/guide.md"):
            with (
                self.subTest(companion=companion),
                tempfile.TemporaryDirectory() as temporary,
            ):
                root = Path(temporary)
                sample = root / "sample"
                phases = text_phases()
                for name, entries in phases:
                    entries[Path(companion)] = f"{name} guide\n".encode()
                make_sample(sample, phases)

                records = compress(sample)

                self.assertEqual(
                    records[1]["storage"]["files"]["main.md"],
                    {"base": "output-initial-to-author/main.md", "diffs": []},
                )
                for name, expected in phases:
                    destination = root / name
                    compressor.restore(sample, name, destination)
                    self.assertEqual(directory_snapshot.read(destination), expected)

    def test_diff_threshold_is_strict(self) -> None:
        self.assertTrue(compressor._use_diff(179, 300))
        self.assertFalse(compressor._use_diff(180, 300))

    def test_diff_threshold_uses_later_file_size(self) -> None:
        # A 179-byte patch is below 60% of the new 300-byte file, but above
        # 60% of the old 200-byte file. Using the old size would reject it.
        with tempfile.TemporaryDirectory() as temporary:
            sample = Path(temporary) / "sample"
            make_sample(
                sample,
                [
                    ("initial", {Path("main.md"): b"i" * 199 + b"\n"}),
                    ("author", {Path("main.md"): b"a" * 299 + b"\n"}),
                    ("review1", {Path("main.md"): b"final\n"}),
                ],
            )
            _, phases = compressor._load(sample)
            with mock.patch.object(
                compressor, "_forward_diff", return_value=b"x" * 179
            ):
                stored = compressor._representations(sample, phases)
            self.assertEqual(
                stored[1]["main.md"].diffs,
                ("output-initial-to-author/main.md.diff",),
            )

    def test_keeps_complete_files_without_final_newlines(self) -> None:
        # GNU diff emits a missing-newline marker our reader cannot apply.
        for old, new in ((b"no newline", b"valid\n"), (b"valid\n", b"no newline")):
            with self.subTest(old=old, new=new):
                self.assertIsNone(
                    compressor._forward_diff(
                        Path("."), Path("earlier"), Path("later"), old, new
                    )
                )

    def test_rejects_invalid_phase_sequence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            sample = Path(temporary) / "sample"
            make_sample(
                sample,
                [
                    ("initial", {Path("main.md"): b"initial\n"}),
                    ("final", {Path("main.md"): b"final\n"}),
                ],
            )
            accounting = (sample / "checkpoints.json").read_bytes()
            with self.assertRaises(ValueError):
                compress(sample)
            self.assertEqual((sample / "checkpoints.json").read_bytes(), accounting)

    def test_missing_companion_reconstruction_keeps_sources_and_accounting(
        self,
    ) -> None:
        # A plausible compressor bug restores main.md but drops an author-only
        # companion. Reject it before replacing the map or deleting any sources.
        with tempfile.TemporaryDirectory() as temporary:
            sample = Path(temporary) / "sample"
            phases = text_phases()
            phases[1][1][Path("notes.md")] = b"author companion\n"
            make_sample(sample, phases)
            accounting = (sample / "checkpoints.json").read_bytes()
            reconstruct = compressor._reconstruct

            def missing_companion(sample, storage):
                entries = reconstruct(sample, storage)
                entries.pop(Path("notes.md"), None)
                return entries

            with mock.patch.object(
                compressor, "_reconstruct", side_effect=missing_companion
            ):
                with self.assertRaises(ValueError):
                    compress(sample)
            self.assertEqual((sample / "checkpoints.json").read_bytes(), accounting)
            for record, (_, expected) in zip(json.loads(accounting), phases):
                self.assertEqual(
                    directory_snapshot.read(sample / record["artifact"]), expected
                )

    def test_mapping_failure_keeps_sources_and_accounting_and_can_retry(self) -> None:
        # If the caller cannot save restore commands, leave full phase copies
        # and accounting intact. A retry reuses storage containing both a patch
        # and an author-only full file, then restores the complete author draft.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sample = root / "sample"
            phases = text_phases()
            phases[1][1][Path("notes.md")] = b"author companion\n"
            make_sample(sample, phases)
            accounting = (sample / "checkpoints.json").read_bytes()
            mapping = io.StringIO()
            mapping.close()
            with self.assertRaises(ValueError):
                compressor.compress(sample, mapping)
            self.assertEqual((sample / "checkpoints.json").read_bytes(), accounting)
            for record, (_, expected) in zip(json.loads(accounting), phases):
                self.assertEqual(
                    directory_snapshot.read(sample / record["artifact"]), expected
                )
            compress(sample)
            compressor.restore(sample, "author", root / "restored")
            self.assertEqual(directory_snapshot.read(root / "restored"), phases[1][1])

    def test_restore_rejects_linked_sample_or_storage_parent(self) -> None:
        # Moving a base or patch directory behind a symlink must not let restore
        # read outside the saved tree or publish a partial destination.
        for linked in ("sample", "base", "diff"):
            with (
                self.subTest(linked=linked),
                tempfile.TemporaryDirectory() as temporary,
            ):
                root = Path(temporary)
                sample = root / "sample"
                make_sample(sample, text_phases())
                compress(sample)
                if linked == "sample":
                    alias = root / "alias"
                    alias.symlink_to(sample, target_is_directory=True)
                    sample = alias
                else:
                    parent = (
                        sample / "output-initial"
                        if linked == "base"
                        else sample / "output-initial-to-author"
                    )
                    outside = root / f"outside-{linked}"
                    parent.rename(outside)
                    parent.symlink_to(outside, target_is_directory=True)
                with self.assertRaises(ValueError):
                    compressor.restore(sample, "author", root / "restored")
                self.assertFalse((root / "restored").exists())

    def test_restore_is_exclusive_and_cli_restores_exact_tree(self) -> None:
        # Older samples kept a named final copy as well as output/. Their
        # recorded artifact paths must still compress and restore correctly.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sample = root / "sample"
            phases = text_phases()
            make_sample(sample, phases)
            records = json.loads((sample / "checkpoints.json").read_text())
            records[-1]["artifact"] = "output-review1"
            directory_snapshot.write(phases[-1][1], sample / "output-review1")
            (sample / "checkpoints.json").write_text(json.dumps(records))
            compress(sample)
            destination = root / "restored"
            self.assertEqual(
                compressor.main(
                    [
                        str(sample),
                        "--restore",
                        "author",
                        "--destination",
                        str(destination),
                    ]
                ),
                0,
            )
            self.assertEqual(directory_snapshot.read(destination), phases[1][1])
            existing = root / "existing"
            existing.mkdir()
            with self.assertRaises(FileExistsError):
                compressor.restore(sample, "initial", existing)


if __name__ == "__main__":
    unittest.main()
