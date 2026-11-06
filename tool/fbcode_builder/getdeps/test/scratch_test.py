# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import unittest

from ..buildopts import find_existing_win32_subst_for_path


class Win32SubstTest(unittest.TestCase):
    def test_no_existing_subst(self) -> None:
        self.assertIsNone(
            find_existing_win32_subst_for_path(
                r"C:\users\alice\appdata\local\temp\fbcode_builder_getdeps",
                subst_mapping={},
            )
        )
        self.assertIsNone(
            find_existing_win32_subst_for_path(
                r"C:\users\alice\appdata\local\temp\fbcode_builder_getdeps",
                subst_mapping={"X:\\": r"C:\users\alice\appdata\local\temp\other"},
            )
        )

    def test_exact_match_returns_drive_path(self) -> None:
        self.assertEqual(
            find_existing_win32_subst_for_path(
                r"C:\temp\fbcode_builder_getdeps",
                subst_mapping={"X:\\": r"C:\temp\fbcode_builder_getdeps"},
            ),
            "X:\\",
        )
        self.assertEqual(
            find_existing_win32_subst_for_path(
                r"C:/temp/fbcode_builder_getdeps",
                subst_mapping={"X:\\": r"C:/temp/fbcode_builder_getdeps"},
            ),
            "X:\\",
        )

    def test_multiple_exact_matches_returns_arbitrary_drive_path(self) -> None:
        self.assertIn(
            find_existing_win32_subst_for_path(
                r"C:\temp\fbcode_builder_getdeps",
                subst_mapping={
                    "X:\\": r"C:\temp\fbcode_builder_getdeps",
                    "Y:\\": r"C:\temp\fbcode_builder_getdeps",
                    "Z:\\": r"C:\temp\fbcode_builder_getdeps",
                },
            ),
            ("X:\\", "Y:\\", "Z:\\"),
        )

    def test_drive_letter_is_case_insensitive(self) -> None:
        self.assertEqual(
            find_existing_win32_subst_for_path(
                r"C:\temp\fbcode_builder_getdeps",
                subst_mapping={"X:\\": r"c:\temp\fbcode_builder_getdeps"},
            ),
            "X:\\",
        )

    def test_path_components_are_case_insensitive(self) -> None:
        self.assertEqual(
            find_existing_win32_subst_for_path(
                r"C:\TEMP\FBCODE_builder_getdeps",
                subst_mapping={"X:\\": r"C:\temp\fbcode_builder_getdeps"},
            ),
            "X:\\",
        )
        self.assertEqual(
            find_existing_win32_subst_for_path(
                r"C:\temp\fbcode_builder_getdeps",
                subst_mapping={"X:\\": r"C:\TEMP\FBCODE_builder_getdeps"},
            ),
            "X:\\",
        )
