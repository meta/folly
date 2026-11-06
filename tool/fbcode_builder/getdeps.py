#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""OSS-facing entry point for getdeps.

Real CLI logic lives in `getdeps/cli.py`; this shim only exists so OSS
users can run `python3 getdeps.py ...` directly. Inside Buck, invoke
`buck run //opensource/fbcode_builder:getdeps -- ...` instead, which
targets `getdeps.cli:main` and brings third-party deps along."""

import os
import sys

if sys.version_info < (3, 6):
    sys.exit("getdeps requires Python 3.6 or newer")

# Make `getdeps` resolve to the package sitting next to this script.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from getdeps.cli import main  # noqa: E402


if __name__ == "__main__":
    sys.exit(main())
