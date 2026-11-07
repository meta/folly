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
from pathlib import Path

from ..scripts import isolated_codex


FAKE_CODEX = """import json
import os
import sys
from pathlib import Path

with Path(os.environ["FAKE_CODEX_LOG"]).open("a") as output:
    output.write(json.dumps({"executable": Path(sys.argv[0]).parent.name, "args": sys.argv[1:]}) + "\\n")
Path(sys.argv[sys.argv.index("--output-last-message") + 1]).write_text("response\\n")
print(json.dumps({"type": "thread.started", "thread_id": "thread-1"}))
print(json.dumps({"type": "turn.completed", "usage": {"output_tokens": 1}}))
"""
FAKE_GIT = """import sys
from pathlib import Path

(Path(sys.argv[-1]) / ".git").mkdir()
"""


class IsolatedCodexTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def fake_codex(self, name: str) -> Path:
        executable = self.root / name / "codex"
        executable.parent.mkdir()
        executable.write_text(f"#!/usr/bin/python3 -I\n{FAKE_CODEX}")
        executable.chmod(0o755)
        git = executable.with_name("git")
        git.write_text(f"#!/usr/bin/python3 -I\n{FAKE_GIT}")
        git.chmod(0o755)
        return executable

    def test_cli_records_and_resumes_exact_session(self) -> None:
        run_root = self.root / "run"
        prompt = self.root / "prompt.md"
        prompt.write_text("Prompt")
        log = self.root / "codex.jsonl"
        first = self.fake_codex("first")
        second = self.fake_codex("second")

        def run_cli(
            executable: Path, *arguments: str
        ) -> subprocess.CompletedProcess[str]:
            environment = os.environ.copy()
            environment["HOME"] = str(self.root / "home")
            environment["PATH"] = os.pathsep.join(
                (str(executable.parent), environment["PATH"])
            )
            environment["FAKE_CODEX_LOG"] = str(log)
            return subprocess.run(
                [str(Path(isolated_codex.__file__)), *arguments],
                check=False,
                env=environment,
                capture_output=True,
                text=True,
            )

        prepared = run_cli(first, "prepare", str(run_root))
        started = run_cli(
            first,
            "run",
            str(run_root),
            "--prompt",
            str(prompt),
            "--name",
            "opening",
            "--model",
            "model",
            "--effort",
            "high",
        )
        resumed = run_cli(
            second,
            "resume",
            str(run_root),
            "--prompt",
            str(prompt),
            "--name",
            "full",
        )

        self.assertEqual(prepared.returncode, 0, prepared.stderr)
        self.assertEqual(prepared.stdout.strip(), str(run_root / "workspace/task"))
        for name in (".cache", "packages"):
            self.assertEqual(
                (run_root / "agent-home" / name).resolve(),
                (self.root / "home" / "isolated_agents_cache" / name).resolve(),
            )
        self.assertEqual(started.returncode, 0, started.stderr)
        self.assertEqual(resumed.returncode, 0, resumed.stderr)
        self.assertEqual(
            json.loads((run_root / isolated_codex.STATE_FILE).read_text()),
            {
                "effort": "high",
                "engine": "codex",
                "executable": str(first),
                "model": "model",
                "session_id": "thread-1",
            },
        )
        invocations = [json.loads(line) for line in log.read_text().splitlines()]
        self.assertEqual(
            [invocation["executable"] for invocation in invocations],
            ["first", "first"],
        )
        self.assertIn("resume", invocations[1]["args"])
        self.assertIn("thread-1", invocations[1]["args"])
        resume_arguments = invocations[1]["args"]
        self.assertEqual(
            resume_arguments[resume_arguments.index("--model") + 1], "model"
        )
        self.assertIn('model_reasoning_effort="high"', resume_arguments)
        for name in ("opening", "full"):
            attempt = run_root / "attempts" / name
            self.assertEqual(
                (attempt / "prompt.md").read_text(),
                isolated_codex.TASK_ROOT_INSTRUCTION + "Prompt",
            )
            self.assertEqual((attempt / "response.md").read_text(), "response\n")
            self.assertTrue((attempt / "trace.jsonl").is_file())
            self.assertEqual((attempt / "err.txt").read_text(), "")
