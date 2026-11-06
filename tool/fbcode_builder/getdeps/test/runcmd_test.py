# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import os
import unittest
from unittest.mock import patch

from ..runcmd import make_memory_limit_preexec_fn


@patch("sys.platform", "linux")
class MemoryLimitTest(unittest.TestCase):
    def test_limit_applied_by_default(self) -> None:
        with patch.dict(os.environ, clear=True):
            self.assertIsNotNone(make_memory_limit_preexec_fn(1024))

    def test_limit_skipped_when_opted_out(self) -> None:
        with patch.dict(os.environ, {"GETDEPS_NO_MEMORY_LIMIT": "1"}, clear=True):
            self.assertIsNone(make_memory_limit_preexec_fn(1024))
