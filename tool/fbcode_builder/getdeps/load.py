# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import annotations

import base64
import copy
import hashlib
import os
import typing
from collections.abc import Iterator
from pathlib import Path

from . import fetcher
from .envfuncs import path_search
from .errors import ManifestNotFound
from .manifest import ManifestParser

if typing.TYPE_CHECKING:
    from .buildopts import BuildOptions
    from .manifest import ContextGenerator, ManifestContext


class Loader:
    """The loader allows our tests to patch the load operation"""

    def _list_manifests(self, build_opts: BuildOptions) -> Iterator[str]:
        """Returns a generator that iterates all the available manifests"""
        for path, _, files in os.walk(build_opts.manifests_dir):
            for name in files:
                # skip hidden files
                if name.startswith("."):
                    continue

                yield os.fspath(Path(path, name))

    def _load_manifest(self, path: str) -> ManifestParser:
        return ManifestParser(path)

    def load_project(
        self, build_opts: BuildOptions, project_name: str
    ) -> ManifestParser:
        if "/" in project_name or "\\" in project_name:
            # Assume this is a path already
            return ManifestParser(project_name)

        for manifest in self._list_manifests(build_opts):
            if Path(manifest).name == project_name:
                return ManifestParser(manifest)

        raise ManifestNotFound(project_name)

    def load_all(self, build_opts: BuildOptions) -> dict[str, ManifestParser]:
        manifests_by_name: dict[str, ManifestParser] = {}

        for manifest in self._list_manifests(build_opts):
            m = self._load_manifest(manifest)

            if m.name in manifests_by_name:
                raise Exception("found duplicate manifest '%s'" % m.name)

            manifests_by_name[m.name] = m

        return manifests_by_name


class ResourceLoader(Loader):
    def __init__(self, namespace: str, manifests_dir: str) -> None:
        self.namespace: str = namespace
        self.manifests_dir: str = manifests_dir

    def _list_manifests(self, build_opts: BuildOptions) -> Iterator[str]:
        import pkg_resources

        dirs: list[str] = [self.manifests_dir]

        while dirs:
            current = dirs.pop(0)
            for name in pkg_resources.resource_listdir(self.namespace, current):
                path = "%s/%s" % (current, name)

                if pkg_resources.resource_isdir(self.namespace, path):
                    dirs.append(path)
                else:
                    yield "%s/%s" % (current, name)

    def _find_manifest(self, build_opts: BuildOptions, project_name: str) -> str:
        for name in self._list_manifests(build_opts):
            if name.endswith("/%s" % project_name):
                return name

        raise ManifestNotFound(project_name)

    def _load_manifest(self, path: str) -> ManifestParser:
        import pkg_resources

        contents = pkg_resources.resource_string(self.namespace, path).decode("utf8")
        return ManifestParser(file_name=path, fp=contents)

    def load_project(
        self, build_opts: BuildOptions, project_name: str
    ) -> ManifestParser:
        project_name = self._find_manifest(build_opts, project_name)
        return self._load_manifest(project_name)


LOADER: Loader = Loader()


def patch_loader(namespace: str, manifests_dir: str = "manifests") -> None:
    global LOADER
    LOADER = ResourceLoader(namespace, manifests_dir)


def load_project(build_opts: BuildOptions, project_name: str) -> ManifestParser:
    """given the name of a project or a path to a manifest file,
    load up the ManifestParser instance for it and return it"""
    return LOADER.load_project(build_opts, project_name)


def load_all_manifests(build_opts: BuildOptions) -> dict[str, ManifestParser]:
    return LOADER.load_all(build_opts)


class ManifestLoader:
    """ManifestLoader stores information about project manifest relationships for a
    given set of (build options + platform) configuration.

    The ManifestLoader class primarily serves as a location to cache project dependency
    relationships and project hash values for this build configuration.
    """

    def __init__(
        self, build_opts: BuildOptions, ctx_gen: ContextGenerator | None = None
    ) -> None:
        self._loader: Loader = LOADER
        self.build_opts: BuildOptions = build_opts
        if ctx_gen is None:
            self.ctx_gen: ContextGenerator = self.build_opts.get_context_generator()
        else:
            self.ctx_gen = ctx_gen

        self.manifests_by_name: dict[str, ManifestParser] = {}
        self._loaded_all: bool = False
        self._features_resolved: bool = False
        self._project_hashes: dict[str, str] = {}
        self._fetcher_overrides: dict[str, fetcher.LocalDirFetcher] = {}
        self._build_dir_overrides: dict[str, str] = {}
        self._install_dir_overrides: dict[str, str] = {}
        self._install_prefix_overrides: dict[str, str] = {}

    def load_manifest(self, name: str) -> ManifestParser:
        manifest = self.manifests_by_name.get(name)
        if manifest is None:
            manifest = self._loader.load_project(self.build_opts, name)
            self.manifests_by_name[name] = manifest
        return manifest

    def load_all_manifests(self) -> dict[str, ManifestParser]:
        if not self._loaded_all:
            all_manifests_by_name = self._loader.load_all(self.build_opts)
            if self.manifests_by_name:
                # To help ensure that we only ever have a single manifest object for a
                # given project, and that it can't change once we have loaded it,
                # only update our mapping for projects that weren't already loaded.
                for name, manifest in all_manifests_by_name.items():
                    self.manifests_by_name.setdefault(name, manifest)
            else:
                self.manifests_by_name = all_manifests_by_name
            self._loaded_all = True

        return self.manifests_by_name

    def resolve_features_from(self, root_name: str, project_name: str) -> None:
        """Resolve features from root_name's dependency graph, with tests off.

        Building a dependency on its own resolves its features as if it were
        the root, which can differ from what the projects above it request.
        Resolving from the project that needs the dependency, in a context
        that does not depend on what a step builds, gives every build of it
        the same features and hash. A separate loader walks the root's graph,
        so the projects this loader builds stay those of project_name.
        """
        names = self._resolve_graph_features(root_name, self.ctx_gen)
        if project_name not in names:
            raise ValueError(f"{project_name} is not a dependency of {root_name}")

        # Feature requests made only for test builds would be ignored here, so
        # reject any difference from a resolution with tests on.
        test_ctx_gen = copy.deepcopy(self.ctx_gen)
        test_ctx_gen.set_value_for_all_projects("test", "on")
        test_names = self._resolve_graph_features(root_name, test_ctx_gen)
        for name in sorted(names | test_names):
            off = self.ctx_gen.get_context(name).features()
            on = test_ctx_gen.get_context(name).features()
            if off != on:
                raise ValueError(
                    f"{name} gets features {sorted(on)} in {root_name}'s graph "
                    f"with tests on but {sorted(off)} with tests off; "
                    "--features-from does not support feature requests made "
                    "only for test builds"
                )
        self._features_resolved = True

    def _resolve_graph_features(
        self, root_name: str, ctx_gen: ContextGenerator
    ) -> set[str]:
        graph = ManifestLoader(self.build_opts, ctx_gen)
        graph._loader = self._loader
        root = graph.load_manifest(root_name)
        graph.resolve_features([root])
        return {m.name for m in graph.manifests_in_dependency_order(root)}

    def dependencies_of(self, manifest: ManifestParser) -> list[ManifestParser]:
        """Returns the dependencies of the given project, not including the project itself, in topological order."""
        return [
            dep
            for dep in self.manifests_in_dependency_order(manifest)
            if dep != manifest
        ]

    def resolve_features(self, roots: list[ManifestParser]) -> None:  # noqa:C901
        """Resolve the feature set for each project reachable from the given roots.

        Each root project starts with its own [features] default set. Each edge
        in the dependency graph may add positive feature requests, prohibitions,
        or opt out of the dep's defaults via bracket syntax in its dep spec.
        Features compose by union; prohibitions are checked against the union
        and surface as an error if any consumer's prohibition conflicts with
        another consumer's request.

        Iterates to a fixpoint because dependency edges themselves can be gated
        on feature variables, so adding a feature can introduce new edges that
        contribute further feature requests.
        """
        requested: dict[str, set[str]] = {}
        prohibited: dict[str, set[str]] = {}
        # Per-project, the set of consumer names that requested each feature
        # (and prohibited each feature) — used only for error messages.
        req_sources: dict[str, dict[str, list[str]]] = {}
        pro_sources: dict[str, dict[str, list[str]]] = {}

        def add_request(project: str, feats: set[str], src: str) -> bool:
            new_project = project not in requested
            old = requested.setdefault(project, set())
            new = old | feats
            srcs = req_sources.setdefault(project, {})
            for f in feats:
                fsrcs = srcs.setdefault(f, [])
                if src not in fsrcs:
                    fsrcs.append(src)
            if new != old:
                requested[project] = new
            return new_project or new != old

        def add_prohibit(project: str, feats: set[str], src: str) -> bool:
            old = prohibited.setdefault(project, set())
            new = old | feats
            srcs = pro_sources.setdefault(project, {})
            for f in feats:
                fsrcs = srcs.setdefault(f, [])
                if src not in fsrcs:
                    fsrcs.append(src)
            if new != old:
                prohibited[project] = new
                return True
            return False

        for root in roots:
            add_request(root.name, set(root.default_features), "<root>")

        changed = True
        while changed:
            changed = False
            for project_name in list(requested.keys()):
                manifest = self.load_manifest(project_name)
                self.ctx_gen.set_features_for_project(
                    project_name, requested[project_name]
                )
                ctx = self.ctx_gen.get_context(project_name)
                for (
                    dep_name,
                    edge_req,
                    edge_pro,
                    opt_out,
                ) in manifest.get_dependency_specs(ctx):
                    dep_manifest = self.load_manifest(dep_name)
                    unknown = (edge_req | edge_pro) - dep_manifest.declared_features
                    if unknown:
                        raise Exception(
                            "%s: dependency on %s references undeclared feature(s) %s"
                            % (project_name, dep_name, sorted(unknown))
                        )
                    contributed = set(edge_req)
                    if not opt_out:
                        contributed |= dep_manifest.default_features
                    contributed -= edge_pro
                    if add_request(dep_name, contributed, project_name):
                        changed = True
                    if add_prohibit(dep_name, edge_pro, project_name):
                        changed = True

        conflicts: list[str] = []
        for project_name, req in requested.items():
            pro = prohibited.get(project_name, set())
            bad = req & pro
            for f in sorted(bad):
                req_who = ", ".join(req_sources[project_name].get(f, []))
                pro_who = ", ".join(pro_sources[project_name].get(f, []))
                conflicts.append(
                    "%s: feature %r requested by [%s] but prohibited by [%s]"
                    % (project_name, f, req_who, pro_who)
                )
        if conflicts:
            raise Exception(
                "feature resolution conflicts:\n  " + "\n  ".join(conflicts)
            )

        for project_name, req in requested.items():
            self.ctx_gen.set_features_for_project(project_name, req)
        self._features_resolved = True

    def manifests_in_dependency_order(  # noqa:C901
        self, manifest: ManifestParser | None = None
    ) -> list[ManifestParser]:
        """Compute all dependencies of the specified project.  Returns a list of the
        dependencies plus the project itself, in topologically sorted order.

        Each entry in the returned list only depends on projects that appear before it
        in the list.

        If the input manifest is None, the dependencies for all currently loaded
        projects will be computed.  i.e., if you call load_all_manifests() followed by
        manifests_in_dependency_order() this will return a global dependency ordering of
        all projects."""
        # The list of deps that have been fully processed
        seen: set[str] = set()
        # The list of deps which have yet to be evaluated.  This
        # can potentially contain duplicates.
        if manifest is None:
            deps: list[ManifestParser] = list(self.manifests_by_name.values())
            if not self._features_resolved:
                self.resolve_features(deps)
        else:
            assert manifest.name in self.manifests_by_name
            deps = [manifest]
            if not self._features_resolved:
                self.resolve_features([manifest])
        # The list of manifests in dependency order
        dep_order: list[ManifestParser] = []
        system_packages: dict[str, list[str]] = {}

        while len(deps) > 0:
            m = deps.pop(0)
            if m.name in seen:
                continue

            # Consider its deps, if any.
            # We sort them for increased determinism; we'll produce
            # a correct order even if they aren't sorted, but we prefer
            # to produce the same order regardless of how they are listed
            # in the project manifest files.
            ctx: ManifestContext = self.ctx_gen.get_context(m.name)
            dep_list: list[str] = m.get_dependencies(ctx)

            dep_count: int = 0
            for dep_name in dep_list:
                # If we're not sure whether it is done, queue it up
                if dep_name not in seen:
                    dep = self.manifests_by_name.get(dep_name)
                    if dep is None:
                        dep = self._loader.load_project(self.build_opts, dep_name)
                        self.manifests_by_name[dep.name] = dep

                    deps.append(dep)
                    dep_count += 1

            if dep_count > 0:
                # If we queued anything, re-queue this item, as it depends
                # those new item(s) and their transitive deps.
                deps.append(m)
                continue

            # Its deps are done, so we can emit it
            seen.add(m.name)
            # Capture system packages as we may need to set PATHs to then later
            if (
                self.build_opts.allow_system_packages
                and self.build_opts.host_type.get_package_manager()
            ):
                packages: dict[str, list[str]] = m.get_required_system_packages(ctx)
                for pkg_type, v in packages.items():
                    merged: list[str] = system_packages.get(pkg_type, [])
                    if v not in merged:
                        merged += v
                    system_packages[pkg_type] = merged
                # A manifest depends on all system packages in it dependencies as well
                # pyre-fixme[8]: Attribute has type `Dict[str, str]`; used as
                #  `Dict[str, List[str]]`.
                m.resolved_system_packages = copy.copy(system_packages)
            dep_order.append(m)

        return dep_order

    def set_project_src_dir(self, project_name: str, path: str) -> None:
        self._fetcher_overrides[project_name] = fetcher.LocalDirFetcher(path)

    def set_project_build_dir(self, project_name: str, path: str) -> None:
        self._build_dir_overrides[project_name] = path

    def set_project_install_dir(self, project_name: str, path: str) -> None:
        self._install_dir_overrides[project_name] = path

    def set_project_install_prefix(self, project_name: str, path: str) -> None:
        self._install_prefix_overrides[project_name] = path

    def create_fetcher(
        self, manifest: ManifestParser
    ) -> fetcher.Fetcher | fetcher.LocalDirFetcher:
        override = self._fetcher_overrides.get(manifest.name)
        if override is not None:
            return override

        ctx: ManifestContext = self.ctx_gen.get_context(manifest.name)
        return manifest.create_fetcher(self.build_opts, self, ctx)

    def get_project_hash(self, manifest: ManifestParser) -> str:
        if not self._features_resolved:
            self.manifests_in_dependency_order(manifest)
        h = self._project_hashes.get(manifest.name)
        if h is None:
            h = self._compute_project_hash(manifest)
            self._project_hashes[manifest.name] = h
        return h

    def _compute_project_hash(self, manifest: ManifestParser) -> str:
        """This recursive function computes a hash for a given manifest.
        The hash takes into account some environmental factors on the
        host machine and includes the hashes of its dependencies.
        No caching of the computation is performed, which is theoretically
        wasteful but the computation is fast enough that it is not required
        to cache across multiple invocations."""
        ctx: ManifestContext = self.ctx_gen.get_context(manifest.name)

        hasher = hashlib.sha256()
        # Some environmental and configuration things matter
        env: dict[str, str | None] = {}
        env["install_dir"] = self.build_opts.install_dir
        env["scratch_dir"] = self.build_opts.scratch_dir
        env["vcvars_path"] = self.build_opts.vcvars_path
        env["os"] = self.build_opts.host_type.ostype
        env["distro"] = self.build_opts.host_type.distro
        env["distro_vers"] = self.build_opts.host_type.distrovers
        env["shared_lib"] = str(self.build_opts.shared_lib)
        env["features"] = ",".join(sorted(ctx.features()))
        for name in [
            "CXXFLAGS",
            "CPPFLAGS",
            "LDFLAGS",
            "CXX",
            "CC",
            "GETDEPS_CMAKE_DEFINES",
        ]:
            env[name] = os.environ.get(name)
        for tool in ["cc", "c++", "gcc", "g++", "clang", "clang++"]:
            env["tool-%s" % tool] = path_search(os.environ, tool)
        for name in manifest.get_section_as_args("depends.environment", ctx):
            env[name] = os.environ.get(name)

        fetcher_inst: fetcher.Fetcher | fetcher.LocalDirFetcher = self.create_fetcher(
            manifest
        )
        env["fetcher.hash"] = fetcher_inst.hash()

        for name in sorted(env.keys()):
            hasher.update(name.encode("utf-8"))
            value = env.get(name)
            if value is not None:
                try:
                    hasher.update(value.encode("utf-8"))
                except AttributeError as exc:
                    raise AttributeError("name=%r, value=%r: %s" % (name, value, exc))

        manifest.update_hash(hasher, ctx)

        # If a patchfile is specified, include its contents in the hash
        patchfile: str | None = manifest.get("build", "patchfile", ctx=ctx)
        if patchfile:
            patchfile_path: str = os.fspath(
                Path(self.build_opts.fbcode_builder_dir, "patches", patchfile)
            )
            if not Path(patchfile_path).exists():
                raise RuntimeError(
                    f"Patchfile '{patchfile}' is listed in the '{manifest.name}' manifest "
                    f"but was not found at '{patchfile_path}'. "
                    f"Possible fixes: ensure the patches/ directory is present in your "
                    f"checkout, check that the patchfile name in the manifest matches the "
                    f"filename on disk, or remove the 'patchfile' entry from the manifest "
                    f"if the patch is no longer needed."
                )
            with open(patchfile_path, "rb") as f:
                hasher.update(f.read())

        dep_list: list[str] = manifest.get_dependencies(ctx)
        for dep in dep_list:
            dep_manifest: ManifestParser = self.load_manifest(dep)
            dep_hash: str = self.get_project_hash(dep_manifest)
            hasher.update(dep_hash.encode("utf-8"))

        # Use base64 to represent the hash, rather than the simple hex digest,
        # so that the string is shorter.  Use the URL-safe encoding so that
        # the hash can also be safely used as a filename component.
        h: str = base64.urlsafe_b64encode(hasher.digest()).decode("ascii")
        # ... and because cmd.exe is troublesome with `=` signs, nerf those.
        # They tend to be padding characters at the end anyway, so we can
        # safely discard them.
        h = h.replace("=", "")

        return h

    def _get_project_dir_name(self, manifest: ManifestParser) -> str:
        if manifest.is_first_party_project():
            return manifest.name
        else:
            project_hash: str = self.get_project_hash(manifest)
            return "%s-%s" % (manifest.name, project_hash)

    def get_project_install_dir(self, manifest: ManifestParser) -> str:
        override = self._install_dir_overrides.get(manifest.name)
        if override:
            return override

        project_dir_name: str = self._get_project_dir_name(manifest)
        return os.fspath(Path(self.build_opts.install_dir, project_dir_name))

    def get_project_build_dir(self, manifest: ManifestParser) -> str:
        override = self._build_dir_overrides.get(manifest.name)
        if override:
            return override

        project_dir_name: str = self._get_project_dir_name(manifest)
        return os.fspath(Path(self.build_opts.scratch_dir, "build", project_dir_name))

    def get_project_install_prefix(self, manifest: ManifestParser) -> str | None:
        return self._install_prefix_overrides.get(manifest.name)

    def get_project_install_dir_respecting_install_prefix(
        self, manifest: ManifestParser
    ) -> str:
        inst_dir: str = self.get_project_install_dir(manifest)
        prefix: str | None = self.get_project_install_prefix(manifest)
        if prefix:
            return inst_dir + prefix
        return inst_dir
