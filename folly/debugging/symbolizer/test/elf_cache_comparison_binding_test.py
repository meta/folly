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


def moving(symbols):
    return sorted(s for s in symbols if "_Rb_tree" in s or s in ("memmove", "memcpy"))


class ElfCacheComparisonBindingTest(unittest.TestCase):
    def test_comparison_warmup(self) -> None:
        helper = os.environ["ELF_CACHE_COMPARISON"]
        with open(helper, "rb") as binary:
            contents = binary.read()
        sanitized = b"__asan_init" in contents or b"__tsan_init" in contents
        env = dict(os.environ)
        env.pop("LD_BIND_NOW", None)
        env.pop("LD_DEBUG", None)
        if not sanitized:
            env["LD_DEBUG"] = "bindings"

        cold_inserts: set[str] = set()
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
                _, begin, after = stderr.partition("comparison-begin\n")
                trace, end, _ = after.partition("comparison-end\n")
                self.assertTrue(begin, stderr)
                self.assertTrue(end, stderr)
                _, begin, after = stderr.partition("lookup-begin\n")
                insert_trace, end, _ = after.partition("lookup-end\n")
                self.assertTrue(begin, stderr)
                self.assertTrue(end, stderr)
                if not sanitized:
                    bound = re.compile(r"normal symbol `([^']+)'")
                    symbols = bound.findall(trace)
                    comparisons = [s for s in symbols if s in ("memcmp", "strlen")]
                    inserts = bound.findall(insert_trace)
                    if warm:
                        self.assertEqual(comparisons, [], stderr)
                        if not moving(cold_inserts):
                            self.skipTest(
                                "the cold run bound no tree or copy symbols here"
                            )
                        self.assertEqual(moving(inserts), [], stderr)
                    else:
                        self.assertIn("memcmp", comparisons, stderr)
                        cold_inserts = set(inserts)
