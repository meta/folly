#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import annotations

"""
Include Path Rewriter for getdeps

This module provides functionality to rewrite #include statements in C++ files
to handle differences between fbcode and open source project structures.
"""

import os
import re
import typing
from pathlib import Path
from typing import Any

if typing.TYPE_CHECKING:
    from .manifest import ManifestParser


class IncludePathRewriter:
    """Rewrites #include paths in C++ source files based on path mappings."""

    # C++ file extensions to process
    CPP_EXTENSIONS: set[str] = {
        ".cpp",
        ".cc",
        ".cxx",
        ".c",
        ".h",
        ".hpp",
        ".hxx",
        ".tcc",
        ".inc",
    }

    def __init__(self, mappings: list[tuple[str, str]], verbose: bool = False) -> None:
        """
        Initialize the rewriter with path mappings.

        Args:
            mappings: List of (old_path_prefix, new_path_prefix) tuples
            verbose: Enable verbose output
        """
        self.mappings: list[tuple[str, str]] = mappings
        self.verbose: bool = verbose

        # Compile regex patterns for efficiency
        self.patterns: list[tuple[re.Pattern[str], str, str]] = []
        for old_prefix, new_prefix in mappings:
            # Match both quoted and angle bracket includes
            # Pattern matches: #include "old_prefix/rest" or #include <old_prefix/rest>
            pattern = re.compile(
                r'(#\s*include\s*[<"])(' + re.escape(old_prefix) + r'/[^">]+)([">])',
                re.MULTILINE,
            )
            self.patterns.append((pattern, old_prefix, new_prefix))

    def rewrite_file(self, file_path: Path, dry_run: bool = False) -> bool:
        """
        Rewrite includes in a single file.

        Args:
            file_path: Path to the file to process
            dry_run: If True, don't actually modify files

        Returns:
            True if file was modified, False otherwise
        """
        try:
            with open(file_path, "r", encoding="utf-8") as f:
                original_content: str = f.read()
        except (IOError, UnicodeDecodeError) as e:
            if self.verbose:
                print(f"Warning: Could not read {file_path}: {e}")
            return False

        modified_content: str = original_content
        changes_made: bool = False

        for pattern, old_prefix, new_prefix in self.patterns:

            def make_replace_func(
                old_prefix: str, new_prefix: str
            ) -> typing.Callable[[re.Match[str]], str]:
                def replace_func(match: re.Match[str]) -> str:
                    nonlocal changes_made
                    prefix: str = match.group(1)  # #include [<"]
                    full_path: str = match.group(2)  # full path
                    suffix: str = match.group(3)  # [">]

                    # Replace the old prefix with new prefix
                    new_path: str = full_path.replace(old_prefix, new_prefix, 1)

                    if self.verbose and not changes_made:
                        print(f"  {full_path} -> {new_path}")

                    changes_made = True
                    return f"{prefix}{new_path}{suffix}"

                return replace_func

            modified_content = pattern.sub(
                make_replace_func(old_prefix, new_prefix), modified_content
            )

        if changes_made and not dry_run:
            try:
                with open(file_path, "w", encoding="utf-8") as f:
                    f.write(modified_content)
                if self.verbose:
                    print(f"Modified: {file_path}")
            except IOError as e:
                print(f"Error: Could not write {file_path}: {e}")
                return False
        elif changes_made and dry_run:
            if self.verbose:
                print(f"Would modify: {file_path}")

        return changes_made

    def process_directory(self, source_dir: Path, dry_run: bool = False) -> int:
        """
        Process all C++ files in a directory recursively.

        Args:
            source_dir: Root directory to process
            dry_run: If True, don't actually modify files

        Returns:
            Number of files modified
        """
        if not source_dir.exists():
            if self.verbose:
                print(f"Warning: Directory {source_dir} does not exist")
            return 0

        modified_count: int = 0
        processed_count: int = 0

        for root, dirs, files in os.walk(source_dir):
            # Skip hidden directories and common build directories
            dirs[:] = [
                d
                for d in dirs
                if not d.startswith(".")
                and d not in {"build", "_build", "__pycache__", "CMakeFiles"}
            ]

            for file in files:
                file_path: Path = Path(root) / file

                # Only process C++ files
                if file_path.suffix.lower() not in self.CPP_EXTENSIONS:
                    continue

                processed_count += 1
                if self.verbose:
                    print(f"Processing: {file_path}")

                if self.rewrite_file(file_path, dry_run):
                    modified_count += 1

        if self.verbose or modified_count > 0:
            print(f"Processed {processed_count} files, modified {modified_count} files")
        return modified_count


def rewrite_includes_from_manifest(
    manifest: ManifestParser, ctx: Any, source_dir: str, verbose: bool = False
) -> int:
    """
    Rewrite includes using mappings from a manifest file.

    Args:
        manifest: The manifest object containing shipit.pathmap section
        ctx: The manifest context
        source_dir: Directory containing source files to process
        verbose: Enable verbose output

    Returns:
        Number of files modified
    """
    mappings: list[tuple[str, str]] = []

    # Get mappings from the manifest's shipit.pathmap section
    for src, dest in manifest.get_section_as_ordered_pairs("shipit.pathmap", ctx):
        # Remove fbcode/ or xplat/ prefixes from src since they won't appear in #include statements
        if src.startswith("fbcode/"):
            src = src[len("fbcode/") :]
        elif src.startswith("xplat/"):
            src = src[len("xplat/") :]
        # pyre-fixme[6]: For 1st argument expected `Tuple[str, str]` but got
        #  `Tuple[str, Optional[str]]`.
        mappings.append((src, dest))

    if not mappings:
        if verbose:
            print("No include path mappings found in manifest")
        return 0

    if verbose:
        print("Include path mappings:")
        for old_path, new_path in mappings:
            print(f"  {old_path} -> {new_path}")

    rewriter: IncludePathRewriter = IncludePathRewriter(mappings, verbose)
    return rewriter.process_directory(Path(source_dir), dry_run=False)
