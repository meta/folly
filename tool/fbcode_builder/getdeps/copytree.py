# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


from __future__ import annotations

import os
import shutil
import stat
import subprocess
from collections.abc import Callable
from pathlib import Path

from .getdeps_platform import is_windows
from .runcmd import run_cmd


PREFETCHED_DIRS: set[str] = set()


def containing_repo_type(path: str | Path) -> tuple[str | None, str | None]:
    # The return stays `str` (not `Path`): callers feed it into Env values,
    # argv lists, and hashes, all of which must remain str.
    p = Path(path)
    while True:
        if (p / ".git").exists():
            return ("git", os.fspath(p))
        if (p / ".hg").exists():
            return ("hg", os.fspath(p))

        parent = p.parent
        if parent == p:
            return None, None
        p = parent


def find_eden_root(dirpath: str | Path) -> str | None:
    """If the specified directory is inside an EdenFS checkout, returns
    the canonical absolute path to the root of that checkout.

    Returns None if the specified directory is not in an EdenFS checkout.
    """
    d = Path(dirpath)
    if is_windows():
        repo_type, repo_root = containing_repo_type(d)
        if repo_root is not None:
            if (Path(repo_root) / ".eden" / "config").exists():
                return repo_root
        return None

    try:
        return os.readlink(d / ".eden" / "root")
    except OSError:
        return None


def prefetch_dir_if_eden(dirpath: str | Path) -> None:
    """After an amend/rebase, Eden may need to fetch a large number
    of trees from the servers.  The simplistic single threaded walk
    performed by copytree makes this more expensive than is desirable
    so we help accelerate things by performing a prefetch on the
    source directory"""
    # Normalize to str for the dedup set so str and Path callers share it.
    key = os.fspath(dirpath)
    if key in PREFETCHED_DIRS:
        return
    root = find_eden_root(dirpath)
    if root is None:
        return
    glob = f"{os.path.relpath(key, root).replace(os.sep, '/')}/**"
    print(f"Prefetching {glob}")
    subprocess.call(["edenfsctl", "prefetch", "--repo", root, glob, "--background"])
    PREFETCHED_DIRS.add(key)


def simple_copytree(
    src_dir: str | Path, dest_dir: str | Path, symlinks: bool = False
) -> str:
    """A simple version of shutil.copytree() that can delegate to native tools if faster"""
    src = Path(src_dir)
    dest = Path(dest_dir)
    if is_windows():
        dest.mkdir(parents=True, exist_ok=True)
        cmd = [
            "robocopy.exe",
            # argv must stay str for subprocess.
            os.fspath(src),
            os.fspath(dest),
            # copy directories, including empty ones
            "/E",
            # Ignore Extra files in destination
            "/XX",
            # enable parallel copy
            "/MT",
            # be quiet
            "/NFL",
            "/NDL",
            "/NJH",
            "/NJS",
            "/NP",
        ]
        if symlinks:
            cmd.append("/SL")
        # robocopy exits with code 1 if it copied ok, hence allow_fail
        # https://learn.microsoft.com/en-us/troubleshoot/windows-server/backup-and-storage/return-codes-used-robocopy-utility
        exit_code = run_cmd(cmd, allow_fail=True)
        if exit_code > 1:
            raise subprocess.CalledProcessError(exit_code, cmd)
        return os.fspath(dest)
    else:
        return os.fspath(shutil.copytree(src, dest, symlinks=symlinks))


def _remove_readonly_and_try_again(
    func: Callable[..., object],
    path: str | Path,
    #  `typing.Type[<base type>]` to avoid runtime subscripting errors.
    exc_info: tuple[type, BaseException, object],
) -> None:
    """
    Error handler for shutil.rmtree.
    If the error is due to an access error (read only file)
    it attempts to add write permission and then retries the operation.
    Any other failure propagates.
    """
    # exc_info is a tuple (exc_type, exc_value, traceback)
    exc_type = exc_info[0]
    if exc_type is PermissionError:
        os.chmod(path, stat.S_IWRITE)
        # Retry the original function (os.remove or os.rmdir)
        try:
            func(path)
        except Exception:
            # If it still fails, the original exception from func() will propagate
            raise
    else:
        # If the error is not a PermissionError, re-raise the original exception
        raise exc_info[1]


def rmtree_more(path: str | Path) -> None:
    """Wrapper around shutil.rmtree() that makes it remove readonly files as well.
    Useful when git on windows decides to make some files readonly on checkout"""
    shutil.rmtree(path, onerror=_remove_readonly_and_try_again)
