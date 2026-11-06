#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import annotations

import errno
import hashlib
import os
import random
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tarfile
import time
import zipfile
from abc import ABC, abstractmethod
from collections.abc import Iterator
from datetime import datetime
from pathlib import Path, PurePosixPath
from typing import NamedTuple, TYPE_CHECKING
from urllib.parse import urlparse
from urllib.request import Request, urlopen

from .copytree import prefetch_dir_if_eden
from .envfuncs import Env
from .errors import TransientFailure
from .getdeps_platform import HostType, is_windows
from .runcmd import run_cmd

if TYPE_CHECKING:
    from .buildopts import BuildOptions
    from .manifest import ManifestContext, ManifestParser


def _validate_archive_members(names: list[str], dest_dir: str | Path) -> None:
    """Validate archive member paths to prevent path traversal (Zip Slip) attacks."""
    dest = Path(dest_dir).resolve()
    for name in names:
        if Path(name).is_absolute():
            raise ValueError(f"Blocked absolute path in archive: {name!r}")
        # resolve() normalizes ".." and follows symlinks; relative_to is
        # component-aware, unlike a raw startswith prefix check.
        try:
            (dest / name).resolve().relative_to(dest)
        except ValueError:
            raise ValueError(f"Blocked path traversal in archive: {name!r}")


def safe_extractall(
    archive: tarfile.TarFile | zipfile.ZipFile, dest: str | Path
) -> None:
    """Safely extract a tar or zip archive with path traversal protection."""
    if isinstance(archive, tarfile.TarFile):
        _validate_archive_members(archive.getnames(), dest)
        try:
            archive.extractall(dest, filter="data")
        except TypeError:
            # Python < 3.12 without filter support
            archive.extractall(dest)
    elif isinstance(archive, zipfile.ZipFile):
        _validate_archive_members(archive.namelist(), dest)
        archive.extractall(dest)
    else:
        raise TypeError(f"Unsupported archive type: {type(archive)}")


def file_name_is_cmake_file(file_name: str | Path) -> bool:
    base = Path(file_name).name.lower()
    return (
        base.endswith(".cmake")
        or base.endswith(".cmake.in")
        or base == "cmakelists.txt"
    )


class ChangeStatus:
    """Indicates the nature of changes that happened while updating
    the source directory.  There are two broad uses:
    * When extracting archives for third party software we want to
      know that we did something (eg: we either extracted code or
      we didn't do anything)
    * For 1st party code where we use shipit to transform the code,
      we want to know if we changed anything so that we can perform
      a build, but we generally want to be a little more nuanced
      and be able to distinguish between just changing a source file
      and whether we might need to reconfigure the build system.
    """

    def __init__(self, all_changed: bool = False) -> None:
        """Construct a ChangeStatus object.  The default is to create
        a status that indicates no changes, but passing all_changed=True
        will create one that indicates that everything changed"""
        if all_changed:
            self.source_files: int = 1
            self.make_files: int = 1
        else:
            self.source_files: int = 0
            self.make_files: int = 0

    def record_change(self, file_name: str) -> None:
        """Used by the shipit fetcher to record changes as it updates
        files in the destination.  If the file name might be one used
        in the cmake build system that we use for 1st party code, then
        record that as a "make file" change.  We could broaden this
        to match any file used by various build systems, but it is
        only really useful for our internal cmake stuff at this time.
        If the file isn't a build file and is under the `fbcode_builder`
        dir then we don't class that as an interesting change that we
        might need to rebuild, so we ignore it.
        Otherwise we record the file as a source file change."""

        file_name = file_name.lower()
        if file_name_is_cmake_file(file_name):
            self.make_files += 1
        elif "/fbcode_builder/cmake" in file_name:
            self.source_files += 1
        elif "/fbcode_builder/" not in file_name:
            self.source_files += 1

    def sources_changed(self) -> bool:
        """Returns true if any source files were changed during
        an update operation.  This will typically be used to decide
        that the build system to be run on the source dir in an
        incremental mode"""
        return self.source_files > 0

    def build_changed(self) -> bool:
        """Returns true if any build files were changed during
        an update operation.  This will typically be used to decidfe
        that the build system should be reconfigured and re-run
        as a full build"""
        return self.make_files > 0


class Fetcher(ABC):
    """The Fetcher is responsible for fetching and extracting the
    sources for project.  The Fetcher instance defines where the
    extracted data resides and reports this to the consumer via
    its `get_src_dir` method."""

    def update(self) -> ChangeStatus:
        """Brings the src dir up to date, ideally minimizing
        changes so that a subsequent build doesn't over-build.
        Returns a ChangeStatus object that helps the caller to
        understand the nature of the changes required during
        the update."""
        return ChangeStatus()

    @abstractmethod
    def clean(self) -> None:
        """Reverts any changes that might have been made to
        the src dir"""
        pass

    @abstractmethod
    def hash(self) -> str:
        """Returns a hash that identifies the version of the code in the
        working copy.  For a git repo this is commit hash for the working
        copy.  For other Fetchers this should relate to the version of
        the code in the src dir.  The intent is that if a manifest
        changes the version/rev of a project that the hash be different.
        Importantly, this should be computable without actually fetching
        the code, as we want this to factor into a hash used to download
        a pre-built version of the code, without having to first download
        and extract its sources (eg: boost on windows is pretty painful).
        """
        pass

    @abstractmethod
    def get_src_dir(self) -> str:
        """Returns the source directory that the project was
        extracted into"""
        pass


class LocalDirFetcher:
    """This class exists to override the normal fetching behavior, and
    use an explicit user-specified directory for the project sources.

    This fetcher cannot update or track changes.  It always reports that the
    project has changed, forcing it to always be built."""

    def __init__(self, path: str | Path) -> None:
        self.path: str = os.fspath(Path(path).resolve())

    def update(self) -> ChangeStatus:
        return ChangeStatus(all_changed=True)

    def hash(self) -> str:
        return "0" * 40

    def get_src_dir(self) -> str:
        return self.path

    def clean(self) -> None:
        pass


class SystemPackageFetcher:
    def __init__(
        self, build_options: BuildOptions, packages: dict[str, list[str]]
    ) -> None:
        self.manager: str | None = build_options.host_type.get_package_manager()
        # pyre-fixme[6]: For 1st argument expected `str` but got `Optional[str]`.
        self.packages: list[str] | None = packages.get(self.manager)
        self.host_type: HostType = build_options.host_type
        if self.packages:
            self.installed: bool | None = None
        else:
            self.installed = False

    def packages_are_installed(self) -> bool:
        if self.installed is not None:
            return self.installed

        cmd = None
        if self.manager == "rpm":
            # pyre-fixme[6]: For 1st argument expected
            #  `pyre_extensions.PyreReadOnly[Iterable[SupportsRichComparisonT]]` but
            #  got `Optional[List[str]]`.
            cmd = ["rpm", "-q"] + sorted(self.packages)
        elif self.manager == "deb":
            # pyre-fixme[6]: For 1st argument expected
            #  `pyre_extensions.PyreReadOnly[Iterable[SupportsRichComparisonT]]` but
            #  got `Optional[List[str]]`.
            cmd = ["dpkg", "-s"] + sorted(self.packages)
        elif self.manager == "homebrew":
            # pyre-fixme[6]: For 1st argument expected
            #  `pyre_extensions.PyreReadOnly[Iterable[SupportsRichComparisonT]]` but
            #  got `Optional[List[str]]`.
            cmd = ["brew", "ls", "--versions"] + sorted(self.packages)

        if cmd:
            proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if proc.returncode == 0:
                # captured as binary as we will hash this later
                # pyre-fixme[8]: Attribute has type `Optional[bool]`; used as `bytes`.
                self.installed = proc.stdout
            else:
                # Need all packages to be present to consider us installed
                self.installed = False

        else:
            self.installed = False

        return bool(self.installed)

    def update(self) -> ChangeStatus:
        assert self.installed
        return ChangeStatus(all_changed=False)

    def hash(self) -> str:
        if self.packages_are_installed():
            # SystemPackageFetcher stashes the package-manager query output
            # (bytes including versions) in self.installed so upgrades change
            # the hash. PreinstalledNopFetcher just sets self.installed=True
            # and has no .packages, so fall back to an empty package list.
            if isinstance(self.installed, (bytes, bytearray)):
                payload = bytes(self.installed)
            else:
                packages = getattr(self, "packages", None) or []
                payload = ",".join(sorted(packages)).encode("utf-8")
            return hashlib.sha256(payload).hexdigest()
        else:
            return "0" * 40

    def get_src_dir(self) -> None:
        return None


class PreinstalledNopFetcher(SystemPackageFetcher):
    def __init__(self) -> None:
        self.installed = True


class GitFetcher(Fetcher):
    DEFAULT_DEPTH = 1

    def __init__(
        self,
        build_options: BuildOptions,
        manifest: ManifestParser,
        repo_url: str,
        rev: str,
        depth: int,
        branch: str,
    ) -> None:
        # Extract the host/path portions of the URL and generate a flattened
        # directory name.  eg:
        # github.com/facebook/folly.git -> github.com-facebook-folly.git
        url = urlparse(repo_url)
        directory = "%s%s%s" % (url.netloc, url.path, branch if branch else "")
        for s in ["/", "\\", ":"]:
            directory = directory.replace(s, "-")

        # Place it in a repos dir in the scratch space
        repos_dir = Path(build_options.scratch_dir, "repos")
        repos_dir.mkdir(parents=True, exist_ok=True)
        self.repo_dir: str = os.fspath(repos_dir / directory)

        if not rev and build_options.project_hashes:
            hash_file = Path(build_options.project_hashes) / re.sub(
                "\\.git$", "-rev.txt", url.path[1:]
            )
            if hash_file.exists():
                with open(hash_file, "r") as f:
                    data = f.read()
                    m = re.match("Subproject commit ([a-fA-F0-9]{40})", data)
                    if not m:
                        raise Exception("Failed to parse rev from %s" % hash_file)
                    rev = m.group(1)
                    print(
                        "Using pinned rev %s for %s" % (rev, repo_url), file=sys.stderr
                    )

        self.rev: str = rev or branch or "main"
        self.origin_repo: str = repo_url
        self.manifest: ManifestParser = manifest
        self.depth: int = depth if depth else GitFetcher.DEFAULT_DEPTH
        self.branch: str = branch

    def _update(self) -> ChangeStatus:
        current_hash = (
            subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=self.repo_dir)
            .strip()
            .decode("utf-8")
        )
        target_hash = (
            subprocess.check_output(["git", "rev-parse", self.rev], cwd=self.repo_dir)
            .strip()
            .decode("utf-8")
        )
        if target_hash == current_hash:
            # It's up to date, so there are no changes.  This doesn't detect eg:
            # if origin/main moved and rev='main', but that's ok for our purposes;
            # we should be using explicit hashes or eg: a stable branch for the cases
            # that we care about, and it isn't unreasonable to require that the user
            # explicitly perform a clean build if those have moved.  For the most
            # part we prefer that folks build using a release tarball from github
            # rather than use the git protocol, as it is generally a bit quicker
            # to fetch and easier to hash and verify tarball downloads.
            return ChangeStatus()

        print("Updating %s -> %s" % (self.repo_dir, self.rev))
        run_cmd(["git", "fetch", "origin", self.rev], cwd=self.repo_dir)
        run_cmd(["git", "checkout", self.rev], cwd=self.repo_dir)
        run_cmd(["git", "submodule", "update", "--init"], cwd=self.repo_dir)

        return ChangeStatus(True)

    def update(self) -> ChangeStatus:
        if Path(self.repo_dir).exists():
            return self._update()
        self._clone()
        return ChangeStatus(True)

    def _clone(self) -> None:
        print("Cloning %s..." % self.origin_repo)
        # The basename/dirname stuff allows us to dance around issues where
        # eg: this python process is native win32, but the git.exe is cygwin
        # or msys and doesn't like the absolute windows path that we'd otherwise
        # pass to it.  Careful use of cwd helps avoid headaches with cygpath.
        cmd = [
            "git",
            "clone",
            "--depth=" + str(self.depth),
        ]
        if self.branch:
            cmd.append("--branch=" + self.branch)
        cmd += [
            "--",
            self.origin_repo,
            Path(self.repo_dir).name,
        ]
        run_cmd(cmd, cwd=os.fspath(Path(self.repo_dir).parent))
        self._update()

    def clean(self) -> None:
        if Path(self.repo_dir).exists():
            run_cmd(["git", "clean", "-fxd"], cwd=self.repo_dir)

    def hash(self) -> str:
        return self.rev

    def get_src_dir(self) -> str:
        return self.repo_dir


def does_file_need_update(
    src_name: str | Path, src_st: os.stat_result, dest_name: str | Path
) -> bool:
    try:
        target_st = os.lstat(dest_name)
    except OSError as exc:
        if exc.errno != errno.ENOENT:
            raise
        return True

    if src_st.st_size != target_st.st_size:
        return True

    if stat.S_IFMT(src_st.st_mode) != stat.S_IFMT(target_st.st_mode):
        return True
    if stat.S_ISLNK(src_st.st_mode):
        return os.readlink(src_name) != os.readlink(dest_name)
    if not stat.S_ISREG(src_st.st_mode):
        return True

    # They might have the same content; compare.
    with open(src_name, "rb") as sf, open(dest_name, "rb") as df:
        chunk_size = 8192
        while True:
            src_data = sf.read(chunk_size)
            dest_data = df.read(chunk_size)
            if src_data != dest_data:
                return True
            if len(src_data) < chunk_size:
                # EOF
                break
    return False


def copy_if_different(src_name: str | Path, dest_name: str | Path) -> bool:
    """Copy src_name -> dest_name, but only touch dest_name
    if src_name is different from dest_name, making this a
    more build system friendly way to copy."""
    src = Path(src_name)
    dest = Path(dest_name)
    src_st = os.lstat(src)
    if not does_file_need_update(src, src_st, dest):
        return False

    dest_parent = dest.parent
    if not dest_parent.exists():
        dest_parent.mkdir(parents=True)
    if stat.S_ISLNK(src_st.st_mode):
        try:
            os.unlink(dest)
        except OSError as exc:
            if exc.errno != errno.ENOENT:
                raise
        target = os.readlink(src)
        os.symlink(target, dest)
    else:
        shutil.copy2(src, dest)

    return True


def filter_strip_marker(dest_name: str | Path, marker: str) -> None:
    """Strip lines/blocks tagged with the given marker from a file."""
    try:
        with open(dest_name, "r") as f:
            content = f.read()
    except (UnicodeDecodeError, PermissionError):
        return

    if marker not in content:
        return

    escaped = re.escape(marker)
    block_re = re.compile(
        r"[^\n]*" + escaped + r"-start[^\n]*\n.*?[^\n]*" + escaped + r"-end[^\n]*\n?",
        re.DOTALL,
    )
    line_re = re.compile(r".*" + escaped + r".*\n?")

    filtered = block_re.sub("", content)
    filtered = line_re.sub("", filtered)
    if filtered != content:
        with open(dest_name, "w") as f:
            f.write(filtered)


def list_files_under_dir_newer_than_timestamp(
    dir_to_scan: str | Path, ts: float
) -> Iterator[str]:
    for root, _dirs, files in os.walk(dir_to_scan):
        for src_file in files:
            full_name = Path(root, src_file)
            st = os.lstat(full_name)
            if st.st_mtime > ts:
                yield os.fspath(full_name)


class ShipitPathMap:
    def __init__(self) -> None:
        self.roots: list[str] = []
        self.mapping: list[str] = []
        self.exclusion: list[str] = []
        self.strip_marker: str = "@fb-only"

    def add_mapping(self, fbsource_dir: str, target_dir: str) -> None:
        """Add a posix path or pattern.  We cannot normpath the input
        here because that would change the paths from posix to windows
        form and break the logic throughout this class."""
        self.roots.append(fbsource_dir)
        # pyre-fixme[6]: For 1st argument expected `str` but got `Tuple[str, str]`.
        self.mapping.append((fbsource_dir, target_dir))

    def add_exclusion(self, pattern: str) -> None:
        # pyre-fixme[6]: For 1st argument expected `str` but got `Pattern[str]`.
        self.exclusion.append(re.compile(pattern))

    def _minimize_roots(self) -> None:
        """compute the de-duplicated set of roots within fbsource.
        We take the shortest common directory prefix to make this
        determination"""
        self.roots.sort(key=len)
        minimized = []

        for r in self.roots:
            add_this_entry = True
            for existing in minimized:
                if r.startswith(existing + "/"):
                    add_this_entry = False
                    break
            if add_this_entry:
                minimized.append(r)

        self.roots = minimized

    def _sort_mapping(self) -> None:
        self.mapping.sort(reverse=True, key=lambda x: len(x[0]))

    def _map_name(self, norm_name: str, dest_root: str) -> str | None:
        if norm_name.endswith(".pyc") or norm_name.endswith(".swp"):
            # Ignore some incidental garbage while iterating
            return None

        for excl in self.exclusion:
            # pyre-fixme[16]: `str` has no attribute `match`.
            if excl.match(norm_name):
                return None

        for src_name, dest_name in self.mapping:
            if norm_name == src_name or norm_name.startswith(src_name + "/"):
                rel_name = os.path.relpath(norm_name, src_name)
                # We can have "." as a component of some paths, depending
                # on the contents of the shipit transformation section.
                # normpath doesn't always remove `.` as the final component
                # of the path, which be problematic when we later mkdir
                # the dirname of the path that we return.  Take care to avoid
                # returning a path with a `.` in it.
                rel_name = os.path.normpath(rel_name)
                if dest_name == ".":
                    return os.path.normpath(os.path.join(dest_root, rel_name))
                dest_name = os.path.normpath(dest_name)
                return os.path.normpath(os.path.join(dest_root, dest_name, rel_name))

        raise Exception("%s did not match any rules" % norm_name)

    def mirror(self, fbsource_root: str, dest_root: str) -> ChangeStatus:
        self._minimize_roots()
        self._sort_mapping()

        change_status = ChangeStatus()

        # Record the full set of files that should be in the tree
        full_file_list = set()

        if sys.platform == "win32":
            # Let's not assume st_dev has a consistent value on Windows.
            def st_dev(path: str) -> int:
                return 1

        else:

            def st_dev(path: str) -> int:
                return os.lstat(path).st_dev

        for fbsource_subdir in self.roots:
            dir_to_mirror = os.path.join(fbsource_root, fbsource_subdir)
            root_dev = st_dev(dir_to_mirror)
            prefetch_dir_if_eden(dir_to_mirror)
            if not os.path.exists(dir_to_mirror):
                raise Exception(
                    "%s doesn't exist; check your sparse profile!" % dir_to_mirror
                )
            update_count = 0
            for root, dirs, files in os.walk(dir_to_mirror):
                dirs[:] = [d for d in dirs if root_dev == st_dev(os.path.join(root, d))]

                for src_file in files:
                    full_name = os.path.join(root, src_file)
                    rel_name = os.path.relpath(full_name, fbsource_root)
                    norm_name = rel_name.replace("\\", "/")

                    target_name = self._map_name(norm_name, dest_root)
                    if target_name:
                        full_file_list.add(target_name)
                        if copy_if_different(full_name, target_name):
                            if not os.path.islink(full_name):
                                filter_strip_marker(target_name, self.strip_marker)
                            change_status.record_change(target_name)
                            if update_count < 10:
                                print("Updated %s -> %s" % (full_name, target_name))
                            elif update_count == 10:
                                print("...")
                            update_count += 1
            if update_count:
                print("Updated %s for %s" % (update_count, fbsource_subdir))

        # Compare the list of previously shipped files; if a file is
        # in the old list but not the new list then it has been
        # removed from the source and should be removed from the
        # destination.
        # Why don't we simply create this list by walking dest_root?
        # Some builds currently have to be in-source builds and
        # may legitimately need to keep some state in the source tree :-/
        installed_name = os.path.join(dest_root, ".shipit_shipped")
        if os.path.exists(installed_name):
            with open(installed_name, "rb") as f:
                for name in f.read().decode("utf-8").splitlines():
                    name = name.strip()
                    if name not in full_file_list:
                        print("Remove %s" % name)
                        os.unlink(name)
                        change_status.record_change(name)

        with open(installed_name, "wb") as f:
            for name in sorted(full_file_list):
                f.write(("%s\n" % name).encode("utf-8"))

        return change_status


class FbsourceRepoData(NamedTuple):
    hash: str
    date: str


FBSOURCE_REPO_DATA: dict[str, FbsourceRepoData] = {}


def get_fbsource_repo_data(build_options: BuildOptions) -> FbsourceRepoData:
    """Returns the commit metadata for the fbsource repo.
    Since we may have multiple first party projects to
    hash, and because we don't mutate the repo, we cache
    this hash in a global."""
    # pyre-fixme[6]: For 1st argument expected `str` but got `Optional[str]`.
    cached_data = FBSOURCE_REPO_DATA.get(build_options.fbsource_dir)
    if cached_data:
        return cached_data

    if "GETDEPS_HG_REPO_DATA" in os.environ:
        log_data = os.environ["GETDEPS_HG_REPO_DATA"]
    else:
        cmd = ["hg", "log", "-r.", "-T{node}\n{date|hgdate}"]
        env = Env()
        env.set("HGPLAIN", "1")
        log_data = subprocess.check_output(
            cmd, cwd=build_options.fbsource_dir, env=dict(env.items())
        ).decode("ascii")

    (hash, datestr) = log_data.split("\n")

    # datestr is like "seconds fractionalseconds"
    # We want "20200324.113140"
    (unixtime, _fractional) = datestr.split(" ")
    date = datetime.fromtimestamp(int(unixtime)).strftime("%Y%m%d.%H%M%S")
    cached_data = FbsourceRepoData(hash=hash, date=date)

    # pyre-fixme[6]: For 1st argument expected `str` but got `Optional[str]`.
    FBSOURCE_REPO_DATA[build_options.fbsource_dir] = cached_data

    return cached_data


def is_public_commit(build_options: BuildOptions) -> bool:  # noqa: C901
    """Check if the current commit is public (shipped/will be shipped to remote).

    Works across git, sapling (sl), and hg repositories:
    - For hg/sapling: Uses 'phase' command to check if commit is public
    - For git: Checks if commit exists in remote branches

    Returns True if public, False if draft/local-only or on error (conservative).
    """
    # Use fbsource_dir if available (Meta internal), otherwise fall back to repo_root
    repo_dir = build_options.fbsource_dir or build_options.repo_root
    if not repo_dir:
        # No repository detected, be conservative
        return False

    env = Env()
    env.set("HGPLAIN", "1")
    env_dict = dict(env.items())

    try:
        # Try hg/sapling phase command first (works for both hg and sl)
        # Try 'sl' first as it's the preferred tool at Meta
        for cmd in [["sl", "phase", "-r", "."], ["hg", "phase", "-r", "."]]:
            try:
                output = (
                    subprocess.check_output(
                        cmd, cwd=repo_dir, env=env_dict, stderr=subprocess.DEVNULL
                    )
                    .decode("ascii")
                    .strip()
                )
                # Output format: "hash: public" or "hash: draft"
                return "public" in output
            except (subprocess.CalledProcessError, FileNotFoundError):
                continue

        # Try git if hg/sl didn't work
        try:
            # Detect the default branch for origin remote
            default_branch = None
            try:
                # Get the symbolic ref for origin/HEAD to find default branch
                output = (
                    subprocess.check_output(
                        ["git", "symbolic-ref", "refs/remotes/origin/HEAD"],
                        cwd=repo_dir,
                        stderr=subprocess.DEVNULL,
                    )
                    .decode("ascii")
                    .strip()
                )
                # Output format: "refs/remotes/origin/main"
                if output.startswith("refs/remotes/"):
                    default_branch = output
            except subprocess.CalledProcessError:
                # If symbolic-ref fails, fall back to common names
                pass

            # Build list of branches to check
            branches_to_check = []
            if default_branch:
                branches_to_check.append(default_branch)
            # Also try common defaults as fallback
            branches_to_check.extend(["origin/main", "origin/master"])

            # Check if HEAD is an ancestor of any of these branches
            for branch in branches_to_check:
                try:
                    subprocess.check_output(
                        ["git", "merge-base", "--is-ancestor", "HEAD", branch],
                        cwd=repo_dir,
                        stderr=subprocess.DEVNULL,
                    )
                    # If command succeeds (exit 0), HEAD is an ancestor of the branch
                    return True
                except subprocess.CalledProcessError:
                    # Not an ancestor of this branch, try next
                    continue
            # HEAD is not in any default branch
            return False
        except FileNotFoundError:
            pass

        # If all VCS commands failed, be conservative and don't upload
        return False

    except Exception:
        # On any unexpected error, be conservative and don't upload
        return False


class SimpleShipitTransformerFetcher(Fetcher):
    def __init__(
        self,
        build_options: BuildOptions,
        manifest: ManifestParser,
        ctx: ManifestContext,
    ) -> None:
        self.build_options: BuildOptions = build_options
        self.manifest: ManifestParser = manifest
        self.repo_dir: str = os.fspath(
            Path(build_options.scratch_dir, "shipit", manifest.name)
        )
        self.ctx: ManifestContext = ctx

    def clean(self) -> None:
        if Path(self.repo_dir).exists():
            shutil.rmtree(self.repo_dir)

    def update(self) -> ChangeStatus:
        mapping = ShipitPathMap()
        for src, dest in self.manifest.get_section_as_ordered_pairs(
            "shipit.pathmap", self.ctx
        ):
            # pyre-fixme[6]: For 2nd argument expected `str` but got `Optional[str]`.
            mapping.add_mapping(src, dest)
        if self.manifest.shipit_fbcode_builder:
            mapping.add_mapping(
                "fbcode/opensource/fbcode_builder",
                self.manifest.shipit_fbcode_builder_dir,
            )
        for pattern in self.manifest.get_section_as_args("shipit.strip", self.ctx):
            mapping.add_exclusion(pattern)

        # pyre-fixme[8]: Attribute has type `str`; used as `Optional[str]`.
        mapping.strip_marker = self.manifest.shipit_strip_marker

        # pyre-fixme[6]: In call `ShipitPathMap.mirror`, for 1st positional argument, expected `str` but got `Optional[str]`
        return mapping.mirror(self.build_options.fbsource_dir, self.repo_dir)

    def hash(self) -> str:
        # We return a fixed non-hash string for in-fbsource builds.
        # We're relying on the `update` logic to correctly invalidate
        # the build in the case that files have changed.
        return "fbsource"

    def get_src_dir(self) -> str:
        return self.repo_dir


class SubFetcher(Fetcher):
    """Fetcher for a project with subprojects"""

    def __init__(self, base: Fetcher, subs: list[tuple[Fetcher, str]]) -> None:
        self.base: Fetcher = base
        self.subs: list[tuple[Fetcher, str]] = subs

    def update(self) -> ChangeStatus:
        base = self.base.update()
        changed = base.build_changed() or base.sources_changed()
        for fetcher, dir in self.subs:
            stat = fetcher.update()
            if stat.build_changed() or stat.sources_changed():
                changed = True
            link = Path(self.base.get_src_dir()) / dir
            if not link.exists():
                os.symlink(fetcher.get_src_dir(), link)
        return ChangeStatus(changed)

    def clean(self) -> None:
        self.base.clean()
        for fetcher, _ in self.subs:
            fetcher.clean()

    def hash(self) -> str:
        my_hash = self.base.hash()
        for fetcher, _ in self.subs:
            my_hash += fetcher.hash()
        return my_hash

    def get_src_dir(self) -> str:
        return self.base.get_src_dir()


class ShipitTransformerFetcher(Fetcher):
    @classmethod
    def _shipit_paths(cls, build_options: BuildOptions) -> list[str]:
        www_path = ["/var/www/scripts/opensource/codesync"]
        if build_options.fbsource_dir:
            fbcode_path = [
                os.fspath(
                    Path(
                        build_options.fbsource_dir,
                        "fbcode/opensource/codesync/codesync-cli/codesync",
                    )
                )
            ]
        else:
            fbcode_path = []
        return www_path + fbcode_path

    def __init__(
        self, build_options: BuildOptions, project_name: str, external_branch: str
    ) -> None:
        self.build_options: BuildOptions = build_options
        self.project_name: str = project_name
        self.external_branch: str = external_branch
        self.repo_dir: str = os.fspath(
            Path(build_options.scratch_dir, "shipit", project_name)
        )
        self.shipit: str | None = None
        for path in ShipitTransformerFetcher._shipit_paths(build_options):
            if Path(path).exists():
                self.shipit = path
                break

    def update(self) -> ChangeStatus:
        if Path(self.repo_dir).exists():
            return ChangeStatus()
        self.run_shipit()
        return ChangeStatus(True)

    def clean(self) -> None:
        if Path(self.repo_dir).exists():
            shutil.rmtree(self.repo_dir)

    @classmethod
    def available(cls, build_options: BuildOptions) -> bool:
        return any(
            Path(path).exists()
            for path in ShipitTransformerFetcher._shipit_paths(build_options)
        )

    def run_shipit(self) -> None:
        tmp_path = self.repo_dir + ".new"
        try:
            if Path(tmp_path).exists():
                shutil.rmtree(tmp_path)
            Path(tmp_path).parent.mkdir(parents=True, exist_ok=True)
            cmd = [
                self.shipit,
                "shipit",
                "--project=" + self.project_name,
                "--create-new-repo",
                # pyre-fixme[58]: `+` is not supported for operand types `str` and
                #  `Optional[str]`.
                "--source-repo-dir=" + self.build_options.fbsource_dir,
                "--source-branch=.",
                "--skip-source-init",
                "--skip-source-pull",
                "--skip-source-clean",
                "--skip-push",
                "--destination-use-anonymous-https",
                "--create-new-repo-output-path=" + tmp_path,
            ]
            if self.external_branch:
                cmd += [
                    f"--external-branch={self.external_branch}",
                ]

            # Run shipit
            # pyre-fixme[6]: For 1st argument expected `List[str]` but got
            #  `List[Optional[str]]`.
            run_cmd(cmd)

            # Remove the .git directory from the repository it generated.
            # There is no need to commit this.
            repo_git_dir = Path(tmp_path) / ".git"
            shutil.rmtree(repo_git_dir)
            os.rename(tmp_path, self.repo_dir)
        except Exception:
            # Clean up after a failed extraction
            if Path(tmp_path).exists():
                shutil.rmtree(tmp_path)
            self.clean()
            raise

    def hash(self) -> str:
        # We return a fixed non-hash string for in-fbsource builds.
        return "fbsource"

    def get_src_dir(self) -> str:
        return self.repo_dir


def download_url_to_file_with_progress(url: str, file_name: str | Path) -> None:
    print("Download with %s -> %s ..." % (url, file_name))

    class Progress:
        last_report: float = 0

        def write_update(self, total: int, amount: int) -> None:
            if total == -1:
                # pyrefly: ignore [bad-assignment]
                total = "(Unknown)"

            if sys.stdout.isatty():
                sys.stdout.write("\r downloading %s of %s " % (amount, total))
            else:
                # When logging to CI logs, avoid spamming the logs and print
                # status every few seconds
                now = time.time()
                if now - self.last_report > 5:
                    sys.stdout.write(".. %s of %s " % (amount, total))
                    self.last_report = now
            sys.stdout.flush()

        def progress_pycurl(
            self, total: float, amount: float, _uploadtotal: float, _uploadamount: float
        ) -> None:
            # pyrefly: ignore [bad-argument-type]
            self.write_update(total, amount)

    progress = Progress()
    start = time.time()
    try:
        if os.environ.get("GETDEPS_USE_WGET") is not None:
            procargs = (
                [
                    "wget",
                ]
                + os.environ.get("GETDEPS_WGET_ARGS", "").split()
                + [
                    "-O",
                    file_name,
                    url,
                ]
            )
            subprocess.run(procargs, capture_output=True)
            headers = None

        elif os.environ.get("GETDEPS_USE_LIBCURL") is not None:
            import pycurl

            with open(file_name, "wb") as f:
                c = pycurl.Curl()
                c.setopt(pycurl.URL, url)
                c.setopt(pycurl.WRITEDATA, f)
                # display progress
                c.setopt(pycurl.NOPROGRESS, False)
                c.setopt(pycurl.XFERINFOFUNCTION, progress.progress_pycurl)
                c.perform()
                c.close()
            headers = None
        else:
            try:
                req_header = {"Accept": "application/*"}
                res = urlopen(Request(url, None, req_header))
                chunk_size = 8192  # urlretrieve uses this value
                headers = res.headers
                content_length = res.headers.get("Content-Length")
                total = int(content_length.strip()) if content_length else -1
                amount = 0
                with open(file_name, "wb") as f:
                    chunk = res.read(chunk_size)
                    while chunk:
                        f.write(chunk)
                        amount += len(chunk)
                        progress.write_update(total, amount)
                        chunk = res.read(chunk_size)
            except (OSError, IOError) as exc:  # noqa: B014
                # Downloading from within Meta's network needs to use a proxy.
                if shutil.which("fwdproxy-config") is None:
                    print(
                        "Note: Could not find Meta-specific fallback 'fwdproxy-config'. "
                        "If you are working externally, you can ignore this message."
                    )
                    raise

                print("Default download failed, retrying with curl and fwdproxy...")
                cmd = f"curl -L $(fwdproxy-config curl) -o {shlex.quote(os.fspath(file_name))} {shlex.quote(url)}"
                print(f"Running command: {cmd}")
                result = subprocess.run(cmd, shell=True, capture_output=True)
                if result.returncode != 0:
                    raise TransientFailure(
                        f"Failed to download {url} to {file_name}: {exc} (fwdproxy fallback failed: {result.stderr.decode()})"
                    )
                headers = None
    except (OSError, IOError) as exc:  # noqa: B014
        raise TransientFailure(
            "Failed to download %s to %s: %s" % (url, file_name, str(exc))
        )

    end = time.time()
    sys.stdout.write(" [Complete in %f seconds]\n" % (end - start))
    sys.stdout.flush()
    if headers is not None:
        print(f"{headers}")


class ArchiveFetcher(Fetcher):
    def __init__(
        self,
        build_options: BuildOptions,
        manifest: ManifestParser,
        url: str,
        sha256: str,
    ) -> None:
        self.manifest: ManifestParser = manifest
        self.url: str = url
        self.sha256: str = sha256
        self.build_options: BuildOptions = build_options

        parsed_url = urlparse(self.url)
        # URL paths are posix regardless of platform.
        basename = "%s-%s" % (manifest.name, PurePosixPath(parsed_url.path).name)
        self.file_name: str = os.fspath(
            Path(build_options.scratch_dir, "downloads", basename)
        )
        self.src_dir: str = os.fspath(
            Path(build_options.scratch_dir, "extracted", basename)
        )
        self.hash_file: str = self.src_dir + ".hash"

    def _verify_hash(self) -> None:
        h = hashlib.sha256()
        with open(self.file_name, "rb") as f:
            while True:
                block = f.read(8192)
                if not block:
                    break
                h.update(block)
        digest = h.hexdigest()
        if digest != self.sha256:
            os.unlink(self.file_name)
            raise Exception(
                "%s: expected sha256 %s but got %s" % (self.url, self.sha256, digest)
            )

    def _download_dir(self) -> str:
        """returns the download dir, creating it if it doesn't already exist"""
        download_dir = Path(self.file_name).parent
        download_dir.mkdir(parents=True, exist_ok=True)
        return os.fspath(download_dir)

    def _download(self) -> None:
        self._download_dir()
        max_attempts = 5
        delay = 1
        for attempt in range(max_attempts):
            try:
                download_url_to_file_with_progress(self.url, self.file_name)
                break
            except TransientFailure as tf:
                if attempt < max_attempts - 1:
                    delay *= 2
                    delay_with_jitter = delay * (1 + random.random() * 0.1)
                    time.sleep(min(delay_with_jitter, 10))
                else:
                    print(f"Failed after retries: {tf}")
                    raise
        self._verify_hash()

    def clean(self) -> None:
        if Path(self.src_dir).exists():
            shutil.rmtree(self.src_dir)

    def update(self) -> ChangeStatus:
        try:
            with open(self.hash_file, "r") as f:
                saved_hash = f.read().strip()
                if saved_hash == self.sha256 and Path(self.src_dir).exists():
                    # Everything is up to date
                    return ChangeStatus()
                print(
                    "saved hash %s doesn't match expected hash %s, re-validating"
                    % (saved_hash, self.sha256)
                )
                Path(self.hash_file).unlink()
        except EnvironmentError:
            pass

        # If we got here we know the contents of src_dir are either missing
        # or wrong, so blow away whatever happened to be there first.
        if Path(self.src_dir).exists():
            shutil.rmtree(self.src_dir)

        # If we already have a file here, make sure it looks legit before
        # proceeding: any errors and we just remove it and re-download
        if Path(self.file_name).exists():
            try:
                self._verify_hash()
            except Exception:
                if Path(self.file_name).exists():
                    Path(self.file_name).unlink()

        if not Path(self.file_name).exists():
            self._download()
            self._verify_hash()

        if tarfile.is_tarfile(self.file_name):
            opener = tarfile.open
        elif zipfile.is_zipfile(self.file_name):
            opener = zipfile.ZipFile
        else:
            raise Exception("don't know how to extract %s" % self.file_name)
        os.makedirs(self.src_dir)
        print("Extract %s -> %s" % (self.file_name, self.src_dir))
        src = self.src_dir
        if is_windows():
            # Ensure that we don't fall over when dealing with long paths
            # on windows
            src = "\\\\?\\" + os.path.normpath(src)

        with opener(self.file_name) as t:
            safe_extractall(t, src)

        if is_windows():
            subdir = self.manifest.get("build", "subdir")
            checkdir = Path(src)
            if subdir:
                checkdir = checkdir / subdir
            if checkdir.exists():
                children = [p.name for p in checkdir.iterdir()]
                print(f"Extracted to {checkdir} contents: {children}")

        with open(self.hash_file, "w") as f:
            f.write(self.sha256)

        return ChangeStatus(True)

    def hash(self) -> str:
        return self.sha256

    def get_src_dir(self) -> str:
        return self.src_dir


def homebrew_package_prefix(package: str) -> str | None:
    cmd = ["brew", "--prefix", package]
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except FileNotFoundError:
        return None

    if proc.returncode == 0:
        return proc.stdout.decode("utf-8").rstrip()
