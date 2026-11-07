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

import unittest
from typing import Any

from . import debrief_tables


def record(
    phase: str,
    changed: float | None,
    wall_seconds: float,
    author: tuple[int, int, int, int],
    reviewers: tuple[tuple[int, int, int, int], ...] = (),
) -> dict[str, Any]:
    def tokens(values: tuple[int, int, int, int]) -> dict[str, int]:
        total, cached, cache_write, output = values
        return {
            "input_tokens": total,
            "cached_input_tokens": cached,
            "cache_write_input_tokens": cache_write,
            "output_tokens": output,
            "reasoning_output_tokens": 0,
        }

    return {
        "author_tokens": tokens(author),
        "changed_word_percent": changed,
        "phase": phase,
        "reviewer_tokens": [tokens(reviewer) for reviewer in reviewers],
        "wall_seconds": wall_seconds,
    }


def cells(line: str) -> list[str]:
    return [cell.strip().strip("*") for cell in line.strip("|").split("|")]


class DebriefTablesTest(unittest.TestCase):
    def test_duration_rounding_carries_to_the_next_minute(self) -> None:
        self.assertEqual(debrief_tables._duration(59.96), "1:00.0")

    def test_total_duration_sums_displayed_values(self) -> None:
        records = [
            record("initial", None, 0.06, (0, 0, 0, 0)),
            record("author", 0.0, 0.06, (0, 0, 0, 0)),
        ]

        self.assertIn("| **0:00.2** |", debrief_tables.render(records, "gpt-5.6-sol"))

    def test_renders_production_tables(self) -> None:
        records = [
            record("initial", None, 10.04, (100, 40, 10, 5)),
            record(
                "author",
                12.5,
                9.04,
                (200, 150, 0, 10),
                ((50, 20, 5, 2), (10, 5, 0, 1)),
            ),
        ]
        records[0]["changed_paths"] = ["main.md"]
        records[1]["changed_paths"] = ["main.md", "notes.md"]

        output = debrief_tables.render(records, "gpt-5.6-sol")

        self.assertEqual(
            output,
            """| Phase | Main effect | Changed words vs prior | Changed paths | Wall time | Est. cost |
| --- | --- | ---: | --- | ---: | ---: |
| Initial draft | Initial artifact | — | main.md | 0:10.0 | $0.0008 |
| Author review | TODO | 12.5% | main.md, notes.md | 0:09.0 | $0.0013 |
| **Production total** |  | — | — | **0:19.0** | **$0.0021** |

| Phase | Uncached input | Cached input | Cache write | Output |
| --- | ---: | ---: | ---: | ---: |
| Initial draft | 60 | 40 | 10 | 5 |
| Author review | 85 | 175 | 5 | 13 |
| **Production total** | **145** | **215** | **15** | **18** |

Estimated cost uses published `gpt-5.6-sol` rates for all production tokens.
""",
        )

    def test_marks_unknown_pricing(self) -> None:
        output = debrief_tables.render(
            [record("review1", 1.0, 1.0, (1, 0, 0, 1))], "other-model"
        )

        self.assertIn("| Review 1 fixes | TODO | 1.0% | 0:01.0 | unavailable |", output)
        self.assertIn(
            "| **Production total** |  | — | **0:01.0** | **unavailable** |", output
        )
        self.assertIn("Published rates are not configured for `other-model`.", output)

    def test_failed_review_retains_known_retry_usage_and_marks_unknown_cost(
        self,
    ) -> None:
        records = [
            record("initial", None, 10.04, (100, 40, 10, 5)),
            record("review1", 12.5, 9.04, (200, 150, 0, 10), ((50, 20, 5, 2),)),
        ]
        records[1]["reviewer_tokens"].insert(0, {})

        output = debrief_tables.render(records, "gpt-5.6-sol")
        impact, tokens, *_ = output.split("\n\n")
        impact_rows = impact.splitlines()
        token_rows = tokens.splitlines()
        self.assertEqual(cells(impact_rows[3])[2:], ["12.5%", "0:09.0", "unavailable"])
        self.assertEqual(cells(impact_rows[4])[-2:], ["0:19.0", "unavailable"])
        self.assertEqual(
            cells(token_rows[3])[1:], ["80 + ?", "170 + ?", "5 + ?", "12 + ?"]
        )
        self.assertEqual(
            cells(token_rows[4])[1:], ["140 + ?", "210 + ?", "15 + ?", "17 + ?"]
        )
