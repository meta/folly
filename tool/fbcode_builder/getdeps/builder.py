#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


from __future__ import annotations

import glob
import json
import os
import os.path
import pathlib
import re
import shutil
import stat
import subprocess
import sys
import typing
from collections.abc import Callable, Sequence
from shlex import quote as shellquote

from .copytree import rmtree_more, simple_copytree
from .dyndeps import create_dyn_dep_munger
from .envfuncs import add_path_entry, Env, path_search
from .fetcher import copy_if_different, is_public_commit
from .runcmd import make_memory_limit_preexec_fn, run_cmd
from .shared_lib import apply_shared_lib_dep_env

if typing.TYPE_CHECKING:
    from .buildopts import BuildOptions
    from .dyndeps import DepBase
    from .load import ManifestLoader
    from .manifest import ManifestContext, ManifestParser


class BuilderBase:
    def __init__(
        self,
        loader: ManifestLoader,
        dep_manifests: list[ManifestParser],
        build_opts: BuildOptions,
        ctx: ManifestContext,
        manifest: ManifestParser,
        src_dir: str,
        build_dir: str | None,
        inst_dir: str,
        env: Env | None = None,
        final_install_prefix: str | None = None,
    ) -> None:
        self.env: Env = Env()
        if env:
            #  `Env`.
            self.env.update(env)

        subdir: str | None = manifest.get("build", "subdir", ctx=ctx)
        if subdir:
            src_dir = os.fspath(pathlib.Path(src_dir, subdir))

        self.patchfile: str | None = manifest.get("build", "patchfile", ctx=ctx)
        self.patchfile_opts: str = (
            manifest.get("build", "patchfile_opts", ctx=ctx) or ""
        )
        self.ctx: ManifestContext = ctx
        self.src_dir: str = src_dir
        self.build_dir: str = build_dir or src_dir
        self.inst_dir: str = inst_dir
        self.build_opts: BuildOptions = build_opts
        self.manifest: ManifestParser = manifest
        self.final_install_prefix: str | None = final_install_prefix
        self.loader: ManifestLoader = loader
        self.dep_manifests: list[ManifestParser] = dep_manifests
        self.install_dirs: list[str] = [
            loader.get_project_install_dir(m) for m in dep_manifests
        ]

    def _get_cmd_prefix(self) -> list[str]:
        if self.build_opts.is_windows():
            vcvarsall = self.build_opts.get_vcvars_path()
            if vcvarsall is not None:
                # Since it sets rather a large number of variables we mildly abuse
                # the cmd quoting rules to assemble a command that calls the script
                # to prep the environment and then triggers the actual command that
                # we wanted to run.

                # Due to changes in vscrsall.bat, it now reports an ERRORLEVEL of 1
                # even when succeeding. This occurs when an extension is not present.
                # To continue, we must ignore the ERRORLEVEL returned. We do this by
                # wrapping the call in a batch file that always succeeds.
                wrapper = os.fspath(pathlib.Path(self.build_dir, "succeed.bat"))
                with open(wrapper, "w") as f:
                    f.write("@echo off\n")
                    f.write(f'call "{vcvarsall}" amd64\n')
                    f.write("set ERRORLEVEL=0\n")
                    f.write("exit /b 0\n")
                return [wrapper, "&&"]
        return []

    def _check_cmd(self, cmd: list[str], **kwargs: object) -> None:
        """Run the command and abort on failure"""
        # pyre-fixme[6]: For 2nd argument expected `Optional[Env]` but got `object`.
        # pyre-fixme[6]: For 2nd argument expected `Optional[str]` but got `object`.
        # pyre-fixme[6]: For 2nd argument expected `bool` but got `object`.
        rc = self._run_cmd(cmd, **kwargs)
        if rc != 0:
            raise RuntimeError(f"Failure exit code {rc} for command {cmd}")

    def _run_cmd(
        self,
        cmd: list[str],
        cwd: str | None = None,
        env: Env | None = None,
        use_cmd_prefix: bool = True,
        allow_fail: bool = False,
        preexec_fn: Callable[[], None] | None = None,
    ) -> int:
        if env:
            e = self.env.copy()
            #  `Env`.
            e.update(env)
            env = e
        else:
            env = self.env

        if use_cmd_prefix:
            cmd_prefix = self._get_cmd_prefix()
            if cmd_prefix:
                cmd = cmd_prefix + cmd

        log_file = os.fspath(pathlib.Path(self.build_dir, "getdeps_build.log"))
        return run_cmd(
            cmd=cmd,
            env=env,
            cwd=cwd or self.build_dir,
            log_file=log_file,
            allow_fail=allow_fail,
            preexec_fn=preexec_fn,
        )

    def _reconfigure(self, reconfigure: bool) -> bool:
        if self.build_dir is not None:
            if not pathlib.Path(self.build_dir).is_dir():
                pathlib.Path(self.build_dir).mkdir(parents=True, exist_ok=True)
                reconfigure = True
        return reconfigure

    def _apply_patchfile(self) -> None:
        if self.patchfile is None:
            return
        patched_sentinel_file = pathlib.Path(self.src_dir + "/.getdeps_patched")
        if patched_sentinel_file.exists():
            return
        old_wd = os.getcwd()
        os.chdir(self.src_dir)
        # Apply patches from the git repo root so paths resolve correctly
        # even when src_dir is a subdirectory of the repo.
        try:
            git_root = subprocess.check_output(
                ["git", "rev-parse", "--show-toplevel"], text=True
            ).strip()
            os.chdir(git_root)
        except subprocess.CalledProcessError:
            pass  # not a git repo, stay in src_dir
        print(f"Patching {self.manifest.name} with {self.patchfile} in {os.getcwd()}")
        patchfile = os.fspath(
            pathlib.Path(
                self.build_opts.fbcode_builder_dir,
                "patches",
                #  but got `Optional[str]`.
                self.patchfile,
            )
        )
        patchcmd = ["git", "apply", "--ignore-space-change"]
        if self.patchfile_opts:
            patchcmd.append(self.patchfile_opts)
        try:
            subprocess.check_call(patchcmd + [patchfile])
        except subprocess.CalledProcessError:
            raise ValueError(f"Failed to apply patch to {self.manifest.name}")
        os.chdir(old_wd)
        patched_sentinel_file.touch()

    def prepare(self, reconfigure: bool) -> None:
        print("Preparing %s..." % self.manifest.name)
        reconfigure = self._reconfigure(reconfigure)
        self._apply_patchfile()
        self._prepare(reconfigure=reconfigure)

    def debug(self, reconfigure: bool) -> None:
        reconfigure = self._reconfigure(reconfigure)
        self._apply_patchfile()
        self._prepare(reconfigure=reconfigure)
        env = self._compute_env()
        print("Starting a shell in %s, ^D to exit..." % self.build_dir)
        # TODO: print the command to run the build
        shell = ["powershell.exe"] if sys.platform == "win32" else ["/bin/sh", "-i"]
        self._run_cmd(shell, cwd=self.build_dir, env=env)

    def printenv(self, reconfigure: bool) -> None:
        """print the environment in a shell sourcable format"""
        reconfigure = self._reconfigure(reconfigure)
        self._apply_patchfile()
        self._prepare(reconfigure=reconfigure)
        env = self._compute_env(env=Env(src={}))
        prefix = "export "
        sep = ":"
        expand = "$"
        expandpost = ""
        if self.build_opts.is_windows():
            prefix = "SET "
            sep = ";"
            expand = "%"
            expandpost = "%"
        for k, v in sorted(env.items()):
            existing = os.environ.get(k, None)
            if k.endswith("PATH") and existing:
                v = shellquote(v) + sep + f"{expand}{k}{expandpost}"
            else:
                v = shellquote(v)
            print("%s%s=%s" % (prefix, k, v))

    def build(self, reconfigure: bool) -> None:
        print("Building %s..." % self.manifest.name)
        reconfigure = self._reconfigure(reconfigure)
        self._apply_patchfile()
        self._prepare(reconfigure=reconfigure)
        self._build(reconfigure=reconfigure)

        if self.build_opts.free_up_disk:
            # don't clean --src-dir=. case as user may want to build again or
            # run tests on the build; vendored sources are ours to clean up after.
            managed = [self.build_opts.scratch_dir]
            if self.build_opts.vendor_dir:
                managed.append(self.build_opts.vendor_dir)
            # Compare by path component so a sibling directory that merely
            # shares a name prefix (e.g. /tmp/vendor-copy vs /tmp/vendor)
            # is not mistaken for a managed source tree.
            if any(
                self.src_dir == d or self.src_dir.startswith(d + os.sep)
                for d in managed
            ) and os.path.isdir(self.build_dir):
                if os.path.islink(self.build_dir):
                    os.remove(self.build_dir)
                else:
                    rmtree_more(self.build_dir)
        elif self.build_opts.is_windows():
            # On Windows, emit a wrapper script that can be used to run build artifacts
            # directly from the build directory, without installing them.  On Windows $PATH
            # needs to be updated to include all of the directories containing the runtime
            # library dependencies in order to run the binaries.
            script_path = self.get_dev_run_script_path()
            dep_munger = create_dyn_dep_munger(
                self.build_opts, self._compute_env(), self.install_dirs
            )
            dep_dirs = self.get_dev_run_extra_path_dirs(dep_munger)
            # pyre-fixme[16]: Optional type has no attribute `emit_dev_run_script`.
            dep_munger.emit_dev_run_script(script_path, dep_dirs)

    @property
    def _job_weight_mib(self) -> int:
        # This is a hack, but we don't have a "defaults manifest" that we can
        # customize per platform.
        # TODO: Introduce some sort of defaults config that can select by
        # platform, just like manifest contexts.
        if sys.platform.startswith("freebsd"):
            # clang on FreeBSD is quite memory-efficient.
            default_job_weight = 512
        else:
            # 1.5 GiB is a lot to assume, but it's typical of Facebook-style C++.
            # Some manifests are even heavier and should override.
            default_job_weight = 1536
        return int(
            self.manifest.get(
                "build", "job_weight_mib", str(default_job_weight), ctx=self.ctx
            )
            or default_job_weight
        )

    @property
    def num_jobs(self) -> int:
        return self.build_opts.get_num_jobs(self._job_weight_mib)

    @property
    def memory_limit_preexec_fn(self) -> Callable[[], None] | None:
        """Return a preexec_fn that caps per-process virtual memory.

        Uses the same job_weight_mib that controls parallelism, so the memory
        limit is consistent with the parallelism budget.
        """
        return make_memory_limit_preexec_fn(self._job_weight_mib)

    def run_tests(
        self,
        schedule_type: str,
        owner: str | None,
        test_filter: str | None,
        test_exclude: str | None,
        retry: int,
        no_testpilot: bool,
        timeout: int | None = None,
    ) -> None:
        """Execute any tests that we know how to run.  If they fail,
        raise an exception."""
        pass

    def _prepare(self, reconfigure: bool) -> None:
        """Prepare the build. Useful when need to generate config,
        but builder is not the primary build system.
        e.g. cargo when called from cmake"""
        pass

    def _build(self, reconfigure: bool) -> None:
        """Perform the build.
        reconfigure will be set to true if the fetcher determined
        that the sources have changed in such a way that the build
        system needs to regenerate its rules."""
        pass

    def _compute_env(self, env: Env | None = None) -> Env:
        if env is None:
            env = self.env
        # CMAKE_PREFIX_PATH is only respected when passed through the
        # environment, so we construct an appropriate path to pass down
        computed = self.build_opts.compute_env_for_install_dirs(
            self.loader,
            self.dep_manifests,
            self.ctx,
            env=env,
            manifest=self.manifest,
        )
        apply_shared_lib_dep_env(computed, self.build_opts, self.manifest.name)
        return computed

    def get_dev_run_script_path(self) -> str:
        assert self.build_opts.is_windows()
        return os.fspath(pathlib.Path(self.build_dir, "run.ps1"))

    def get_dev_run_extra_path_dirs(
        self, dep_munger: DepBase | None = None
    ) -> list[str]:
        assert self.build_opts.is_windows()
        if dep_munger is None:
            dep_munger = create_dyn_dep_munger(
                self.build_opts, self._compute_env(), self.install_dirs
            )
        # pyre-fixme[16]: Optional type has no attribute `compute_dependency_paths`.
        return dep_munger.compute_dependency_paths(self.build_dir)


class MakeBuilder(BuilderBase):
    def __init__(
        self,
        loader: ManifestLoader,
        dep_manifests: list[ManifestParser],
        build_opts: BuildOptions,
        ctx: ManifestContext,
        manifest: ManifestParser,
        src_dir: str,
        build_dir: str,
        inst_dir: str,
        build_args: list[str] | None,
        install_args: list[str] | None,
        test_args: list[str] | None,
    ) -> None:
        super(MakeBuilder, self).__init__(
            loader,
            dep_manifests,
            build_opts,
            ctx,
            manifest,
            src_dir,
            build_dir,
            inst_dir,
        )
        self.build_args: list[str] = build_args or []
        self.install_args: list[str] = install_args or []
        self.test_args: list[str] | None = test_args

    @property
    def _make_binary(self) -> str | None:
        return self.manifest.get("build", "make_binary", "make", ctx=self.ctx)

    def _get_prefix(self) -> list[str]:
        return ["PREFIX=" + self.inst_dir, "prefix=" + self.inst_dir]

    def _build(self, reconfigure: bool) -> None:

        env = self._compute_env()

        # Need to ensure that PREFIX is set prior to install because
        # libbpf uses it when generating its pkg-config file.
        # The lowercase prefix is used by some projects.
        cmd = (
            [self._make_binary, "-j%s" % self.num_jobs]
            + self.build_args
            + self._get_prefix()
        )
        # pyre-fixme[6]: For 1st argument expected `List[str]` but got
        #  `List[Optional[str]]`.
        self._check_cmd(cmd, env=env)

        install_cmd = [self._make_binary] + self.install_args + self._get_prefix()
        # pyre-fixme[6]: For 1st argument expected `List[str]` but got
        #  `List[Optional[str]]`.
        self._check_cmd(install_cmd, env=env)

        # bz2's Makefile doesn't install its .so properly
        if self.manifest and self.manifest.name == "bz2":
            libdir = os.fspath(pathlib.Path(self.inst_dir, "lib"))
            srcpattern = os.fspath(pathlib.Path(self.src_dir, "lib*.so.*"))
            print(f"copying to {libdir} from {srcpattern}")
            for file in glob.glob(srcpattern):
                shutil.copy(file, libdir)

    def run_tests(
        self,
        schedule_type: str,
        owner: str | None,
        test_filter: str | None,
        test_exclude: str | None,
        retry: int,
        no_testpilot: bool,
        timeout: int | None = None,
    ) -> None:
        if not self.test_args:
            return

        env = self._compute_env()
        if test_filter:
            env["GETDEPS_TEST_FILTER"] = test_filter
        else:
            env["GETDEPS_TEST_FILTER"] = ""

        if retry:
            # pyre-fixme[6]: Expected `str` but got `int`.
            env["GETDEPS_TEST_RETRY"] = retry
        else:
            # pyre-fixme[6]: Expected `str` but got `int`.
            env["GETDEPS_TEST_RETRY"] = 0

        if timeout is not None:
            env["GETDEPS_TEST_TIMEOUT"] = str(timeout)

        cmd = (
            [self._make_binary, "-j%s" % self.num_jobs]
            #  `list[Optional[str]]` and `Optional[list[str]]`.
            + self.test_args
            + self._get_prefix()
        )
        # pyre-fixme[6]: For 1st argument expected `List[str]` but got
        #  `List[Optional[str]]`.
        self._check_cmd(cmd, allow_fail=False, env=env)


class CMakeBootStrapBuilder(MakeBuilder):
    def _build(self, reconfigure: bool) -> None:
        self._check_cmd(
            [
                "./bootstrap",
                "--prefix=" + self.inst_dir,
                f"--parallel={self.num_jobs}",
            ]
        )
        super(CMakeBootStrapBuilder, self)._build(reconfigure)


class AutoconfBuilder(BuilderBase):
    def __init__(
        self,
        loader: ManifestLoader,
        dep_manifests: list[ManifestParser],
        build_opts: BuildOptions,
        ctx: ManifestContext,
        manifest: ManifestParser,
        src_dir: str,
        build_dir: str,
        inst_dir: str,
        args: list[str] | None,
        conf_env_args: dict[str, list[str]] | None,
    ) -> None:
        super(AutoconfBuilder, self).__init__(
            loader,
            dep_manifests,
            build_opts,
            ctx,
            manifest,
            src_dir,
            build_dir,
            inst_dir,
        )
        self.args: list[str] = args or []
        if "--disable-shared" not in self.args and "--enable-shared" not in self.args:
            self.args.append("--disable-shared")
        self.conf_env_args: dict[str, list[str]] = conf_env_args or {}

    @property
    def _make_binary(self) -> str | None:
        return self.manifest.get("build", "make_binary", "make", ctx=self.ctx)

    def _build(self, reconfigure: bool) -> None:
        configure_path = os.fspath(pathlib.Path(self.src_dir, "configure"))
        autogen_path = os.fspath(pathlib.Path(self.src_dir, "autogen.sh"))

        env = self._compute_env()

        # Some configure scripts need additional env values passed derived from cmds
        for k, cmd_args in self.conf_env_args.items():
            out = (
                subprocess.check_output(cmd_args, env=dict(env.items()))
                .decode("utf-8")
                .strip()
            )
            if out:
                env.set(k, out)

        if not pathlib.Path(configure_path).exists():
            print("%s doesn't exist, so reconfiguring" % configure_path)
            # This libtoolize call is a bit gross; the issue is that
            # `autoreconf` as invoked by libsodium's `autogen.sh` doesn't
            # seem to realize that it should invoke libtoolize and then
            # error out when the configure script references a libtool
            # related symbol.
            self._check_cmd(["libtoolize"], cwd=self.src_dir, env=env)

            # We generally prefer to call the `autogen.sh` script provided
            # by the project on the basis that it may know more than plain
            # autoreconf does.
            if pathlib.Path(autogen_path).exists():
                self._check_cmd(["bash", autogen_path], cwd=self.src_dir, env=env)
            else:
                self._check_cmd(["autoreconf", "-ivf"], cwd=self.src_dir, env=env)
        configure_cmd = [configure_path, "--prefix=" + self.inst_dir] + self.args
        self._check_cmd(configure_cmd, env=env)
        only_install = self.manifest.get("build", "only_install", ctx=self.ctx)
        if not only_install or only_install.lower() == "false":
            # pyre-fixme[6]: For 1st argument expected `List[str]` but got
            #  `List[Optional[str]]`.
            self._check_cmd([self._make_binary, "-j%s" % self.num_jobs], env=env)
        # pyre-fixme[6]: For 1st argument expected `List[str]` but got
        #  `List[Union[str, None, str]]`.
        self._check_cmd([self._make_binary, "install"], env=env)


class Iproute2Builder(BuilderBase):
    # ./configure --prefix does not work for iproute2.
    # Thus, explicitly copy sources from src_dir to build_dir, build,
    # and then install to inst_dir using DESTDIR
    # lastly, also copy include from build_dir to inst_dir
    def __init__(
        self,
        loader: ManifestLoader,
        dep_manifests: list[ManifestParser],
        build_opts: BuildOptions,
        ctx: ManifestContext,
        manifest: ManifestParser,
        src_dir: str,
        build_dir: str,
        inst_dir: str,
    ) -> None:
        super(Iproute2Builder, self).__init__(
            loader,
            dep_manifests,
            build_opts,
            ctx,
            manifest,
            src_dir,
            build_dir,
            inst_dir,
        )

    def _build(self, reconfigure: bool) -> None:
        configure_path = os.fspath(pathlib.Path(self.src_dir, "configure"))
        env = self.env.copy()
        self._check_cmd([configure_path], env=env)
        shutil.rmtree(self.build_dir)
        shutil.copytree(self.src_dir, self.build_dir)
        self._check_cmd(["make", "-j%s" % self.num_jobs], env=env)
        install_cmd = ["make", "install", "DESTDIR=" + self.inst_dir]

        for d in ["include", "lib"]:
            if not pathlib.Path(self.inst_dir, d).is_dir():
                shutil.copytree(
                    pathlib.Path(self.build_dir, d), pathlib.Path(self.inst_dir, d)
                )

        self._check_cmd(install_cmd, env=env)


class MesonBuilder(BuilderBase):
    # MesonBuilder assumes that meson build tool has already been installed on
    # the machine.
    def __init__(
        self,
        loader: ManifestLoader,
        dep_manifests: list[ManifestParser],
        build_opts: BuildOptions,
        ctx: ManifestContext,
        manifest: ManifestParser,
        src_dir: str,
        build_dir: str,
        inst_dir: str,
    ) -> None:
        super(MesonBuilder, self).__init__(
            loader,
            dep_manifests,
            build_opts,
            ctx,
            manifest,
            src_dir,
            build_dir,
            inst_dir,
        )

    def _build(self, reconfigure: bool) -> None:
        env = self._compute_env()
        meson: str | None = path_search(env, "meson")
        if meson is None:
            raise Exception("Failed to find Meson")

        setup_args = self.manifest.get_section_as_args("meson.setup_args", self.ctx)

        # Meson builds typically require setup, compile, and install steps.
        # During this setup step we ensure that the static library is built and
        # the prefix is empty.
        self._check_cmd(
            [
                meson,
                "setup",
            ]
            + setup_args
            + [
                self.build_dir,
                self.src_dir,
            ]
        )

        # Compile step needs to satisfy the build directory that was previously
        # prepared during setup.
        self._check_cmd([meson, "compile", "-C", self.build_dir])

        # Install step
        self._check_cmd(
            [meson, "install", "-C", self.build_dir, "--destdir", self.inst_dir]
        )


class CMakeBuilder(BuilderBase):
    MANUAL_BUILD_SCRIPT = """\
#!{sys.executable}


import argparse
import subprocess
import sys

CMAKE = {cmake!r}
CTEST = {ctest!r}
SRC_DIR = {src_dir!r}
BUILD_DIR = {build_dir!r}
INSTALL_DIR = {install_dir!r}
CMD_PREFIX = {cmd_prefix!r}
CMAKE_ENV = {env_str}
CMAKE_DEFINE_ARGS = {define_args_str}


def get_jobs_argument(num_jobs_arg: int) -> str:
    if num_jobs_arg > 0:
        return "-j" + str(num_jobs_arg)

    import multiprocessing
    num_jobs = multiprocessing.cpu_count() // 2
    return "-j" + str(num_jobs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
      "cmake_args",
      nargs=argparse.REMAINDER,
      help='Any extra arguments after an "--" argument will be passed '
      "directly to CMake."
    )
    ap.add_argument(
      "--mode",
      choices=["configure", "build", "install", "test"],
      default="configure",
      help="The mode to run: configure, build, or install.  "
      "Defaults to configure",
    )
    ap.add_argument(
      "--build",
      action="store_const",
      const="build",
      dest="mode",
      help="An alias for --mode=build",
    )
    ap.add_argument(
      "-j",
      "--num-jobs",
      action="store",
      type=int,
      default=0,
      help="Run the build or tests with the specified number of parallel jobs",
    )
    ap.add_argument(
      "--install",
      action="store_const",
      const="install",
      dest="mode",
      help="An alias for --mode=install",
    )
    ap.add_argument(
      "--test",
      action="store_const",
      const="test",
      dest="mode",
      help="An alias for --mode=test",
    )
    args = ap.parse_args()

    # Strip off a leading "--" from the additional CMake arguments
    if args.cmake_args and args.cmake_args[0] == "--":
        args.cmake_args = args.cmake_args[1:]

    env = CMAKE_ENV

    if args.mode == "configure":
        full_cmd = CMD_PREFIX + [CMAKE, SRC_DIR] + CMAKE_DEFINE_ARGS + args.cmake_args
    elif args.mode in ("build", "install"):
        target = "all" if args.mode == "build" else "install"
        full_cmd = CMD_PREFIX + [
                CMAKE,
                "--build",
                BUILD_DIR,
                "--target",
                target,
                "--config",
                "{build_type}",
                get_jobs_argument(args.num_jobs),
        ] + args.cmake_args
    elif args.mode == "test":
        full_cmd = CMD_PREFIX + [
            {dev_run_script}CTEST,
            "--output-on-failure",
            get_jobs_argument(args.num_jobs),
        ] + args.cmake_args
    else:
        ap.error("unknown invocation mode: %s" % (args.mode,))

    cmd_str = " ".join(full_cmd)
    print("Running: %r" % (cmd_str,))
    proc = subprocess.run(full_cmd, env=env, cwd=BUILD_DIR)
    sys.exit(proc.returncode)


if __name__ == "__main__":
    main()
"""

    def __init__(
        self,
        loader: ManifestLoader,
        dep_manifests: list[ManifestParser],
        build_opts: BuildOptions,
        ctx: ManifestContext,
        manifest: ManifestParser,
        src_dir: str,
        build_dir: str,
        inst_dir: str,
        defines: dict[str, str] | None,
        final_install_prefix: str | None = None,
        extra_cmake_defines: dict[str, str] | None = None,
        cmake_targets: list[str] | None = None,
    ) -> None:
        super(CMakeBuilder, self).__init__(
            loader,
            dep_manifests,
            build_opts,
            ctx,
            manifest,
            src_dir,
            build_dir,
            inst_dir,
            final_install_prefix=final_install_prefix,
        )
        self.defines: dict[str, str] = defines or {}
        self.cmake_targets: list[str] = cmake_targets or ["install"]

        if build_opts.is_windows():
            try:
                from .facebook.vcvarsall import extra_vc_cmake_defines
            except ImportError:
                pass
            else:
                self.defines.update(extra_vc_cmake_defines)

        self.loader = loader
        if build_opts.shared_lib:
            self.defines["CMAKE_POSITION_INDEPENDENT_CODE"] = "ON"

        # Apply caller-supplied extra defines last so per-package
        # --extra-cmake-defines can override defaults — including the
        # BUILD_SHARED_LIBS=ON that --shared-lib injects for the
        # top-level project.
        if extra_cmake_defines:
            self.defines.update(extra_cmake_defines)

    def _invalidate_cache(self) -> None:
        for name in [
            "CMakeCache.txt",
            "CMakeFiles/CMakeError.log",
            "CMakeFiles/CMakeOutput.log",
        ]:
            entry = pathlib.Path(self.build_dir, name)
            if entry.is_dir():
                shutil.rmtree(entry)
            elif entry.exists():
                entry.unlink()

    def _needs_reconfigure(self) -> bool:
        for name in ["CMakeCache.txt", "build.ninja"]:
            if not pathlib.Path(self.build_dir, name).exists():
                return True
        return False

    def _write_build_script(self, **kwargs: object) -> None:
        # pyre-fixme[16]: `object` has no attribute `items`.
        env_lines = ["    {!r}: {!r},".format(k, v) for k, v in kwargs["env"].items()]
        kwargs["env_str"] = "\n".join(["{"] + env_lines + ["}"])

        if self.build_opts.is_windows():
            kwargs["dev_run_script"] = '"powershell.exe", {!r}, '.format(
                self.get_dev_run_script_path()
            )
        else:
            kwargs["dev_run_script"] = ""

        define_arg_lines = ["["]
        # pyre-fixme[16]: `object` has no attribute `__iter__`.
        for arg in kwargs["define_args"]:
            # Replace the CMAKE_INSTALL_PREFIX argument to use the INSTALL_DIR
            # variable that we define in the MANUAL_BUILD_SCRIPT code.
            if arg.startswith("-DCMAKE_INSTALL_PREFIX="):
                value = "    {!r}.format(INSTALL_DIR),".format(
                    "-DCMAKE_INSTALL_PREFIX={}"
                )
            else:
                value = "    {!r},".format(arg)
            define_arg_lines.append(value)
        define_arg_lines.append("]")
        kwargs["define_args_str"] = "\n".join(define_arg_lines)

        # In order to make it easier for developers to manually run builds for
        # CMake-based projects, write out some build scripts that can be used to invoke
        # CMake manually.
        build_script_path = pathlib.Path(self.build_dir, "run_cmake.py")
        script_contents = self.MANUAL_BUILD_SCRIPT.format(**kwargs)
        with open(build_script_path, "wb") as f:
            f.write(script_contents.encode())
        os.chmod(build_script_path, 0o755)

    def _dep_install_dir(self, name: str) -> str | None:
        for dep_manifest, install_dir in zip(self.dep_manifests, self.install_dirs):
            if dep_manifest.name == name:
                return install_dir
        return None

    def _compute_cmake_define_args(self, env: Env) -> list[str]:
        defines = {
            "CMAKE_INSTALL_PREFIX": self.final_install_prefix or self.inst_dir,
            "BUILD_SHARED_LIBS": "OFF",
            # Some of the deps (rsocket) default to UBSAN enabled if left
            # unspecified.  Some of the deps fail to compile in release mode
            # due to warning->error promotion.  RelWithDebInfo is the happy
            # medium.
            "CMAKE_BUILD_TYPE": self.build_opts.build_type,
        }

        if "SANDCASTLE" not in os.environ:
            # We sometimes see intermittent ccache related breakages on some
            # of the FB internal CI hosts, so we prefer to disable ccache
            # when running in that environment.
            # Prefer sccache over ccache when both are available; sccache
            # supports cloud-backed caches (e.g. GitHub Actions cache) which
            # accelerate CI builds across runs.
            sccache = path_search(env, "sccache")
            ccache = path_search(env, "ccache")
            if sccache:
                defines["CMAKE_CXX_COMPILER_LAUNCHER"] = sccache
            elif ccache:
                defines["CMAKE_CXX_COMPILER_LAUNCHER"] = ccache
        else:
            # rocksdb does its own probing for ccache.
            # Ensure that it is disabled on sandcastle
            env["CCACHE_DISABLE"] = "1"
            # Some sandcastle hosts have broken ccache related dirs, and
            # even though we've asked for it to be disabled ccache is
            # still invoked by rocksdb's cmake.
            # Redirect its config directory to somewhere that is guaranteed
            # fresh to us, and that won't have any ccache data inside.
            env["CCACHE_DIR"] = f"{self.build_opts.scratch_dir}/ccache"

        if "GITHUB_ACTIONS" in os.environ and self.build_opts.is_windows():
            # GitHub actions: the host has both gcc and msvc installed, and
            # the default behavior of cmake is to prefer gcc.
            # Instruct cmake that we want it to use cl.exe; this is important
            # because Boost prefers cl.exe and the mismatch results in cmake
            # with gcc not being able to find boost built with cl.exe.
            defines["CMAKE_C_COMPILER"] = "cl.exe"
            defines["CMAKE_CXX_COMPILER"] = "cl.exe"

        if self.build_opts.is_darwin():
            # Try to persuade cmake to set the rpath to match the lib
            # dirs of the dependencies.  This isn't automatic, and to
            # make things more interesting, cmake uses `;` as the path
            # separator, so translate the runtime path to something
            # that cmake will parse
            defines["CMAKE_INSTALL_RPATH"] = ";".join(
                env.get("DYLD_LIBRARY_PATH", "").split(":")
            )
            # Tell cmake that we want to set the rpath in the tree
            # at build time.  Without this the rpath is only set
            # at the moment that the binaries are installed.  That
            # default is problematic for example when using the
            # gtest integration in cmake which runs the built test
            # executables during the build to discover the set of
            # tests.
            defines["CMAKE_BUILD_WITH_INSTALL_RPATH"] = "ON"

            # CMake's FindOpenSSL probes `brew --prefix openssl` on macOS when
            # OPENSSL_ROOT_DIR is unset, which can select a wrong-arch Homebrew
            # keg (e.g. an x86_64 openssl@1.1 on an arm64 host) over the OpenSSL
            # getdeps built. Point it at our OpenSSL so dependents like liboqs
            # link the right libcrypto.
            openssl_dir = self._dep_install_dir("openssl")
            if openssl_dir:
                defines["OPENSSL_ROOT_DIR"] = openssl_dir

        if self.build_opts.is_windows():
            # Use embedded debug info (/Z7) instead of MSVC's default /Zi, which
            # writes debug info to a single shared .pdb. /Zi can't be wrapped by
            # sccache and races under parallel ninja (fatal error C1041); /Z7 is
            # self-contained per .obj. Requires CMake >= 3.25, and the policy
            # default forces CMP0141=NEW into deps whose cmake_minimum_required
            # predates 3.25 so the variable actually takes effect.
            defines["CMAKE_MSVC_DEBUG_INFORMATION_FORMAT"] = "Embedded"
            defines["CMAKE_POLICY_DEFAULT_CMP0141"] = "NEW"

        defines.update(self.defines)
        define_args = ["-D%s=%s" % (k, v) for (k, v) in defines.items()]

        # if self.build_opts.is_windows():
        #    define_args += ["-G", "Visual Studio 15 2017 Win64"]
        define_args += ["-G", "Ninja"]

        return define_args

    def _run_include_rewriter(self) -> None:
        """Run include path rewriting on source files before building."""
        from .include_rewriter import rewrite_includes_from_manifest

        print(f"Rewriting include paths for {self.manifest.name}...")
        try:
            modified_count = rewrite_includes_from_manifest(
                self.manifest, self.ctx, self.src_dir, verbose=True
            )
            if modified_count > 0:
                print(f"Successfully modified {modified_count} files")
            else:
                print("No files needed modification")
        except Exception as e:
            print(f"Warning: Include path rewriting failed: {e}")
            # Don't fail the build for include rewriting issues

    def _build(self, reconfigure: bool) -> None:
        # Check if include rewriting is enabled
        rewrite_includes: str | None = self.manifest.get(
            "build", "rewrite_includes", "false", ctx=self.ctx
        )
        # pyre-fixme[16]: Optional type has no attribute `lower`.
        if rewrite_includes.lower() == "true":
            self._run_include_rewriter()

        reconfigure = reconfigure or self._needs_reconfigure()

        env = self._compute_env()
        if not self.build_opts.is_windows() and self.final_install_prefix:
            env["DESTDIR"] = self.inst_dir

        # Resolve the cmake that we installed
        cmake = path_search(env, "cmake")
        if cmake is None:
            raise Exception("Failed to find CMake")

        if self.build_opts.is_windows():
            checkdir = pathlib.Path(self.src_dir)
            if checkdir.exists():
                children = [p.name for p in checkdir.iterdir()]
                print(f"Building from source {checkdir} contents: {children}")
            else:
                print(f"Source {checkdir} not found")

        if reconfigure:
            define_args = self._compute_cmake_define_args(env)
            self._write_build_script(
                cmd_prefix=self._get_cmd_prefix(),
                cmake=cmake,
                #  got `Env`.
                ctest=path_search(env, "ctest"),
                env=env,
                define_args=define_args,
                src_dir=self.src_dir,
                build_dir=self.build_dir,
                install_dir=self.inst_dir,
                sys=sys,
                build_type=self.build_opts.build_type,
            )

            self._invalidate_cache()
            self._check_cmd([cmake, self.src_dir] + define_args, env=env)

        preexec_fn = self.memory_limit_preexec_fn
        self._restart_sccache_server(env, preexec_fn)
        self._check_cmd(
            # pyre-fixme[6]: For 1st argument expected `List[str]` but got
            #  `List[Optional[str]]`.
            [cmake, "--build", self.build_dir, "--target"]
            + self.cmake_targets
            + [
                "--config",
                self.build_opts.build_type,
                "-j",
                str(self.num_jobs),
                "--",
                # Continue building even if we see an error
                # Increases dev velocity by surfacing all errors
                # `-k 0` is understood by ninja - the only build system we use
                "-k",
                "0",
            ],
            env=env,
            preexec_fn=preexec_fn,
        )

    def _build_targets(self, targets: Sequence[str]) -> None:
        """Build one or more cmake targets in parallel.

        Args:
            targets: Sequence of target names (strings) to build
        """
        if not targets:
            return

        env = self._compute_env()
        cmake = path_search(env, "cmake")
        if cmake is None:
            raise RuntimeError("unable to find cmake")

        # Build all targets in a single cmake invocation for better parallelism
        cmd = [
            cmake,
            "--build",
            self.build_dir,
        ]

        # Add all targets
        for target in targets:
            cmd.extend(["--target", target])

        cmd.extend(
            # pyre-fixme[6]: For 1st argument expected `Iterable[str]` but got
            #  `Iterable[Union[str, str, None, str]]`.
            [
                "--config",
                self.build_opts.build_type,
                "-j",
                str(self.num_jobs),
                "--",
                # Continue building even if we see an error
                # Increases dev velocity by surfacing all errors
                # `-k 0` is understood by ninja - the only build system we use
                "-k",
                "0",
            ]
        )

        preexec_fn = self.memory_limit_preexec_fn
        self._restart_sccache_server(env, preexec_fn)
        self._check_cmd(cmd, env=env, preexec_fn=preexec_fn)

    def _restart_sccache_server(
        self, env: Env, preexec_fn: Callable[[], None] | None
    ) -> None:
        """Restart sccache under the memory cap before a capped build.

        Compiles run inside the long-lived sccache server, not under ninja, so
        they inherit the RLIMIT_AS of whichever earlier build spawned it (15 GiB
        from a default-weight dependency) rather than this manifest's cap.

        Start the replacement serially so parallel compiler processes do not all
        try to spawn it at once and exhaust the build container's PID limit.
        """
        if "SANDCASTLE" in os.environ or preexec_fn is None:
            return
        sccache = path_search(env, "sccache")
        if sccache:
            self._run_cmd([sccache, "--stop-server"], env=env, allow_fail=True)
            self._check_cmd([sccache, "--start-server"], env=env, preexec_fn=preexec_fn)

    def _get_missing_test_executables(
        self, test_filter: str | None, env: Env, ctest: str | None
    ) -> set[str]:
        """Discover which test executables are missing for the given filter.
        Returns a set of missing executable basenames (without path)."""
        if ctest is None:
            return set()

        # Run ctest -N (show tests without running) with the filter to see which tests match
        cmd = [ctest, "-N"]
        if test_filter:
            cmd += ["-R", test_filter]

        try:
            output = subprocess.check_output(
                cmd,
                env=dict(env.items()),
                cwd=self.build_dir,
                stderr=subprocess.STDOUT,
                text=True,
            )
        except subprocess.CalledProcessError as e:
            # If ctest fails, it might be because executables don't exist yet
            # Parse the error output to find the missing executables
            output = e.output

        # Parse output to find missing executable paths
        # Look for lines like "Could not find executable /path/to/test_binary"
        missing_executables = set()
        for line in output.split("\n"):
            match = re.search(r"Could not find executable (.+)", line)
            if match:
                exe_path = match.group(1)
                exe_name = pathlib.Path(exe_path).name
                missing_executables.add(exe_name)

        return missing_executables

    def run_tests(
        self,
        schedule_type: str,
        owner: str | None,
        test_filter: str | None,
        test_exclude: str | None,
        retry: int,
        no_testpilot: bool,
        timeout: int | None = None,
    ) -> None:
        env = self._compute_env()
        ctest: str | None = path_search(env, "ctest")
        cmake = path_search(env, "cmake")

        # Build only the missing test executables needed for the given filter.
        # This is especially important for LocalDirFetcher projects (like fboss)
        # where the build marker gets removed when building specific cmake targets.
        missing_test_executables = self._get_missing_test_executables(
            test_filter, env, ctest
        )
        if missing_test_executables:
            sorted_executables = sorted(missing_test_executables)
            print(f"Building missing test executables: {', '.join(sorted_executables)}")
            # Build all missing executables in one cmake invocation for better parallelism
            self._build_targets(sorted_executables)

        def require_command(path: str | None, name: str) -> str:
            if path is None:
                raise RuntimeError("unable to find command `{}`".format(name))
            return path

        # On Windows, we also need to update $PATH to include the directories that
        # contain runtime library dependencies.  This is not needed on other platforms
        # since CMake will emit RPATH properly in the binary so they can find these
        # dependencies.
        if self.build_opts.is_windows():
            path_entries = self.get_dev_run_extra_path_dirs()
            path = env.get("PATH")
            if path:
                path_entries.insert(0, path)
            env["PATH"] = ";".join(path_entries)

        # Don't use the cmd_prefix when running tests.  This is vcvarsall.bat on
        # Windows.  vcvarsall.bat is only needed for the build, not tests.  It
        # unfortunately fails if invoked with a long PATH environment variable when
        # running the tests.
        use_cmd_prefix = False

        def get_property(
            test: dict[str, object], propname: str, defval: object = None
        ) -> object:
            """extracts a named property from a cmake test info json blob.
            The properties look like:
            [{"name": "WORKING_DIRECTORY"},
             {"value": "something"}]
            We assume that it is invalid for the same named property to be
            listed more than once.
            """
            props = test.get("properties", [])
            # pyre-fixme[16]: `object` has no attribute `__iter__`.
            for p in props:
                if p.get("name", None) == propname:
                    return p.get("value", defval)
            return defval

        def list_tests() -> list[dict[str, object]]:
            output = subprocess.check_output(
                [require_command(ctest, "ctest"), "--show-only=json-v1"],
                env=env,
                cwd=self.build_dir,
            )
            try:
                data = json.loads(output.decode("utf-8"))
            except ValueError as exc:
                raise Exception(
                    "Failed to decode cmake test info using %s: %s.  Output was: %r"
                    % (ctest, str(exc), output)
                )

            tests = []
            machine_suffix = self.build_opts.host_type.as_tuple_string()
            for test in data["tests"]:
                working_dir = get_property(test, "WORKING_DIRECTORY")
                labels = []
                machine_suffix = self.build_opts.host_type.as_tuple_string()
                labels.append("tpx-fb-test-type=3")
                labels.append("tpx_test_config::buildsystem=getdeps")
                labels.append("tpx_test_config::platform={}".format(machine_suffix))

                if get_property(test, "DISABLED"):
                    labels.append("disabled")
                command = test["command"]
                if working_dir:
                    command = [
                        require_command(cmake, "cmake"),
                        "-E",
                        "chdir",
                        working_dir,
                    ] + command

                tests.append(
                    {
                        "type": "custom",
                        "target": "%s-%s-getdeps-%s"
                        % (self.manifest.name, test["name"], machine_suffix),
                        "command": command,
                        "labels": labels,
                        "env": {},
                        "required_paths": [],
                        "contacts": [],
                        "cwd": os.getcwd(),
                    }
                )
            return tests

        discover_like_continuous = False
        if schedule_type == "continuous" or (
            schedule_type == "base_retry" and is_public_commit(self.build_opts)
        ):
            discover_like_continuous = True

        if discover_like_continuous or schedule_type == "testwarden":
            # for continuous and testwarden runs, disabling retry can give up
            # better signals for flaky tests.
            retry = 0

        tpx = None
        try:
            from .facebook.testinfra import start_run

            #  `Env`.
            tpx = path_search(env, "tpx")
        except ImportError:
            # internal testinfra not available
            pass

        if tpx and not no_testpilot:
            buck_test_info = list_tests()

            buck_test_info_name = os.fspath(
                pathlib.Path(self.build_dir, ".buck-test-info.json")
            )
            with open(buck_test_info_name, "w") as f:
                json.dump(buck_test_info, f)

            env.set("http_proxy", "")
            env.set("https_proxy", "")
            runs = []

            # pyrefly: ignore [unbound-name]
            with start_run(env["FBSOURCE_HASH"]) as run_id:
                testpilot_args = [
                    tpx,
                    "--force-local-execution",
                    "--buck-test-info",
                    buck_test_info_name,
                    "--retry=%d" % retry,
                    "-j=%s" % str(self.num_jobs),
                    "--print-long-results",
                ]

                if owner:
                    testpilot_args += ["--contacts", owner]

                if env:
                    testpilot_args.append("--env")
                    testpilot_args.extend(f"{key}={val}" for key, val in env.items())

                if run_id is not None:
                    testpilot_args += ["--run-id", run_id]

                if timeout is not None:
                    testpilot_args += ["--timeout", str(timeout)]

                if test_filter:
                    testpilot_args += ["--", test_filter]

                if schedule_type == "diff":
                    runs.append(["--collection", "oss-diff", "--purpose", "diff"])
                elif discover_like_continuous:
                    runs.append(
                        [
                            "--tag-new-tests",
                            "--collection",
                            "oss-continuous",
                            "--purpose",
                            "continuous",
                        ]
                    )
                elif schedule_type == "testwarden":
                    # One run to assess new tests
                    runs.append(
                        [
                            "--tag-new-tests",
                            "--collection",
                            "oss-new-test-stress",
                            "--stress-runs",
                            "10",
                            "--purpose",
                            "stress-run-new-test",
                        ]
                    )
                    # And another for existing tests
                    runs.append(
                        [
                            "--tag-new-tests",
                            "--collection",
                            "oss-existing-test-stress",
                            "--stress-runs",
                            "10",
                            "--purpose",
                            "stress-run",
                        ]
                    )
                else:
                    runs.append([])

                for run in runs:
                    # FIXME: What is this trying to accomplish? Should it fail on first or >=1 errors?
                    self._run_cmd(
                        testpilot_args + run,
                        cwd=self.build_opts.fbcode_builder_dir,
                        env=env,
                        use_cmd_prefix=use_cmd_prefix,
                    )
        else:
            args = [
                require_command(ctest, "ctest"),
                "--output-on-failure",
                "-j",
                str(self.num_jobs),
            ]
            if test_filter:
                args += ["-R", test_filter]
            if test_exclude:
                args += ["--exclude-regex", test_exclude]
            if timeout is not None:
                args += ["--timeout", str(timeout)]

            count: int = 0
            retcode: int = -1
            while count <= retry:
                retcode = self._run_cmd(
                    args, env=env, use_cmd_prefix=use_cmd_prefix, allow_fail=True
                )

                if retcode == 0:
                    break
                if count == 0:
                    # Only add this option in the second run.
                    args += ["--rerun-failed"]
                count += 1
            if retcode != 0:
                # Allow except clause in getdeps.main to catch and exit gracefully
                # This allows non-testpilot runs to fail through the same logic as failed testpilot runs, which may become handy in case if post test processing is needed in the future
                raise subprocess.CalledProcessError(retcode, args)


class NinjaBootstrap(BuilderBase):
    def __init__(
        self,
        loader: ManifestLoader,
        dep_manifests: list[ManifestParser],
        build_opts: BuildOptions,
        ctx: ManifestContext,
        manifest: ManifestParser,
        build_dir: str,
        src_dir: str,
        inst_dir: str,
    ) -> None:
        super(NinjaBootstrap, self).__init__(
            loader,
            dep_manifests,
            build_opts,
            ctx,
            manifest,
            src_dir,
            build_dir,
            inst_dir,
        )

    def _build(self, reconfigure: bool) -> None:
        self._check_cmd(
            [sys.executable, "configure.py", "--bootstrap"], cwd=self.src_dir
        )
        src_ninja = pathlib.Path(self.src_dir, "ninja")
        dest_ninja = pathlib.Path(self.inst_dir, "bin/ninja")
        bin_dir = dest_ninja.parent
        if not bin_dir.exists():
            bin_dir.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src_ninja, dest_ninja)
        shutil.copymode(src_ninja, dest_ninja)


class OpenSSLBuilder(BuilderBase):
    def __init__(
        self,
        loader: ManifestLoader,
        dep_manifests: list[ManifestParser],
        build_opts: BuildOptions,
        ctx: ManifestContext,
        manifest: ManifestParser,
        build_dir: str,
        src_dir: str,
        inst_dir: str,
    ) -> None:
        super(OpenSSLBuilder, self).__init__(
            loader,
            dep_manifests,
            build_opts,
            ctx,
            manifest,
            src_dir,
            build_dir,
            inst_dir,
        )

    def _build(self, reconfigure: bool) -> None:
        configure = os.fspath(pathlib.Path(self.src_dir, "Configure"))

        # prefer to resolve the perl that we installed from
        # our manifest on windows, but fall back to the system
        # path on eg: darwin
        env = self.env.copy()
        for m in self.dep_manifests:
            bindir = os.fspath(
                pathlib.Path(self.loader.get_project_install_dir(m), "bin")
            )
            add_path_entry(env, "PATH", bindir, append=False)

        perl = typing.cast(str, path_search(env, "perl", "perl"))

        make_j_args = []
        extra_args = []
        if self.build_opts.is_windows():
            # jom is compatible with nmake, adds the /j argument for parallel build
            make = "jom.exe"
            make_j_args = ["/j%s" % self.num_jobs]
            args = ["VC-WIN64A-masm", "-utf-8"]
            # fixes "if multiple CL.EXE write to the same .PDB file, please use /FS"
            extra_args = ["/FS"]
        elif self.build_opts.is_darwin():
            make = "make"
            make_j_args = ["-j%s" % self.num_jobs]
            args = (
                ["darwin64-x86_64-cc"]
                if not self.build_opts.is_arm()
                else ["darwin64-arm64-cc"]
            )
        elif self.build_opts.is_linux():
            make = "make"
            make_j_args = ["-j%s" % self.num_jobs]
            args = (
                ["linux-x86_64"] if not self.build_opts.is_arm() else ["linux-aarch64"]
            )
        else:
            raise Exception("don't know how to build openssl for %r" % self.ctx)

        self._check_cmd(
            [
                perl,
                configure,
                "--prefix=%s" % self.inst_dir,
                "--openssldir=%s" % self.inst_dir,
            ]
            + args
            + [
                "enable-static-engine",
                "enable-capieng",
                "no-makedepend",
                "no-unit-test",
                "no-tests",
            ]
            + extra_args
        )
        # show the config produced
        self._check_cmd([perl, "configdata.pm", "--dump"], env=env)
        make_build = [make] + make_j_args
        self._check_cmd(make_build, env=env)
        make_install = [make, "install_sw", "install_ssldirs"]
        self._check_cmd(make_install, env=env)


class NopBuilder(BuilderBase):
    def __init__(
        self,
        loader: ManifestLoader,
        dep_manifests: list[ManifestParser],
        build_opts: BuildOptions,
        ctx: ManifestContext,
        manifest: ManifestParser,
        src_dir: str,
        inst_dir: str,
    ) -> None:
        super(NopBuilder, self).__init__(
            loader, dep_manifests, build_opts, ctx, manifest, src_dir, None, inst_dir
        )

    def build(self, reconfigure: bool) -> None:
        print("Installing %s -> %s" % (self.src_dir, self.inst_dir))
        parent = pathlib.Path(self.inst_dir).parent
        if not parent.exists():
            parent.mkdir(parents=True, exist_ok=True)

        install_files = self.manifest.get_section_as_ordered_pairs(
            "install.files", self.ctx
        )
        if install_files:
            for src_name, dest_name in self.manifest.get_section_as_ordered_pairs(
                "install.files", self.ctx
            ):
                # pyre-fixme[6]: For 2nd argument expected `Union[PathLike[str],
                #  str]` but got `Optional[str]`.
                full_dest = pathlib.Path(self.inst_dir, dest_name)
                full_src = pathlib.Path(self.src_dir, src_name)

                dest_parent = full_dest.parent
                if not dest_parent.exists():
                    dest_parent.mkdir(parents=True, exist_ok=True)
                if full_src.is_dir():
                    if not full_dest.exists():
                        simple_copytree(full_src, full_dest)
                else:
                    shutil.copyfile(full_src, full_dest)
                    shutil.copymode(full_src, full_dest)
                    # This is a bit gross, but the mac ninja.zip doesn't
                    # give ninja execute permissions, so force them on
                    # for things that look like they live in a bin dir
                    # pyre-fixme[6]: For 1st argument expected `PathLike[AnyStr]`
                    #  but got `Optional[str]`.
                    if os.fspath(pathlib.Path(dest_name).parent) == "bin":
                        st = os.lstat(full_dest)
                        os.chmod(full_dest, st.st_mode | stat.S_IXUSR)
        else:
            if not pathlib.Path(self.inst_dir).exists():
                simple_copytree(self.src_dir, self.inst_dir)


class SetupPyBuilder(BuilderBase):
    def _build(self, reconfigure: bool) -> None:
        env = self._compute_env()

        setup_env = self.manifest.get_section_as_dict("setup-py.env", self.ctx)
        for key, value in setup_env.items():
            # pyre-fixme[6]: For 2nd argument expected `str` but got `Optional[str]`.
            env[key] = value

        self._check_cmd(
            # pyre-fixme[6]: For 1st argument expected `List[str]` but got
            #  `List[Union[str, None, str]]`.
            # pyre-fixme[6]: For 1st argument expected `Mapping[str, str]` but got
            #  `Env`.
            [
                path_search(env, "python3"),
                "-m",
                "pip",
                "install",
                ".",
            ],
            cwd=self.src_dir,
            env=env,
        )

        # Create the installation directory if it doesn't exist
        pathlib.Path(self.inst_dir).mkdir(parents=True, exist_ok=True)

        # Mark the project as built
        with open(pathlib.Path(self.inst_dir, ".built-by-getdeps"), "w") as f:
            f.write("built")

    def run_tests(
        self,
        schedule_type: str,
        owner: str | None,
        test_filter: str | None,
        test_exclude: str | None,
        retry: int,
        no_testpilot: bool,
        timeout: int | None = None,
    ) -> None:
        # setup.py actually no longer has a standard command for running tests.
        # Instead we let manifest files specify an arbitrary Python file to run
        # as a test.

        # Get the test command from the manifest
        python_script = self.manifest.get(
            "setup-py.test", "python_script", ctx=self.ctx
        )
        if not python_script:
            print(f"No test script specified for {self.manifest.name}")
            return

        # Run the command
        env = self._compute_env()
        self._check_cmd(["python3", python_script], cwd=self.src_dir, env=env)


class SqliteBuilder(BuilderBase):
    def __init__(
        self,
        loader: ManifestLoader,
        dep_manifests: list[ManifestParser],
        build_opts: BuildOptions,
        ctx: ManifestContext,
        manifest: ManifestParser,
        src_dir: str,
        build_dir: str,
        inst_dir: str,
    ) -> None:
        super(SqliteBuilder, self).__init__(
            loader,
            dep_manifests,
            build_opts,
            ctx,
            manifest,
            src_dir,
            build_dir,
            inst_dir,
        )

    def _build(self, reconfigure: bool) -> None:
        for f in ["sqlite3.c", "sqlite3.h", "sqlite3ext.h"]:
            src = pathlib.Path(self.src_dir, f)
            dest = pathlib.Path(self.build_dir, f)
            copy_if_different(src, dest)

        cmake_lists = """
cmake_minimum_required(VERSION 3.5 FATAL_ERROR)
project(sqlite3 C)
add_library(sqlite3 STATIC sqlite3.c)
# These options are taken from the defaults in Makefile.msc in
# the sqlite distribution
target_compile_definitions(sqlite3 PRIVATE
    -DSQLITE_ENABLE_COLUMN_METADATA=1
    -DSQLITE_ENABLE_FTS3=1
    -DSQLITE_ENABLE_RTREE=1
    -DSQLITE_ENABLE_GEOPOLY=1
    -DSQLITE_ENABLE_JSON1=1
    -DSQLITE_ENABLE_STMTVTAB=1
    -DSQLITE_ENABLE_DBPAGE_VTAB=1
    -DSQLITE_ENABLE_DBSTAT_VTAB=1
    -DSQLITE_INTROSPECTION_PRAGMAS=1
    -DSQLITE_ENABLE_DESERIALIZE=1
)
install(TARGETS sqlite3)
install(FILES sqlite3.h sqlite3ext.h DESTINATION include)
            """

        with open(pathlib.Path(self.build_dir, "CMakeLists.txt"), "w") as f:
            f.write(cmake_lists)

        defines = {
            "CMAKE_INSTALL_PREFIX": self.inst_dir,
            "BUILD_SHARED_LIBS": "OFF",
            "CMAKE_BUILD_TYPE": "RelWithDebInfo",
        }
        if self.build_opts.shared_lib:
            defines["CMAKE_POSITION_INDEPENDENT_CODE"] = "ON"
        define_args = ["-D%s=%s" % (k, v) for (k, v) in defines.items()]
        define_args += ["-G", "Ninja"]

        env = self._compute_env()

        # Resolve the cmake that we installed
        cmake = path_search(env, "cmake")

        # pyre-fixme[6]: For 1st argument expected `List[str]` but got
        #  `List[Optional[str]]`.
        self._check_cmd([cmake, self.build_dir] + define_args, env=env)
        self._check_cmd(
            # pyre-fixme[6]: For 1st argument expected `List[str]` but got
            #  `List[Union[str, str, str, str, str, None, str]]`.
            [
                cmake,
                "--build",
                self.build_dir,
                "--target",
                "install",
                "--config",
                self.build_opts.build_type,
                "-j",
                str(self.num_jobs),
            ],
            env=env,
        )
