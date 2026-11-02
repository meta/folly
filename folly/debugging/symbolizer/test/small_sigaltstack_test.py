#!/usr/bin/env python3
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

import os
import resource
import signal
import subprocess
import unittest


ASAN_SWAPCONTEXT_WARNING = (
    "ASan doesn't fully support makecontext/swapcontext functions"
)


class SmallSigAltStackTest(unittest.TestCase):
    def setUp(self) -> None:
        self.helper = os.environ["SMALL_SIGALTSTACK_CRASH"]
        with open(self.helper, "rb") as binary:
            contents = binary.read()
        asan = b"__asan_init" in contents
        tsan = b"__tsan_init" in contents
        self.sanitized = asan or tsan
        self.raw_fallback = tsan

    def test_installation_is_quiet(self) -> None:
        env = dict(os.environ)
        env.pop("LD_BIND_NOW", None)
        env.pop("LD_DEBUG", None)

        result = subprocess.run(
            [self.helper, "--install-only"],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=30,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b"")
        self.assertEqual(result.stderr, b"")

    def test_crash_prints_trace(self) -> None:
        env = dict(os.environ)
        env.pop("LD_BIND_NOW", None)
        env.pop("LD_DEBUG", None)
        if not self.sanitized:
            env["LD_DEBUG"] = "bindings"

        def disable_core_dumps() -> None:
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

        result = subprocess.run(
            [self.helper],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            preexec_fn=disable_core_dumps,
            timeout=30,
        )
        stderr = result.stderr.decode("utf-8", errors="replace")

        self.assertEqual(result.returncode, -signal.SIGILL, stderr[-20000:])
        self.assertNotIn(ASAN_SWAPCONTEXT_WARNING, stderr)
        _, header, trace = stderr.partition("*** Signal 4 (SIGILL)")
        self.assertTrue(header, stderr[-20000:])
        if self.raw_fallback:
            self.assertIn("safe mode, symbolizer not available", trace)
        else:
            self.assertIn("SmallSigAltStackCrash.cpp:", trace, stderr[-20000:])
