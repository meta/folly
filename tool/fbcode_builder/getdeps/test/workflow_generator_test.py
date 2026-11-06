# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


"""Golden tests for `getdeps.py generate-github-actions`.

The Jinja-based emitter must produce byte-identical output to the legacy
out.write() emitter. Each scenario below names a fixtures/expected/<dir>/
that holds the expected per-OS .yml files; the test runs the generator
in-process and compares each emitted file to its fixture.

To regenerate fixtures after an intentional workflow-shape change, set
`UPDATE_FIXTURES=1`:

    UPDATE_FIXTURES=1 buck run //opensource/fbcode_builder/getdeps/test:test

Each scenario then writes its emitted .yml files into the fixtures
directory (creating the per-scenario subdir if needed) and the
assertions are skipped. Review the resulting `sl status` diff as the
change record.
"""

from __future__ import annotations

import argparse
import os
import tempfile
import unittest
from pathlib import Path
from typing import Any

from .. import cmd_base, workflow_generator
from ..buildopts import setup_build_options as real_setup_build_options


_FIXTURES_DIR: str = os.fspath(
    Path(os.path.abspath(__file__)).parent / "fixtures" / "expected"
)
_UPDATE_FIXTURES: bool = os.environ.get("UPDATE_FIXTURES") == "1"

_UPDATE_HINT: str = (
    "\n\nIf this drift is intentional, regenerate fixtures with:\n"
    "    UPDATE_FIXTURES=1 buck run"
    " //opensource/fbcode_builder/getdeps/test:test\n"
    "then review the resulting `sl status` diff."
)
_MANIFESTS_DIR: str = os.fspath(Path(os.path.abspath(__file__)).parent / "manifests")


def _make_args(project: str, output_dir: str, **overrides: Any) -> argparse.Namespace:
    """Build a Namespace mirroring what argparse would produce for
    `generate-github-actions`. Only override what each scenario needs."""
    base: dict[str, Any] = {
        "project": project,
        "output_dir": output_dir,
        "os_types": [],
        "no_tests": False,
        "enable_tests": True,
        "test_dependencies": False,
        "current_project": None,
        "src_dir": [],
        "build_dir": [],
        "install_dir": [],
        "project_install_prefix": [],
        "disallow_system_packages": False,
        "run_on_all_branches": False,
        "ubuntu_version": "24.04",
        "cpu_cores": None,
        "runs_on": None,
        "cron": None,
        "main_branch": [],
        "job_file_prefix": None,
        "job_name_prefix": None,
        "free_up_disk": False,
        "free_up_disk_before_build": False,
        "build_type": "RelWithDebInfo",
        "package_extra_cmake_defines": [],
        "scratch_path": None,
        "vcvars_path": None,
        "install_prefix": None,
        "num_jobs": None,
        "use_shipit": False,
        "facebook_internal": False,
        "shared_lib": False,
        "extra_cmake_defines": None,
        "allow_system_packages": False,
        "verbose": False,
        "skip_upload": True,
        "lfs_path": None,
        "build_skip_lfs_download": False,
        "schedule_type": None,
    }
    base.update(overrides)
    return argparse.Namespace(**base)


def _patched_setup_build_options(
    args: argparse.Namespace, host_type: Any = None
) -> Any:
    opts = real_setup_build_options(args, host_type)
    # In a Buck PAR, buildopts.py is wrapped and its derived
    # `fbcode_builder_dir` does not contain `manifests/`. Point at
    # the manifests resource shipped next to this test instead.
    if not Path(opts.manifests_dir).is_dir():
        opts.fbcode_builder_dir = os.fspath(Path(_MANIFESTS_DIR).parent)
    return opts


def _run(args: argparse.Namespace) -> None:
    orig_wg = workflow_generator.setup_build_options
    orig_cb = cmd_base.setup_build_options
    workflow_generator.setup_build_options = _patched_setup_build_options
    cmd_base.setup_build_options = _patched_setup_build_options
    try:
        workflow_generator.GenerateGitHubActionsCmd().run(args)
    finally:
        workflow_generator.setup_build_options = orig_wg
        cmd_base.setup_build_options = orig_cb


# (fixture_dir, project, extra-arg-overrides)
_SCENARIOS: list[tuple[str, str, dict[str, Any]]] = [
    ("xxhash_plain", "xxhash", {}),
    (
        "folly_shared_libs",
        "folly",
        {
            "os_types": ["linux"],
            "shared_lib": True,
            "job_file_prefix": "getdeps_shared-libs_",
            "job_name_prefix": "Shared Libs ",
            "free_up_disk": True,
            "allow_system_packages": True,
        },
    ),
    (
        "openr",
        "openr",
        {
            "cpu_cores": "8",
            "allow_system_packages": False,
            "disallow_system_packages": True,
            "run_on_all_branches": True,
        },
    ),
    (
        "rebalancer_linux",
        "rebalancer",
        {
            "os_types": ["linux"],
            "free_up_disk": True,
            "free_up_disk_before_build": True,
            "runs_on": "16-core-ubuntu",
            "extra_cmake_defines": '{"CMAKE_POSITION_INDEPENDENT_CODE":"ON"}',
            "package_extra_cmake_defines": [
                'fbthrift={"THRIFT_SERIALIZATION_ONLY":"ON"}'
            ],
            "allow_system_packages": True,
        },
    ),
]


class WorkflowGeneratorGoldenTest(unittest.TestCase):
    def _check_scenario(
        self, fixture_dir: str, project: str, overrides: dict[str, Any]
    ) -> None:
        expected_root = os.fspath(Path(_FIXTURES_DIR, fixture_dir))
        with tempfile.TemporaryDirectory() as tmp:
            args = _make_args(
                project,
                tmp,
                scratch_path=os.fspath(Path(tmp, "scratch")),
                **overrides,
            )
            _run(args)
            if _UPDATE_FIXTURES:
                Path(expected_root).mkdir(parents=True, exist_ok=True)
                for name in sorted(os.listdir(tmp)):
                    src = os.fspath(Path(tmp, name))
                    if not Path(src).is_file():
                        continue
                    with open(src) as f:
                        actual = f.read()
                    with open(os.fspath(Path(expected_root, name)), "w") as f:
                        f.write(actual)
                return
            for name in sorted(os.listdir(expected_root)):
                with open(os.fspath(Path(expected_root, name))) as f:
                    expected = f.read()
                actual_path = os.fspath(Path(tmp, name))
                self.assertTrue(
                    Path(actual_path).exists(),
                    f"generator did not emit expected file {name!r} for "
                    f"scenario {fixture_dir!r}{_UPDATE_HINT}",
                )
                with open(actual_path) as f:
                    actual = f.read()
                self.assertEqual(
                    expected,
                    actual,
                    f"output for {fixture_dir}/{name} drifted from fixture"
                    f"{_UPDATE_HINT}",
                )

    def test_xxhash_plain(self) -> None:
        self._check_scenario(*_SCENARIOS[0])

    def test_folly_shared_libs(self) -> None:
        self._check_scenario(*_SCENARIOS[1])

    def test_openr(self) -> None:
        self._check_scenario(*_SCENARIOS[2])

    def test_rebalancer_linux(self) -> None:
        self._check_scenario(*_SCENARIOS[3])
