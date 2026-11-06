# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


from __future__ import annotations

import configparser
import hashlib
import io
import os
import sys
import typing
from pathlib import Path
from typing import overload

from .builder import (
    AutoconfBuilder,
    CMakeBootStrapBuilder,
    CMakeBuilder,
    Iproute2Builder,
    MakeBuilder,
    MesonBuilder,
    NinjaBootstrap,
    NopBuilder,
    OpenSSLBuilder,
    SetupPyBuilder,
    SqliteBuilder,
)
from .cargo import CargoBuilder
from .expr import ExprNode, parse_expr
from .fetcher import (
    ArchiveFetcher,
    GitFetcher,
    LocalDirFetcher,
    PreinstalledNopFetcher,
    ShipitTransformerFetcher,
    SimpleShipitTransformerFetcher,
    SubFetcher,
    SystemPackageFetcher,
)
from .py_wheel_builder import PythonWheelBuilder

if typing.TYPE_CHECKING:
    from .builder import BuilderBase
    from .buildopts import BuildOptions
    from .fetcher import Fetcher
    from .load import ManifestLoader

REQUIRED: str = "REQUIRED"
OPTIONAL: str = "OPTIONAL"

# Where ShipIt puts fbcode_builder in a project's repo, by the manifest's
# shipit_fbcode_builder_layout. Pinned revisions sit in ../deps/github_hashes
# from there, where BuildOptions finds them. "tool" keeps build/ free for build
# output, as in `cmake -B build`.
FBCODE_BUILDER_LAYOUT_DIRS: dict[str, str] = {
    "build": "build/fbcode_builder",
    "tool": "tool/fbcode_builder",
}

SCHEMA: dict[str, dict[str, object]] = {
    "manifest": {
        "optional_section": False,
        "fields": {
            "name": REQUIRED,
            "fbsource_path": OPTIONAL,
            "shipit_project": OPTIONAL,
            "shipit_fbcode_builder": OPTIONAL,
            "shipit_fbcode_builder_layout": OPTIONAL,
            "use_shipit": OPTIONAL,
            "shipit_external_branch": OPTIONAL,
            "shipit_strip_marker": OPTIONAL,
        },
    },
    "dependencies": {"optional_section": True},
    "features": {"optional_section": True},
    "depends.environment": {"optional_section": True},
    "git": {
        "optional_section": True,
        "fields": {
            "repo_url": REQUIRED,
            "rev": OPTIONAL,
            "depth": OPTIONAL,
            "branch": OPTIONAL,
        },
    },
    "download": {
        "optional_section": True,
        "fields": {"url": REQUIRED, "sha256": REQUIRED},
    },
    "build": {
        "optional_section": True,
        "fields": {
            "builder": REQUIRED,
            "subdir": OPTIONAL,
            "make_binary": OPTIONAL,
            "build_in_src_dir": OPTIONAL,
            "only_install": OPTIONAL,
            "job_weight_mib": OPTIONAL,
            "patchfile": OPTIONAL,
            "patchfile_opts": OPTIONAL,
            "rewrite_includes": OPTIONAL,
        },
    },
    "msbuild": {"optional_section": True, "fields": {"project": REQUIRED}},
    "cargo": {
        "optional_section": True,
        "fields": {
            "build_doc": OPTIONAL,
            "workspace_dir": OPTIONAL,
            "manifests_to_build": OPTIONAL,
            # Where to write cargo config (defaults to build_dir/.cargo/config.toml)
            "cargo_config_file": OPTIONAL,
        },
    },
    "github.actions": {
        "optional_section": True,
        "fields": {
            "run_tests": OPTIONAL,
            "required_locales": OPTIONAL,
            "rust_version": OPTIONAL,
            "build_type": OPTIONAL,
            "sccache": OPTIONAL,
            "timeout_minutes": OPTIONAL,
        },
    },
    "crate.pathmap": {"optional_section": True},
    "cmake.defines": {"optional_section": True},
    "autoconf.args": {"optional_section": True},
    "autoconf.envcmd.LDFLAGS": {"optional_section": True},
    "rpms": {"optional_section": True},
    "debs": {"optional_section": True},
    "homebrew": {"optional_section": True},
    "pps": {"optional_section": True},
    "preinstalled.env": {"optional_section": True},
    "make.build_args": {"optional_section": True},
    "make.install_args": {"optional_section": True},
    "make.test_args": {"optional_section": True},
    "meson.setup_args": {"optional_section": True},
    "header-only": {"optional_section": True, "fields": {"includedir": REQUIRED}},
    "shipit.pathmap": {"optional_section": True},
    "shipit.strip": {"optional_section": True},
    "install.files": {"optional_section": True},
    "subprojects": {"optional_section": True},
    # fb-only
    "sandcastle": {"optional_section": True, "fields": {"run_tests": OPTIONAL}},
    "setup-py.test": {"optional_section": True, "fields": {"python_script": REQUIRED}},
    "setup-py.env": {"optional_section": True},
}

# These sections are allowed to vary for different platforms
# using the expression syntax to enable/disable sections
ALLOWED_EXPR_SECTIONS: list[str] = [
    "autoconf.args",
    "autoconf.envcmd.LDFLAGS",
    "build",
    "cmake.defines",
    "dependencies",
    "make.build_args",
    "make.install_args",
    "download",
    "git",
    "install.files",
    "rpms",
    "debs",
    "shipit.pathmap",
    "shipit.strip",
    "homebrew",
    "github.actions",
    "pps",
]


def parse_dep_spec(spec: str | None) -> tuple[set[str], set[str], bool]:
    """Parse the value of a [dependencies] entry into a feature spec.

    Syntax: comma-separated tokens. A token is either a feature name (positive
    request), `!feature` (prohibition), or `!default` (opt this edge out of the
    dep's default features).

    Returns (requested_features, prohibited_features, opt_out_default).
    """
    requested: set[str] = set()
    prohibited: set[str] = set()
    opt_out_default: bool = False
    if not spec:
        return (requested, prohibited, opt_out_default)
    for token in (t.strip() for t in spec.split(",")):
        if not token:
            continue
        if token == "!default":
            opt_out_default = True
        elif token.startswith("!"):
            prohibited.add(token[1:].strip())
        else:
            requested.add(token)
    return (requested, prohibited, opt_out_default)


def parse_conditional_section_name(name: str, section_def: str) -> ExprNode:
    expr = name[len(section_def) + 1 :]
    return parse_expr(expr, ManifestContext.ALLOWED_VARIABLES)


def validate_allowed_fields(
    file_name: str,
    section: str,
    config: configparser.RawConfigParser,
    allowed_fields: dict[str, str],
) -> None:
    for field in config.options(section):
        if not allowed_fields.get(field):
            raise Exception(
                ("manifest file %s section '%s' contains " "unknown field '%s'")
                % (file_name, section, field)
            )

    for field in allowed_fields:
        if allowed_fields[field] == REQUIRED and not config.has_option(section, field):
            raise Exception(
                ("manifest file %s section '%s' is missing " "required field '%s'")
                % (file_name, section, field)
            )


def validate_allow_values(
    file_name: str, section: str, config: configparser.RawConfigParser
) -> None:
    for field in config.options(section):
        value = config.get(section, field)
        if value is not None:
            raise Exception(
                (
                    "manifest file %s section '%s' has '%s = %s' but "
                    "this section doesn't allow specifying values "
                    "for its entries"
                )
                % (file_name, section, field, value)
            )


def validate_section(
    file_name: str, section: str, config: configparser.RawConfigParser
) -> str:
    section_def = SCHEMA.get(section)
    if not section_def:
        for name in ALLOWED_EXPR_SECTIONS:
            if section.startswith(name + "."):
                # Verify that the conditional parses, but discard it
                try:
                    parse_conditional_section_name(section, name)
                except Exception as exc:
                    raise Exception(
                        ("manifest file %s section '%s' has invalid " "conditional: %s")
                        % (file_name, section, str(exc))
                    )
                section_def = SCHEMA.get(name)
                canonical_section_name = name
                break
        if not section_def:
            raise Exception(
                "manifest file %s contains unknown section '%s'" % (file_name, section)
            )
    else:
        canonical_section_name = section

    allowed_fields = section_def.get("fields")
    if allowed_fields:
        # pyre-ignore[6]: Expected `dict[str, str]` but got `object`.
        validate_allowed_fields(file_name, section, config, allowed_fields)
    elif not section_def.get("allow_values", True):
        validate_allow_values(file_name, section, config)
    # pyre-fixme[61]: `canonical_section_name` is undefined, or not always defined.
    return canonical_section_name


class ManifestParser:
    def __init__(self, file_name: str, fp: str | typing.IO[str] | None = None) -> None:
        # allow_no_value enables listing parameters in the
        # autoconf.args section one per line
        config = configparser.RawConfigParser(allow_no_value=True)
        config.optionxform = str  # type: ignore[assignment]  # make it case sensitive
        if fp is None:
            with open(file_name, "r") as fp:
                config.read_file(fp)
        elif isinstance(fp, type("")):
            # For testing purposes, parse from a string (str
            # or unicode)
            config.read_file(io.StringIO(fp))
        else:
            config.read_file(fp)

        # validate against the schema
        seen_sections: set[str] = set()

        for section in config.sections():
            seen_sections.add(validate_section(file_name, section, config))

        for section in SCHEMA.keys():
            section_def = SCHEMA[section]
            if (
                not section_def.get("optional_section", False)
                and section not in seen_sections
            ):
                raise Exception(
                    "manifest file %s is missing required section %s"
                    % (file_name, section)
                )

        self._config: configparser.RawConfigParser = config

        # [features] section. Keys other than "default" declare valid feature
        # names. The "default" key holds a comma-separated list of features
        # that are enabled when a consumer references this project without an
        # explicit feature spec. See README.md for the full feature system.
        declared: set[str] = set()
        defaults: set[str] = set()
        if config.has_section("features"):
            for key in config.options("features"):
                if key == "default":
                    raw = config.get("features", "default") or ""
                    defaults = {t.strip() for t in raw.split(",") if t.strip()}
                else:
                    declared.add(key)
            unknown = defaults - declared
            if unknown:
                raise Exception(
                    "manifest %s [features] default lists undeclared features: %s"
                    % (file_name, sorted(unknown))
                )
        self.declared_features: set[str] = declared
        self.default_features: set[str] = defaults

        self.name: str = config.get("manifest", "name")
        self.fbsource_path: str | None = self.get("manifest", "fbsource_path")
        self.shipit_project: str | None = self.get("manifest", "shipit_project")
        self.shipit_fbcode_builder: str | None = self.get(
            "manifest", "shipit_fbcode_builder"
        )
        layout = self.get("manifest", "shipit_fbcode_builder_layout") or "build"
        if layout not in FBCODE_BUILDER_LAYOUT_DIRS:
            raise Exception(
                "manifest %s has shipit_fbcode_builder_layout=%s; use one of %s"
                % (file_name, layout, ", ".join(sorted(FBCODE_BUILDER_LAYOUT_DIRS)))
            )
        self.shipit_fbcode_builder_dir: str = FBCODE_BUILDER_LAYOUT_DIRS[layout]
        self.resolved_system_packages: dict[str, str] = {}
        self.shipit_strip_marker: str | None = self.get(
            "manifest", "shipit_strip_marker", defval="@fb-only"
        )

        if self.name != Path(file_name).name:
            raise Exception(
                "filename of the manifest '%s' does not match the manifest name '%s'"
                % (file_name, self.name)
            )

        if "." in self.name:
            raise Exception(
                f"manifest name ({self.name}) must not contain the '.' character (it is incompatible with github actions)"
            )

    @overload
    def get(
        self,
        section: str,
        key: str,
        defval: str,
        ctx: ManifestContext | dict[str, str | None] | None = ...,
    ) -> str: ...

    @overload
    def get(
        self,
        section: str,
        key: str,
        defval: str | None = ...,
        ctx: ManifestContext | dict[str, str | None] | None = ...,
    ) -> str | None: ...

    def get(
        self,
        section: str,
        key: str,
        defval: str | None = None,
        ctx: ManifestContext | dict[str, str | None] | None = None,
    ) -> str | None:
        ctx = ctx or {}

        for s in self._config.sections():
            if s == section:
                if self._config.has_option(s, key):
                    return self._config.get(s, key)
                return defval

            if s.startswith(section + "."):
                expr = parse_conditional_section_name(s, section)
                # pyre-fixme[6]: For 1st argument expected `Dict[str,
                #  Optional[str]]` but got `Union[Dict[str, Optional[str]],
                #  ManifestContext]`.
                if not expr.eval(ctx):
                    continue

                if self._config.has_option(s, key):
                    return self._config.get(s, key)

        return defval

    def get_dependency_specs(
        self, ctx: ManifestContext
    ) -> list[tuple[str, set[str], set[str], bool]]:
        """Returns dependencies as (name, requested_features, prohibited_features,
        opt_out_default) tuples, in declaration order, deduplicated by name."""
        seen: set[str] = set()
        specs: list[tuple[str, set[str], set[str], bool]] = []
        for name, value in self.get_section_as_ordered_pairs("dependencies", ctx):
            if name in seen:
                continue
            seen.add(name)
            req, pro, od = parse_dep_spec(value)
            specs.append((name, req, pro, od))
        return specs

    def get_dependencies(self, ctx: ManifestContext) -> list[str]:
        dep_list = list(self.get_section_as_dict("dependencies", ctx).keys())
        dep_list.sort()
        builder = self.get("build", "builder", ctx=ctx)
        if builder in ("cmake", "python-wheel"):
            dep_list.insert(0, "cmake")
        elif builder == "autoconf" and self.name not in (
            "autoconf",
            "libtool",
            "automake",
        ):
            # they need libtool and its deps (automake, autoconf) so add
            # those as deps (but obviously not if we're building those
            # projects themselves)
            dep_list.insert(0, "libtool")

        return dep_list

    def get_section_as_args(
        self,
        section: str,
        ctx: ManifestContext | dict[str, str | None] | None = None,
    ) -> list[str]:
        """Intended for use with the make.[build_args/install_args] and
        autoconf.args sections, this method collects the entries and returns an
        array of strings.
        If the manifest contains conditional sections, ctx is used to
        evaluate the condition and merge in the values.
        """
        args: list[str] = []
        ctx = ctx or {}

        for s in self._config.sections():
            if s != section:
                if not s.startswith(section + "."):
                    continue
                expr = parse_conditional_section_name(s, section)
                # pyre-fixme[6]: For 1st argument expected `Dict[str,
                #  Optional[str]]` but got `Union[Dict[str, Optional[str]],
                #  ManifestContext]`.
                if not expr.eval(ctx):
                    continue
            for field in self._config.options(s):
                value = self._config.get(s, field)
                if value is None:
                    args.append(field)
                else:
                    args.append("%s=%s" % (field, value))
        return args

    def get_section_as_ordered_pairs(
        self,
        section: str,
        ctx: ManifestContext | dict[str, str | None] | None = None,
    ) -> list[tuple[str, str | None]]:
        """Used for eg: shipit.pathmap which has strong
        ordering requirements"""
        res: list[tuple[str, str | None]] = []
        ctx = ctx or {}

        for s in self._config.sections():
            if s != section:
                if not s.startswith(section + "."):
                    continue
                expr = parse_conditional_section_name(s, section)
                # pyre-fixme[6]: For 1st argument expected `Dict[str,
                #  Optional[str]]` but got `Union[Dict[str, Optional[str]],
                #  ManifestContext]`.
                if not expr.eval(ctx):
                    continue

            for key in self._config.options(s):
                value = self._config.get(s, key)
                res.append((key, value))
        return res

    def get_section_as_dict(
        self,
        section: str,
        ctx: ManifestContext | dict[str, str | None] | None,
    ) -> dict[str, str | None]:
        d: dict[str, str | None] = {}

        for s in self._config.sections():
            if s != section:
                if not s.startswith(section + "."):
                    continue
                expr = parse_conditional_section_name(s, section)
                # pyre-fixme[6]: For 1st argument expected `Dict[str,
                #  Optional[str]]` but got `Union[None, Dict[str, Optional[str]],
                #  ManifestContext]`.
                if not expr.eval(ctx):
                    continue
            for field in self._config.options(s):
                value = self._config.get(s, field)
                d[field] = value
        return d

    def update_hash(self, hasher: hashlib._Hash, ctx: ManifestContext) -> None:
        """Compute a hash over the configuration for the given
        context.  The goal is for the hash to change if the config
        for that context changes, but not if a change is made to
        the config only for a different platform than that expressed
        by ctx.  The hash is intended to be used to help invalidate
        a future cache for the third party build products.
        The hasher argument is a hash object returned from hashlib."""
        for section in sorted(SCHEMA.keys()):
            hasher.update(section.encode("utf-8"))

            # Note: at the time of writing, nothing in the implementation
            # relies on keys in any config section being ordered.
            # In theory we could have conflicting flags in different
            # config sections and later flags override earlier flags.
            # For the purposes of computing a hash we're not super
            # concerned about this: manifest changes should be rare
            # enough and we'd rather that this trigger an invalidation
            # than strive for a cache hit at this time.
            pairs = self.get_section_as_ordered_pairs(section, ctx)
            pairs.sort(key=lambda pair: pair[0])
            for key, value in pairs:
                hasher.update(key.encode("utf-8"))
                if value is not None:
                    hasher.update(value.encode("utf-8"))

    def is_first_party_project(self) -> bool:
        """returns true if this is an FB first-party project"""
        return self.shipit_project is not None

    def get_required_system_packages(
        self, ctx: ManifestContext
    ) -> dict[str, list[str]]:
        """Returns dictionary of packager system -> list of packages"""
        return {
            "rpm": self.get_section_as_args("rpms", ctx),
            "deb": self.get_section_as_args("debs", ctx),
            "homebrew": self.get_section_as_args("homebrew", ctx),
            "pacman-package": self.get_section_as_args("pps", ctx),
        }

    def _is_satisfied_by_preinstalled_environment(self, ctx: ManifestContext) -> bool:
        envs = self.get_section_as_args("preinstalled.env", ctx)
        if not envs:
            return False
        for key in envs:
            val = os.environ.get(key, None)
            print(
                f"Testing ENV[{key}]: {repr(val)}",
                file=sys.stderr,
            )
            if val is None:
                return False
            if len(val) == 0:
                return False

        return True

    def get_repo_url(self, ctx: ManifestContext) -> str | None:
        return self.get("git", "repo_url", ctx=ctx)

    def _create_fetcher(  # noqa: C901
        self, build_options: BuildOptions, ctx: ManifestContext
    ) -> Fetcher:
        real_shipit_available = ShipitTransformerFetcher.available(build_options)
        use_real_shipit = real_shipit_available and (
            build_options.use_shipit
            or self.get("manifest", "use_shipit", defval="false", ctx=ctx) == "true"
        )
        if (
            not use_real_shipit
            and self.fbsource_path
            and build_options.fbsource_dir
            and self.shipit_project
        ):
            return SimpleShipitTransformerFetcher(build_options, self, ctx)

        if (
            self.fbsource_path
            and build_options.fbsource_dir
            and self.shipit_project
            and real_shipit_available
        ):
            # We can use the code from fbsource
            return ShipitTransformerFetcher(
                build_options,
                self.shipit_project,
                # pyre-fixme[6]: For 3rd argument expected `str` but got
                #  `Optional[str]`.
                self.get("manifest", "shipit_external_branch"),
            )

        # If both of these are None, the package can only be coming from
        # preinstalled toolchain or system packages
        repo_url = self.get_repo_url(ctx)
        url = self.get("download", "url", ctx=ctx)

        # Can we satisfy this dep with system packages?
        if (repo_url is None and url is None) or build_options.allow_system_packages:
            if self._is_satisfied_by_preinstalled_environment(ctx):
                # pyre-fixme[7]: Expected `Fetcher` but got `PreinstalledNopFetcher`.
                return PreinstalledNopFetcher()

            if build_options.host_type.get_package_manager():
                packages = self.get_required_system_packages(ctx)
                package_fetcher = SystemPackageFetcher(build_options, packages)
                if package_fetcher.packages_are_installed():
                    # pyre-fixme[7]: Expected `Fetcher` but got `SystemPackageFetcher`.
                    return package_fetcher

        # Only enforce the vendor dir when a network fetcher would otherwise
        # be chosen: projects with no fetch configuration are meant to come
        # from the preinstalled environment or system packages, and `vendor`
        # skips those too, so failing here would point at a dir that can
        # never contain them.
        if build_options.vendor_dir and (repo_url is not None or url is not None):
            vendored = os.path.join(build_options.vendor_dir, self.name)
            if not os.path.isdir(vendored):
                raise Exception(
                    f"project {self.name} is not present in "
                    f"{build_options.vendor_dir}; populate it with "
                    "`getdeps.py vendor` using the same options"
                )
            # pyre-fixme[7]: Expected `Fetcher` but got `LocalDirFetcher`.
            return LocalDirFetcher(vendored)

        if repo_url:
            rev = self.get("git", "rev")
            depth = self.get("git", "depth")
            branch = self.get("git", "branch")
            # pyre-fixme[6]: For 4th argument expected `str` but got `Optional[str]`.
            # pyre-fixme[6]: For 5th argument expected `int` but got `Optional[str]`.
            # pyre-fixme[6]: For 6th argument expected `str` but got `Optional[str]`.
            return GitFetcher(build_options, self, repo_url, rev, depth, branch)

        if url:
            # We need to defer this import until now to avoid triggering
            # a cycle when the facebook/__init__.py is loaded.
            try:
                from .facebook.lfs import LFSCachingArchiveFetcher

                return LFSCachingArchiveFetcher(
                    build_options,
                    self,
                    url,
                    # pyre-fixme[6]: For 4th argument expected `str` but got
                    #  `Optional[str]`.
                    self.get("download", "sha256", ctx=ctx),
                )
            except ImportError:
                # This FB internal module isn't shippped to github,
                # so just use its base class
                return ArchiveFetcher(
                    build_options,
                    self,
                    url,
                    # pyre-fixme[6]: For 4th argument expected `str` but got
                    #  `Optional[str]`.
                    self.get("download", "sha256", ctx=ctx),
                )

        raise KeyError(
            f"project {self.name} has no fetcher configuration or system packages matching {ctx} - have you run `getdeps.py install-system-deps --recursive`?"
        )

    def create_fetcher(
        self,
        build_options: BuildOptions,
        loader: ManifestLoader,
        ctx: ManifestContext,
    ) -> Fetcher:
        fetcher = self._create_fetcher(build_options, ctx)
        subprojects = self.get_section_as_ordered_pairs("subprojects", ctx)
        if subprojects:
            subs: list[tuple[Fetcher, str | None]] = []
            for project, subdir in subprojects:
                submanifest = loader.load_manifest(project)
                subfetcher = submanifest.create_fetcher(build_options, loader, ctx)
                subs.append((subfetcher, subdir))
            # pyre-fixme[6]: For 2nd argument expected `List[Tuple[Fetcher, str]]`
            #  but got `List[Tuple[Fetcher, Optional[str]]]`.
            return SubFetcher(fetcher, subs)
        else:
            return fetcher

    def get_builder_name(self, ctx: ManifestContext) -> str:
        builder = self.get("build", "builder", ctx=ctx)
        if not builder:
            raise Exception("project %s has no builder for %r" % (self.name, ctx))
        return builder

    def create_builder(  # noqa:C901
        self,
        build_options: BuildOptions,
        src_dir: str,
        build_dir: str,
        inst_dir: str,
        ctx: ManifestContext,
        loader: ManifestLoader,
        dep_manifests: list[ManifestParser],
        final_install_prefix: str | None = None,
        extra_cmake_defines: dict[str, str] | None = None,
        cmake_targets: list[str] | None = None,
    ) -> BuilderBase:
        builder = self.get_builder_name(ctx)
        build_in_src_dir = self.get("build", "build_in_src_dir", "false", ctx=ctx)
        if build_in_src_dir == "true":
            # Some scripts don't work when they are configured and build in
            # a different directory than source (or when the build directory
            # is not a subdir of source).
            build_dir = src_dir
            subdir = self.get("build", "subdir", None, ctx=ctx)
            if subdir is not None:
                build_dir = os.fspath(Path(build_dir, subdir))
            print("build_dir is %s" % build_dir)  # just to quiet lint

        if builder == "make" or builder == "cmakebootstrap":
            build_args = self.get_section_as_args("make.build_args", ctx)
            install_args = self.get_section_as_args("make.install_args", ctx)
            test_args = self.get_section_as_args("make.test_args", ctx)
            if builder == "cmakebootstrap":
                return CMakeBootStrapBuilder(
                    loader,
                    dep_manifests,
                    build_options,
                    ctx,
                    self,
                    src_dir,
                    # pyre-fixme[6]: For 7th argument expected `str` but got `None`.
                    None,
                    inst_dir,
                    build_args,
                    install_args,
                    test_args,
                )
            else:
                return MakeBuilder(
                    loader,
                    dep_manifests,
                    build_options,
                    ctx,
                    self,
                    src_dir,
                    # pyre-fixme[6]: For 7th argument expected `str` but got `None`.
                    None,
                    inst_dir,
                    build_args,
                    install_args,
                    test_args,
                )

        if builder == "autoconf":
            args = self.get_section_as_args("autoconf.args", ctx)
            conf_env_args: dict[str, list[str]] = {}
            ldflags_cmd = self.get_section_as_args("autoconf.envcmd.LDFLAGS", ctx)
            if ldflags_cmd:
                conf_env_args["LDFLAGS"] = ldflags_cmd
            return AutoconfBuilder(
                loader,
                dep_manifests,
                build_options,
                ctx,
                self,
                src_dir,
                build_dir,
                inst_dir,
                args,
                conf_env_args,
            )

        if builder == "cmake":
            defines = self.get_section_as_dict("cmake.defines", ctx)
            return CMakeBuilder(
                loader,
                dep_manifests,
                build_options,
                ctx,
                self,
                src_dir,
                build_dir,
                inst_dir,
                # pyre-fixme[6]: For 9th argument expected `Optional[Dict[str,
                #  str]]` but got `Dict[str, Optional[str]]`.
                defines,
                final_install_prefix,
                extra_cmake_defines,
                cmake_targets,
            )

        if builder == "python-wheel":
            return PythonWheelBuilder(
                loader,
                dep_manifests,
                build_options,
                ctx,
                self,
                src_dir,
                build_dir,
                inst_dir,
            )

        if builder == "sqlite":
            return SqliteBuilder(
                loader,
                dep_manifests,
                build_options,
                ctx,
                self,
                src_dir,
                build_dir,
                inst_dir,
            )

        if builder == "ninja_bootstrap":
            return NinjaBootstrap(
                loader,
                dep_manifests,
                build_options,
                ctx,
                self,
                build_dir,
                src_dir,
                inst_dir,
            )

        if builder == "nop":
            return NopBuilder(
                loader, dep_manifests, build_options, ctx, self, src_dir, inst_dir
            )

        if builder == "openssl":
            return OpenSSLBuilder(
                loader,
                dep_manifests,
                build_options,
                ctx,
                self,
                build_dir,
                src_dir,
                inst_dir,
            )

        if builder == "iproute2":
            return Iproute2Builder(
                loader,
                dep_manifests,
                build_options,
                ctx,
                self,
                src_dir,
                build_dir,
                inst_dir,
            )

        if builder == "meson":
            return MesonBuilder(
                loader,
                dep_manifests,
                build_options,
                ctx,
                self,
                src_dir,
                build_dir,
                inst_dir,
            )

        if builder == "setup-py":
            return SetupPyBuilder(
                loader,
                dep_manifests,
                build_options,
                ctx,
                self,
                src_dir,
                build_dir,
                inst_dir,
            )

        if builder == "cargo":
            return self.create_cargo_builder(
                loader,
                dep_manifests,
                build_options,
                ctx,
                src_dir,
                build_dir,
                inst_dir,
            )

        raise KeyError("project %s has no known builder" % (self.name))

    def create_prepare_builders(
        self,
        build_options: BuildOptions,
        ctx: ManifestContext,
        src_dir: str,
        build_dir: str,
        inst_dir: str,
        loader: ManifestLoader,
        dep_manifests: list[ManifestParser],
    ) -> list[BuilderBase]:
        """Create builders that have a prepare step run, e.g. to write config files"""
        prepare_builders: list[BuilderBase] = []
        builder = self.get_builder_name(ctx)
        cargo = self.get_section_as_dict("cargo", ctx)
        if not builder == "cargo" and cargo:
            cargo_builder = self.create_cargo_builder(
                loader,
                dep_manifests,
                build_options,
                ctx,
                src_dir,
                build_dir,
                inst_dir,
            )
            prepare_builders.append(cargo_builder)
        return prepare_builders

    def create_cargo_builder(
        self,
        loader: ManifestLoader,
        dep_manifests: list[ManifestParser],
        build_options: BuildOptions,
        ctx: ManifestContext,
        src_dir: str,
        build_dir: str,
        inst_dir: str,
    ) -> CargoBuilder:
        # pyre-fixme[6]: For 3rd argument expected `Optional[str]` but got `bool`.
        build_doc = self.get("cargo", "build_doc", False, ctx)
        workspace_dir = self.get("cargo", "workspace_dir", None, ctx)
        manifests_to_build = self.get("cargo", "manifests_to_build", None, ctx)
        cargo_config_file = self.get("cargo", "cargo_config_file", None, ctx)
        return CargoBuilder(
            loader,
            dep_manifests,
            build_options,
            ctx,
            self,
            src_dir,
            build_dir,
            inst_dir,
            build_doc,
            workspace_dir,
            manifests_to_build,
            cargo_config_file,
        )


class ManifestContext:
    """ProjectContext contains a dictionary of values to use when evaluating boolean
    expressions in a project manifest.

    This object should be passed as the `ctx` parameter in ManifestParser.get() calls.
    """

    ALLOWED_VARIABLES: set[str] = {
        "os",
        "distro",
        "distro_vers",
        "distro_family",
        "fb",
        "fbsource",
        "test",
    }

    def __init__(self, ctx_dict: dict[str, str | None]) -> None:
        base_keys = {k for k in ctx_dict if not k.startswith("feature_")}
        assert base_keys == self.ALLOWED_VARIABLES
        self.ctx_dict: dict[str, str | None] = ctx_dict

    def get(self, key: str) -> str | None:
        # Undeclared/unset features evaluate to "off" so that
        # `[section.feature_foo=off]` matches when feature_foo is absent.
        if key.startswith("feature_"):
            return self.ctx_dict.get(key, "off")
        return self.ctx_dict[key]

    def set(self, key: str, value: str | None) -> None:
        assert key in self.ALLOWED_VARIABLES or key.startswith("feature_")
        self.ctx_dict[key] = value

    def features(self) -> typing.Set[str]:
        return {
            k[len("feature_") :]
            for k, v in self.ctx_dict.items()
            if k.startswith("feature_") and v == "on"
        }

    def copy(self) -> ManifestContext:
        return ManifestContext(dict(self.ctx_dict))

    def __str__(self) -> str:
        s = ", ".join(
            "%s=%s" % (key, value) for key, value in sorted(self.ctx_dict.items())
        )
        return "{" + s + "}"


class ContextGenerator:
    """ContextGenerator allows creating ManifestContext objects on a per-project basis.
    This allows us to evaluate different projects with slightly different contexts.

    For instance, this can be used to only enable tests for some projects."""

    def __init__(self, default_ctx: dict[str, str | None]) -> None:
        self.default_ctx: ManifestContext = ManifestContext(default_ctx)
        self.ctx_by_project: dict[str, ManifestContext] = {}

    def set_value_for_project(
        self, project_name: str, key: str, value: str | None
    ) -> None:
        project_ctx = self.ctx_by_project.get(project_name)
        if project_ctx is None:
            project_ctx = self.default_ctx.copy()
            self.ctx_by_project[project_name] = project_ctx
        project_ctx.set(key, value)

    def set_features_for_project(self, project_name: str, features: set[str]) -> None:
        project_ctx = self.ctx_by_project.get(project_name)
        if project_ctx is None:
            project_ctx = self.default_ctx.copy()
            self.ctx_by_project[project_name] = project_ctx
        for k in list(project_ctx.ctx_dict.keys()):
            if k.startswith("feature_"):
                del project_ctx.ctx_dict[k]
        for f in features:
            project_ctx.set("feature_" + f, "on")

    def set_value_for_all_projects(self, key: str, value: str | None) -> None:
        self.default_ctx.set(key, value)
        for ctx in self.ctx_by_project.values():
            ctx.set(key, value)

    def get_context(self, project_name: str) -> ManifestContext:
        return self.ctx_by_project.get(project_name, self.default_ctx)
