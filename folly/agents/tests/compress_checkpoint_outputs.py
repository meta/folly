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

"""Compress a saved run without losing files from any draft.

Before compression, a sample keeps its final draft in output/ and earlier drafts
in output-PHASE/.

Saved stages contain the author's files, not reviewer reports:

- initial: the first draft, before author self-review;
- author: after author self-review, before external review;
- reviewN: after incorporating external review N and checking the revisions.

Usage: python3 -m folly.agents.tests.compress_checkpoint_outputs SAMPLE

Keep final output/ readable and share identical file contents across phases.
For the same filename in adjacent phases, use a forward normal diff when it is
smaller than 60% of the later file. Append .diff to the complete relative filename.
Store patches and whole replacement files together in output-A-to-B/.

Example: two external review rounds. Author review makes a small edit to main.md
but rewrites docs/guide.md, so main.md uses a patch and guide.md stays whole.
It also adds notes.md and deletes retired.md. Review1 makes small edits to both
documents. The final review2 draft lives only in output/.
Empty directories are omitted from these trees.

    Before compression                 After compression
    sample/                            sample/
      output-initial/                     output-initial/
        main.md                             main.md
        docs/guide.md                       docs/guide.md
        retired.md                          retired.md
      output-author/                      output-initial-to-author/
        main.md                             main.md.diff
        docs/guide.md                       docs/guide.md
        notes.md
      output-review1/                     output-author-to-review1/
        main.md                             main.md.diff
        docs/guide.md                       docs/guide.md.diff
        notes.md                          output/
      output/  (after review2)               main.md
        main.md                             docs/guide.md
        docs/guide.md                       notes.md
        notes.md                          checkpoints.json
      checkpoints.json

checkpoints.json lists each phase's complete file set. Restore author's main.md
from its initial file plus output-initial-to-author/main.md.diff. Copy guide.md
whole from output-initial-to-author/docs/guide.md, and notes.md from output/.
Review1 adds another patch to main.md's chain, but patches guide.md's author
version. Each file has its own base and ordered patches.

Restoration creates a new tree from that file map, not an overlay on the previous
phase; retired.md therefore stays absent from author and later drafts. Identical
contents can share a base even when their filenames differ.

Verify every restored path and byte before removing redundant copies.
If a patch name matches a real file or directory, keep the changed file whole.
Files without a final newline stay whole: the normal-diff reader cannot apply
their missing-newline markers. Restore with SAMPLE --restore PHASE --destination PATH; the destination
must not exist.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from collections import Counter
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Sequence, TextIO

from . import directory_snapshot


_STAGED_SCRIPTS = Path(__file__).parent / "folly/agents/scripts"
_APPLY_DIFFS = (
    _STAGED_SCRIPTS / "apply_diffs"
    if _STAGED_SCRIPTS.is_dir()
    else Path(__file__).resolve().parent.parent / "scripts/apply_diffs"
)


@dataclass(frozen=True)
class Phase:
    name: str
    artifact: str
    contents: directory_snapshot.Snapshot


@dataclass(frozen=True)
class Representation:
    base: str
    diffs: tuple[str, ...] = ()


def _read_regular(sample: Path, relative: str | Path) -> bytes:
    parent = sample
    if parent.is_symlink() or not parent.is_dir():
        raise ValueError(f"sample input is not a regular directory: {parent}")
    for component in Path(relative).parts[:-1]:
        parent /= component
        if parent.is_symlink() or not parent.is_dir():
            raise ValueError(f"sample input is not a regular directory: {parent}")
    path = sample / relative
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"sample input is not a regular file: {path}")
    return path.read_bytes()


def _load(sample: Path) -> tuple[list[dict[str, Any]], list[Phase]]:
    records = json.loads(_read_regular(sample, "checkpoints.json").decode())
    if not isinstance(records, list) or not records:
        raise ValueError("checkpoints.json must contain a nonempty list")
    phases = []
    for index, record in enumerate(records):
        if not isinstance(record, dict):
            raise ValueError("checkpoint record is not an object")
        expected = (
            "initial"
            if index == 0
            else "author"
            if index == 1
            else f"review{index - 1}"
        )
        expected_artifact = (
            "output" if index == len(records) - 1 else f"output-{expected}"
        )
        # Older saved runs may have a separate named copy of the final phase.
        if record.get("phase") != expected or record.get("artifact") not in {
            expected_artifact,
            f"output-{expected}",
        }:
            raise ValueError("checkpoint phase or artifact is invalid")
        phases.append(
            Phase(
                expected,
                record["artifact"],
                directory_snapshot.read(sample / record["artifact"]),
            )
        )
    if (
        phases[-1].artifact != "output"
        and directory_snapshot.read(sample / "output") != phases[-1].contents
    ):
        raise ValueError("output/ does not match the final checkpoint")
    return records, phases


def _use_diff(diff_size: int, full_size: int) -> bool:
    """Apply the strict 60% threshold without floating-point math."""
    return diff_size * 5 < full_size * 3


def _forward_diff(
    sample: Path, earlier: Path, later: Path, old: bytes, new: bytes
) -> bytes | None:
    if not old.endswith(b"\n") or not new.endswith(b"\n"):
        return None
    result = subprocess.run(
        ["diff", "--text", "--", str(earlier), str(later)],
        cwd=sample,
        stdout=subprocess.PIPE,
    )
    if result.returncode not in (0, 1):
        result.check_returncode()
    return result.stdout


def _representations(
    sample: Path, phases: list[Phase]
) -> list[dict[str, Representation]]:
    storage_directories = [Path("output-initial")] + [
        Path(f"output-{earlier.name}-to-{later.name}")
        for earlier, later in zip(phases, phases[1:])
    ]
    storage_directories[-1] = Path("output")
    final = phases[-1].contents
    contents_to_base = {
        contents: f"output/{relative}" for relative, contents in final.items()
    }
    for phase, directory in zip(phases, storage_directories):
        for relative, contents in phase.contents.items():
            contents_to_base.setdefault(contents, f"{directory}/{relative}")
    counts = Counter(
        contents for phase in phases for contents in phase.contents.values()
    )
    contents_to_representation = {
        contents: Representation(contents_to_base[contents])
        for contents in final.values()
    }
    result = []
    storage: dict[Path, directory_snapshot.Snapshot] = {}
    for index, phase in enumerate(phases):
        directory = storage_directories[index]
        files = {}
        directories = {parent for path in phase.contents for parent in path.parents}
        for relative, contents in phase.contents.items():
            representation = contents_to_representation.get(contents)
            if representation is None and counts[contents] > 1:
                representation = Representation(contents_to_base[contents])
            if representation is None and index:
                earlier = phases[index - 1]
                old = earlier.contents.get(relative)
                patch_path = Path(f"{relative}.diff")
                # A real file named x.diff must not collide with x's patch.
                if (
                    old is not None
                    and patch_path not in phase.contents
                    and patch_path not in directories
                ):
                    diff = _forward_diff(
                        sample,
                        Path(earlier.artifact) / relative,
                        Path(phase.artifact) / relative,
                        old,
                        contents,
                    )
                    if diff is not None and _use_diff(len(diff), len(contents)):
                        storage.setdefault(directory, {})[patch_path] = diff
                        previous = result[-1][relative.as_posix()]
                        representation = Representation(
                            previous.base,
                            (*previous.diffs, f"{directory}/{patch_path}"),
                        )
            if representation is None:
                representation = Representation(contents_to_base[contents])
            # Copy only new full bases; initial and final bases already exist.
            if (
                index not in (0, len(phases) - 1)
                and representation.base == f"{directory}/{relative}"
            ):
                storage.setdefault(directory, {})[relative] = contents
            contents_to_representation[contents] = representation
            files[relative.as_posix()] = representation
        result.append(files)
    for directory, entries in storage.items():
        destination = sample / directory
        if destination.exists() or destination.is_symlink():
            if directory_snapshot.read(destination) != entries:
                raise ValueError(
                    "existing checkpoint storage differs from this compression"
                )
        else:
            directory_snapshot.write(entries, destination)
    return result


def _reconstruct(sample: Path, storage: dict[str, Any]) -> directory_snapshot.Snapshot:
    entries: directory_snapshot.Snapshot = {}
    for name, stored in storage["files"].items():
        relative = Path(name)
        representation = Representation(stored["base"], tuple(stored["diffs"]))
        for source in (representation.base, *representation.diffs):
            path = Path(source)
            if not path.parts or path.is_absolute() or ".." in path.parts:
                raise ValueError(f"invalid storage reference: {source}")
            _read_regular(sample, path)
        if representation.diffs:
            result = subprocess.run(
                [str(_APPLY_DIFFS), representation.base, *representation.diffs],
                cwd=sample,
                check=True,
                stdout=subprocess.PIPE,
            )
            entries[relative] = result.stdout
        else:
            entries[relative] = _read_regular(sample, representation.base)
    return entries


def restore(sample: Path, phase: str, destination: Path) -> None:
    """Restore each file at its original relative path."""
    records = json.loads(_read_regular(sample, "checkpoints.json").decode())
    matching = [record for record in records if record["phase"] == phase]
    if len(matching) != 1:
        raise ValueError(f"expected one checkpoint phase: {phase}")
    entries = _reconstruct(sample, matching[0]["storage"])
    if destination.exists() or destination.is_symlink():
        raise FileExistsError(f"restore destination already exists: {destination}")
    directory_snapshot.write(entries, destination)


def _mapping(phases: list[Phase]) -> str:
    lines = [
        "## Checkpoints",
        "",
        "From the checkout's `fbcode/`, replace `SAMPLE_DIRECTORY` and each destination path.",
        "",
    ]
    for phase in phases:
        label = (
            "Initial draft"
            if phase.name == "initial"
            else (
                "Author review"
                if phase.name == "author"
                else f"Review {phase.name.removeprefix('review')}"
            )
        )
        lines.append(
            f"- **{label}:** `python3 -m folly.agents.tests.compress_checkpoint_outputs "
            f"SAMPLE_DIRECTORY --restore {phase.name} --destination /path/to/{phase.name}`"
        )
    return "\n".join(lines) + "\n"


def compress(sample: Path, mapping_output: TextIO) -> None:
    """Verify every complete phase and emit its map before deleting sources."""
    records, phases = _load(sample)
    stored = _representations(sample, phases)
    for record, phase, files in zip(records, phases, stored):
        record["storage"] = {
            "files": {
                name: asdict(representation) for name, representation in files.items()
            },
        }
        if _reconstruct(sample, record["storage"]) != phase.contents:
            raise ValueError(f"could not reconstruct phase {phase.name}")
    mapping_output.write(_mapping(phases))
    mapping_output.flush()
    retained = {
        representation.base for files in stored for representation in files.values()
    }
    for record in records:
        record.pop("artifact")
    accounting = sample / "checkpoints.json"
    temporary_accounting = sample / ".checkpoints.json.tmp"
    temporary_accounting.unlink(missing_ok=True)
    temporary_accounting.write_text(
        json.dumps(records, indent=2, sort_keys=True) + "\n"
    )
    temporary_accounting.replace(accounting)
    for phase in phases:
        if phase.artifact == "output":
            continue
        for relative in phase.contents:
            if f"{phase.artifact}/{relative}" not in retained:
                (sample / phase.artifact / relative).unlink()


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sample", type=Path)
    parser.add_argument("--restore", metavar="PHASE")
    parser.add_argument("--destination", type=Path)
    args = parser.parse_args(argv)
    if (args.restore is None) != (args.destination is None):
        parser.error("--restore and --destination must be supplied together")
    if args.restore is None:
        compress(args.sample, sys.stdout)
    else:
        restore(args.sample, args.restore, args.destination)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
