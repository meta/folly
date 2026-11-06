# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import json
import os
import re
from pathlib import Path

from .buildopts import setup_build_options
from .cmd_base import BUILD_TYPE_ARG, ProjectCmdBase
from .getdeps_platform import HostType
from .load import ManifestLoader
from .subcmd import cmd


_PLATFORMS: list[HostType] = [
    HostType("linux", "ubuntu", "24"),
    HostType("darwin", None, None),
    HostType("windows", None, None),
]


def _resolve_platform(args, build_opts) -> tuple[str, str, str]:
    """Return (artifacts, runs_on, py3) for the active platform."""
    if build_opts.is_linux():
        if args.runs_on:
            runs_on = args.runs_on
        elif args.cpu_cores:
            runs_on = f"{args.cpu_cores}-core-ubuntu-{args.ubuntu_version}"
        else:
            runs_on = f"ubuntu-{args.ubuntu_version}"
        return "linux", runs_on, "python3"
    if build_opts.is_windows():
        # On Windows GHA runners the python 3 interpreter is exposed as
        # `python`, not `python3`.
        return "windows", args.runs_on or "windows-2022", "python"
    return "mac", args.runs_on or "macOS-latest", "python3"


@cmd("generate-github-actions", "generate a GitHub actions configuration")
class GenerateGitHubActionsCmd(ProjectCmdBase):
    RUN_ON_ALL: str = """ [push, pull_request]"""

    def run_project_cmd(self, args, loader, manifest):
        for p in _PLATFORMS:
            if args.os_types and p.ostype not in args.os_types:
                continue
            self.write_job_for_platform(p, args)

    def get_run_on(self, args):
        if args.run_on_all_branches:
            return self.RUN_ON_ALL
        if args.cron:
            if args.cron == "never":
                return " {}"
            elif args.cron == "workflow_dispatch":
                return "\n  workflow_dispatch"
            else:
                return f"""
  schedule:
    - cron: '{args.cron}'"""

        branches = args.main_branch or ["main"]
        branch_lines = "\n".join(f"    - {b}" for b in branches)
        return f"""
  push:
    branches:
{branch_lines}
  pull_request:
    branches:
{branch_lines}"""

    def write_job_for_platform(self, platform, args):
        build_opts = setup_build_options(args, platform)
        ctx_gen = build_opts.get_context_generator()
        ctx_gen.set_value_for_project(
            args.project, "test", "on" if args.enable_tests else "off"
        )
        loader = ManifestLoader(build_opts, ctx_gen)
        self.process_project_dir_arguments(args, loader)
        manifest = loader.load_manifest(args.project)
        manifest_ctx = loader.ctx_gen.get_context(manifest.name)

        def gh(key):
            return manifest.get("github.actions", key, ctx=manifest_ctx)

        run_tests = args.enable_tests and gh("run_tests") != "off"
        rust_version = gh("rust_version") or "stable"
        use_sccache = gh("sccache") != "off"
        use_homebrew_llvm = build_opts.is_darwin()
        override_build_type = args.build_type or gh("build_type")
        timeout_minutes = gh("timeout_minutes") or "60"
        if run_tests:
            manifest_ctx.set("test", "on")
        run_on = self.get_run_on(args)

        tests_arg = "" if run_tests else "--no-tests "

        # Some projects don't do anything "useful" as a leaf project, only
        # as a dep for a leaf project. Check for those here; we don't want
        # to waste the effort scheduling them on CI.
        # We do this by looking at the builder type in the manifest file
        # rather than creating a builder and checking its type because we
        # don't know enough to create the full builder instance here.
        builder_name = manifest.get("build", "builder", ctx=manifest_ctx)
        if builder_name == "nop":
            return None

        artifacts, runs_on, py3 = _resolve_platform(args, build_opts)

        Path(args.output_dir).mkdir(parents=True, exist_ok=True)
        job_file_prefix = args.job_file_prefix or "getdeps_"
        output_file = os.fspath(
            Path(args.output_dir, f"{job_file_prefix}{artifacts}.yml")
        )
        job_name = (
            args.job_name_prefix + artifacts.capitalize()
            if args.job_name_prefix
            else artifacts
        )

        ctx = self._build_render_context(
            args=args,
            build_opts=build_opts,
            loader=loader,
            manifest=manifest,
            manifest_ctx=manifest_ctx,
            run_tests=run_tests,
            rust_version=rust_version,
            use_sccache=use_sccache,
            use_homebrew_llvm=use_homebrew_llvm,
            override_build_type=override_build_type,
            timeout_minutes=timeout_minutes,
            tests_arg=tests_arg,
            builder_name=builder_name,
            py3=py3,
            artifacts=artifacts,
            runs_on=runs_on,
            run_on=run_on,
            job_name=job_name,
        )
        with open(output_file, "w") as out:
            out.write(_render_workflow(ctx))

    def _build_render_context(  # noqa: C901
        self,
        *,
        args,
        build_opts,
        loader,
        manifest,
        manifest_ctx,
        run_tests: bool,
        rust_version: str,
        use_sccache: bool,
        use_homebrew_llvm: bool,
        override_build_type,
        timeout_minutes,
        tests_arg: str,
        builder_name: str,
        py3: str,
        artifacts: str,
        runs_on: str,
        run_on: str,
        job_name: str,
    ) -> dict:
        getdepscmd = f"{py3} {manifest.shipit_fbcode_builder_dir}/getdeps.py"

        env_lines = []
        if build_opts.is_darwin():
            env_lines.append(
                "DEVELOPER_DIR: /Applications/Xcode.app/Contents/Developer"
            )
        if use_homebrew_llvm:
            env_lines.append("CC: /opt/homebrew/opt/llvm/bin/clang")
            env_lines.append("CXX: /opt/homebrew/opt/llvm/bin/clang++")
        if use_sccache:
            env_lines.append('SCCACHE_GHA_ENABLED: "on"')

        extra_cmake_defines = (
            json.loads(args.extra_cmake_defines) if args.extra_cmake_defines else {}
        )
        per_package_defines = _parse_per_package_defines(
            getattr(args, "package_extra_cmake_defines", []) or []
        )

        def cmake_arg_for(name):
            merged = dict(extra_cmake_defines)
            merged.update(per_package_defines.get(name, {}))
            if merged:
                return (
                    " --extra-cmake-defines '"
                    + json.dumps(merged, separators=(",", ":"))
                    + "'"
                )
            return ""

        build_type_arg = ""
        if override_build_type:
            build_type_arg = f"--build-type {override_build_type} "
        if args.shared_lib:
            build_type_arg += "--shared-lib "

        allow_sys_arg = ""
        system_deps = None
        if (
            build_opts.allow_system_packages
            and build_opts.host_type.get_package_manager()
        ):
            sudo_arg = "sudo --preserve-env=http_proxy "
            allow_sys_arg = " --allow-system-packages"
            apt_update_cmd = ""
            if build_opts.host_type.get_package_manager() == "deb":
                apt_update_cmd = f"{sudo_arg}apt-get update"

            install_sudo = "" if build_opts.is_darwin() else sudo_arg
            install_cmd = (
                f"{install_sudo}{getdepscmd}{allow_sys_arg} install-system-deps "
                f"{tests_arg}--recursive {manifest.name}"
            )
            if build_opts.is_linux() or build_opts.is_freebsd():
                install_cmd += (
                    f" && {install_sudo}{getdepscmd}{allow_sys_arg} install-system-deps "
                    f"{tests_arg}--recursive patchelf"
                )

            locales_str = manifest.get(
                "github.actions", "required_locales", ctx=manifest_ctx
            )
            locales = (
                locales_str.split()
                if (locales_str and build_opts.host_type.get_package_manager() == "deb")
                else []
            )
            system_deps = {
                "apt_update_cmd": apt_update_cmd,
                "install_cmd": install_cmd,
                "locales": locales,
                "locale_install_cmd": f"{sudo_arg}apt-get install locales",
                "locale_gen_prefix": f"{sudo_arg}locale-gen ",
            }

        if build_opts.is_windows():
            query_paths_cmd = (
                f"{getdepscmd}{allow_sys_arg} query-paths {tests_arg}"
                f"--recursive --src-dir=. {manifest.name}  >> $env:GITHUB_OUTPUT"
            )
        else:
            query_paths_cmd = (
                f"{getdepscmd}{allow_sys_arg} query-paths {tests_arg}"
                f'--recursive --src-dir=. {manifest.name}  >> "$GITHUB_OUTPUT"'
            )

        projects = loader.manifests_in_dependency_order()
        main_repo_url = manifest.get_repo_url(manifest_ctx)
        # Rust install detection (rust dep has no manifest entry)
        emit_rust = False
        for m in projects:
            if m == manifest:
                continue
            mbuilder_name = m.get("build", "builder", ctx=manifest_ctx)
            if m.name == "rust" or builder_name == "cargo" or mbuilder_name == "cargo":
                emit_rust = True
                break

        same_repo_fetches = []
        for m in projects:
            if m == manifest or m.name == "rust":
                continue
            mctx = loader.ctx_gen.get_context(m.name)
            if m.get_repo_url(mctx) != main_repo_url:
                same_repo_fetches.append(m.name)

        project_prefix = ""
        if not build_opts.is_windows():
            prefix = loader.get_project_install_prefix(manifest) or "/usr/local"
            project_prefix = f" --project-install-prefix {manifest.name}:{prefix}"

        # Build all transitive deps and the project in a single recursive pass.
        # sccache handles compile-unit caching; per-dep actions/cache
        # round-trips are net-negative at typical sccache hit rates.
        final_build_cmd = (
            f"{getdepscmd}{allow_sys_arg} build {build_type_arg}{tests_arg}"
            f"--src-dir=. {manifest.name}{project_prefix}"
            f"{cmake_arg_for(manifest.name)}"
        )

        strip = " --strip" if build_opts.is_linux() else ""
        copy_artifacts_cmd = (
            f"{getdepscmd}{allow_sys_arg} fixup-dyn-deps{strip} "
            f"--src-dir=. {manifest.name} _artifacts/{artifacts}{project_prefix} "
            f"--final-install-prefix /usr/local"
        )

        test_cmd = None
        if run_tests:
            num_jobs_arg = f"--num-jobs {args.num_jobs} " if args.num_jobs else ""
            test_cmd = (
                f"{getdepscmd}{allow_sys_arg} test {build_type_arg}{num_jobs_arg}"
                f"--src-dir=. {manifest.name}{project_prefix}"
                f"{cmake_arg_for(manifest.name)}"
            )

        return {
            "job_name": job_name,
            "run_on_block": run_on,
            "runs_on": runs_on,
            "timeout_minutes": timeout_minutes,
            "env_lines": env_lines,
            "is_linux": build_opts.is_linux(),
            "is_darwin": build_opts.is_darwin(),
            "is_windows": build_opts.is_windows(),
            "use_sccache": use_sccache,
            "use_homebrew_llvm": use_homebrew_llvm,
            "free_up_disk": build_opts.free_up_disk,
            "free_up_disk_before_build": args.free_up_disk_before_build,
            "system_deps": system_deps,
            "query_paths_cmd": query_paths_cmd,
            "rust_version": rust_version if emit_rust else None,
            "rust_version_cap": rust_version.capitalize() if emit_rust else None,
            "fetch_cmd_prefix": f"{getdepscmd}{allow_sys_arg} fetch --no-tests ",
            "same_repo_fetches": same_repo_fetches,
            "project_name": manifest.name,
            "final_build_cmd": final_build_cmd,
            "copy_artifacts_cmd": copy_artifacts_cmd,
            "test_cmd": test_cmd,
        }

    def setup_project_cmd_parser(self, parser):
        parser.add_argument(
            "--disallow-system-packages",
            help="Disallow satisfying third party deps from installed system packages",
            action="store_true",
            default=False,
        )
        parser.add_argument(
            "--output-dir", help="The directory that will contain the yml files"
        )
        parser.add_argument(
            "--run-on-all-branches",
            action="store_true",
            help="Allow CI to fire on all branches - Handy for testing",
        )
        parser.add_argument(
            "--ubuntu-version", default="24.04", help="Version of Ubuntu to use"
        )
        parser.add_argument(
            "--cpu-cores",
            help="Number of CPU cores to use (applicable for Linux OS)",
        )
        parser.add_argument(
            "--runs-on",
            help="Allow specifying explicit runs-on: for github actions",
        )
        parser.add_argument(
            "--cron",
            help="Specify that the job runs on a cron schedule instead of on pushes. Pass never to disable the action.",
        )
        parser.add_argument(
            "--main-branch",
            default=[],
            action="append",
            help="Branch to trigger GitHub Action on. May be repeated; defaults to 'main' if not given.",
        )
        parser.add_argument(
            "--os-type",
            help="Filter to just this OS type to run",
            choices=["linux", "darwin", "windows"],
            action="append",
            dest="os_types",
            default=[],
        )
        parser.add_argument(
            "--job-file-prefix",
            type=str,
            help="add a prefix to all job file names",
            default=None,
        )
        parser.add_argument(
            "--job-name-prefix",
            type=str,
            help="add a prefix to all job names",
            default=None,
        )
        parser.add_argument(
            "--free-up-disk",
            help="After installing each dependency, delete its build directory to free disk space during the build",
            action="store_true",
            default=False,
        )
        parser.add_argument(
            "--free-up-disk-before-build",
            help="Remove large system tools (dotnet, powershell, ghcup) immediately before the final project build to free disk space",
            action="store_true",
            default=False,
        )
        parser.add_argument("--build-type", **BUILD_TYPE_ARG)
        parser.add_argument(
            "--package-extra-cmake-defines",
            action="append",
            default=[],
            metavar="PACKAGE=JSON",
            help=(
                "Add cmake defines that apply only to the named package's "
                "build step in the generated workflow. Example: "
                "--package-extra-cmake-defines "
                '\'fbthrift={"THRIFT_SERIALIZATION_ONLY":"ON"}\'. May be '
                "passed multiple times."
            ),
        )


def _render_workflow(ctx: dict) -> str:
    # Imported lazily so OSS consumers running other commands (`build`,
    # `test`, `install-system-deps`, …) on GHA runners are not forced to
    # install jinja2 — only `generate-github-actions` needs it.
    import jinja2

    env = jinja2.Environment(
        loader=jinja2.FileSystemLoader(
            os.path.join(os.path.dirname(os.path.abspath(__file__)), "templates")
        ),
        variable_start_string="<<",
        variable_end_string=">>",
        block_start_string="<%",
        block_end_string="%>",
        comment_start_string="<#",
        comment_end_string="#>",
        trim_blocks=True,
        lstrip_blocks=True,
        keep_trailing_newline=True,
        autoescape=False,
    )
    template = env.get_template("workflow.yml.j2")
    # Conditional sections in the template are separated by blank lines for
    # readability; when a conditional is skipped its surrounding blanks pile
    # up. Collapse runs of blank lines, and strip the leading newlines that
    # the macro definitions at the top of the template leave behind.
    rendered = template.render(**ctx).lstrip("\n")
    return re.sub(r"\n{3,}", "\n\n", rendered)


def _parse_per_package_defines(values):
    """Parse a list of `package=json` strings into a dict of dicts."""
    result = {}
    for entry in values or []:
        if "=" not in entry:
            raise SystemExit(
                f"--package-extra-cmake-defines value {entry!r} must be of the "
                "form PACKAGE=JSON"
            )
        package, raw = entry.split("=", 1)
        try:
            parsed = json.loads(raw)
        except json.JSONDecodeError as e:
            raise SystemExit(
                f"--package-extra-cmake-defines for {package!r} is not valid "
                f"JSON: {e}"
            )
        if not isinstance(parsed, dict):
            raise SystemExit(
                f"--package-extra-cmake-defines for {package!r} must be a JSON "
                "object"
            )
        result.setdefault(package, {}).update(parsed)
    return result
