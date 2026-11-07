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

# pyre-strict

import argparse
import contextlib
import importlib.resources
import io
import json
import sys
import tempfile
import unittest
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from .. import benchmark_ab


def _write_fake_benchmark_output(
    arguments: Sequence[str],
    *,
    log_path: Path,
    value: float,
    run_incomplete: bool = False,
) -> None:
    json_path = Path(
        next(
            argument.removeprefix("--bm_json_verbose=")
            for argument in arguments
            if argument.startswith("--bm_json_verbose=")
        )
    )
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(
        "[RUN INCOMPLETE] Did not converge:\n" if run_incomplete else "",
        encoding="utf-8",
    )
    json_path.write_text(
        json.dumps([["fixture.cpp", "measured", value]]),
        encoding="utf-8",
    )


class CheckoutTrackingWorkspace:
    def __init__(self) -> None:
        self.current_revision = "start"

    def query_buck(self, query: str) -> list[str]:
        raise AssertionError("Buck query should not run")

    def run_buck(
        self,
        arguments: Sequence[str],
        *,
        log_path: Path,
    ) -> int:
        raise AssertionError("Buck should not run")

    def run_executable(
        self,
        executable: Path,
        arguments: Sequence[str],
        *,
        log_path: Path,
    ) -> int:
        raise AssertionError("benchmark executable should not run")

    def has_changes(self, *, include_untracked: bool) -> bool:
        return False

    def resolve_revision(self, revision: str) -> str:
        if revision == ".":
            return self.current_revision
        return f"{revision}_node"

    def checkout(self, revision: str) -> None:
        self.current_revision = revision


class RelativeRevisionWorkspace(CheckoutTrackingWorkspace):
    def __init__(self, build_target: str) -> None:
        super().__init__()
        self.build_target = build_target
        self.resolve_calls: list[tuple[str, str]] = []
        self.query_revisions: list[str] = []

    def resolve_revision(self, revision: str) -> str:
        self.resolve_calls.append((revision, self.current_revision))
        return super().resolve_revision(revision)

    def query_buck(self, query: str) -> list[str]:
        self.query_revisions.append(self.current_revision)
        return [self.build_target]


class BuildTrackingWorkspace(RelativeRevisionWorkspace):
    def __init__(self, build_target: str, returncodes: list[int]) -> None:
        super().__init__(build_target)
        self.returncodes = returncodes
        self.build_calls: list[tuple[str, list[str], Path]] = []
        self.run_calls: list[tuple[Path, list[str], Path]] = []

    def run_buck(
        self,
        arguments: Sequence[str],
        *,
        log_path: Path,
    ) -> int:
        self.build_calls.append((self.current_revision, list(arguments), log_path))
        returncode = self.returncodes.pop(0)
        log_path.parent.mkdir(parents=True, exist_ok=True)
        lines = [f"build output at {self.current_revision}"]
        if returncode == 0:
            source_dir = log_path.parent / ".fake_buck" / self.current_revision
            source_dir.mkdir(parents=True, exist_ok=True)
            for index, build_target in enumerate(arguments[3:], start=1):
                source = source_dir / f"benchmark_{index}"
                source.write_text(
                    f"{build_target} at {self.current_revision}\n",
                    encoding="utf-8",
                )
                source.chmod(0o755)
                lines.append(f"{build_target} {source}")
        log_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return returncode

    def run_executable(
        self,
        executable: Path,
        arguments: Sequence[str],
        *,
        log_path: Path,
    ) -> int:
        self.run_calls.append((executable, list(arguments), log_path))
        _write_fake_benchmark_output(
            arguments,
            log_path=log_path,
            value=1.0,
        )
        return 0


@dataclass
class FakeBenchmarkRunner:
    expected_executable: Path
    incomplete_runs: int = 0
    run_count: int = 0

    def run_executable(
        self,
        executable: Path,
        arguments: Sequence[str],
        *,
        log_path: Path,
    ) -> int:
        self.run_count += 1
        if (
            executable != self.expected_executable
            or list(arguments[:4])
            != [
                "--benchmark",
                "--bm_mode=adaptive",
                "--bm_target_percentile=33.3",
                "--bm_max_secs=30",
            ]
            or arguments[-1] != "--bm_verbose"
        ):
            return 2
        # Distinguish runs so tests can verify which attempt supplied the result.
        _write_fake_benchmark_output(
            arguments,
            log_path=log_path,
            value=float(self.run_count),
            run_incomplete=self.run_count <= self.incomplete_runs,
        )
        return 0


class BenchmarkAbTest(unittest.TestCase):
    maxDiff: int | None = None
    target: benchmark_ab.BenchmarkTarget

    def setUp(self) -> None:
        self.target = benchmark_ab.BenchmarkTarget(
            build_target="fbcode//folly/test:bench",
            artifact_name="bench",
        )

    @staticmethod
    def _golden_text(name: str) -> str:
        return (
            importlib.resources.files(__package__)
            .joinpath("testdata")
            .joinpath(name)
            .read_text(encoding="utf-8")
        )

    @staticmethod
    def _benchmark(name: str) -> benchmark_ab.BenchmarkId:
        return benchmark_ab.BenchmarkId(file="fixture.cpp", name=name)

    def _row(
        self,
        name: str,
        pairs: Sequence[tuple[float, float]],
    ) -> benchmark_ab.ComparisonRow:
        return benchmark_ab.ComparisonRow(
            build_target=self.target.build_target,
            benchmark=self._benchmark(name),
            observations=tuple(
                benchmark_ab.Observation(round_number, before, after)
                for round_number, (before, after) in enumerate(pairs, start=1)
            ),
        )

    def _spread_rows(self) -> tuple[benchmark_ab.ComparisonRow, ...]:
        stable_after = (100.0, 101.0, 100.0, 101.0, 100.0)
        return tuple(
            self._row(name, tuple(zip(before, after, strict=True)))
            for name, before, after in (
                (
                    "persistent_spread",
                    (100.0, 100.0, 100.0, 140.0, 200.0),
                    stable_after,
                ),
                (
                    # Effect estimation uses every timing (Δ=10ns); trimming
                    # both sides would yield 0.
                    "effect_and_spread",
                    (10.0, 10.0, 10.0, 20.0, 20.0),
                    (10.0, 10.0, 20.0, 30.0, 30.0),
                ),
                (
                    "spread_threshold_boundary",
                    (100.0, 100.0, 100.0, 105.0, 105.0),
                    stable_after,
                ),
                (
                    "single_slow_timing",
                    (100.0, 100.0, 100.0, 100.0, 200.0),
                    stable_after,
                ),
                *(
                    (
                        f"stable_{index}",
                        (100.0, 100.0, 100.0, 101.0, 101.0),
                        stable_after,
                    )
                    for index in range(4, 10)
                ),
            )
        )

    def _analyze_spread_report(
        self,
        args: argparse.Namespace,
    ) -> benchmark_ab.ComparisonReport:
        rows = self._spread_rows()
        # Mark the in-memory results usable without writing attempt files.
        attempt = benchmark_ab.AttemptArtifact(
            json_path=Path(__file__),
            returncode=0,
            run_incomplete=False,
        )
        artifacts: dict[tuple[int, str, str], benchmark_ab.RunArtifact] = {}
        for round_index in range(len(rows[0].observations)):
            for side in (benchmark_ab.BEFORE_SIDE, benchmark_ab.AFTER_SIDE):
                artifacts[(round_index + 1, side, self.target.build_target)] = (
                    benchmark_ab.RunArtifact(
                        round_number=round_index + 1,
                        side=side,
                        target=self.target,
                        attempts=(attempt,),
                        results={
                            row.benchmark: getattr(row.observations[round_index], side)
                            for row in rows
                        },
                    )
                )

        return benchmark_ab.analyze_report(artifacts, args, (self.target,))

    def _write_attempt_artifact(
        self,
        out: Path,
        round_number: int,
        side: str,
        attempt: int,
        results: dict[benchmark_ab.BenchmarkId, float] | None,
    ) -> None:
        paths = benchmark_ab.attempt_paths(
            out / f"round_{round_number}" / f"{side}_{self.target.artifact_name}",
            self.target,
            attempt,
        )
        paths.directory.mkdir(parents=True)
        run_incomplete = results is None
        paths.json.write_text(
            json.dumps(
                []
                if results is None
                else [
                    [benchmark.file, benchmark.name, value]
                    for benchmark, value in results.items()
                ]
            )
            + "\n",
            encoding="utf-8",
        )
        paths.log.write_text(
            "[RUN INCOMPLETE] Did not converge:\n" if run_incomplete else "",
            encoding="utf-8",
        )
        benchmark_ab.write_attempt_completion(
            paths.completion,
            returncode=0,
            run_incomplete=run_incomplete,
        )

    def _materialize_artifacts(self, out: Path) -> None:
        benchmark_to_round_pairs = {
            self._benchmark("below_threshold"): (
                (10.0, 10.1),
                (10.0, 10.2),
                (10.0, 10.3),
            ),
            # Round 1 is absent, but the remaining 3x3 differences all agree.
            benchmark_ab.BenchmarkId(file="loss.cpp", name="same_name"): (
                (1.0, 3.0),
                (1.5, 3.0),
                (2.0, 2.2),
            ),
            benchmark_ab.BenchmarkId(file="win.cpp", name="same_name"): (
                (20.0, 19.5),
                (10.0, 8.0),
                (15.0, 13.0),
            ),
            self._benchmark("low_loss"): (
                (20.0, 20.8),
                (10.0, 10.8),
                (15.0, 15.8),
            ),
            self._benchmark("low_win"): (
                (20.0, 19.2),
                (10.0, 9.2),
                (15.0, 14.2),
            ),
        }
        rounds: list[
            dict[
                str,
                tuple[dict[benchmark_ab.BenchmarkId, float] | None, ...],
            ]
        ] = [
            {
                "before": (None,),
                "after": ({self._benchmark("unpaired_due_to_failed_run"): 1.0},),
            }
        ]
        for round_index in range(3):
            before = {self._benchmark("only_before"): 1.0}
            after = {self._benchmark("only_after"): 2.0}
            for benchmark, round_pairs in benchmark_to_round_pairs.items():
                before[benchmark], after[benchmark] = round_pairs[round_index]
            rounds.append(
                {
                    "before": (None, before) if round_index == 0 else (before,),
                    "after": (after,),
                }
            )
        for round_number, revisions in enumerate(rounds, start=1):
            for side, attempts in revisions.items():
                for attempt, results in enumerate(attempts, start=1):
                    self._write_attempt_artifact(
                        out,
                        round_number,
                        side,
                        attempt,
                        results,
                    )

    def _manifest(self) -> benchmark_ab.MeasurementManifest:
        return benchmark_ab.MeasurementManifest(
            before="1" * 40,
            after="2" * 40,
            mode="@mode/dev",
            bm_max_secs=17,
            bm_target_percentile=33.3,
            target_patterns=("//folly/test/...",),
            targets=(self.target,),
        )

    def test_reanalyze_matches_golden_outputs(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp)
            self._materialize_artifacts(out)
            benchmark_ab.write_manifest(out, self._manifest())
            stdout = io.StringIO()
            stderr = io.StringIO()
            with (
                contextlib.redirect_stdout(stdout),
                contextlib.redirect_stderr(stderr),
            ):
                self.assertEqual(
                    1,
                    benchmark_ab.main(
                        [
                            "reanalyze",
                            "--out",
                            str(out),
                        ]
                    ),
                )

            self.assertEqual("\n", stderr.getvalue())
            self.assertEqual(
                self._golden_text("benchmark_ab_expected.stdout"),
                stdout.getvalue().replace(str(out), "<OUT>"),
            )
            self.assertEqual(
                self._golden_text("benchmark_ab_expected.md"),
                (out / "comparison.md")
                .read_text(encoding="utf-8")
                .replace(str(out), "<OUT>"),
            )
            self.assertEqual(
                self._golden_text("benchmark_ab_expected.tsv"),
                (out / "comparison.tsv").read_text(encoding="utf-8"),
            )
            _, separator, comparison = stdout.getvalue().partition("\n\n")
            self.assertEqual("\n\n", separator)
            self.assertEqual(
                (out / "comparison.txt").read_text(encoding="utf-8"),
                comparison,
            )

            # Measurement artifacts retain results below report thresholds, so
            # reanalysis can reveal smaller changes without rerunning benchmarks.
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                self.assertEqual(
                    1,
                    benchmark_ab.main(
                        [
                            "reanalyze",
                            "--out",
                            str(out),
                            "--lo-ns=0.05",
                            "--lo-pct=0.5",
                        ]
                    ),
                )
            self.assertIn("below_threshold", stdout.getvalue())

    def test_measurement_restores_starting_revision_after_discovery_failure(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temp:
            args = benchmark_ab.parse_args(
                [
                    "measure",
                    "--before=before",
                    "--after=after",
                    "--out",
                    str(Path(temp) / "out"),
                    "not-a-buck-selector",
                ]
            )
            workspace = CheckoutTrackingWorkspace()

            with self.assertRaisesRegex(SystemExit, "must be a Buck selector"):
                benchmark_ab.run_measurement(
                    args,
                    workspace,
                )

            self.assertEqual("start", workspace.current_revision)

    def test_measurement_resolves_relative_revisions_before_checkout(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            args = benchmark_ab.parse_args(
                [
                    "measure",
                    "--before=.^",
                    "--after=.",
                    "--rounds=0",
                    "--out",
                    str(Path(temp) / "out"),
                    "//folly/test/...",
                ]
            )
            workspace = RelativeRevisionWorkspace(self.target.build_target)

            self.assertEqual(0, benchmark_ab.run_measurement(args, workspace))
            # `--after=.` must name the starting revision, not the "before"
            # revision checked out for discovery. Resolve both args first.
            self.assertEqual(2, workspace.resolve_calls.count((".", "start")))
            # Target discovery itself runs at the resolved "before" revision.
            self.assertEqual([".^_node"], workspace.query_revisions)
            self.assertEqual("start", workspace.current_revision)

    def test_measurement_rejects_nonempty_output_before_running(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp)
            (out / "stale").write_text("stale\n", encoding="utf-8")
            args = benchmark_ab.parse_args(
                [
                    "measure",
                    "--before=before",
                    "--after=after",
                    "--out",
                    str(out),
                    "//folly/test/...",
                ]
            )

            with self.assertRaises(SystemExit) as raised:
                benchmark_ab.run_measurement(
                    args,
                    CheckoutTrackingWorkspace(),
                )
            self.assertEqual(
                f"measure output directory is not empty: {out}",
                str(raised.exception),
            )

    def test_measurement_stops_after_preflight_build_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / "out"
            args = benchmark_ab.parse_args(
                [
                    "measure",
                    "--before=before",
                    "--after=after",
                    "--out",
                    str(out),
                    "//folly/test/...",
                ]
            )
            workspace = BuildTrackingWorkspace(
                self.target.build_target,
                returncodes=[0, 3],
            )

            with self.assertRaises(SystemExit) as raised:
                benchmark_ab.run_measurement(args, workspace)

            self.assertEqual(
                f"after revision build failed with exit 3; full output: "
                f"{out / 'build_after.log'}",
                str(raised.exception),
            )

            expected_arguments = [
                "build",
                "@mode/opt",
                "--show-full-output",
                self.target.build_target,
            ]
            self.assertEqual(
                [
                    ("before_node", expected_arguments, out / "build_before.log"),
                    ("after_node", expected_arguments, out / "build_after.log"),
                ],
                workspace.build_calls,
            )
            self.assertEqual("start", workspace.current_revision)
            self.assertIn(
                "build output at after_node",
                (out / "build_after.log").read_text(encoding="utf-8"),
            )
            self.assertFalse((out / "round_1").exists())

    def test_measurement_runs_saved_executables(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / "out"
            args = benchmark_ab.parse_args(
                [
                    "measure",
                    "--before=before",
                    "--after=after",
                    "--rounds=1",
                    "--out",
                    str(out),
                    "//folly/test/...",
                ]
            )
            workspace = BuildTrackingWorkspace(
                self.target.build_target,
                returncodes=[0, 0],
            )

            with (
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                self.assertEqual(0, benchmark_ab.run_measurement(args, workspace))

            artifact_name = benchmark_ab.make_benchmark_targets(
                (self.target.build_target,)
            )[0].artifact_name
            self.assertEqual(
                [
                    out / "binaries" / "before" / artifact_name,
                    out / "binaries" / "after" / artifact_name,
                ],
                [call[0] for call in workspace.run_calls],
            )
            self.assertEqual("start", workspace.current_revision)

    def test_validate_args_rejects_dangerous_measurement_values(self) -> None:
        for options, error in (
            (("--bm-max-secs=0",), "--bm-max-secs must be at least 1"),
            (
                ("--max-run-attempts=0",),
                "--max-run-attempts must be at least 1",
            ),
            (("--hi-ns=1", "--lo-ns=2"), "--lo-ns must not exceed --hi-ns"),
            (
                ("--hi-pct=10", "--lo-pct=20"),
                "--lo-pct must not exceed --hi-pct",
            ),
            (
                ("--broad-spread-pct=101",),
                "--broad-spread-pct must be greater than 0 and at most 100",
            ),
            (
                ("--broad-spread-pct=0",),
                "--broad-spread-pct must be greater than 0 and at most 100",
            ),
            (
                ("--spread-ignore-slowest=-1",),
                "--spread-ignore-slowest must be nonnegative",
            ),
        ):
            with self.subTest(options=options):
                args = benchmark_ab.parse_args(
                    [
                        "measure",
                        "--before=before",
                        "--after=after",
                        *options,
                        "//folly/test/...",
                    ]
                )
                with self.assertRaises(SystemExit) as raised:
                    benchmark_ab.validate_args(args)
                self.assertEqual(error, str(raised.exception))

    def test_report_thresholds_must_be_nonnegative_and_finite(self) -> None:
        for option in (
            "--hi-ns",
            "--hi-pct",
            "--lo-ns",
            "--lo-pct",
            "--broad-spread-pct",
        ):
            for value in ("-1", "nan"):
                with (
                    self.subTest(option=option, value=value),
                    contextlib.redirect_stderr(io.StringIO()),
                    self.assertRaises(SystemExit),
                ):
                    benchmark_ab.parse_args(
                        ["reanalyze", "--out=out", f"{option}={value}"]
                    )

    def test_zero_thresholds_still_separate_wins_and_regressions(self) -> None:
        rows = (
            self._row("faster", ((10.0, 9.0),)),
            self._row("slower", ((10.0, 11.0),)),
            self._row("displayed_zero", ((10.0, 10.04),)),
        )
        threshold = benchmark_ab.Threshold(ns=0.0, pct=0.0)

        # With zero thresholds, direction at report precision is the only guard
        # against reporting slowdowns as wins and speedups as regressions.
        wins = benchmark_ab.bucket_rows(rows, direction=-1, threshold=threshold)
        regressions = benchmark_ab.bucket_rows(rows, direction=1, threshold=threshold)

        self.assertEqual(
            ["faster"],
            [row.benchmark.name for row in wins],
        )
        self.assertEqual(
            ["slower"],
            [row.benchmark.name for row in regressions],
        )

    def test_classification_matches_report_precision(self) -> None:
        summary = benchmark_ab.ComparisonSummary(
            before=9.0,
            after=9.96,
            delta=0.96,
        )

        self.assertEqual(
            "9.0+1.0ns (+10.7%)",
            benchmark_ab.summary_text(summary, pct_min_before_ns=2.0),
        )
        self.assertTrue(
            benchmark_ab.meets_threshold(
                summary,
                benchmark_ab.Threshold(ns=1.0, pct=10.0),
                direction=1,
            ),
        )

        args = benchmark_ab.parse_args(["reanalyze", "--out=out", "--hi-ns=0.96"])
        report = benchmark_ab.analyze_report({}, args, (self.target,))
        for rendered in (
            benchmark_ab.render_markdown(report, args, self._manifest()),
            benchmark_ab.render_terminal(report, args, self._manifest()),
        ):
            self.assertIn(">=1.0ns", rendered)
            self.assertIn(
                "Thresholds were rounded to one decimal place when deciding "
                "results and alarms.",
                rendered,
            )

    def test_target_artifact_names_are_unique_and_bounded(self) -> None:
        self.assertEqual(
            ["001_fbcode_foo_a_bench", "002_fbcode_foo_a_bench"],
            [
                target.artifact_name
                for target in benchmark_ab.make_benchmark_targets(
                    ("fbcode//foo/a:bench", "fbcode//foo_a:bench")
                )
            ],
        )

        long_name = benchmark_ab.make_benchmark_targets(
            (f"//{'a' * 250}:important_benchmark",)
        )[0].artifact_name
        self.assertEqual(200, len(long_name))
        self.assertTrue(long_name.endswith("important_benchmark"))

    def test_save_built_executables_handles_duplicate_basenames(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            targets = benchmark_ab.make_benchmark_targets(
                ("fbcode//foo/a:bench", "fbcode//foo_a:bench")
            )
            sources = []
            for index in range(2):
                source = root / f"source_{index}" / "bench"
                source.parent.mkdir()
                source.write_text(f"binary {index}\n", encoding="utf-8")
                source.chmod(0o755)
                sources.append(source)
            log_path = root / "build_before.log"
            log_path.write_text(
                "\n".join(
                    f"{target.build_target} {source}"
                    for target, source in zip(targets, sources, strict=True)
                )
                + "\n",
                encoding="utf-8",
            )

            saved = benchmark_ab.save_built_executables(
                side=benchmark_ab.BEFORE_SIDE,
                log_path=log_path,
                out_dir=root,
                targets=targets,
            )

            self.assertEqual(
                [target.artifact_name for target in targets],
                [saved[target].name for target in targets],
            )
            self.assertEqual(
                ["binary 0\n", "binary 1\n"],
                [saved[target].read_text(encoding="utf-8") for target in targets],
            )
            self.assertTrue(all(path.stat().st_mode & 0o111 for path in saved.values()))

    def test_run_one_benchmark_retries_until_result_is_usable(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            args = benchmark_ab.parse_args(
                [
                    "measure",
                    "--before=before",
                    "--after=after",
                    "--max-run-attempts=2",
                    "--out",
                    str(Path(temp) / "out"),
                    "//folly/test/...",
                ]
            )
            executable = Path(temp) / "bench"
            runner = FakeBenchmarkRunner(
                executable,
                incomplete_runs=1,
            )

            artifact = benchmark_ab.run_one_benchmark(
                args,
                round_number=1,
                round_count=1,
                side=benchmark_ab.BEFORE_SIDE,
                target=self.target,
                executable=executable,
                runner=runner,
            )

            self.assertEqual(2, runner.run_count)
            self.assertEqual(
                [True, False],
                [attempt.run_incomplete for attempt in artifact.attempts],
            )
            self.assertEqual(
                {self._benchmark("measured"): 2.0},
                artifact.results,
            )

    def test_workspace_combines_executable_stderr_with_log(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            log_path = root / "benchmark.log"
            workspace = benchmark_ab.SaplingWorkspace(
                root=root,
                buck_executable=Path(sys.executable),
            )

            # Folly writes the incomplete-run marker to stderr. If it escapes
            # the log, run_one_benchmark() can accept an unusable result.
            returncode = workspace.run_executable(
                Path(sys.executable),
                [
                    "-c",
                    "import sys; print('[RUN INCOMPLETE]', file=sys.stderr)",
                ],
                log_path=log_path,
            )

            self.assertEqual(0, returncode)
            self.assertTrue(benchmark_ab.log_has_incomplete_run(log_path))

    def test_workspace_queries_buck_from_discovered_cell(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            cell = root / "cell"
            cell.mkdir()
            tool_dir = root / "tool"
            tool_dir.mkdir()
            buck = root / "buck"
            # The fake maps the caller's directory to a distinct cell, then
            # accepts a query only from that cell.
            buck.write_text(
                f"""#!{sys.executable}
import pathlib
import sys

if sys.argv[1:] == ["root", "--kind", "cell", "--dir", {str(tool_dir)!r}]:
    print({str(cell)!r})
elif sys.argv[1] == "uquery" and pathlib.Path.cwd() == pathlib.Path({str(cell)!r}):
    print("other//folly/test:bench")
else:
    sys.exit(2)
""",
                encoding="utf-8",
            )
            buck.chmod(0o755)

            self.assertEqual(
                ["other//folly/test:bench"],
                benchmark_ab.SaplingWorkspace.discover(
                    buck,
                    directory=tool_dir,
                ).query_buck("//folly/test:bench"),
            )

    def test_results_distinguish_same_name_in_different_files(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "results.json"
            path.write_text(
                json.dumps(
                    [
                        ["one.cpp", "same_name", 1.0],
                        [
                            "two.cpp",
                            "same_name",
                            2.0,
                            {"items": {"value": 2, "type": 2}},
                        ],
                    ]
                ),
                encoding="utf-8",
            )

            self.assertEqual(
                {
                    benchmark_ab.BenchmarkId(file="one.cpp", name="same_name"): 1.0,
                    benchmark_ab.BenchmarkId(file="two.cpp", name="same_name"): 2.0,
                },
                benchmark_ab.load_results(path),
            )

    def test_results_reject_google_benchmark_json(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "results.json"
            path.write_text(
                '{"benchmarks": [{"time_unit": "ns"}]}',
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "benchmark JSON"):
                benchmark_ab.load_results(path)

    def test_reanalyze_does_not_use_incomplete_attempt(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            base_dir = Path(temp)
            paths = benchmark_ab.attempt_paths(base_dir, self.target, 1)
            paths.directory.mkdir()
            # An interruption can leave valid output without recording completion.
            paths.json.write_text(
                '[["fixture.cpp", "measured", 1.0]]', encoding="utf-8"
            )
            paths.log.write_text("", encoding="utf-8")

            artifact = benchmark_ab.load_run_artifact(
                round_number=1,
                side=benchmark_ab.BEFORE_SIDE,
                target=self.target,
                base_dir=base_dir,
            )

            self.assertFalse(artifact.attempts[0].completed)
            self.assertEqual({}, artifact.results)

    def test_benchmark_text_adds_file_only_for_same_target_collision(self) -> None:
        report = benchmark_ab.ComparisonReport(
            needs_attention=(),
            unpaired_rows={},
            benchmark_names_with_multiple_files=frozenset(
                {(self.target.build_target, "duplicate")}
            ),
            rows=(),
            spread=benchmark_ab.SpreadAnalysis(
                row_to_eligible_spreads={},
                round_count=0,
                full_series_count=0,
                broad_spread_count=0,
                ignore_n_slowest=0,
                median_pct=None,
                calibration=None,
            ),
            sections=(),
            unclassified_high_spread_rows=(),
        )

        for build_target, benchmark, expected in (
            (self.target.build_target, self._benchmark("unique"), "unique"),
            (
                self.target.build_target,
                self._benchmark("duplicate"),
                "duplicate (fixture.cpp)",
            ),
            ("fbcode//other:bench", self._benchmark("duplicate"), "duplicate"),
        ):
            with self.subTest(build_target=build_target, benchmark=benchmark):
                self.assertEqual(
                    expected,
                    benchmark_ab.benchmark_text(report, build_target, benchmark),
                )

    def test_reanalyze_requires_a_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp)
            with self.assertRaises(SystemExit) as raised:
                benchmark_ab.main(["reanalyze", "--out", str(out)])

            self.assertEqual(
                f"measurement manifest does not exist: {out / benchmark_ab.MANIFEST_FILENAME}",
                str(raised.exception),
            )

    def test_manifest_rejects_target_collisions_and_escape(self) -> None:
        cases = (
            (
                (
                    benchmark_ab.BenchmarkTarget(
                        build_target="target", artifact_name="one"
                    ),
                    benchmark_ab.BenchmarkTarget(
                        build_target="target", artifact_name="two"
                    ),
                ),
                "build targets must be unique",
            ),
            (
                (
                    benchmark_ab.BenchmarkTarget(
                        build_target="one", artifact_name="same"
                    ),
                    benchmark_ab.BenchmarkTarget(
                        build_target="two", artifact_name="same"
                    ),
                ),
                "target artifact names must be unique",
            ),
            (
                (
                    benchmark_ab.BenchmarkTarget(
                        build_target="target", artifact_name="../escape"
                    ),
                ),
                "single path component",
            ),
        )
        for targets, error in cases:
            with self.subTest(error=error), self.assertRaisesRegex(ValueError, error):
                benchmark_ab.MeasurementManifest(
                    before="before",
                    after="after",
                    mode="@mode/opt",
                    bm_max_secs=30,
                    bm_target_percentile=33.3,
                    target_patterns=("//folly/test/...",),
                    targets=targets,
                )

    def test_load_manifest_rejects_malformed_data(self) -> None:
        valid_data: dict[str, object] = {
            "before": "before",
            "after": "after",
            "mode": "@mode/opt",
            "bm_max_secs": 30,
            "bm_target_percentile": 33.3,
            "target_patterns": ["//folly/test/..."],
            "targets": [{"build_target": "target", "artifact_name": "target"}],
        }
        cases = (
            ([], "top level must be an object"),
            (
                {
                    **valid_data,
                    "targets": [{"build_target": 1, "artifact_name": "target"}],
                },
                "build_target and artifact_name must be strings",
            ),
        )
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / benchmark_ab.MANIFEST_FILENAME
            for data, error in cases:
                with self.subTest(error=error):
                    path.write_text(
                        json.dumps(data),
                        encoding="utf-8",
                    )
                    with self.assertRaisesRegex(SystemExit, error):
                        benchmark_ab.load_manifest(path.parent)

    def test_missing_attempts_need_attention(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp)
            artifact = benchmark_ab.load_run_artifact(
                round_number=1,
                side=benchmark_ab.BEFORE_SIDE,
                target=self.target,
                base_dir=out / "missing",
            )

            attention = benchmark_ab.needs_attention_for_run(
                artifact,
                out_dir=out,
            )
            self.assertIsNotNone(attention)
            self.assertEqual(
                "No attempts found for this benchmark run",
                attention.reason,
            )

    def test_empty_results_need_attention(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp)
            self._write_attempt_artifact(
                out,
                round_number=1,
                side=benchmark_ab.BEFORE_SIDE,
                attempt=1,
                results={},
            )
            attention = benchmark_ab.needs_attention_for_run(
                benchmark_ab.load_run_artifact(
                    round_number=1,
                    side=benchmark_ab.BEFORE_SIDE,
                    target=self.target,
                    base_dir=out / "round_1" / "before_bench",
                ),
                out_dir=out,
            )
            self.assertIsNotNone(attention)
            self.assertEqual("Benchmark run produced no results", attention.reason)

    def test_summary_uses_all_cross_side_differences(self) -> None:
        summary = benchmark_ab.comparison_summary(
            (
                benchmark_ab.Observation(1, 10.0, 1010.0),
                benchmark_ab.Observation(2, 100.0, 150.0),
                benchmark_ab.Observation(3, 1000.0, 1200.0),
            )
        )

        self.assertEqual(100.0, summary.before)
        self.assertEqual(1010.0, summary.after)
        # The marginal medians differ by 910ns, while the Hodges-Lehmann
        # estimate reflects the typical difference across all combinations.
        self.assertEqual(200.0, summary.delta)
        self.assertEqual(200.0, summary.pct)

    def test_spread_eligibility_is_per_side(self) -> None:
        # Exclusion moves the before median below the relative-reporting floor,
        # but must not remove its full series from calibration or the broad alarm.
        row = self._row(
            "tiny_before",
            (
                (1.0, 10.0),
                (1.0, 10.0),
                (1.0, 10.0),
                (4.0, 10.0),
                (4.0, 10.0),
                (100.0, 10.0),
            ),
        )
        spread = benchmark_ab.analyze_spread(
            (row,),
            round_count=6,
            broad_threshold=benchmark_ab.Threshold(ns=1.0, pct=10.0),
            high_spread_minimum=benchmark_ab.Threshold(ns=0.5, pct=5.0),
            ignore_n_slowest=1,
        )
        report = benchmark_ab.ComparisonReport(
            needs_attention=(),
            unpaired_rows={},
            benchmark_names_with_multiple_files=frozenset(),
            rows=(row,),
            spread=spread,
            sections=(),
            unclassified_high_spread_rows=(),
        )

        self.assertEqual(
            (benchmark_ab.AFTER_SIDE,),
            tuple(item.side for item in spread.eligible_spreads_for(row)),
        )
        self.assertEqual(2, spread.full_series_count)
        self.assertEqual(1, spread.broad_spread_count)
        tsv = benchmark_ab.tsv_comparison_row(
            report,
            row,
            classification="below-threshold",
            section=None,
        )
        self.assertNotIn("before_range_ns", tsv)
        self.assertEqual("0.0", tsv["after_range_ns"])
        self.assertEqual("1", tsv["ignore_n_slowest"])

    def test_spread_requires_two_rounds(self) -> None:
        row = self._row("single_round", ((100.0, 100.0),))
        spread = benchmark_ab.analyze_spread(
            (row,),
            round_count=1,
            broad_threshold=benchmark_ab.Threshold(ns=1.0, pct=10.0),
            high_spread_minimum=benchmark_ab.Threshold(ns=0.5, pct=5.0),
            ignore_n_slowest=0,
        )

        self.assertEqual(0, spread.full_series_count)
        self.assertIsNone(spread.median_pct)

    def test_spread_requires_two_timings_after_exclusions(self) -> None:
        row = self._row("too_few_retained", ((100.0, 100.0), (101.0, 101.0)))
        spread = benchmark_ab.analyze_spread(
            (row,),
            round_count=2,
            broad_threshold=benchmark_ab.Threshold(ns=1.0, pct=10.0),
            high_spread_minimum=benchmark_ab.Threshold(ns=0.5, pct=5.0),
            ignore_n_slowest=1,
        )

        self.assertEqual(2, spread.full_series_count)
        self.assertIsNone(spread.median_pct)
        self.assertEqual(
            "Not calculated: need at least 2 timings after exclusions (1 available)",
            benchmark_ab.spread_summary_lines(spread)[-1].strip(),
        )

    def test_spread_calibrates_outliers(self) -> None:
        # Ten benchmarks measured in every round contribute 20 before/after
        # series.
        rows = self._spread_rows()
        rows_by_name = {row.benchmark.name: row for row in rows}

        hi_threshold = benchmark_ab.Threshold(ns=1.0, pct=10.0)
        high_spread_minimum = benchmark_ab.Threshold(ns=0.5, pct=4.96)
        too_small = benchmark_ab.analyze_spread(
            rows[:-1],
            round_count=5,
            broad_threshold=hi_threshold,
            high_spread_minimum=high_spread_minimum,
            ignore_n_slowest=1,
        )
        calibrated = benchmark_ab.analyze_spread(
            rows,
            round_count=5,
            broad_threshold=hi_threshold,
            high_spread_minimum=high_spread_minimum,
            ignore_n_slowest=1,
        )

        self.assertIsNone(too_small.calibration)
        self.assertEqual(
            (), too_small.high_spreads_for(rows_by_name["persistent_spread"])
        )
        self.assertEqual(20, calibrated.full_series_count)
        self.assertEqual(4, calibrated.broad_spread_count)
        # All timings, including isolated slow ones, set the Tukey cutoff.
        self.assertEqual(
            benchmark_ab.SpreadCalibration(
                outlier_cutoff_pct=5.0,
                high_spread_threshold=benchmark_ab.Threshold(ns=0.5, pct=5.0),
            ),
            calibrated.calibration,
        )
        self.assertEqual(
            (benchmark_ab.BEFORE_SIDE,),
            tuple(
                spread.side
                for spread in calibrated.high_spreads_for(
                    rows_by_name["persistent_spread"]
                )
            ),
        )
        # 5.0% does not exceed the displayed >5.0% cutoff.
        self.assertEqual(
            (),
            calibrated.high_spreads_for(rows_by_name["spread_threshold_boundary"]),
        )
        # A single slow timing is omitted from the row flag but remains visible
        # to the benchmark-wide alarm.
        self.assertEqual(
            (), calibrated.high_spreads_for(rows_by_name["single_slow_timing"])
        )

    def test_report_places_and_renders_high_spread_rows_once(self) -> None:
        args = benchmark_ab.parse_args(["reanalyze", "--out=out"])
        report = self._analyze_spread_report(args)
        placed_names = [
            row.benchmark.name for section in report.sections for row in section.rows
        ] + [row.benchmark.name for row in report.unclassified_high_spread_rows]
        self.assertEqual(1, placed_names.count("persistent_spread"))
        self.assertEqual(1, placed_names.count("effect_and_spread"))
        self.assertEqual(
            ["persistent_spread"],
            [row.benchmark.name for row in report.unclassified_high_spread_rows],
        )

        terminal = benchmark_ab.render_terminal(report, args, self._manifest())
        for rendered in (
            terminal,
            benchmark_ab.render_markdown(report, args, self._manifest()),
        ):
            self.assertEqual(3, rendered.count("High spread ("))
            self.assertIn(
                "High spread (before; excluding 1 slowest): "
                "100.0-140.0ns (40.0% of median)",
                rendered,
            )
            self.assertIn(
                "High run-to-run spread without a reportable change",
                rendered,
            )
            self.assertIn(
                "High spread threshold: >5.0% (Tukey outlier cutoff) and >=0.5ns",
                rendered,
            )
            self.assertLess(
                rendered.index("High run-to-run spread without a reportable change"),
                rendered.index("High-priority regressions"),
            )
        self.assertLessEqual(max(map(len, terminal.splitlines())), 80)

        benchmark_to_tsv = {
            row["benchmark"]: row
            for row in benchmark_ab.tsv_rows(report, out_dir=Path("out"))
        }
        self.assertEqual(
            [("high-spread", "40.0"), ("loss-hi-pri", "100.0")],
            [
                (
                    benchmark_to_tsv[name]["class"],
                    benchmark_to_tsv[name]["before_range_pct_of_median"],
                )
                for name in ("persistent_spread", "effect_and_spread")
            ],
        )

    def test_broad_spread_warning_uses_configured_share(self) -> None:
        args = benchmark_ab.parse_args(
            ["reanalyze", "--out=out", "--broad-spread-pct=20"]
        )
        report = self._analyze_spread_report(args)
        for rendered in (
            benchmark_ab.render_terminal(report, args, self._manifest()),
            benchmark_ab.render_markdown(report, args, self._manifest()),
        ):
            self.assertIn(
                "High run-to-run spread across the benchmark set",
                rendered,
            )
            self.assertIn("4/20 before/after series (20.0%)", rendered)

        quiet_args = benchmark_ab.parse_args(
            ["reanalyze", "--out=out", "--broad-spread-pct=20.1"]
        )
        for rendered in (
            benchmark_ab.render_terminal(report, quiet_args, self._manifest()),
            benchmark_ab.render_markdown(report, quiet_args, self._manifest()),
        ):
            self.assertNotIn(
                "High run-to-run spread across the benchmark set", rendered
            )

    def test_percentage_floors_sub_picosecond_timings(self) -> None:
        # Adaptive baseline subtraction can produce zero; sub-picosecond
        # differences should remain noise rather than create an infinite ratio.
        self.assertEqual(0.0, benchmark_ab.Observation(1, 0.0, 0.0).pct)
        self.assertEqual(0.0, benchmark_ab.Observation(1, 0.0, 0.0005).pct)
        self.assertEqual(99_900.0, benchmark_ab.Observation(1, 0.0, 1.0).pct)
        self.assertEqual(
            0.0,
            benchmark_ab.comparison_summary(
                (benchmark_ab.Observation(1, 0.0, 0.0005),)
            ).pct,
        )

    def test_direction_agreement_ignores_sub_display_precision(self) -> None:
        almost_tied = self._row("almost_tied", ((10.0, 9.96), (10.0, 11.0)))
        self.assertEqual("", benchmark_ab.direction_agreement(almost_tied).text)

    def test_zero_estimate_has_no_direction_agreement(self) -> None:
        row = self._row(
            "mixed_directions",
            tuple((10.0, after) for after in (8.0, 8.0, 8.0, 12.0, 12.0, 12.0)),
        )

        # A zero estimate must not inherit either nonzero direction: the mixed
        # timing combinations should be visible as disagreement, not confidence.
        self.assertEqual("0/36 agree", benchmark_ab.direction_agreement(row).text)

    def test_bucket_classifies_by_cross_side_estimate_not_round_votes(self) -> None:
        rows = (
            self._row(
                "regression",
                (
                    (1.0, 101.0),
                    (2.0, 1.5),
                    (3.0, 2.5),
                    (4.0, 3.5),
                    (100.0, 5.0),
                ),
            ),
        )

        # Four paired deltas are negative; inclusion proves that classification
        # uses the cross-side estimate rather than a per-round vote.
        self.assertEqual(
            ["regression"],
            [
                row.benchmark.name
                for row in benchmark_ab.bucket_rows(
                    rows,
                    direction=1,
                    threshold=benchmark_ab.Threshold(ns=0.1, pct=10.0),
                )
            ],
        )

    def test_bucket_sorts_by_estimated_delta(self) -> None:
        rows = tuple(
            self._row(benchmark, pairs)
            for benchmark, pairs in (
                ("large_win", ((20.0, 19.5), (10.0, 9.5), (15.0, 2.0))),
                ("small_win", ((20.0, 15.0), (10.0, 8.0), (15.0, 14.0))),
                ("large_loss", ((20.0, 20.5), (10.0, 10.5), (15.0, 28.0))),
                ("small_loss", ((20.0, 25.0), (10.0, 12.0), (15.0, 16.0))),
            )
        )
        threshold = benchmark_ab.Threshold(ns=0.1, pct=0.1)

        self.assertEqual(
            ["large_win", "small_win"],
            [
                row.benchmark.name
                for row in benchmark_ab.bucket_rows(
                    rows, direction=-1, threshold=threshold
                )
            ],
        )
        self.assertEqual(
            ["small_loss", "large_loss"],
            [
                row.benchmark.name
                for row in benchmark_ab.bucket_rows(
                    rows, direction=1, threshold=threshold
                )
            ],
        )
