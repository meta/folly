# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


from __future__ import annotations

import os
import shlex
import sys
from collections.abc import ItemsView, Iterator, KeysView, Mapping, ValuesView
from pathlib import Path
from typing import overload, TypeVar

_T = TypeVar("_T")


class Env(Mapping[str, str]):
    def __init__(self, src: Mapping[str, str] | None = None) -> None:
        self._dict: dict[str, str] = {}
        if src is None:
            self.update(os.environ)
        else:
            self.update(src)

    def update(self, src: Mapping[str, str]) -> None:
        for k, v in src.items():
            self.set(k, v)

    def copy(self) -> Env:
        return Env(self._dict)

    def _key(self, key: str) -> str | None:
        # The `str` cast may not appear to be needed, but without it we run
        # into issues when passing the environment to subprocess.  The main
        # issue is that in python2 `os.environ` (which is the initial source
        # of data for the environment) uses byte based strings, but this
        # project uses `unicode_literals`.  `subprocess` will raise an error
        # if the environment that it is passed has a mixture of byte and
        # unicode strings.
        # It is simplest to force everything to be `str` for the sake of
        # consistency.
        key = str(key)
        if sys.platform.startswith("win"):
            # Windows env var names are case insensitive but case preserving.
            # An implementation of PAR files on windows gets confused if
            # the env block contains keys with conflicting case, so make a
            # pass over the contents to remove any.
            # While this O(n) scan is technically expensive and gross, it
            # is practically not a problem because the volume of calls is
            # relatively low and the cost of manipulating the env is dwarfed
            # by the cost of spawning a process on windows.  In addition,
            # since the processes that we run are expensive anyway, this
            # overhead is not the worst thing to worry about.
            for k in list(self._dict.keys()):
                if str(k).lower() == key.lower():
                    return k
        elif key in self._dict:
            return key
        return None

    @overload
    def get(self, key: str) -> str | None: ...

    @overload
    def get(self, key: str, defval: _T) -> str | _T: ...

    def get(self, key: str, defval: _T | None = None) -> str | _T | None:
        resolved_key = self._key(key)
        if resolved_key is None:
            return defval
        return self._dict[resolved_key]

    def __getitem__(self, key: str) -> str:
        val = self.get(key)
        if val is None:
            raise KeyError(key)
        return val

    def unset(self, key: str) -> None:
        if key is None:
            raise KeyError("attempting to unset env[None]")

        resolved_key = self._key(key)
        if resolved_key:
            del self._dict[resolved_key]

    def __delitem__(self, key: str) -> None:
        self.unset(key)

    def __repr__(self) -> str:
        return repr(self._dict)

    def set(self, key: str, value: str) -> None:
        if key is None:
            raise KeyError("attempting to assign env[None] = %r" % value)

        if value is None:
            raise ValueError("attempting to assign env[%s] = None" % key)

        # The `str` conversion is important to avoid triggering errors
        # with subprocess if we pass in a unicode value; see commentary
        # in the `_key` method.
        key = str(key)
        value = str(value)

        # The `unset` call is necessary on windows where the keys are
        # case insensitive.   Since this dict is case sensitive, simply
        # assigning the value to the new key is not sufficient to remove
        # the old value.  The `unset` call knows how to match keys and
        # remove any potential duplicates.
        self.unset(key)
        self._dict[key] = value

    def __setitem__(self, key: str, value: str) -> None:
        self.set(key, value)

    def __iter__(self) -> Iterator[str]:
        return self._dict.__iter__()

    def __len__(self) -> int:
        return len(self._dict)

    def keys(self) -> KeysView[str]:
        return self._dict.keys()

    def values(self) -> ValuesView[str]:
        return self._dict.values()

    def items(self) -> ItemsView[str, str]:
        return self._dict.items()


def add_path_entry(
    env: Env, name: str, item: str, append: bool = True, separator: str = os.pathsep
) -> None:
    """Cause `item` to be added to the path style env var named
    `name` held in the `env` dict.  `append` specifies whether
    the item is added to the end (the default) or should be
    prepended if `name` already exists."""
    val = env.get(name, "")
    if val is not None and len(val) > 0:
        val_list = val.split(separator)
    else:
        val_list = []
    if append:
        val_list.append(item)
    else:
        val_list.insert(0, item)
    env.set(name, separator.join(val_list))


def add_flag(env: Env, name: str, flag: str, append: bool = True) -> None:
    """Cause `flag` to be added to the CXXFLAGS-style env var named
    `name` held in the `env` dict.  `append` specifies whether the
    flag is added to the end (the default) or should be prepended if
    `name` already exists."""
    val = shlex.split(env.get(name, "") or "")
    if append:
        val.append(flag)
    else:
        val.insert(0, flag)
    env.set(name, " ".join(val))


_path_search_cache: dict[object, str | None] = {}
_not_found: object = object()


def tpx_path() -> str:
    return "xplat/testinfra/tpx/ctp.tpx"


def path_search(
    env: Mapping[str, str], exename: str, defval: str | None = None
) -> str | None:
    """Search for exename in the PATH specified in env.
    exename is eg: `ninja` and this function knows to append a .exe
    to the end on windows.
    Returns the path to the exe if found, or None if either no
    PATH is set in env or no executable is found.

    NOTE: the return stays `str` (not `Path`) on purpose: results feed
    Env values, argv lists, and project-hash inputs, all of which must
    remain str. Revisit when the core builders/loaders are retyped."""

    path = env.get("PATH", None)
    if path is None:
        return defval

    # The project hash computation code searches for C++ compilers (g++, clang, etc)
    # repeatedly.  Cache the result so we don't end up searching for these over and over
    # again.
    cache_key = (path, exename)
    result = _path_search_cache.get(cache_key, _not_found)
    if result is _not_found:
        result = _perform_path_search(path, exename)
        _path_search_cache[cache_key] = result
    # pyre-fixme[7]: Expected `Optional[str]` but got `Optional[object]`.
    return result


def _perform_path_search(path: str, exename: str) -> str | None:
    is_win = sys.platform.startswith("win")
    if is_win:
        exename = "%s.exe" % exename

    for bindir in path.split(os.pathsep):
        full_name = Path(bindir, exename)
        if full_name.exists() and full_name.is_file():
            if not is_win and not os.access(full_name, os.X_OK):
                continue
            return os.fspath(full_name)

    return None
