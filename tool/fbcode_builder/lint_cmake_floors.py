#!/usr/bin/env python3
# (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

"""
Arc lint wrapper enforcing a single cmake_minimum_required floor across the
folly-family OSS CMake projects.

folly requires CMake 3.14, which is the effective floor for every
folly-dependent OSS project: declaring anything lower buys deprecation
warnings today and hard configure failures once CMake removes the next
compatibility band. The floor lives in exactly one place (FLOOR below).

This linter is a pure function of the files arc lint passes it: every input
is checked, nothing else is read. The set of covered files is owned by the
lint engine glob; the value they must meet is owned by FLOOR here.
See https://github.com/facebook/folly/issues/2703.

Usage:
    python3 lint_cmake_floors.py @pathsfile
    python3 lint_cmake_floors.py path/to/CMakeLists.txt ...
"""

import argparse
import json
import re
from pathlib import Path
from typing import Optional, Tuple

# Single source of truth for the minimum CMake across the folly-family OSS
# projects. Every checked file must declare at least this version; higher is
# fine (fbthrift is at 3.24, mcrouter at 3.16).
FLOOR: Tuple[int, ...] = (3, 14)

_MINIMUM_RE = re.compile(
    r"cmake_minimum_required\s*\(\s*VERSION\s+([0-9]+(?:\.[0-9]+){1,2})"
)


def parse_minimum(text: str) -> Optional[Tuple[int, ...]]:
    """Extract the declared minimum as an int tuple, or None if absent."""
    match = _MINIMUM_RE.search(text)
    if not match:
        return None
    return tuple(int(part) for part in match.group(1).split("."))


def meets_floor(found: Tuple[int, ...]) -> bool:
    """True when the declared minimum is at or above FLOOR."""
    width = max(len(found), len(FLOOR))
    padded = found + (0,) * (width - len(found))
    floor = FLOOR + (0,) * (width - len(FLOOR))
    return padded >= floor


def bump_to_floor(line: str) -> str:
    """Rewrite just the version token of a minimum declaration to FLOOR."""
    floor_str = ".".join(str(part) for part in FLOOR)
    return re.sub(
        r"(cmake_minimum_required\s*\(\s*VERSION\s+)[0-9]+(?:\.[0-9]+){1,2}",
        r"\g<1>" + floor_str,
        line,
        count=1,
    )


def emit(
    path: str,
    line: int,
    char: int,
    severity: str,
    original: Optional[str],
    replacement: Optional[str],
    description: str,
) -> None:
    print(
        json.dumps(
            {
                "path": path,
                "line": line,
                "char": char,
                "severity": severity,
                "name": "cmake-floor",
                "description": description,
                "original": original,
                "replacement": replacement,
                "bypassChangedLineFiltering": None,
            }
        ),
        flush=True,
    )


def check_content(display: str, content: str) -> None:
    """Check one file's content, emitting zero or one finding."""
    floor_str = ".".join(str(part) for part in FLOOR)
    found_line: Optional[str] = None
    found_version: Optional[Tuple[int, ...]] = None
    found_lineno = 1
    for i, line in enumerate(content.splitlines(), 1):
        match = _MINIMUM_RE.search(line)
        if match:
            # Keep the raw line so the autofix preserves indentation.
            found_line = line
            found_version = tuple(int(part) for part in match.group(1).split("."))
            found_lineno = i
            break
    if found_line is None or found_version is None:
        emit(
            display,
            1,
            1,
            "error",
            None,
            None,
            f"{display} declares no cmake_minimum_required. Add one at or "
            f"above the OSS floor {floor_str}.",
        )
        return
    if not meets_floor(found_version):
        found_str = ".".join(str(part) for part in found_version)
        emit(
            display,
            found_lineno,
            found_line.find("cmake_minimum_required") + 1,
            "error",
            found_line,
            bump_to_floor(found_line),
            f"cmake_minimum_required is {found_str}, below the OSS floor "
            f"{floor_str} (folly requires {floor_str}). Run `arc lint -a` "
            "to apply the bump. See "
            "https://github.com/facebook/folly/issues/2703.",
        )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Lint cmake_minimum_required floor on the given files.",
        fromfile_prefix_chars="@",
    )
    parser.add_argument(
        "filenames",
        nargs="+",
        help="paths to lint (arc lint passes @pathsfile, repo-relative)",
    )
    args = parser.parse_args()

    # Linttool guarantees the working directory is the repo root: resolve
    # every input against it, and check each input as given.
    root = Path.cwd()
    for raw in args.filenames:
        path = Path(raw)
        full = path if path.is_absolute() else root / path
        try:
            content = full.read_text()
        except OSError:
            emit(
                raw,
                1,
                1,
                "error",
                None,
                None,
                f"Could not read {raw}; skipping.",
            )
            continue
        check_content(raw, content)


if __name__ == "__main__":
    main()
