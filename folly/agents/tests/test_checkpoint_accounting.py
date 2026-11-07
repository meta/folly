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
import shutil
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

from . import checkpoint, checkpoint_accounting, directory_snapshot


THREAD_ID = "01a00000-0000-7000-8000-000000000000"
CHECKPOINT_KEY = bytes.fromhex("00112233445566778899aabbccddeeff")


def usage(input_tokens: int, output_tokens: int) -> dict[str, int]:
    return {
        "input_tokens": input_tokens,
        "cached_input_tokens": input_tokens // 2,
        "cache_write_input_tokens": 0,
        "output_tokens": output_tokens,
        "reasoning_output_tokens": output_tokens // 2,
    }


def write_jsonl(path: Path, events: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(json.dumps(event) + "\n" for event in events))


def checkpoint_events(
    index: int, *, json_output: bool = False
) -> tuple[dict[str, object], ...]:
    marker = checkpoint.make_checkpoint_marker(index, CHECKPOINT_KEY)
    stdout = f"{marker}\n{checkpoint.next_instruction(index)}\n"
    text = (
        json.dumps({"output": stdout, "exit_code": 0})
        if json_output
        else f"Process exited with code 0\nFinal output:\n{stdout}"
    )
    # CommandExecution.stdout can be empty on a successful checkpoint. Its
    # model-visible tool result still carries the marker accounting needs.
    return (
        {
            "type": "event_msg",
            "payload": {
                "type": "item_completed",
                "item": {
                    "type": "CommandExecution",
                    "stdout": "",
                    "exit_code": 0,
                },
            },
        },
        {
            "type": "response_item",
            "payload": {
                "type": "custom_tool_call_output",
                "output": [
                    {"type": "input_text", "text": "Chunk ID: test"},
                    {
                        "type": "input_text",
                        "text": text,
                    },
                ],
            },
        },
    )


def token_event(tokens: dict[str, int], second: int) -> dict[str, object]:
    return {
        "timestamp": f"2026-09-04T00:00:{second:02d}Z",
        "type": "event_msg",
        "payload": {
            "type": "token_count",
            "info": {
                "total_token_usage": tokens,
                "model_context_window": 1000,
            },
        },
    }


class AccountingFixture:
    """Build saved trees, cumulative author usage, and reviewer attempts."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.workdir = root / "workspace/task"
        self.agent_home = root / "agent-home"
        self.checkpoints = root / "checkpoints"
        self.reviews = root / "reviews"
        self.workdir.mkdir(parents=True)
        self.checkpoints.mkdir()
        self.reviews.mkdir()
        (root / "run.json").write_text(
            json.dumps({"checkpoint_key": CHECKPOINT_KEY.hex()})
        )
        self.author_events: list[dict[str, object]] = [
            {
                "timestamp": "2026-09-04T00:00:00Z",
                "type": "session_meta",
            }
        ]
        self.checkpoint_count = 0
        self.review_count = 0

    @staticmethod
    def _set_time(path: Path, second: int) -> None:
        timestamp = datetime(2026, 9, 4, 0, 0, second, tzinfo=timezone.utc).timestamp()
        os.utime(path, (timestamp, timestamp))

    def checkpoint(
        self,
        text: str,
        tokens: dict[str, int],
        second: int,
        *,
        json_output: bool = False,
        companions: directory_snapshot.Snapshot | None = None,
    ) -> None:
        index = self.checkpoint_count
        snapshot = self.checkpoints / str(index)
        entries = {Path("main.md"): text.encode(), **(companions or {})}
        directory_snapshot.write(entries, snapshot)
        self._set_time(snapshot, second)
        output = self.workdir / "output"
        if output.exists():
            shutil.rmtree(output)
        directory_snapshot.write(entries, output)
        self.author_events.extend(
            (
                *checkpoint_events(index, json_output=json_output),
                token_event(tokens, second),
            )
        )
        self.checkpoint_count += 1

    def review(
        self,
        tokens: dict[str, int] | None,
        second: int,
        model: str = "review-model",
        effort: str = "high",
    ) -> None:
        directory = self.reviews / str(self.review_count)
        directory.mkdir()
        if tokens is None:
            self._set_time(directory, second)
        else:
            (directory / "metadata.json").write_text(
                json.dumps({"model": model, "reasoning_effort": effort})
            )
            trace = directory / "run.jsonl"
            write_jsonl(trace, [{"type": "turn.completed", "usage": tokens}])
            self._set_time(trace, second)
        self.review_count += 1

    def collect(
        self, review_budget: int | None, final_message: str = "Done."
    ) -> list[dict[str, object]]:
        # The runner saves the delivered tree before collecting phase accounting.
        output = self.root / "output"
        if output.exists():
            shutil.rmtree(output)
        directory_snapshot.write(
            directory_snapshot.read(self.workdir / "output"), output
        )
        write_jsonl(
            self.root / "trace.jsonl",
            [
                {"type": "thread.started", "thread_id": THREAD_ID},
                {
                    "type": "item.completed",
                    "item": {"type": "agent_message", "text": final_message},
                },
            ],
        )
        write_jsonl(
            self.agent_home / "sessions/2026/09/04" / f"rollout-test-{THREAD_ID}.jsonl",
            self.author_events,
        )
        return checkpoint_accounting.collect(
            self.root,
            review_budget,
            agent_home=self.agent_home,
        )


class CheckpointAccountingTest(unittest.TestCase):
    def test_ignores_long_running_tool_output_before_checkpoint(self) -> None:
        # An unfinished tool call has not saved a draft. It must not consume a
        # checkpoint or claim the following usage event.
        with tempfile.TemporaryDirectory() as temporary:
            run = AccountingFixture(Path(temporary))
            run.author_events.append(
                {
                    "type": "response_item",
                    "payload": {
                        "type": "custom_tool_call_output",
                        "output": (
                            "Script running with cell ID 35\n"
                            "Wall time 31.0 seconds\n"
                            "Output:\n"
                        ),
                    },
                }
            )
            run.checkpoint("initial", usage(100, 10), 10)
            run.author_events.append(token_event(usage(150, 15), 15))

            records = run.collect(review_budget=None)

            self.assertEqual([record["phase"] for record in records], ["initial"])

    def test_word_change_ignores_markdown_reflow(self) -> None:
        self.assertEqual(
            checkpoint_accounting._artifact_changes(
                {Path("main.md"): b"> one two\n> three\n"},
                {Path("main.md"): b"> one\n> two three\n"},
            )[0],
            0.0,
        )

    def test_word_change_is_bounded_for_reordering(self) -> None:
        self.assertEqual(
            checkpoint_accounting._artifact_changes(
                {Path("main.md"): b"one two three\n"},
                {Path("main.md"): b"three two one\n"},
            )[0],
            66.7,
        )

    def test_words_are_aggregated_per_file(self) -> None:
        self.assertEqual(
            checkpoint_accounting._artifact_changes(
                {
                    Path("a"): b"one two",
                    Path("b"): b"three four five",
                },
                {
                    Path("a"): b"one changed",
                    Path("b"): b"three four five",
                },
            ),
            (20.0, ["a"]),
        )

    def test_renames_do_not_cancel_word_changes(self) -> None:
        self.assertEqual(
            checkpoint_accounting._artifact_changes(
                {Path("old"): b"same words"},
                {Path("new"): b"same words"},
            ),
            (100.0, ["new", "old"]),
        )

    def test_empty_file_addition_is_reported(self) -> None:
        self.assertEqual(
            checkpoint_accounting._artifact_changes(
                {Path("main.md"): b"same"},
                {Path("main.md"): b"same", Path("empty-file"): b""},
            ),
            (0.0, ["empty-file"]),
        )

    def test_word_count_tolerates_invalid_utf8(self) -> None:
        # A stray undecodable byte must not invalidate an otherwise saved run.
        self.assertEqual(
            checkpoint_accounting._artifact_changes(
                {Path("main.md"): b"same words \xff\n"},
                {Path("main.md"): b"same words\n"},
            )[0],
            0.0,
        )

    def test_companion_only_mutation_after_final_checkpoint_is_rejected(self) -> None:
        # main.md stays fixed. Changing only the notes' line endings, or deleting
        # them, must still invalidate the final checkpoint's saved file set.
        with tempfile.TemporaryDirectory() as temporary:
            run = AccountingFixture(Path(temporary))
            run.checkpoint(
                "same",
                usage(100, 10),
                10,
                companions={Path("notes.md"): b"checkpointed notes\r\n"},
            )
            run.author_events.append(token_event(usage(150, 15), 15))
            for mutation in ("change", "delete"):
                with self.subTest(mutation=mutation):
                    output = run.workdir / "output"
                    shutil.rmtree(output)
                    directory_snapshot.write(
                        directory_snapshot.read(run.checkpoints / "0"), output
                    )
                    if mutation == "change":
                        (output / "notes.md").write_text("checkpointed notes\n")
                    else:
                        (output / "notes.md").unlink()
                    with self.assertRaisesRegex(
                        ValueError, "changed after the final checkpoint"
                    ):
                        run.collect(review_budget=None)

    def test_tracks_two_review_rounds(self) -> None:
        # Two reviewers finish in each round. Charge them to the next author
        # checkpoint, and charge final delivery to the last phase, not a new one.
        with tempfile.TemporaryDirectory() as temporary:
            run = AccountingFixture(Path(temporary))
            run.checkpoint("initial", usage(100, 10), 10)
            run.checkpoint("author", usage(250, 20), 20)
            # Reviews are assigned to phases and ordered by completion time.
            run.review(usage(50, 6), 25)
            run.review(usage(30, 4), 24)
            run.checkpoint("reviewed once", usage(450, 30), 30, json_output=True)
            run.review(usage(20, 2), 34)
            run.review(usage(10, 1), 35)
            run.checkpoint("reviewed twice", usage(600, 40), 40)
            run.author_events.append(token_event(usage(650, 45), 45))

            records = run.collect(review_budget=2)

            self.assertEqual(
                [record["phase"] for record in records],
                ["initial", "author", "review1", "review2"],
            )
            self.assertEqual(
                [record["outcome"] for record in records],
                ["continued", "continued", "continued", "converged"],
            )
            self.assertEqual(
                [record["changed_word_percent"] for record in records],
                [None, 100.0, 100.0, 50.0],
            )
            self.assertEqual(
                [record["author_tokens"] for record in records],
                [usage(100, 10), usage(150, 10), usage(200, 10), usage(200, 15)],
            )
            self.assertEqual(
                [record["reviewer_tokens"] for record in records],
                [
                    [],
                    [],
                    [usage(30, 4), usage(50, 6)],
                    [usage(20, 2), usage(10, 1)],
                ],
            )
            self.assertEqual(records[2]["wall_seconds"], 10.0)
            self.assertEqual(records[3]["wall_seconds"], 15.0)
            self.assertEqual(
                (run.root / "output/main.md").read_text(), "reviewed twice"
            )
            self.assertEqual(records[-1]["artifact"], "output")
            self.assertFalse((run.root / "output-review2").exists())
            self.assertEqual(
                json.loads((run.root / "checkpoints.json").read_text()), records
            )
            self.assertEqual(
                json.loads((run.root / "run.json").read_text()),
                {
                    "checkpoint_key": CHECKPOINT_KEY.hex(),
                    "reviewer_model": "review-model",
                    "reviewer_reasoning_effort": "high",
                },
            )

    def test_rejects_mixed_reviewer_settings(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            run = AccountingFixture(Path(temporary))
            run.checkpoint("initial", usage(100, 10), 10)
            run.checkpoint("author", usage(200, 20), 20)
            run.review(usage(30, 3), 24)
            run.review(usage(30, 3), 25, model="other-model")
            run.checkpoint("reviewed", usage(300, 30), 30)
            run.author_events.append(token_event(usage(350, 35), 35))

            with self.assertRaisesRegex(ValueError, "changed during the run"):
                run.collect(review_budget=1)

    def test_failed_review_is_recorded_before_completed_retry(self) -> None:
        # A reviewer can abort and be retried before the author checkpoints.
        # Keep both attempts; the completed retry validates checkpoint 2 as `review1`.
        with tempfile.TemporaryDirectory() as temporary:
            run = AccountingFixture(Path(temporary))
            run.checkpoint("initial", usage(100, 10), 10)
            run.checkpoint("author", usage(250, 20), 20)
            run.review(None, 22)
            retry_tokens = usage(50, 6)
            run.review(retry_tokens, 25)
            run.checkpoint("reviewed", usage(450, 30), 30)
            run.author_events.append(token_event(usage(500, 35), 35))

            records = run.collect(review_budget=1)

            self.assertEqual(records[2]["reviewer_tokens"], [{}, retry_tokens])

    def test_failed_review_does_not_complete_review_phase(self) -> None:
        # Checkpoint 2 is inferred to be `review1`. If the reviewer aborts and
        # the author still checkpoints, accepting it would mislabel unreviewed work.
        with tempfile.TemporaryDirectory() as temporary:
            run = AccountingFixture(Path(temporary))
            run.checkpoint("initial", usage(100, 10), 10)
            run.checkpoint("author", usage(250, 20), 20)
            run.review(None, 22)
            run.checkpoint("not reviewed", usage(450, 30), 30)
            run.author_events.append(token_event(usage(500, 35), 35))

            with self.assertRaisesRegex(ValueError, "reviewer count"):
                run.collect(review_budget=1)

    def test_requires_usage_after_the_final_checkpoint(self) -> None:
        # The last checkpoint excludes the author's final response. Without the
        # later usage event, the run would underreport the last phase's cost.
        with tempfile.TemporaryDirectory() as temporary:
            run = AccountingFixture(Path(temporary))
            run.checkpoint("initial", usage(100, 10), 10)

            with self.assertRaisesRegex(
                ValueError, "final response has no token usage"
            ):
                run.collect(review_budget=None)

    def test_outcomes_distinguish_budget_continuation_and_stop(self) -> None:
        # (checkpoint index, final index, review budget, final response) -> outcome.
        # Going beyond budget differs from stopping at it with an OutOfBudget notice.
        cases = (
            ((2, 3, 2, ""), "continued"),
            ((2, 3, 1, ""), "must-continue"),
            ((2, 2, 2, ""), "converged"),
            ((2, 2, 2, "> OutOfBudget: no rounds remain."), "budget-stop"),
            ((2, 2, 2, "The rule contains OutOfBudget: literally."), "budget-stop"),
            ((2, 2, 2, "The rule contains OutOfBudget literally."), "converged"),
        )
        for arguments, expected in cases:
            with self.subTest(arguments=arguments, expected=expected):
                self.assertEqual(checkpoint_accounting._outcome(*arguments), expected)

    def test_tokens_preserve_zero_and_require_every_field(self) -> None:
        tokens = usage(0, 0)
        self.assertEqual(checkpoint_accounting._tokens(tokens), tokens)
        del tokens["cache_write_input_tokens"]
        with self.assertRaises(KeyError):
            checkpoint_accounting._tokens(tokens)
