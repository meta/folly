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
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Mapping
from unittest import mock

from ..scripts import isolated_agent


class FakeImpl:
    name = "fake"
    executable_name = sys.executable

    def initialize(
        self,
        workspace: isolated_agent.Workspace,
        environment: Mapping[str, str],
        command_runner: isolated_agent.CommandRunner,
    ) -> None:
        (workspace.workspace_root / ".git").mkdir()

    def command(
        self,
        workspace: isolated_agent.Workspace,
        executable: Path,
        request: isolated_agent.Request,
    ) -> list[str]:
        operation = "resume" if request.resume_session_id else "run"
        return [
            str(executable),
            operation,
            request.resume_session_id or "new",
            str(request.response_path or ""),
        ]

    def validate(self, workspace: isolated_agent.Workspace) -> None:
        if not (workspace.workspace_root / ".git").is_dir():
            raise isolated_agent.IsolationError("missing fake boundary")

    def configure_environment(
        self,
        environment: dict[str, str],
        workspace: isolated_agent.Workspace,
        request: isolated_agent.Request | None,
    ) -> None:
        environment["FAKE_HOME"] = str(workspace.home)

    def extract_session_id(self, trace: Path) -> str | None:
        return None


class IsolatedAgentTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.agent = isolated_agent.Agent(
            FakeImpl(), _shared_cache_user_home_for_testing=self.root / "home"
        )

    def test_prepare_separates_engine_home_from_workspace(self) -> None:
        prepared = self.agent.prepare(self.root / "run")

        self.assertEqual(prepared.task.parent, prepared.workspace_root)
        self.assertFalse(prepared.home.is_relative_to(prepared.workspace_root))
        self.assertTrue((prepared.workspace_root / ".git").is_dir())

    def test_prepare_links_shared_cache_directories(self) -> None:
        for has_local_directory in (True, False):
            with self.subTest(has_local_directory=has_local_directory):
                user_home = self.root / f"user-home-{has_local_directory}"
                if has_local_directory:
                    (user_home / "local").mkdir(parents=True)
                agent = isolated_agent.Agent(
                    FakeImpl(), _shared_cache_user_home_for_testing=user_home
                )
                prepared = agent.prepare(self.root / f"run-{has_local_directory}")

                cache_root = (
                    user_home / "local" / "isolated_agents_cache"
                    if has_local_directory
                    else user_home / "isolated_agents_cache"
                )
                for name in (".cache", "packages"):
                    shared_directory = cache_root / name
                    isolated_directory = prepared.home / name
                    self.assertTrue(shared_directory.is_dir())
                    self.assertTrue(isolated_directory.is_symlink())
                    self.assertEqual(
                        isolated_directory.resolve(), shared_directory.resolve()
                    )

    def test_environment_removes_injection_variables(self) -> None:
        prepared = self.agent.prepare(self.root / "run")
        inherited = {
            "BASH_ENV": "bad",
            "CODEX_HOME": "bad",
            "GIT_DIR": "bad",
            "META_CODEX_LLM_RULES": "1",
            "PATH": "/bin",
            "PYTHONPATH": "bad",
            "SAFE": "kept",
            "XDG_CONFIG_HOME": "/ambient",
        }

        with mock.patch.dict(os.environ, inherited, clear=True):
            environment = self.agent.environment(
                prepared,
                isolated_agent.Request("model", "high"),
                additions={"EXTRA": "added"},
            )

        self.assertEqual(environment["EXTRA"], "added")
        self.assertEqual(environment["FAKE_HOME"], str(prepared.home))
        self.assertEqual(environment["HOME"], str(prepared.home))
        self.assertEqual(environment["PATH"], "/bin")
        self.assertEqual(environment["SAFE"], "kept")
        self.assertEqual(environment["TMPDIR"], str(prepared.temporary))
        self.assertEqual(environment["W"], str(prepared.task))
        self.assertNotIn("META_CODEX_LLM_RULES", environment)
        self.assertNotIn("XDG_CONFIG_HOME", environment)
        with self.assertRaisesRegex(
            isolated_agent.IsolationError, "reserved environment override"
        ):
            self.agent.environment(
                prepared,
                additions={"GIT_DIR": "bad"},
            )

    def test_load_rejects_a_symlinked_task(self) -> None:
        prepared = self.agent.prepare(self.root / "run")
        outside = self.root / "outside"
        outside.mkdir()
        prepared.task.rmdir()
        prepared.task.symlink_to(outside, target_is_directory=True)

        with self.assertRaisesRegex(
            isolated_agent.IsolationError, "workspace is incomplete"
        ):
            self.agent.load(prepared.root)

    def test_run_uses_implementation_command_and_workspace(self) -> None:
        prepared = self.agent.prepare(self.root / "run")
        commands: list[list[str]] = []

        def execute(
            command: list[str], **kwargs: object
        ) -> subprocess.CompletedProcess[bytes]:
            commands.append(command)
            self.assertEqual(kwargs["cwd"], prepared.workspace_root)
            environment = kwargs["env"]
            assert isinstance(environment, dict)
            self.assertEqual(environment["W"], str(prepared.task))
            return subprocess.CompletedProcess(command, 0)

        result = self.agent.run(
            prepared,
            isolated_agent.Request("model", "high"),
            stdin=io.BytesIO(),
            stdout=io.BytesIO(),
            stderr=io.BytesIO(),
            executable=Path(sys.executable),
            command_runner=execute,
        )

        self.assertEqual(result.returncode, 0)
        self.assertEqual(commands, [[sys.executable, "run", "new", ""]])

    def test_codex_resumes_the_trace_session(self) -> None:
        prepared = self.agent.prepare(self.root / "run")
        trace = self.root / "trace.jsonl"
        trace.write_text('not-json\n{"type":"thread.started","thread_id":"thread-1"}\n')
        commands: list[list[str]] = []

        def execute(
            command: list[str], **unused: object
        ) -> subprocess.CompletedProcess[bytes]:
            commands.append(command)
            return subprocess.CompletedProcess(command, 0)

        isolated_agent.CODEX.run(
            prepared,
            isolated_agent.Request("model", "high", resume_session_id="thread-1"),
            stdin=io.BytesIO(),
            stdout=io.BytesIO(),
            stderr=io.BytesIO(),
            executable=Path("/codex"),
            command_runner=execute,
        )

        (command,) = commands
        exec_index = command.index("exec")
        self.assertEqual(command[exec_index : exec_index + 2], ["exec", "resume"])
        self.assertEqual(command[-2:], ["thread-1", "-"])
        self.assertEqual(isolated_agent.CODEX.extract_session_id(trace), "thread-1")
