# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import os
from pathlib import Path
from typing import Any

from .buildopts import setup_build_options
from .load import ManifestLoader
from .manifest import ManifestParser
from .subcmd import SubCmd


class UsageError(Exception):
    pass


# Shared argument definition for --build-type used by multiple commands
BUILD_TYPE_ARG: dict[str, Any] = {
    "help": "Set the build type explicitly: Debug (unoptimized, debug symbols), RelWithDebInfo (optimized with debug symbols, default), MinSizeRel (size-optimized, no debug), or Release (optimized, no debug).",
    "choices": ["Debug", "Release", "RelWithDebInfo", "MinSizeRel"],
    "action": "store",
    "default": "RelWithDebInfo",
}


class ProjectCmdBase(SubCmd):
    def run(self, args):
        loader, manifest = self.create_loader(args)
        return self.run_project_cmd(args, loader, manifest)

    def create_loader(self, args) -> tuple[ManifestLoader, ManifestParser]:
        """Set up the loader and project manifest that `run` operates on."""
        opts = setup_build_options(args)

        if args.current_project is not None:
            opts.repo_project = args.current_project
        if args.project is None:
            if opts.repo_project is None:
                raise UsageError(
                    "no project name specified, and no .projectid file found"
                )
            if opts.repo_project == "fbsource":
                # The fbsource repository is a little special.  There is no project
                # manifest file for it.  A specific project must always be explicitly
                # specified when building from fbsource.
                raise UsageError(
                    "no project name specified (required when building in fbsource)"
                )
            args.project = opts.repo_project

        ctx_gen = opts.get_context_generator()
        loader = ManifestLoader(opts, ctx_gen)
        self.process_project_dir_arguments(args, loader)
        features_from = getattr(args, "features_from", None)
        if features_from:
            # Before the test settings below, so that every step of a job
            # resolves features with the same context.
            loader.resolve_features_from(features_from, args.project)

        if args.test_dependencies:
            ctx_gen.set_value_for_all_projects("test", "on")
        if args.enable_tests:
            ctx_gen.set_value_for_project(args.project, "test", "on")
        else:
            ctx_gen.set_value_for_project(args.project, "test", "off")

        return loader, loader.load_manifest(args.project)

    def process_project_dir_arguments(self, args, loader):
        def parse_project_arg(arg, arg_type):
            parts = arg.split(":")
            if len(parts) == 2:
                project, path = parts
            elif len(parts) == 1:
                project = args.project
                path = parts[0]
            # On Windows path contains colon, e.g. C:\open
            elif os.name == "nt" and len(parts) == 3:
                project = parts[0]
                path = parts[1] + ":" + parts[2]
            else:
                raise UsageError(
                    "invalid %s argument; too many ':' characters: %s" % (arg_type, arg)
                )

            return project, os.path.abspath(path)

        # If we are currently running from a project repository,
        # use the current repository for the project sources.
        build_opts = loader.build_opts
        if build_opts.repo_project is not None and build_opts.repo_root is not None:
            loader.set_project_src_dir(build_opts.repo_project, build_opts.repo_root)

        for arg in args.src_dir:
            project, path = parse_project_arg(arg, "--src-dir")
            loader.set_project_src_dir(project, path)

        for arg in args.build_dir:
            project, path = parse_project_arg(arg, "--build-dir")
            loader.set_project_build_dir(project, path)

        for arg in args.install_dir:
            project, path = parse_project_arg(arg, "--install-dir")
            loader.set_project_install_dir(project, path)

        for arg in args.project_install_prefix:
            project, path = parse_project_arg(arg, "--install-prefix")
            loader.set_project_install_prefix(project, path)

    def setup_parser(self, parser):
        parser.add_argument(
            "project",
            nargs="?",
            help=(
                "name of the project or path to a manifest "
                "file describing the project"
            ),
        )
        parser.add_argument(
            "--no-tests",
            action="store_false",
            dest="enable_tests",
            default=True,
            help="Disable building tests for this project.",
        )
        parser.add_argument(
            "--test-dependencies",
            action="store_true",
            help="Enable building tests for dependencies as well.",
        )
        parser.add_argument(
            "--current-project",
            help="Specify the name of the fbcode_builder manifest file for the "
            "current repository.  If not specified, the code will attempt to find "
            "this in a .projectid file in the repository root.",
        )
        parser.add_argument(
            "--src-dir",
            default=[],
            action="append",
            help="Specify a local directory to use for the project source, "
            "rather than fetching it.",
        )
        parser.add_argument(
            "--build-dir",
            default=[],
            action="append",
            help="Explicitly specify the build directory to use for the "
            "project, instead of the default location in the scratch path. "
            "This only affects the project specified, and not its dependencies.",
        )
        parser.add_argument(
            "--install-dir",
            default=[],
            action="append",
            help="Explicitly specify the install directory to use for the "
            "project, instead of the default location in the scratch path. "
            "This only affects the project specified, and not its dependencies.",
        )
        parser.add_argument(
            "--project-install-prefix",
            default=[],
            action="append",
            help="Specify the final deployment installation path for a project",
        )

        self.setup_project_cmd_parser(parser)

    def setup_project_cmd_parser(self, parser):
        pass

    def create_builder(self, loader, manifest):
        fetcher = loader.create_fetcher(manifest)
        src_dir = fetcher.get_src_dir()
        ctx = loader.ctx_gen.get_context(manifest.name)
        build_dir = loader.get_project_build_dir(manifest)
        inst_dir = loader.get_project_install_dir(manifest)
        return manifest.create_builder(
            loader.build_opts,
            src_dir,
            build_dir,
            inst_dir,
            ctx,
            loader,
            loader.dependencies_of(manifest),
        )

    def check_built(self, loader, manifest):
        built_marker = Path(
            loader.get_project_install_dir(manifest), ".built-by-getdeps"
        )

        return built_marker.exists()
