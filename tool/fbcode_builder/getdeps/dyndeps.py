# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import annotations

import errno
import glob
import os
import re
import shlex
import shutil
import stat
import subprocess
import sys
import typing
from collections.abc import Generator
from pathlib import Path
from struct import unpack

if typing.TYPE_CHECKING:
    from .buildopts import BuildOptions
    from .envfuncs import Env

OBJECT_SUBDIRS: tuple[str, ...] = ("bin", "lib", "lib64")


def copyfile(src: str, dest: str) -> None:
    shutil.copyfile(src, dest)
    shutil.copymode(src, dest)


class DepBase:
    def __init__(
        self,
        buildopts: BuildOptions,
        env: Env,
        install_dirs: list[str],
        strip: bool,
    ) -> None:
        self.buildopts: BuildOptions = buildopts
        self.env: Env = env
        self.install_dirs: list[str] = install_dirs
        self.strip: bool = strip

        # Deduplicates dependency processing. Keyed on the library
        # destination path.
        self.processed_deps: set[str] = set()

        self.munged_lib_dir: str = ""

    def list_dynamic_deps(self, objfile: str) -> list[str]:
        raise RuntimeError("list_dynamic_deps not implemented")

    def interesting_dep(self, d: str) -> bool:
        return True

    # final_install_prefix must be the equivalent path to `destdir` on the
    # installed system.  For example, if destdir is `/tmp/RANDOM/usr/local' which
    # is intended to map to `/usr/local` in the install image, then
    # final_install_prefix='/usr/local'.
    # If left unspecified, destdir will be used.
    def process_deps(
        self, destdir: str, final_install_prefix: str | None = None
    ) -> None:
        if self.buildopts.is_windows():
            lib_dir = "bin"
        else:
            lib_dir = "lib"
        self.munged_lib_dir = os.fspath(Path(destdir, lib_dir))

        final_lib_dir: str = os.fspath(Path(final_install_prefix or destdir, lib_dir))

        if not Path(self.munged_lib_dir).is_dir():
            Path(self.munged_lib_dir).mkdir(parents=True, exist_ok=True)

        # Look only at the things that got installed in the leaf package,
        # which will be the last entry in the install dirs list
        inst_dir: str = self.install_dirs[-1]
        print("Process deps under %s" % inst_dir, file=sys.stderr)

        for dir in OBJECT_SUBDIRS:
            src_dir: str = os.fspath(Path(inst_dir, dir))
            if not Path(src_dir).is_dir():
                continue
            dest_dir: str = os.fspath(Path(destdir, dir))
            if not Path(dest_dir).exists():
                Path(dest_dir).mkdir(parents=True, exist_ok=True)

            for objfile in self.list_objs_in_dir(src_dir):
                print("Consider %s/%s" % (dir, objfile))
                dest_obj: str = os.fspath(Path(dest_dir, objfile))
                copyfile(os.fspath(Path(src_dir, objfile)), dest_obj)
                self.munge_in_place(dest_obj, final_lib_dir)

    def find_all_dependencies(self, build_dir: str) -> list[str]:
        all_deps: set[str] = set()
        for objfile in self.list_objs_in_dir(
            build_dir, recurse=True, output_prefix=build_dir
        ):
            for d in self.list_dynamic_deps(objfile):
                all_deps.add(d)

        interesting_deps: set[str] = {d for d in all_deps if self.interesting_dep(d)}
        dep_paths: list[str] = []
        for dep in interesting_deps:
            dep_path: str | None = self.resolve_loader_path(dep)
            if dep_path:
                dep_paths.append(dep_path)

        return dep_paths

    def munge_in_place(self, objfile: str, final_lib_dir: str) -> None:
        print("Munging %s" % objfile)
        for d in self.list_dynamic_deps(objfile):
            if not self.interesting_dep(d):
                continue

            # Resolve this dep: does it exist in any of our installation
            # directories?  If so, then it is a candidate for processing
            dep: str | None = self.resolve_loader_path(d)
            if dep:
                dest_dep: str = os.fspath(Path(self.munged_lib_dir, Path(dep).name))
                print("dep: %s -> %s" % (d, dest_dep))
                if dest_dep in self.processed_deps:
                    # A previous dependency with the same name has already
                    # been installed at dest_dep, so there is no need to copy
                    # or munge the dependency again.
                    # TODO: audit that both source paths have the same inode number
                    pass
                else:
                    self.processed_deps.add(dest_dep)
                    copyfile(dep, dest_dep)
                    self.munge_in_place(dest_dep, final_lib_dir)

                self.rewrite_dep(objfile, d, dep, dest_dep, final_lib_dir)

        if self.strip:
            self.strip_debug_info(objfile)

    def rewrite_dep(
        self,
        objfile: str,
        depname: str,
        old_dep: str,
        new_dep: str,
        final_lib_dir: str,
    ) -> None:
        raise RuntimeError("rewrite_dep not implemented")

    def resolve_loader_path(self, dep: str) -> str | None:
        if Path(dep).is_absolute():
            return dep
        d: str = Path(dep).name
        for inst_dir in self.install_dirs:
            for libdir in OBJECT_SUBDIRS:
                candidate: str = os.fspath(Path(inst_dir, libdir, d))
                if Path(candidate).exists():
                    return candidate
        return None

    def list_objs_in_dir(
        self, dir: str, recurse: bool = False, output_prefix: str = ""
    ) -> Generator[str, None, None]:
        for entry in os.listdir(dir):
            entry_path: str = os.fspath(Path(dir, entry))
            st: os.stat_result = os.lstat(entry_path)
            if stat.S_ISREG(st.st_mode):
                if self.is_objfile(entry_path):
                    relative_result: str = os.fspath(Path(output_prefix, entry))
                    yield os.path.normcase(relative_result)
            elif recurse and stat.S_ISDIR(st.st_mode):
                child_prefix: str = os.fspath(Path(output_prefix, entry))
                for result in self.list_objs_in_dir(
                    entry_path, recurse=recurse, output_prefix=child_prefix
                ):
                    yield result

    def is_objfile(self, objfile: str) -> bool:
        return True

    def strip_debug_info(self, objfile: str) -> None:
        """override this to define how to remove debug information
        from an object file"""
        pass

    def check_call_verbose(self, args: list[str]) -> None:
        print(" ".join(map(shlex.quote, args)))
        subprocess.check_call(args)


class WinDeps(DepBase):
    def __init__(
        self,
        buildopts: BuildOptions,
        env: Env,
        install_dirs: list[str],
        strip: bool,
    ) -> None:
        super(WinDeps, self).__init__(buildopts, env, install_dirs, strip)
        self.dumpbin: str = self.find_dumpbin()

    def find_dumpbin(self) -> str:
        # Looking for dumpbin in the following hardcoded paths.
        # The registry option to find the install dir doesn't work anymore.
        globs: list[str] = [
            (
                "C:/Program Files/"
                "Microsoft Visual Studio/"
                "*/*/VC/Tools/"
                "MSVC/*/bin/Hostx64/x64/dumpbin.exe"
            ),
            (
                "C:/Program Files (x86)/"
                "Microsoft Visual Studio/"
                "*/*/VC/Tools/"
                "MSVC/*/bin/Hostx64/x64/dumpbin.exe"
            ),
            (
                "C:/Program Files (x86)/"
                "Common Files/"
                "Microsoft/Visual C++ for Python/*/"
                "VC/bin/dumpbin.exe"
            ),
            ("c:/Program Files (x86)/Microsoft Visual Studio */VC/bin/dumpbin.exe"),
            (
                "C:/Program Files/Microsoft Visual Studio/*/Professional/VC/Tools/MSVC/*/bin/HostX64/x64/dumpbin.exe"
            ),
        ]
        for pattern in globs:
            for exe in glob.glob(pattern):
                return exe

        raise RuntimeError("could not find dumpbin.exe")

    # pyre-fixme[14]: `list_dynamic_deps` overrides method defined in `DepBase`
    #  inconsistently.
    def list_dynamic_deps(self, exe: str) -> list[str]:
        deps: list[str] = []
        print("Resolve deps for %s" % exe)
        output: str = subprocess.check_output(
            [self.dumpbin, "/nologo", "/dependents", exe]
        ).decode("utf-8")

        lines: list[str] = output.split("\n")
        for line in lines:
            m: re.Match[str] | None = re.match("\\s+(\\S+.dll)", line, re.IGNORECASE)
            if m:
                deps.append(m.group(1).lower())

        return deps

    def rewrite_dep(
        self,
        objfile: str,
        depname: str,
        old_dep: str,
        new_dep: str,
        final_lib_dir: str,
    ) -> None:
        # We can't rewrite on windows, but we will
        # place the deps alongside the exe so that
        # they end up in the search path
        pass

    # These are the Windows system dll, which we don't want to copy while
    # packaging.
    SYSTEM_DLLS: set[str] = set(  # noqa: C405
        [
            "advapi32.dll",
            "dbghelp.dll",
            "kernel32.dll",
            "msvcp140.dll",
            "vcruntime140.dll",
            "ws2_32.dll",
            "ntdll.dll",
            "shlwapi.dll",
        ]
    )

    def interesting_dep(self, d: str) -> bool:
        if "api-ms-win-crt" in d:
            return False
        if d in self.SYSTEM_DLLS:
            return False
        return True

    def is_objfile(self, objfile: str) -> bool:
        if not Path(objfile).is_file():
            return False
        if objfile.lower().endswith(".exe"):
            return True
        return False

    def emit_dev_run_script(self, script_path: str, dep_dirs: list[str]) -> None:
        """Emit a script that can be used to run build artifacts directly from the
        build directory, without installing them.

        The dep_dirs parameter should be a list of paths that need to be added to $PATH.
        This can be computed by calling compute_dependency_paths() or
        compute_dependency_paths_fast().

        This is only necessary on Windows, which does not have RPATH, and instead
        requires the $PATH environment variable be updated in order to find the proper
        library dependencies.
        """
        contents: str = self._get_dev_run_script_contents(dep_dirs)
        with open(script_path, "w") as f:
            f.write(contents)

    def compute_dependency_paths(self, build_dir: str) -> list[str]:
        """Return a list of all directories that need to be added to $PATH to ensure
        that library dependencies can be found correctly.  This is computed by scanning
        binaries to determine exactly the right list of dependencies.

        The compute_dependency_paths_fast() is a alternative function that runs faster
        but may return additional extraneous paths.
        """
        dep_dirs: set[str] = set()
        # Find paths by scanning the binaries.
        for dep in self.find_all_dependencies(build_dir):
            dep_dirs.add(os.fspath(Path(dep).parent))

        dep_dirs.update(self.read_custom_dep_dirs(build_dir))
        return sorted(dep_dirs)

    def compute_dependency_paths_fast(self, build_dir: str) -> list[str]:
        """Similar to compute_dependency_paths(), but rather than actually scanning
        binaries, just add all library paths from the specified installation
        directories.  This is much faster than scanning the binaries, but may result in
        more paths being returned than actually necessary.
        """
        dep_dirs: set[str] = set()
        for inst_dir in self.install_dirs:
            for subdir in OBJECT_SUBDIRS:
                path: str = os.fspath(Path(inst_dir, subdir))
                if Path(path).exists():
                    dep_dirs.add(path)

        dep_dirs.update(self.read_custom_dep_dirs(build_dir))
        return sorted(dep_dirs)

    def read_custom_dep_dirs(self, build_dir: str) -> set[str]:
        # The build system may also have included libraries from other locations that
        # we might not be able to find normally in find_all_dependencies().
        # To handle this situation we support reading additional library paths
        # from a LIBRARY_DEP_DIRS.txt file that may have been generated in the build
        # output directory.
        dep_dirs: set[str] = set()
        try:
            explicit_dep_dirs_path: str = os.fspath(
                Path(build_dir, "LIBRARY_DEP_DIRS.txt")
            )
            with open(explicit_dep_dirs_path, "r") as f:
                for line in f.read().splitlines():
                    dep_dirs.add(line)
        except OSError as ex:
            if ex.errno != errno.ENOENT:
                raise

        return dep_dirs

    def _get_dev_run_script_contents(self, path_dirs: list[str]) -> str:
        path_entries: list[str] = ["$env:PATH"] + path_dirs
        path_str: str = ";".join(path_entries)
        return """\
$orig_env = $env:PATH
$env:PATH = "{path_str}"

try {{
    $cmd_args = $args[1..$args.length]
    & $args[0] @cmd_args
}} finally {{
    $env:PATH = $orig_env
}}
""".format(
            path_str=path_str
        )


class ElfDeps(DepBase):
    def __init__(
        self,
        buildopts: BuildOptions,
        env: Env,
        install_dirs: list[str],
        strip: bool,
    ) -> None:
        super(ElfDeps, self).__init__(buildopts, env, install_dirs, strip)

        # We need patchelf to rewrite deps, so ensure that it is built...
        args: list[str] = [sys.executable, sys.argv[0]]
        if buildopts.allow_system_packages:
            args.append("--allow-system-packages")
        subprocess.check_call(args + ["build", "patchelf"])

        # ... and that we know where it lives
        patchelf_install: str = os.fsdecode(
            subprocess.check_output(args + ["show-inst-dir", "patchelf"]).strip()
        )
        if not patchelf_install:
            # its a system package, so we assume it is in the path
            patchelf_install = "patchelf"
        else:
            patchelf_install = os.fspath(Path(patchelf_install, "bin", "patchelf"))
        self.patchelf: str = patchelf_install

    def list_dynamic_deps(self, objfile: str) -> list[str]:
        out: str = (
            subprocess.check_output(
                [self.patchelf, "--print-needed", objfile], env=dict(self.env.items())
            )
            .decode("utf-8")
            .strip()
        )
        lines: list[str] = out.split("\n")
        return lines

    def rewrite_dep(
        self,
        objfile: str,
        depname: str,
        old_dep: str,
        new_dep: str,
        final_lib_dir: str,
    ) -> None:
        final_dep: str = os.fspath(
            Path(final_lib_dir, os.path.relpath(new_dep, self.munged_lib_dir))
        )
        self.check_call_verbose(
            [self.patchelf, "--replace-needed", depname, final_dep, objfile]
        )

    def is_objfile(self, objfile: str) -> bool:
        if not Path(objfile).is_file():
            return False
        with open(objfile, "rb") as f:
            # https://en.wikipedia.org/wiki/Executable_and_Linkable_Format#File_header
            magic: bytes = f.read(4)
            return magic == b"\x7fELF"

    def strip_debug_info(self, objfile: str) -> None:
        self.check_call_verbose(["strip", objfile])


# MACH-O magic number
MACH_MAGIC: int = 0xFEEDFACF


class MachDeps(DepBase):
    def interesting_dep(self, d: str) -> bool:
        if d.startswith("/usr/lib/") or d.startswith("/System/"):
            return False
        return True

    def is_objfile(self, objfile: str) -> bool:
        if not Path(objfile).is_file():
            return False
        with open(objfile, "rb") as f:
            # mach stores the magic number in native endianness,
            # so unpack as native here and compare
            header: bytes = f.read(4)
            if len(header) != 4:
                return False
            magic: int = unpack("I", header)[0]
            return magic == MACH_MAGIC

    def list_dynamic_deps(self, objfile: str) -> list[str]:
        if not self.interesting_dep(objfile):
            return []
        out: str = (
            subprocess.check_output(
                ["otool", "-L", objfile], env=dict(self.env.items())
            )
            .decode("utf-8")
            .strip()
        )
        lines: list[str] = out.split("\n")
        deps: list[str] = []
        for line in lines:
            m: re.Match[str] | None = re.match("\t(\\S+)\\s", line)
            if m:
                if Path(m.group(1)).name != Path(objfile).name:
                    deps.append(os.path.normcase(m.group(1)))
        return deps

    def rewrite_dep(
        self,
        objfile: str,
        depname: str,
        old_dep: str,
        new_dep: str,
        final_lib_dir: str,
    ) -> None:
        if objfile.endswith(".dylib"):
            # Erase the original location from the id of the shared
            # object.  It doesn't appear to hurt to retain it, but
            # it does look weird, so let's rewrite it to be sure.
            self.check_call_verbose(
                ["install_name_tool", "-id", Path(objfile).name, objfile]
            )
        final_dep: str = os.fspath(
            Path(final_lib_dir, os.path.relpath(new_dep, self.munged_lib_dir))
        )

        self.check_call_verbose(
            ["install_name_tool", "-change", depname, final_dep, objfile]
        )


def create_dyn_dep_munger(
    buildopts: BuildOptions,
    env: Env,
    install_dirs: list[str],
    strip: bool = False,
) -> DepBase | None:
    if buildopts.is_linux():
        return ElfDeps(buildopts, env, install_dirs, strip)
    if buildopts.is_darwin():
        return MachDeps(buildopts, env, install_dirs, strip)
    if buildopts.is_windows():
        return WinDeps(buildopts, env, install_dirs, strip)
    if buildopts.is_freebsd():
        return ElfDeps(buildopts, env, install_dirs, strip)
    return None
