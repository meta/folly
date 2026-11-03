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
import re
import subprocess
import unittest


WARMED_FUNCTIONS = {
    "open",
    "open64",
    "fstat",
    "fstat64",
    "__fxstat",
    "__fxstat64",
    "__fstat64_time64",
    "lseek",
    "lseek64",
    "pread",
    "pread64",
    "read",
    "close",
    "readlink",
    "memcmp",
    "strrchr",
    "sigfillset",
    "sigprocmask",
    "mmap",
    "mmap64",
    "munmap",
    "memcpy",
    "strlcpy",
    "strlen",
}


class SignalSafeLibcWarmUpTest(unittest.TestCase):
    def test_error_probes_warm_successful_operations(self) -> None:
        helper = os.environ["SIGNAL_SAFE_LIBC_WARMUP"]
        with open(helper, "rb") as binary:
            contents = binary.read()
        sanitized = b"__asan_init" in contents or b"__tsan_init" in contents
        env = dict(os.environ)
        env.pop("LD_BIND_NOW", None)
        env.pop("LD_DEBUG", None)
        if not sanitized:
            env["LD_DEBUG"] = "bindings"
        for warm in (False, True):
            with self.subTest(warm=warm):
                args = [helper] if warm else [helper, "cold"]
                result = subprocess.run(
                    args,
                    env=env,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    timeout=30,
                )
                stderr = result.stderr.decode("utf-8", errors="replace")
                self.assertEqual(result.returncode, 0, stderr)
                self.assertEqual(result.stdout, b"")
                _, begin, after = stderr.partition("libc-operations-begin\n")
                trace, end, _ = after.partition("libc-operations-end\n")
                self.assertTrue(begin, stderr)
                self.assertTrue(end, stderr)
                if not sanitized:
                    bound = set(re.findall(r"normal symbol `([^']+)'", trace))
                    relevant = {
                        symbol
                        for symbol in bound
                        if symbol in WARMED_FUNCTIONS or "signal_safe_libc" in symbol
                    }
                    if warm:
                        self.assertEqual(relevant, set(), stderr)
                    else:
                        self.assertTrue(relevant, stderr)
