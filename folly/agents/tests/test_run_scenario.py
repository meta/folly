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
from collections.abc import Mapping
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, BinaryIO
from unittest import mock

from . import checkpoint, run_scenario as runner

REVISION = "a" * 40


def write(path: Path, contents: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents)


def executable(path: Path, contents: str = "tool") -> Path:
    write(path, contents)
    path.chmod(0o755)
    return path


def write_manifest(path: Path, contents: Mapping[str, object]) -> runner.Manifest:
    path.write_text(json.dumps(contents))
    return runner.load_manifest(path)


def make_tools(rules_root: Path) -> None:
    for relative in (
        *runner.TOOL_FILES.values(),
        *runner.CHECKPOINT_TOOL_FILES.values(),
        *runner.CHECKPOINT_RUNTIME_FILES,
        *runner.AGENT_RUNTIME_FILES,
    ):
        executable(rules_root / relative)


class PackageStagingTest(unittest.TestCase):
    # A committed Git fixture exercises discovery and the clean-source check
    # together. Runtime-only tests below supply their rule files directly.
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        if git := os.environ.get("FOLLY_TEST_GIT"):
            self.enterContext(
                mock.patch.dict(
                    os.environ,
                    {
                        "PATH": f"{Path(git).resolve().parent}{os.pathsep}{os.environ.get('PATH', '')}"
                    },
                )
            )
        self.rules_root = self.root / "agents"
        self.scenario = self.root / "scenario"
        self.scenario.mkdir()
        write(self.scenario / "prompt.md", "task")
        write(self.root / "run_scenario.py", "runner")
        make_tools(self.rules_root)
        for name in (
            "alpha.md",
            "alpha.loader.md",
            "alpha/nested/rule.md",
            "alpha/remove.md",
            "other.md",
            "alpha/README.md",
            "alpha/rule.contrib.md",
            "alpha/tests/input.md",
            "alpha/backtest/input.md",
            "alpha/tool.py",
        ):
            write(self.rules_root / name, name)
        self.manifest = write_manifest(
            self.scenario / "scenario.json",
            {
                "prompt": "prompt.md",
                "inputs": [],
                "rules": ["alpha/nested/rule.md", "alpha.md"],
                "packages_under_test": ["alpha"],
            },
        )
        self.git("init", "--quiet")
        self.git("add", ".")
        self.git(
            "-c",
            "user.name=Test",
            "-c",
            "user.email=test@example.com",
            "commit",
            "--quiet",
            "-m",
            "fixture",
        )

    def git(self, *arguments: str) -> None:
        subprocess.run(
            ["git", *arguments],
            cwd=self.root,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def revision(self) -> str:
        return runner.generation_revision(
            self.scenario,
            self.manifest,
            self.rules_root,
            runner_path=self.root / "run_scenario.py",
        )

    def test_stages_only_package_rules_without_expanding_startup(self) -> None:
        write(self.rules_root / "alpha/untracked.md", "untracked")
        workdir = self.root / "workdir"
        runner.stage(self.scenario, self.manifest, self.rules_root, workdir)
        self.assertEqual(
            {
                path.relative_to(workdir / "rules").as_posix()
                for path in (workdir / "rules").rglob("*")
                if path.is_file()
            },
            {
                "alpha.md",
                "alpha.loader.md",
                "alpha/nested/rule.md",
                "alpha/remove.md",
                "rules-inventory.md",
            },
        )
        inventory = (workdir / "rules/rules-inventory.md").read_text()
        # Available helpers must not become additional startup instructions.
        self.assertEqual(
            [line for line in inventory.splitlines() if line[:1].isdigit()],
            ["1. `alpha/nested/rule.md`", "2. `alpha.md`"],
        )

    def test_rejects_edits_additions_and_removals_outside_startup(self) -> None:
        self.revision()
        # Index-only discovery misses removals; HEAD-only discovery misses adds.
        write(self.rules_root / "alpha.loader.md", "changed")
        write(self.rules_root / "alpha/added.md", "added")
        self.git("add", "agents/alpha/added.md")
        self.git("rm", "--quiet", "agents/alpha/remove.md")
        with self.assertRaisesRegex(runner.RunnerError, "refusing to run") as raised:
            self.revision()
        # Check each reported path: one dirty file must not mask another omission.
        for name in ("alpha.loader.md", "alpha/added.md", "alpha/remove.md"):
            with self.subTest(name=name):
                self.assertIn(f"agents/{name}", str(raised.exception))

    def test_package_symlinks_cannot_hide_excluded_or_retargeted_sources(self) -> None:
        alias = self.rules_root / "alpha/alias.md"
        alias.symlink_to("tests/input.md")
        self.git("add", "agents/alpha/alias.md")
        with self.assertRaisesRegex(runner.RunnerError, "may not contain symlinks"):
            self.revision()
        self.git("rm", "--force", "--quiet", "agents/alpha/alias.md")

        # Checking only the final filename would still allow a directory alias.
        nested = self.rules_root / "alpha/nested"
        nested.rename(self.root / "retargeted")
        nested.symlink_to(self.root / "retargeted", target_is_directory=True)
        with self.assertRaisesRegex(runner.RunnerError, "may not contain symlinks"):
            self.revision()

    def test_rejects_undeclared_startup_or_missing_dependency(self) -> None:
        for startup, packages, error in (
            ("other.md", ("alpha",), "startup rules"),
            ("alpha.md", ("alpha", "missing"), "packages: missing"),
        ):
            manifest = runner.Manifest(
                self.manifest.prompt,
                (),
                (runner.PurePosixPath(startup),),
                packages_under_test=packages,
            )
            with self.subTest(startup=startup, packages=packages):
                with self.assertRaisesRegex(runner.RunnerError, error):
                    runner.stage(
                        self.scenario, manifest, self.rules_root, self.root / "workdir"
                    )


class RunScenarioTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.scenario = self.root / "scenario"
        self.rules_root = self.root / "agents"
        self.run_root = self.root / "runs"
        self.scenario.mkdir()
        self.rules_root.mkdir()
        write(self.scenario / "prompt.md", "prompt")
        # Runtime tests supply rule fixtures directly; PackageStagingTest covers
        # discovery from source control.
        package_discovery = mock.patch.object(runner, "_package_files", return_value=())
        self.package_files = package_discovery.start()
        self.addCleanup(package_discovery.stop)

    def manifest(
        self,
        *,
        prompt: str = "prompt.md",
        no_rules_prompt: str | None = None,
        outputs: list[str] | None = None,
        inputs: list[dict[str, str]] | None = None,
        rules: list[str] | None = None,
        scenario: Path | None = None,
    ) -> runner.Manifest:
        scenario = scenario or self.scenario
        contents: dict[str, object] = {
            "prompt": prompt,
            "inputs": inputs or [],
            "rules": rules or [],
        }
        if no_rules_prompt is not None:
            contents["no_rules_prompt"] = no_rules_prompt
        self.package_files.return_value = tuple(
            runner.PurePosixPath(path)
            for path in rules or []
            if (self.rules_root / path).is_file()
        )
        if outputs is not None:
            contents["outputs"] = outputs
        return write_manifest(scenario / "scenario.json", contents)

    def prepare(
        self,
        scenario: Path | None = None,
        *,
        install_rules: bool = True,
        critic_iterate_rounds: int | None = None,
    ) -> runner.Run:
        if install_rules:
            make_tools(self.rules_root)
        return runner.prepare(
            scenario or self.scenario,
            self.rules_root,
            self.run_root,
            "model",
            "high",
            REVISION,
            install_rules=install_rules,
            critic_iterate_rounds=critic_iterate_rounds,
        )

    def prepare_launch(self) -> tuple[runner.Run, Path]:
        codex = executable(self.root / "codex")
        return self.prepare(), codex

    @staticmethod
    def source_control(
        *, status: bytes = b"", revision: str = REVISION
    ) -> tuple[list[list[str]], runner.CommandRunner]:
        commands: list[list[str]] = []

        def run(
            command: list[str], **unused: object
        ) -> subprocess.CompletedProcess[bytes]:
            commands.append(command)
            stdout = status if command[1] == "status" else (revision + "\n").encode()
            return subprocess.CompletedProcess(command, 0, stdout, b"")

        return commands, run

    def fake_codex(
        self,
        run: runner.Run,
        *,
        returncode: int,
        output: str | None,
        trace: bytes = b"",
        errors: bytes = b"",
    ) -> tuple[list[list[str]], list[dict[str, str]], runner.CommandRunner]:
        commands: list[list[str]] = []
        environments: list[dict[str, str]] = []

        def execute(
            command: list[str],
            *,
            cwd: str | Path | None = None,
            stdout: BinaryIO | int,
            stderr: BinaryIO | int,
            env: dict[str, str],
            **unused: object,
        ) -> subprocess.CompletedProcess[bytes]:
            if command[0] == "git":
                self.assertEqual(
                    command,
                    [
                        "git",
                        "init",
                        "--quiet",
                        str(run.agent_workspace.workspace_root),
                    ],
                )
                (run.agent_workspace.workspace_root / ".git").mkdir()
                return subprocess.CompletedProcess(command, 0, b"", b"")
            assert cwd is not None
            assert not isinstance(stdout, int)
            assert not isinstance(stderr, int)
            codex_cwd = Path(cwd)
            self.assertEqual(codex_cwd, run.workdir)
            self.assertTrue((run.agent_workspace.workspace_root / ".git").is_dir())
            self.assertFalse(
                run.agent_home.is_relative_to(run.agent_workspace.workspace_root)
            )
            self.assertEqual(command[command.index("-C") + 1], str(codex_cwd))
            commands.append(command)
            environments.append(env)
            if output is not None:
                (codex_cwd / "output/main.md").write_text(output)
            stdout.write(trace)
            stderr.write(errors)
            return subprocess.CompletedProcess(command, returncode)

        return commands, environments, execute

    def test_parser_has_no_alternate_rules_root(self) -> None:
        args = runner._parser().parse_args(
            ["scenario", "--model", "model", "--reasoning-effort", "high"]
        )

        self.assertNotIn("rules_root", vars(args))
        self.assertFalse(args.no_rules)
        self.assertIsNone(args.critic_iterate_rounds)

    def test_parser_accepts_no_rules(self) -> None:
        args = runner._parser().parse_args(
            [
                "scenario",
                "--model",
                "model",
                "--reasoning-effort",
                "high",
                "--no-rules",
            ]
        )

        self.assertTrue(args.no_rules)

    def test_main_passes_critic_iterate_rounds_to_prepare(self) -> None:
        manifest = runner.Manifest(
            runner.PurePosixPath("prompt.md"),
            (),
            (runner.CRITIC_ITERATE_RULE,),
        )
        run = mock.Mock(root=Path("/run"))
        with (
            mock.patch.object(runner, "load_manifest", return_value=manifest),
            mock.patch.object(runner, "generation_revision", return_value=REVISION),
            mock.patch.object(runner, "prepare", return_value=run) as prepare,
            mock.patch("builtins.print"),
        ):
            result = runner.main(
                [
                    "scenario",
                    "--model",
                    "model",
                    "--reasoning-effort",
                    "high",
                    "--critic-iterate-rounds",
                    "0",
                    "--prepare-only",
                ]
            )

        self.assertEqual(result, 0)
        self.assertEqual(prepare.call_args.kwargs["critic_iterate_rounds"], 0)

    def test_generation_revision_checks_the_files_that_define_a_run(self) -> None:
        (self.root / ".hg").mkdir()
        runner_path = self.root / "run_scenario.py"
        write(runner_path, "runner")
        write(self.scenario / "prompt.no-rules.md", "bare prompt")
        write(self.scenario / "input/data.md", "data")
        write(self.root / "shared.md", "shared")
        write(self.root / "checkpoint_accounting.py", "accounting")
        write(self.rules_root / "write.md", "writing")
        make_tools(self.rules_root)
        manifest = self.manifest(
            no_rules_prompt="prompt.no-rules.md",
            inputs=[
                {"source": "input", "destination": "input"},
                {"source": "../shared.md", "destination": "shared.md"},
            ],
            rules=["write.md"],
        )
        commands, command_runner = self.source_control()

        self.assertEqual(
            runner.generation_revision(
                self.scenario,
                manifest,
                self.rules_root,
                runner_path=runner_path,
                command_runner=command_runner,
            ),
            REVISION,
        )
        status = commands[0]
        self.assertEqual(status[:4], ["sl", "status", "-mardui", "--root-relative"])
        self.assertTrue(
            {
                "run_scenario.py",
                "directory_snapshot.py",
                "scenario/prompt.md",
                "scenario/scenario.json",
                "scenario/input",
                "shared.md",
                "agents/write.md",
                "agents/critic-iterate/codex-reviewer.py",
                "agents/scripts/isolated_agent.py",
            }.issubset(status)
        )
        self.assertNotIn("scenario/prompt.no-rules.md", status)
        self.assertNotIn("agents/tests/checkpoint.py", status)
        self.assertNotIn("checkpoint_accounting.py", status)

        write(self.rules_root / runner.CRITIC_ITERATE_RULE, "critic")
        self.package_files.return_value += (runner.CRITIC_ITERATE_RULE,)
        critic_manifest = runner.Manifest(
            manifest.prompt,
            manifest.inputs,
            (*manifest.rules, runner.CRITIC_ITERATE_RULE),
            manifest.no_rules_prompt,
        )
        commands.clear()
        self.assertEqual(
            runner.generation_revision(
                self.scenario,
                critic_manifest,
                self.rules_root,
                runner_path=runner_path,
                command_runner=command_runner,
            ),
            REVISION,
        )
        self.assertIn("agents/tests/checkpoint.py", commands[0])
        self.assertIn("agents/tests/directory_snapshot.py", commands[0])
        self.assertIn("checkpoint_accounting.py", commands[0])

    def test_generation_revision_rejects_dirty_or_untracked_inputs(self) -> None:
        (self.root / ".hg").mkdir()
        runner_path = self.root / "run_scenario.py"
        write(runner_path, "runner")
        make_tools(self.rules_root)
        manifest = self.manifest()

        for marker in ("M", "?", "I"):
            status = f"{marker} scenario/prompt.md\n".encode()
            with self.subTest(status=status):
                _, command_runner = self.source_control(status=status)
                with self.assertRaises(runner.RunnerError) as raised:
                    runner.generation_revision(
                        self.scenario,
                        manifest,
                        self.rules_root,
                        runner_path=runner_path,
                        command_runner=command_runner,
                    )
                message = str(raised.exception)
                self.assertIn("refusing to run", message)
                self.assertIn(
                    "ask the user to authorize a commit containing only the listed "
                    "source files",
                    message,
                )
                self.assertIn("scenario/prompt.md", message)

    def test_generation_revision_supports_git_checkouts(self) -> None:
        (self.root / ".git").mkdir()
        runner_path = self.root / "run_scenario.py"
        write(runner_path, "runner")
        make_tools(self.rules_root)
        manifest = self.manifest()
        commands, command_runner = self.source_control()

        self.assertEqual(
            runner.generation_revision(
                self.scenario,
                manifest,
                self.rules_root,
                runner_path=runner_path,
                command_runner=command_runner,
            ),
            REVISION,
        )
        self.assertEqual(commands[0][0:3], ["git", "status", "--porcelain=v1"])
        self.assertIn("--untracked-files=all", commands[0])
        self.assertIn("--ignored=matching", commands[0])
        self.assertTrue(any("critic-iterate" in argument for argument in commands[0]))
        self.assertEqual(commands[1], ["git", "rev-parse", "HEAD"])

    def test_generation_revision_omits_rules_for_no_rules_mode(self) -> None:
        (self.root / ".git").mkdir()
        runner_path = self.root / "run_scenario.py"
        write(runner_path, "runner")
        write(self.scenario / "prompt.no-rules.md", "bare prompt")
        write(self.rules_root / "write.md", "writing")
        make_tools(self.rules_root)
        manifest = self.manifest(
            no_rules_prompt="prompt.no-rules.md", rules=["write.md"]
        )
        commands, command_runner = self.source_control()

        self.assertEqual(
            runner.generation_revision(
                self.scenario,
                manifest,
                self.rules_root,
                install_rules=False,
                runner_path=runner_path,
                command_runner=command_runner,
            ),
            REVISION,
        )
        status = commands[0]
        self.assertIn("scenario/prompt.no-rules.md", status)
        self.assertNotIn("scenario/prompt.md", status)
        self.assertFalse(any("write.md" in argument for argument in status))
        self.assertFalse(any("codex-reviewer.py" in argument for argument in status))
        self.assertTrue(any("isolated_agent.py" in argument for argument in status))

    def test_generation_revision_rejects_multiple_checkouts(self) -> None:
        agents_root = self.root / "rules-checkout/folly/agents"
        scenario = self.root / "scenario-checkout/scenario"
        runner_path = agents_root / "tests/run_scenario.py"
        (self.root / "rules-checkout/.git").mkdir(parents=True)
        (self.root / "scenario-checkout/.git").mkdir(parents=True)
        write(runner_path, "runner")
        write(scenario / "prompt.md", "prompt")
        make_tools(agents_root)
        manifest = write_manifest(
            scenario / "scenario.json",
            {"prompt": "prompt.md", "inputs": [], "rules": []},
        )

        with self.assertRaisesRegex(runner.RunnerError, "one source checkout"):
            runner.generation_revision(
                scenario,
                manifest,
                agents_root,
                runner_path=runner_path,
            )

    def test_manifest_rejects_unsafe_paths_and_development_rules(self) -> None:
        cases = (
            (
                "input destination escape",
                {
                    "prompt": "prompt.md",
                    "inputs": [{"source": "evidence.md", "destination": "../secret"}],
                    "rules": [],
                },
                "without '..'",
            ),
            (
                "development rule",
                {"prompt": "prompt.md", "inputs": [], "rules": ["write.contrib.md"]},
                "development material",
            ),
            (
                "reserved rules destination",
                {
                    "prompt": "prompt.md",
                    "inputs": [
                        {"source": "evidence.md", "destination": "rules/injected.md"}
                    ],
                    "rules": [],
                },
                "reserved rules",
            ),
            (
                "hidden Codex policy",
                {
                    "prompt": "prompt.md",
                    "inputs": [
                        {
                            "source": "evidence.md",
                            "destination": "evidence/AGENTS.md",
                        }
                    ],
                    "rules": [],
                },
                "hidden policy",
            ),
            *(
                (
                    f"reserved {root} destination",
                    {
                        "prompt": "prompt.md",
                        "inputs": [
                            {
                                "source": "evidence.md",
                                "destination": f"{root}/injected",
                            }
                        ],
                        "rules": [],
                    },
                    "reserved policy root",
                )
                for root in (".codex", ".git", ".llms")
            ),
        )
        for name, contents, error in cases:
            with self.subTest(name), self.assertRaisesRegex(runner.RunnerError, error):
                write_manifest(self.scenario / "scenario.json", contents)

    def test_prepare_rejects_a_run_root_inside_inputs_or_rules(self) -> None:
        self.manifest()
        for run_root, error in (
            (self.scenario / "runs", "inside the scenario"),
            (self.rules_root / "runs", "inside the rules"),
        ):
            with self.subTest(error), self.assertRaisesRegex(runner.RunnerError, error):
                runner.prepare(
                    self.scenario,
                    self.rules_root,
                    run_root,
                    "model",
                    "high",
                    REVISION,
                )

    def test_prepare_removes_a_partial_run_when_staging_fails(self) -> None:
        self.manifest(rules=["missing.md"])

        with self.assertRaisesRegex(runner.RunnerError, "tracked files"):
            self.prepare()

        self.assertEqual(list(self.run_root.iterdir()), [])

    def test_prepare_prefixes_the_rule_loading_instruction(self) -> None:
        write(self.scenario / "prompt.md", "Do the task.\n")
        write(self.scenario / "prompt.no-rules.md", "Do the bare task.\n")
        self.manifest(no_rules_prompt="prompt.no-rules.md")

        run = self.prepare()

        staged_rules = run.workdir / "rules"
        self.assertEqual(
            run.prompt.read_text(),
            runner._rule_loading_instruction(staged_rules) + "Do the task.\n",
        )
        reviewer = runner.TOOL_FILES["codex-reviewer.py"]
        (self.rules_root / reviewer).write_text("changed after prepare")
        self.assertEqual((staged_rules / reviewer).read_text(), "tool")
        self.assertTrue((staged_rules / runner.AGENT_RUNTIME_FILES[0]).is_file())
        metadata = json.loads((run.root / "run.json").read_text())
        self.assertEqual(metadata["generation_revision"], REVISION)
        self.assertNotIn("critic_iterate_rounds", metadata)
        self.assertNotIn("no_rules", metadata)

    def test_outputs_cannot_overwrite_run_evidence(self) -> None:
        for output in ("run.json", "trace.jsonl", "workspace", "nested/review.md"):
            with (
                self.subTest(output=output),
                self.assertRaisesRegex(runner.RunnerError, "below output/"),
            ):
                self.manifest(outputs=[output])

    def test_output_namespace_allows_metadata_names_and_rejects_required_collisions(
        self,
    ) -> None:
        self.manifest(
            outputs=["output/run.json", "output/AGENTS.md", "output/nested/result"]
        )
        run = self.prepare()
        for relative in run.outputs:
            write(run.workdir / relative, "artifact")
        self.assertEqual(runner._finish_run(run, 0), 0)
        self.assertEqual((run.root / "output/run.json").read_text(), "artifact")
        self.assertEqual(
            json.loads((run.root / "run.json").read_text())["status"], "complete"
        )
        for outputs in (
            ["output/a", "output/a"],
            ["output/a", "output/a/child"],
            ["output"],
        ):
            with self.subTest(outputs=outputs), self.assertRaises(runner.RunnerError):
                self.manifest(outputs=outputs)

    def test_unspecified_output_names_still_require_content(self) -> None:
        # No filenames are prescribed; empty files alone do not finish the task.
        self.manifest(outputs=[])
        for populated in (False, True):
            with self.subTest(populated=populated):
                run = self.prepare()
                (run.workdir / "output/empty-file").touch()
                if populated:
                    (run.workdir / "output/review.md").write_text("finding")
                self.assertEqual(runner._finish_run(run, 0), 0 if populated else 2)
                self.assertEqual(
                    json.loads((run.root / "run.json").read_text())["status"],
                    "complete" if populated else "missing-output",
                )

    def test_prepare_injects_and_records_critic_iterate_rounds(self) -> None:
        write(self.scenario / "prompt.md", "Do the task.\n")
        write(self.rules_root / runner.CRITIC_ITERATE_RULE, "critic")
        self.manifest(rules=[runner.CRITIC_ITERATE_RULE.as_posix()])

        run = self.prepare(critic_iterate_rounds=0)

        self.assertTrue(run.checkpoint)
        self.assertEqual(
            run.prompt.read_text(),
            runner._rule_loading_instruction(run.workdir / "rules")
            + "c-i-0\n\n"
            + runner._checkpoint_instruction()
            + "Do the task.\n",
        )
        metadata = json.loads((run.root / "run.json").read_text())
        self.assertEqual(metadata["critic_iterate_rounds"], 0)
        self.assertRegex(metadata["checkpoint_key"], r"^[0-9a-f]{32}$")

        default_run = self.prepare()
        self.assertTrue(default_run.checkpoint)
        self.assertEqual(
            default_run.prompt.read_text(),
            runner._rule_loading_instruction(default_run.workdir / "rules")
            + runner._checkpoint_instruction()
            + "Do the task.\n",
        )

    def test_prepare_rejects_inapplicable_critic_iterate_rounds(self) -> None:
        cases = (
            (
                "negative",
                [runner.CRITIC_ITERATE_RULE.as_posix()],
                True,
                -1,
                "nonnegative",
            ),
            (
                "no rules",
                [runner.CRITIC_ITERATE_RULE.as_posix()],
                False,
                0,
                "--no-rules",
            ),
            ("critic rule absent", [], True, 0, "scenario rules"),
        )
        for name, rules, install_rules, rounds, message in cases:
            with self.subTest(name):
                self.manifest(rules=rules)
                run_root = self.root / f"runs-{rounds}-{install_rules}-{len(rules)}"

                with self.assertRaisesRegex(runner.RunnerError, message):
                    runner.prepare(
                        self.scenario,
                        self.rules_root,
                        run_root,
                        "model",
                        "high",
                        REVISION,
                        install_rules=install_rules,
                        critic_iterate_rounds=rounds,
                    )

                self.assertFalse(run_root.exists())

    def test_prepare_leaves_review_only_scenarios_uncheckpointed(self) -> None:
        write(self.rules_root / runner.CRITIC_ITERATE_RULE, "critic")
        self.manifest(
            inputs=[{"source": "prompt.md", "destination": "output/main.md"}],
            rules=[runner.CRITIC_ITERATE_RULE.as_posix()],
        )

        run = self.prepare(critic_iterate_rounds=1)

        self.assertFalse(run.checkpoint)
        self.assertIn("c-i-1\n\n", run.prompt.read_text())
        self.assertNotIn("backtest-checkpoint", run.prompt.read_text())

    def test_checkpoint_instruction_names_only_the_first_command(self) -> None:
        instruction = runner._checkpoint_instruction()

        self.assertIn("run exactly `backtest-checkpoint 0`", instruction)
        self.assertNotIn("backtest-checkpoint 1", instruction)
        self.assertNotIn("external review", instruction)

    def test_prepare_without_rules_stages_bare_inputs(self) -> None:
        write(self.scenario / "prompt.md", "Do the task.\n")
        write(self.scenario / "prompt.no-rules.md", "Do the bare task.\n")
        write(self.scenario / "input.md", "input")
        self.manifest(
            no_rules_prompt="prompt.no-rules.md",
            inputs=[{"source": "input.md", "destination": "input.md"}],
            rules=["critic-iterate.md"],
            outputs=["output/review.md"],
        )

        run = self.prepare(install_rules=False)

        self.assertEqual(run.prompt.read_text(), "Do the bare task.\n")
        self.assertEqual(
            {path.name for path in run.workdir.iterdir()}, {"input.md", "output"}
        )
        self.assertFalse(run.install_rules)
        self.assertFalse(run.checkpoint)
        metadata = json.loads((run.root / "run.json").read_text())
        self.assertEqual(metadata["rules"], [])
        self.assertTrue(metadata["no_rules"])

    def test_prepare_without_rules_requires_bare_prompt(self) -> None:
        self.manifest()

        with self.assertRaisesRegex(runner.RunnerError, "requires no_rules_prompt"):
            self.prepare(install_rules=False)

        self.assertFalse(self.run_root.exists())

    def test_staging_rejects_symlinks_outside_declared_roots(self) -> None:
        write(self.root / "outside.md", "outside")
        (self.scenario / "prompt.md").unlink()
        (self.scenario / "input.md").symlink_to(self.root / "outside.md")
        self.manifest(
            prompt="input.md",
            inputs=[{"source": "input.md", "destination": "input.md"}],
        )
        with self.assertRaisesRegex(runner.RunnerError, "resolves outside"):
            self.prepare()

        tree_scenario = self.root / "tree-scenario"
        tree_scenario.mkdir()
        write(tree_scenario / "prompt.md", "prompt")
        (tree_scenario / "source").mkdir()
        (tree_scenario / "source/link.md").symlink_to(self.root / "outside.md")
        tree_manifest = self.manifest(
            inputs=[{"source": "source", "destination": "source"}],
            scenario=tree_scenario,
        )
        with self.assertRaisesRegex(runner.RunnerError, "may not contain symlinks"):
            runner.stage(
                tree_scenario,
                tree_manifest,
                self.rules_root,
                self.root / "tree-workdir",
            )

        (self.rules_root / "write.md").symlink_to(self.root / "outside.md")
        rule_manifest = self.manifest(rules=["write.md"])
        with self.assertRaisesRegex(runner.RunnerError, "resolves outside"):
            runner.stage(
                self.scenario,
                rule_manifest,
                self.rules_root,
                self.root / "rule-workdir",
            )

    def test_stage_copies_declared_inputs_without_other_task_evidence(self) -> None:
        workdir = self.root / "workdir"
        workdir.mkdir()
        write(self.scenario / "draft.md", "draft")
        write(self.scenario / "source/Api.h.txt", "header")
        write(self.root / "shared.md", "shared")
        write(self.scenario / "samples/1/output/main.md", "prior output")
        write(self.rules_root / "write.md", "writing")
        write(self.rules_root / runner.CRITIC_ITERATE_RULE, "critic")
        write(self.rules_root / "write.contrib.md", "decoy")
        manifest = self.manifest(
            inputs=[
                {"source": "draft.md", "destination": "output/main.md"},
                {"source": "source", "destination": "source"},
                {"source": "../shared.md", "destination": "shared.md"},
            ],
            rules=["write.md", "critic-iterate.md"],
        )

        runner.stage(self.scenario, manifest, self.rules_root, workdir)

        self.assertEqual((workdir / "source/Api.h").read_text(), "header")
        self.assertEqual((workdir / "shared.md").read_text(), "shared")
        self.assertTrue((workdir / "output/main.md").stat().st_mode & 0o200)
        self.assertFalse((workdir / "source/Api.h").stat().st_mode & 0o200)
        self.assertFalse((workdir / "rules/write.contrib.md").exists())
        self.assertFalse((workdir / "samples").exists())
        inventory = (workdir / "rules/rules-inventory.md").read_text()
        self.assertIn("1. `write.md`", inventory)
        self.assertIn("2. `critic-iterate.md`", inventory)

    def test_every_seeded_output_file_is_writable_not_just_required_names(self) -> None:
        # Only main.md is required, but the author must be able to revise every
        # supplied draft file. Other inputs remain read-only and unchanged.
        write(self.scenario / "draft/main.md", "main draft")
        write(self.scenario / "draft/nested/notes.md", "notes draft")
        write(self.scenario / "evidence.md", "readonly evidence")
        self.manifest(
            inputs=[
                {"source": "draft", "destination": "output"},
                {"source": "evidence.md", "destination": "evidence.md"},
            ]
        )

        run = self.prepare()

        for name in ("main.md", "nested/notes.md"):
            self.assertTrue((run.workdir / "output" / name).stat().st_mode & 0o222)
        self.assertFalse((run.workdir / "evidence.md").stat().st_mode & 0o222)

        (run.workdir / "output/nested/notes.md").write_text("finished notes")
        self.assertEqual(runner._finish_run(run, 0), 0)

        self.assertEqual(
            (run.root / "output/nested/notes.md").read_text(), "finished notes"
        )
        self.assertEqual(
            (self.scenario / "draft/nested/notes.md").read_text(), "notes draft"
        )

    def test_stage_rejects_hidden_policy_inside_an_input_tree(self) -> None:
        workdir = self.root / "workdir"
        workdir.mkdir()
        write(self.scenario / "source/AGENTS.md", "hidden policy")
        manifest = self.manifest(inputs=[{"source": "source", "destination": "source"}])

        with self.assertRaisesRegex(runner.RunnerError, "hidden policy"):
            runner.stage(self.scenario, manifest, self.rules_root, workdir)
        self.assertEqual(list(workdir.iterdir()), [])

    def test_stage_rejects_suffix_collision_before_writing(self) -> None:
        workdir = self.root / "workdir"
        workdir.mkdir()
        write(self.scenario / "source/Api.h.txt", "snapshot")
        write(self.scenario / "source/Api.h", "native")
        manifest = self.manifest(inputs=[{"source": "source", "destination": "source"}])

        with self.assertRaisesRegex(runner.RunnerError, "map to source/Api.h"):
            runner.stage(self.scenario, manifest, self.rules_root, workdir)
        self.assertEqual(list(workdir.iterdir()), [])

    def test_launch_uses_isolated_tools_and_preserves_output(self) -> None:
        write(self.scenario / "draft.md", "draft")
        self.manifest(inputs=[{"source": "draft.md", "destination": "output/main.md"}])
        run, codex = self.prepare_launch()
        commands, environments, command_runner = self.fake_codex(
            run,
            returncode=0,
            output="complete",
            trace=b"trace\n",
            errors=b"diagnostic\n",
        )
        forbidden_environment = {
            "CODEX_REVIEWER",
            "CODEX_FRESH_REVIEW",
            "CRITIC_ITERATE_RULES_DIR",
            "CODEX_UNRELATED",
            "PYTHONHOME",
            "PYTHONINSPECT",
            "PYTHONPATH",
            "PYTHONWARNINGS",
            "BASH_ENV",
            "ENV",
            "FOLLY_BACKTEST_RUN_DIR",
            "FOLLY_BACKTEST_WORKDIR",
            "GIT_CEILING_DIRECTORIES",
            "GIT_DIR",
            "GIT_WORK_TREE",
            "ZDOTDIR",
        }

        with mock.patch.dict(
            os.environ,
            {
                **dict.fromkeys(forbidden_environment, "set"),
                "TMPDIR": str(self.scenario),
            },
        ):
            result = runner.launch(
                run, codex, "model", "high", command_runner=command_runner
            )

        self.assertEqual(result, 0)
        (command,) = commands
        codex_cwd = Path(command[command.index("-C") + 1])
        self.assertEqual(
            command,
            [
                str(codex.resolve()),
                "-a",
                "never",
                "-C",
                str(codex_cwd),
                "exec",
                "--ignore-user-config",
                "--json",
                "--model",
                "model",
                "--config",
                'model_reasoning_effort="high"',
                "-",
            ],
        )
        self.assertTrue(codex_cwd.is_dir())
        (environment,) = environments
        self.assertTrue(
            forbidden_environment.isdisjoint(environment),
            forbidden_environment & environment.keys(),
        )
        tool_bin = run.root / "bin"
        self.assertEqual(environment["PATH"].split(os.pathsep, 1)[0], str(tool_bin))
        self.assertEqual(environment["HOME"], str(run.agent_home))
        self.assertEqual(environment["TMPDIR"], str(run.agent_workspace.temporary))
        self.assertNotIn("W", environment)
        for name, relative in runner.TOOL_FILES.items():
            self.assertEqual(
                (tool_bin / name).resolve(),
                (run.workdir / "rules" / relative).resolve(),
            )
        self.assertFalse((tool_bin / "backtest-checkpoint").exists())
        policy = (run.agent_home / "rules/default.rules").read_text()
        for name in runner.TOOL_FILES:
            self.assertIn(f'host_executable(name="{name}"', policy)
            self.assertIn(f'prefix_rule(pattern=["{name}"]', policy)
        self.assertEqual((run.root / "output/main.md").read_text(), "complete")
        self.assertEqual((run.root / "trace.jsonl").read_text(), "trace\n")
        self.assertEqual((run.root / "err.txt").read_text(), "diagnostic\n")
        metadata = json.loads((run.root / "run.json").read_text())
        self.assertEqual(metadata["status"], "complete")
        self.assertEqual(metadata["exit_code"], 0)
        self.assertEqual(metadata["executables"]["codex"], str(codex.resolve()))

    def test_launch_preserves_whole_tree_and_runs_installed_checkpoints(self) -> None:
        # Companion files must survive the installed checkpoint CLI and final
        # delivery. Both notes revisions expose a copy that saves only main.md.
        write(self.rules_root / runner.CRITIC_ITERATE_RULE, "critic")
        self.manifest(
            rules=[runner.CRITIC_ITERATE_RULE.as_posix()],
            outputs=["output/main.md", "output/notes.md"],
        )
        make_tools(self.rules_root)
        write(
            self.rules_root / "tests/checkpoint.py",
            Path(checkpoint.__file__).read_text(),
        )
        write(
            self.rules_root / "tests/directory_snapshot.py",
            Path(runner.directory_snapshot.__file__).read_text(),
        )
        run = runner.prepare(
            self.scenario,
            self.rules_root,
            self.run_root,
            "model",
            "high",
            REVISION,
            critic_iterate_rounds=0,
        )
        thread_id = "01a00000-0000-7000-8000-000000000000"
        checkpoint_key = bytes.fromhex("00112233445566778899aabbccddeeff")
        runner._update_metadata(run, checkpoint_key=checkpoint_key.hex())
        trace = b"".join(
            (json.dumps(event) + "\n").encode()
            for event in [
                {"type": "thread.started", "thread_id": thread_id},
                {
                    "type": "item.completed",
                    "item": {"type": "agent_message", "text": "Done."},
                },
            ]
        )
        commands, _, fake_codex = self.fake_codex(
            run, returncode=0, output="initial", trace=trace
        )

        def execute(
            command: list[str], **arguments: Any
        ) -> subprocess.CompletedProcess[bytes]:
            result = fake_codex(command, **arguments)
            if command[0] == "git":
                return result
            output = run.workdir / "output"
            (output / "notes.md").write_text("notes")
            (output / "nested").mkdir()
            (output / "nested/guide.md").write_text(
                "Guide — café\r\n", encoding="utf-8"
            )
            (output / "nested/empty-file").touch()
            events: list[dict[str, object]] = [
                {"timestamp": "2026-09-04T00:00:00Z", "type": "session_meta"}
            ]
            # Final delivery adds usage without creating a third checkpoint.
            for index, (second, tokens) in enumerate(((10, 100), (20, 200), (25, 250))):
                if index < 2:
                    if index:
                        (output / "notes.md").write_text("reviewed notes")
                    captured = subprocess.run(
                        [str(run.root / "bin/backtest-checkpoint"), str(index)],
                        env=arguments["env"],
                        capture_output=True,
                        text=True,
                        check=False,
                    )
                    self.assertEqual(captured.returncode, 0, captured.stderr)
                    self.assertIn(
                        checkpoint.make_checkpoint_marker(index, checkpoint_key),
                        captured.stdout,
                    )
                    timestamp = datetime(
                        2026, 9, 4, 0, 0, second, tzinfo=timezone.utc
                    ).timestamp()
                    os.utime(
                        run.root / "checkpoints" / str(index), (timestamp, timestamp)
                    )
                    events.append(
                        {
                            "type": "response_item",
                            "payload": {
                                "type": "custom_tool_call_output",
                                "output": [
                                    {"type": "input_text", "text": captured.stdout}
                                ],
                            },
                        }
                    )
                events.append(
                    {
                        "timestamp": f"2026-09-04T00:00:{second:02d}Z",
                        "type": "event_msg",
                        "payload": {
                            "type": "token_count",
                            "info": {
                                "model_context_window": 1000,
                                "total_token_usage": {
                                    "input_tokens": tokens,
                                    "cached_input_tokens": tokens // 2,
                                    "cache_write_input_tokens": 0,
                                    "output_tokens": tokens // 10,
                                    "reasoning_output_tokens": tokens // 20,
                                },
                            },
                        },
                    }
                )
            write(
                run.agent_home
                / "sessions/2026/09/04"
                / f"rollout-test-{thread_id}.jsonl",
                "".join(json.dumps(event) + "\n" for event in events),
            )
            return result

        result = runner.launch(
            run,
            executable(self.root / "codex"),
            "model",
            "high",
            command_runner=execute,
        )

        self.assertEqual(result, 0)
        self.assertEqual(len(commands), 1)
        self.assertIn(
            f'host_executable(name="backtest-checkpoint", paths=["{run.root / "bin/backtest-checkpoint"}"])',
            (run.agent_home / "rules/default.rules").read_text().splitlines(),
        )
        self.assertEqual(
            runner.directory_snapshot.read(run.root / "output"),
            runner.directory_snapshot.read(run.workdir / "output"),
        )
        self.assertEqual(
            (run.root / "output/nested/guide.md").read_bytes(),
            "Guide — café\r\n".encode("utf-8"),
        )
        self.assertEqual((run.root / "output/nested/empty-file").read_bytes(), b"")
        self.assertEqual((run.root / "output-initial/notes.md").read_text(), "notes")
        self.assertEqual((run.root / "output/notes.md").read_text(), "reviewed notes")
        self.assertFalse((run.root / "output-author").exists())
        records = json.loads((run.root / "checkpoints.json").read_text())
        self.assertEqual(records[1]["changed_paths"], ["notes.md"])
        self.assertEqual(
            json.loads((run.root / "run.json").read_text())["checkpoint_count"], 2
        )

    def test_invalid_accounting_does_not_complete_a_run(self) -> None:
        # A saved draft is not a valid run without matching checkpoint evidence.
        write(self.rules_root / runner.CRITIC_ITERATE_RULE, "critic")
        self.manifest(rules=[runner.CRITIC_ITERATE_RULE.as_posix()])
        run = self.prepare(critic_iterate_rounds=0)
        (run.workdir / "output/main.md").write_text("complete")

        with self.assertRaises(ValueError):
            runner._finish_run(run, 0)

        metadata = json.loads((run.root / "run.json").read_text())
        self.assertEqual(metadata["status"], "invalid-checkpoints")
        self.assertNotIn("exit_code", metadata)

    def test_failed_launch_preserves_partial_output_and_diagnostics(self) -> None:
        write(self.scenario / "draft.md", "draft")
        self.manifest(inputs=[{"source": "draft.md", "destination": "output/main.md"}])
        run, codex = self.prepare_launch()
        commands, _, command_runner = self.fake_codex(
            run,
            returncode=7,
            output="partial",
            trace=b"trace\n",
            errors=b"failure\n",
        )

        result = runner.launch(
            run, codex, "model", "high", command_runner=command_runner
        )

        self.assertEqual(result, 7)
        codex_cwd = Path(commands[0][commands[0].index("-C") + 1])
        self.assertTrue(codex_cwd.is_dir())
        self.assertEqual((run.root / "output/main.md").read_text(), "partial")
        self.assertEqual((run.root / "trace.jsonl").read_text(), "trace\n")
        self.assertEqual((run.root / "err.txt").read_text(), "failure\n")
        metadata = json.loads((run.root / "run.json").read_text())
        self.assertEqual(metadata["status"], "failed")
        self.assertEqual(metadata["exit_code"], 7)
        self.assertEqual(metadata["executables"]["codex"], str(codex.resolve()))

    def test_declared_outputs_stage_and_preserve_all_artifacts(self) -> None:
        write(self.scenario / "finding.md", "finding")
        write(self.scenario / "notes.md", "notes")
        self.manifest(
            inputs=[
                {"source": "finding.md", "destination": "output/review.md"},
                {"source": "notes.md", "destination": "output/review-notes.md"},
            ],
            outputs=["output/review.md", "output/review-notes.md"],
        )
        run = self.prepare()

        (run.workdir / "output/review.md").write_text("finished finding")

        self.assertEqual(runner._finish_run(run, 0), 0)
        self.assertEqual(
            (run.root / "output/review.md").read_text(), "finished finding"
        )
        self.assertEqual((run.root / "output/review-notes.md").read_text(), "notes")
        self.assertEqual((self.scenario / "finding.md").read_text(), "finding")
        metadata = json.loads((run.root / "run.json").read_text())
        self.assertEqual(
            metadata["outputs"], ["output/review.md", "output/review-notes.md"]
        )
        self.assertEqual(metadata["status"], "complete")

    def test_declared_outputs_require_nonempty_regular_files(self) -> None:
        self.manifest(outputs=["output/review.md", "output/nested/review-notes.md"])
        for kind in ("missing", "empty", "directory"):
            with self.subTest(kind=kind):
                run = self.prepare()
                (run.workdir / "output/review.md").write_text("finding")
                notes = run.workdir / "output/nested/review-notes.md"
                notes.parent.mkdir()
                if kind == "empty":
                    notes.touch()
                elif kind == "directory":
                    notes.mkdir()

                self.assertEqual(runner._finish_run(run, 0), 2)
                self.assertEqual(
                    (run.workdir / "output/review.md").read_text(), "finding"
                )
                if kind in ("missing", "empty"):
                    self.assertEqual(
                        (run.root / "output/review.md").read_text(), "finding"
                    )
                expected = (
                    "missing-output"
                    if kind in ("missing", "empty")
                    else "invalid-output"
                )
                self.assertEqual(
                    json.loads((run.root / "run.json").read_text())["status"], expected
                )

    def test_invalid_output_keeps_workspace_evidence_and_author_status(self) -> None:
        self.manifest(outputs=["output/invalid.md", "output/review.md"])
        run = self.prepare()
        (run.workdir / "output/review.md").write_text("finding")
        (run.workdir / "output/invalid.md").mkdir()

        self.assertEqual(runner._finish_run(run, 7), 7)

        self.assertEqual((run.workdir / "output/review.md").read_text(), "finding")
        metadata = json.loads((run.root / "run.json").read_text())
        self.assertEqual(metadata["status"], "failed")
        self.assertEqual(metadata["exit_code"], 7)
        self.assertIn("invalid.md", metadata["output_preservation_error"])

    def test_output_root_and_workspace_ancestors_cannot_be_worker_symlinks(
        self,
    ) -> None:
        self.manifest()
        outside = self.root / "outside"
        write(outside / "output/main.md", "private")
        for target in ("output", "task", "workspace"):
            with self.subTest(target=target):
                run = self.prepare()
                if target == "output":
                    replaced = run.workdir / "output"
                    replaced.rmdir()
                    replaced.symlink_to(outside / "output")
                elif target == "task":
                    replaced = run.workdir
                    replaced.rename(replaced.with_name("original-task"))
                    replaced.symlink_to(outside)
                else:
                    replaced = run.agent_workspace.workspace_root
                    replaced.rename(replaced.with_name("original-workspace"))
                    (outside / "task").symlink_to(outside)
                    replaced.symlink_to(outside)
                self.assertEqual(runner._finish_run(run, 0), 2)
                self.assertFalse((run.root / "output/main.md").exists())
                metadata = json.loads((run.root / "run.json").read_text())
                self.assertEqual(metadata["status"], "invalid-output")
                self.assertTrue(metadata["output_preservation_error"])

    def test_preservation_write_error_keeps_workspace_and_author_failure(self) -> None:
        self.manifest(outputs=[])
        run = self.prepare()
        (run.workdir / "output/a-failed").write_bytes(b"failed")
        (run.workdir / "output/z-good").write_bytes(b"preserved")

        with mock.patch.object(
            runner.directory_snapshot,
            "write",
            side_effect=PermissionError("cannot preserve output"),
        ):
            self.assertEqual(runner._finish_run(run, 7), 7)

        self.assertEqual((run.workdir / "output/a-failed").read_bytes(), b"failed")
        self.assertEqual((run.workdir / "output/z-good").read_bytes(), b"preserved")
        metadata = json.loads((run.root / "run.json").read_text())
        self.assertEqual(metadata["status"], "failed")
        self.assertEqual(metadata["exit_code"], 7)
        self.assertIn("cannot preserve output", metadata["output_preservation_error"])

    def test_successful_but_inexact_preservation_does_not_complete_a_run(self) -> None:
        # Simulate a copy that succeeds but corrupts a nested companion. The
        # author succeeded; the runner must reject the copy and retain its source.
        self.manifest()
        run = self.prepare()
        (run.workdir / "output/nested").mkdir()
        (run.workdir / "output/main.md").write_text("valid draft")
        (run.workdir / "output/nested/notes.md").write_text("original notes\n")
        expected = runner.directory_snapshot.read(run.workdir / "output")
        write_tree = runner.directory_snapshot.write

        def faulty_write(entries, destination):
            copied = dict(entries)
            copied[Path("nested/notes.md")] = b"wrong notes\n"
            write_tree(copied, destination)

        with mock.patch.object(runner.directory_snapshot, "write", faulty_write):
            self.assertEqual(runner._finish_run(run, 0), 2)

        metadata = json.loads((run.root / "run.json").read_text())
        self.assertEqual(metadata["status"], "invalid-output")
        self.assertTrue(metadata["output_preservation_error"])
        self.assertEqual(
            runner.directory_snapshot.read(run.workdir / "output"), expected
        )

    def test_success_without_output_is_runner_failure(self) -> None:
        self.manifest()
        run, codex = self.prepare_launch()
        _, _, command_runner = self.fake_codex(run, returncode=0, output=None)

        result = runner.launch(
            run, codex, "model", "high", command_runner=command_runner
        )

        self.assertEqual(result, 2)
        metadata = json.loads((run.root / "run.json").read_text())
        self.assertEqual(metadata["status"], "missing-output")
        self.assertEqual(metadata["exit_code"], 2)

    def test_launch_without_rules_preserves_the_ambient_path(self) -> None:
        self.manifest(no_rules_prompt="prompt.md")
        codex = executable(self.root / "codex")
        run = self.prepare(install_rules=False)
        _, environments, command_runner = self.fake_codex(
            run, returncode=0, output="complete"
        )

        result = runner.launch(
            run, codex, "model", "high", command_runner=command_runner
        )

        self.assertEqual(result, 0)
        self.assertEqual((run.root / "output/main.md").read_text(), "complete")
        self.assertEqual(environments[0].get("PATH"), os.environ.get("PATH"))
        self.assertFalse((run.root / "bin").exists())
        self.assertFalse((run.agent_home / "rules/default.rules").exists())
        metadata = json.loads((run.root / "run.json").read_text())
        self.assertEqual(metadata["status"], "complete")
        self.assertEqual(metadata["exit_code"], 0)
        self.assertEqual(metadata["executables"], {"codex": str(codex.resolve())})
