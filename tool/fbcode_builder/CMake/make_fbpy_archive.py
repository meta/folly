#!/usr/bin/env python3
#
# Copyright (c) Facebook, Inc. and its affiliates.
#
import argparse
import collections
import errno
import os
import shutil
import subprocess
import sys
import tempfile
import zipapp

MANIFEST_SEPARATOR = " :: "
MANIFEST_HEADER_V1 = "FBPY_MANIFEST 1\n"


class UsageError(Exception):
    def __init__(self, message):
        self.message = message

    def __str__(self):
        return self.message


class BadManifestError(UsageError):
    def __init__(self, path, line_num, message):
        full_msg = "%s:%s: %s" % (path, line_num, message)
        super().__init__(full_msg)
        self.path = path
        self.line_num = line_num
        self.raw_message = message


PathInfo = collections.namedtuple(
    "PathInfo", ("src", "dest", "manifest_path", "manifest_line")
)


def parse_manifest(manifest, path_map):
    bad_prefix = ".." + os.path.sep
    manifest_dir = os.path.dirname(manifest)
    with open(manifest, "r") as f:
        line_num = 1
        line = f.readline()
        if line != MANIFEST_HEADER_V1:
            raise BadManifestError(
                manifest, line_num, "Unexpected manifest file header"
            )

        for line in f:
            line_num += 1
            if line.startswith("#"):
                continue
            line = line.rstrip("\n")
            parts = line.split(MANIFEST_SEPARATOR)
            if len(parts) != 2:
                msg = "line must be of the form SRC %s DEST" % MANIFEST_SEPARATOR
                raise BadManifestError(manifest, line_num, msg)
            src, dest = parts
            dest = os.path.normpath(dest)
            if dest.startswith(bad_prefix):
                msg = "destination path starts with %s: %s" % (bad_prefix, dest)
                raise BadManifestError(manifest, line_num, msg)

            if not os.path.isabs(src):
                src = os.path.normpath(os.path.join(manifest_dir, src))

            if dest in path_map:
                prev_info = path_map[dest]
                msg = (
                    "multiple source paths specified for destination "
                    "path %s.  Previous source was %s from %s:%s"
                    % (
                        dest,
                        prev_info.src,
                        prev_info.manifest_path,
                        prev_info.manifest_line,
                    )
                )
                raise BadManifestError(manifest, line_num, msg)

            info = PathInfo(
                src=src,
                dest=dest,
                manifest_path=manifest,
                manifest_line=line_num,
            )
            path_map[dest] = info


def populate_install_tree(inst_dir, path_map):
    os.mkdir(inst_dir)
    dest_dirs = {"": False}

    def make_dest_dir(path):
        if path in dest_dirs:
            return
        parent = os.path.dirname(path)
        make_dest_dir(parent)
        abs_path = os.path.join(inst_dir, path)
        os.mkdir(abs_path)
        dest_dirs[path] = False

    def install_file(info):
        dir_name, base_name = os.path.split(info.dest)
        make_dest_dir(dir_name)
        if base_name == "__init__.py":
            dest_dirs[dir_name] = True
        abs_dest = os.path.join(inst_dir, info.dest)
        shutil.copy2(info.src, abs_dest)

    # Copy all of the destination files
    for info in path_map.values():
        install_file(info)

    # Create __init__ files in any directories that don't have them.
    for dir_path, has_init in dest_dirs.items():
        if has_init:
            continue
        init_path = os.path.join(inst_dir, dir_path, "__init__.py")
        with open(init_path, "w"):
            pass


def remove_output(path):
    """Remove an earlier build's output, which is a directory if that build
    used a different --type."""
    if os.path.isdir(path) and not os.path.islink(path):
        shutil.rmtree(path)
    elif os.path.lexists(path):
        os.unlink(path)


def build_pex(args, path_map):
    """Create a self executing python binary using the PEX tool

    This type of Python binary is more complex as it requires a third-party tool,
    but it does support native language extensions (.so/.dll files).
    """
    dest_dir = os.path.dirname(args.output)
    with tempfile.TemporaryDirectory(prefix="make_fbpy.", dir=dest_dir) as tmpdir:
        inst_dir = os.path.join(tmpdir, "tree")
        populate_install_tree(inst_dir, path_map)

        if os.path.exists(os.path.join(inst_dir, "__main__.py")):
            os.rename(
                os.path.join(inst_dir, "__main__.py"),
                os.path.join(inst_dir, "main.py"),
            )
            args.main = "main"

        tmp_output = os.path.abspath(os.path.join(tmpdir, "output.exe"))
        subprocess.check_call(
            ["pex"]
            + ["--output-file", tmp_output]
            + ["--python", args.python]
            + ["--sources-directory", inst_dir]
            + ["-e", args.main]
        )

        remove_output(args.output)
        os.replace(tmp_output, args.output)


def build_zipapp(args, path_map):
    """Create a self executing python binary using Python 3's built-in
    zipapp module.

    This type of Python binary is relatively simple, as zipapp is part of the
    standard library, but it does not support native language extensions
    (.so/.dll files).
    """
    dest_dir = os.path.dirname(args.output)
    with tempfile.TemporaryDirectory(prefix="make_fbpy.", dir=dest_dir) as tmpdir:
        inst_dir = os.path.join(tmpdir, "tree")
        populate_install_tree(inst_dir, path_map)

        tmp_output = os.path.join(tmpdir, "output.exe")
        zipapp.create_archive(
            inst_dir, target=tmp_output, interpreter=args.python, main=args.main
        )
        remove_output(args.output)
        os.replace(tmp_output, args.output)


def create_main_module(args, inst_dir, path_map):
    if not args.main:
        assert "__main__.py" in path_map
        os.chmod(os.path.join(inst_dir, "__main__.py"), 0o755)
        return

    dest_path = os.path.join(inst_dir, "__main__.py")
    main_module, main_fn = args.main.split(":")
    main_contents = """\
#!{python}

if __name__ == "__main__":
    import {main_module}
    {main_module}.{main_fn}()
""".format(
        python=args.python, main_module=main_module, main_fn=main_fn
    )
    with open(dest_path, "w") as f:
        f.write(main_contents)
    os.chmod(dest_path, 0o755)


def build_install_dir(args, path_map):
    """Create a directory that contains all of the sources, with a __main__
    module to run the program.
    """
    # Populate a temporary directory first, then rename to the destination
    # location.  This ensures that we don't ever leave a halfway-built
    # directory behind at the output path if something goes wrong.
    dest_dir = os.path.dirname(args.output)
    with tempfile.TemporaryDirectory(prefix="make_fbpy.", dir=dest_dir) as tmpdir:
        inst_dir = os.path.join(tmpdir, "tree")
        populate_install_tree(inst_dir, path_map)
        create_main_module(args, inst_dir, path_map)
        os.utime(os.path.join(inst_dir, "__main__.py"), None)
        remove_output(args.output)
        os.rename(inst_dir, args.output)


def ensure_directory(path):
    try:
        os.makedirs(path)
    except OSError as ex:
        if ex.errno != errno.EEXIST:
            raise


def install_library(args, path_map):
    """Create an installation directory a python library."""
    out_dir = args.output
    out_manifest = args.output + ".manifest"

    install_dir = args.install_dir
    if not install_dir:
        install_dir = out_dir

    os.makedirs(out_dir)
    with open(out_manifest, "w") as manifest:
        manifest.write(MANIFEST_HEADER_V1)
        for info in path_map.values():
            abs_dest = os.path.join(out_dir, info.dest)
            ensure_directory(os.path.dirname(abs_dest))
            print("copy %r --> %r" % (info.src, abs_dest))
            shutil.copy2(info.src, abs_dest)
            installed_dest = os.path.join(install_dir, info.dest)
            manifest.write("%s%s%s\n" % (installed_dest, MANIFEST_SEPARATOR, info.dest))


def parse_manifests(args):
    # Process args.manifest_separator to help support older versions of CMake
    if args.manifest_separator:
        manifests = []
        for manifest_arg in args.manifests:
            split_arg = manifest_arg.split(args.manifest_separator)
            manifests.extend(split_arg)
        args.manifests = manifests

    path_map = {}
    for manifest in args.manifests:
        parse_manifest(manifest, path_map)

    return path_map


def check_main_module(args, path_map):
    # Translate an empty string in the --main argument to None,
    # just to allow the CMake logic to be slightly simpler and pass in an
    # empty string when it really wants the default __main__.py module to be
    # used.
    if args.main == "":
        args.main = None

    if args.type == "lib-install":
        if args.main is not None:
            raise UsageError("cannot specify a --main argument with --type=lib-install")
        return

    main_info = path_map.get("__main__.py")
    if args.main:
        if main_info is not None:
            msg = (
                "specified an explicit main module with --main, "
                "but the file listing already includes __main__.py"
            )
            raise BadManifestError(
                main_info.manifest_path, main_info.manifest_line, msg
            )
        parts = args.main.split(":")
        if len(parts) != 2:
            raise UsageError(
                "argument to --main must be of the form MODULE:CALLABLE "
                "(received %s)" % (args.main,)
            )
    else:
        if main_info is None:
            raise UsageError(
                "no main module specified with --main, "
                "and no __main__.py module present"
            )


BUILD_TYPES = {
    "pex": build_pex,
    "zipapp": build_zipapp,
    "dir": build_install_dir,
    "lib-install": install_library,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--output", required=True, help="The output file path")
    ap.add_argument(
        "--install-dir",
        help="When used with --type=lib-install, this parameter specifies the "
        "final location where the library where be installed.  This can be "
        "used to generate the library in one directory first, when you plan "
        "to move or copy it to another final location later.",
    )
    ap.add_argument(
        "--manifest-separator",
        help="Split manifest arguments around this separator.  This is used "
        "to support older versions of CMake that cannot supply the manifests "
        "as separate arguments.",
    )
    ap.add_argument(
        "--main",
        help="The main module to run, specified as <module>:<callable>.  "
        "This must be specified if and only if the archive does not contain "
        "a __main__.py file.",
    )
    ap.add_argument(
        "--python",
        help="Explicitly specify the python interpreter to use for the " "executable.",
    )
    ap.add_argument(
        "--type", choices=BUILD_TYPES.keys(), help="The type of output to build."
    )
    ap.add_argument(
        "manifests",
        nargs="+",
        help="The manifest files specifying how to construct the archive",
    )
    args = ap.parse_args()

    if args.python is None:
        args.python = sys.executable

    if args.type is None:
        # In the future we might want different default output types
        # for different platforms.
        args.type = "zipapp"
    build_fn = BUILD_TYPES[args.type]

    try:
        path_map = parse_manifests(args)
        check_main_module(args, path_map)
    except UsageError as ex:
        print("error: %s" % (ex,), file=sys.stderr)
        sys.exit(1)

    build_fn(args, path_map)


if __name__ == "__main__":
    main()
