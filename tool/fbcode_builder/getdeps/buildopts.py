# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import annotations

import argparse
import errno
import glob
import ntpath
import os
import subprocess
import sys
import tempfile
import typing
from collections.abc import Mapping
from pathlib import Path

from .copytree import containing_repo_type
from .envfuncs import add_flag, add_path_entry, Env
from .fetcher import get_fbsource_repo_data, homebrew_package_prefix
from .getdeps_platform import get_available_ram, HostType, is_windows
from .manifest import ContextGenerator

if typing.TYPE_CHECKING:
    from .load import ManifestLoader
    from .manifest import ManifestContext, ManifestParser


GITBASH_TMP: str = "c:\\tools\\fb.gitbash\\tmp"


def detect_project(path: str) -> tuple[str | None, str | None]:
    repo_type, repo_root = containing_repo_type(path)
    if repo_type is None:
        return None, None

    # Look for a .projectid file.  If it exists, read the project name from it.
    # pyre-fixme[6]: For 1st argument expected `LiteralString` but got `Optional[str]`.
    project_id_path = Path(repo_root, ".projectid")
    try:
        with open(project_id_path, "r") as f:
            project_name = f.read().strip()
            return repo_root, project_name
    except EnvironmentError as ex:
        if ex.errno != errno.ENOENT:
            raise

    return repo_root, None


class BuildOptions:
    def __init__(
        self,
        fbcode_builder_dir: str,
        scratch_dir: str,
        host_type: HostType,
        install_dir: str | None = None,
        num_jobs: int = 0,
        use_shipit: bool = False,
        vcvars_path: str | None = None,
        allow_system_packages: bool = False,
        vendor_dir: str | None = None,
        lfs_path: str | None = None,
        shared_lib: bool = False,
        facebook_internal: bool | None = None,
        free_up_disk: bool = False,
        build_type: str | None = None,
    ) -> None:
        """fbcode_builder_dir - the path to either the in-fbsource fbcode_builder dir,
                             or for shipit-transformed repos, the build dir that
                             has been mapped into that dir.
        scratch_dir - a place where we can store repos and build bits.
                      This path should be stable across runs and ideally
                      should not be in the repo of the project being built,
                      but that is ultimately where we generally fall back
                      for builds outside of FB
        install_dir - where the project will ultimately be installed
        num_jobs - the level of concurrency to use while building
        use_shipit - use real shipit instead of the simple shipit transformer
        vcvars_path - Path to external VS toolchain's vsvarsall.bat
        shared_lib - build every dep as a static archive of position-independent
                     code (CMAKE_POSITION_INDEPENDENT_CODE=ON, CFLAGS/CXXFLAGS=-fPIC)
                     and build the top-level cmake project with BUILD_SHARED_LIBS=ON
                     so it links the dep tree into a single shared library. To stop
                     dep symbols from leaking into the produced library's export
                     table (and clashing with other copies in the same process):
                       Linux: links with -Wl,--exclude-libs=ALL.
                       macOS: deps compile with -fvisibility=hidden so their
                              symbols become private_extern in the dylib.
                     Refused on Windows — there is no equivalent isolation story.
        free_up_disk - take extra actions to save runner disk space
        build_type - CMAKE_BUILD_TYPE, used by cmake and cargo builders
        """

        if not install_dir:
            install_dir = os.path.join(scratch_dir, "installed")

        self.project_hashes: str | None = None
        for p in ["../deps/github_hashes", "../project_hashes"]:
            hashes = Path(fbcode_builder_dir, p)
            if hashes.exists():
                self.project_hashes = os.fspath(hashes)
                break

        # Detect what repository and project we are being run from.
        self.repo_root, self.repo_project = detect_project(os.getcwd())

        # If we are running from an fbsource repository, set self.fbsource_dir
        # to allow the ShipIt-based fetchers to use it.
        if self.repo_project == "fbsource":
            self.fbsource_dir: str | None = self.repo_root
        else:
            self.fbsource_dir = None

        if facebook_internal is None:
            if self.fbsource_dir:
                facebook_internal = True
            else:
                facebook_internal = False

        self.facebook_internal: bool = facebook_internal
        self.specified_num_jobs: int = num_jobs
        self.scratch_dir: str = scratch_dir
        self.install_dir: str = install_dir
        self.fbcode_builder_dir: str = fbcode_builder_dir
        self.host_type: HostType = host_type
        self.use_shipit: bool = use_shipit
        self.allow_system_packages: bool = allow_system_packages
        # realpath so prefix checks against LocalDirFetcher paths line up
        self.vendor_dir: str | None = (
            os.fspath(Path(vendor_dir).resolve()) if vendor_dir else None
        )
        self.lfs_path: str | None = lfs_path
        self.shared_lib: bool = shared_lib
        if shared_lib and self.is_windows():
            raise Exception(
                "--shared-lib is not supported on Windows: there is no "
                "blanket equivalent of GNU ld's --exclude-libs=ALL or "
                "Mach-O's hidden-visibility linkage to keep statically "
                "linked dep symbols out of the produced DLL's export table."
            )
        # Set by BuildCmd to the name of the manifest the user requested,
        # so per-dep build steps (which see only their own manifest) can
        # tell whether they're the top-level project — needed to gate
        # behaviors like macOS hidden-visibility that should apply to
        # deps but not the consumer.
        self.top_level_manifest_name: str | None = None
        self.free_up_disk: bool = free_up_disk
        self.build_type: str | None = build_type

        lib_path: str | None = None
        if self.is_darwin():
            lib_path = "DYLD_LIBRARY_PATH"
        elif self.is_linux():
            lib_path = "LD_LIBRARY_PATH"
        elif self.is_windows():
            lib_path = "PATH"
        else:
            lib_path = None
        self.lib_path: str | None = lib_path

        if vcvars_path is None and is_windows():

            try:
                # Allow a site-specific vcvarsall path.
                from .facebook.vcvarsall import build_default_vcvarsall
            except ImportError:
                vcvarsall: list[str] = []
            else:
                vcvarsall = (
                    build_default_vcvarsall(self.fbsource_dir)
                    if self.fbsource_dir is not None
                    else []
                )

            # On Windows, the compiler is not available in the PATH by
            # default so we need to run the vcvarsall script to populate the
            # environment. We use a glob to find some version of this script
            # as deployed with Visual Studio.
            if len(vcvarsall) == 0:
                # check the 64 bit installs
                for year in ["2022"]:
                    vcvarsall += glob.glob(
                        os.fspath(
                            Path(
                                os.environ.get("ProgramFiles", "C:\\Program Files"),
                                "Microsoft Visual Studio",
                                year,
                                "*",
                                "VC",
                                "Auxiliary",
                                "Build",
                                "vcvarsall.bat",
                            )
                        )
                    )

                # then the 32 bit ones
                for year in ["2022", "2019", "2017"]:
                    vcvarsall += glob.glob(
                        os.fspath(
                            Path(
                                os.environ["ProgramFiles(x86)"],
                                "Microsoft Visual Studio",
                                year,
                                "*",
                                "VC",
                                "Auxiliary",
                                "Build",
                                "vcvarsall.bat",
                            )
                        )
                    )
            if len(vcvarsall) == 0:
                raise Exception(
                    "Could not find vcvarsall.bat. Please install Visual Studio."
                )
            vcvars_path = vcvarsall[0]
            print(f"Using vcvarsall.bat from {vcvars_path}", file=sys.stderr)

        self.vcvars_path: str | None = vcvars_path

    @property
    def manifests_dir(self) -> str:
        return os.fspath(Path(self.fbcode_builder_dir, "manifests"))

    def is_darwin(self) -> bool:
        return self.host_type.is_darwin()

    def is_windows(self) -> bool:
        return self.host_type.is_windows()

    def is_arm(self) -> bool:
        return self.host_type.is_arm()

    def get_vcvars_path(self) -> str | None:
        return self.vcvars_path

    def is_linux(self) -> bool:
        return self.host_type.is_linux()

    def is_freebsd(self) -> bool:
        return self.host_type.is_freebsd()

    def get_num_jobs(self, job_weight: int) -> int:
        """Given an estimated job_weight in MiB, compute a reasonable concurrency limit."""
        if self.specified_num_jobs:
            return self.specified_num_jobs

        available_ram = get_available_ram()

        import multiprocessing

        return max(1, min(multiprocessing.cpu_count(), available_ram // job_weight))

    def get_context_generator(
        self, host_tuple: str | HostType | None = None
    ) -> ContextGenerator:
        """Create a manifest ContextGenerator for the specified target platform."""
        if host_tuple is None:
            host_type = self.host_type
        elif isinstance(host_tuple, HostType):
            host_type = host_tuple
        else:
            host_type = HostType.from_tuple_string(host_tuple)

        return ContextGenerator(
            {
                "os": host_type.ostype,
                "distro": host_type.distro,
                "distro_vers": host_type.distrovers,
                "distro_family": host_type.distro_family,
                "fb": "on" if self.facebook_internal else "off",
                "fbsource": "on" if self.fbsource_dir else "off",
                "test": "off",
            }
        )

    def compute_env_for_install_dirs(
        self,
        loader: ManifestLoader,
        dep_manifests: list[ManifestParser],
        ctx: ManifestContext,
        env: Env | None = None,
        manifest: ManifestParser | None = None,
    ) -> Env:  # noqa: C901
        if env is not None:
            env = env.copy()
        else:
            env = Env()

        env["GETDEPS_BUILD_DIR"] = os.fspath(Path(self.scratch_dir, "build"))
        env["GETDEPS_INSTALL_DIR"] = self.install_dir

        # Python setuptools attempts to discover a local MSVC for
        # building Python extensions. On Windows, getdeps already
        # supports invoking a vcvarsall prior to compilation.
        #
        # Tell setuptools to bypass its own search. This fixes a bug
        # where setuptools would fail when run from CMake on GitHub
        # Actions with the inscrutable message 'error: Microsoft
        # Visual C++ 14.0 is required. Get it with "Build Tools for
        # Visual Studio"'. I suspect the actual error is that the
        # environment or PATH is overflowing.
        #
        # For extra credit, someone could patch setuptools to
        # propagate the actual error message from vcvarsall, because
        # often it does not mean Visual C++ is not available.
        #
        # Related discussions:
        # - https://github.com/pypa/setuptools/issues/2028
        # - https://github.com/pypa/setuptools/issues/2307
        # - https://developercommunity.visualstudio.com/t/error-microsoft-visual-c-140-is-required/409173
        # - https://github.com/OpenMS/OpenMS/pull/4779
        # - https://github.com/actions/virtual-environments/issues/1484

        if self.is_windows() and self.get_vcvars_path():
            env["DISTUTILS_USE_SDK"] = "1"

        # On macOS we need to set `SDKROOT` when we use clang for system
        # header files.
        if self.is_darwin() and "SDKROOT" not in env:
            sdkroot = subprocess.check_output(["xcrun", "--show-sdk-path"])
            env["SDKROOT"] = sdkroot.decode().strip()

        if (
            self.is_darwin()
            and self.allow_system_packages
            and self.host_type.get_package_manager() == "homebrew"
            and manifest
            and manifest.resolved_system_packages
        ):
            # Homebrew packages may not be on the default PATHs
            brew_packages = manifest.resolved_system_packages.get("homebrew", [])
            for p in brew_packages:
                found = self.add_homebrew_package_to_env(p, env)
                # Try extra hard to find openssl, needed with homebrew on macOS
                if found and p.startswith("openssl"):
                    candidate = homebrew_package_prefix("openssl@1.1")
                    #  `Union[PathLike[bytes], PathLike[str], bytes, int, str]` but got
                    #  `Optional[str]`.
                    if candidate and Path(candidate).exists():
                        #  `Optional[str]`.
                        os.environ["OPENSSL_ROOT_DIR"] = candidate
                        env["OPENSSL_ROOT_DIR"] = os.environ["OPENSSL_ROOT_DIR"]

        if self.fbsource_dir:
            env["YARN_YARN_OFFLINE_MIRROR"] = os.fspath(
                Path(self.fbsource_dir, "xplat/third-party/yarn/offline-mirror")
            )
            yarn_exe = "yarn.bat" if self.is_windows() else "yarn"
            env["YARN_PATH"] = os.fspath(
                Path(
                    #  `Optional[str]`.
                    self.fbsource_dir,
                    "xplat/third-party/yarn/",
                    yarn_exe,
                )
            )
            node_exe = "node-win-x64.exe" if self.is_windows() else "node"
            env["NODE_BIN"] = os.fspath(
                Path(
                    #  `Optional[str]`.
                    self.fbsource_dir,
                    "xplat/third-party/node/bin/",
                    node_exe,
                )
            )
            env["RUST_VENDORED_CRATES_DIR"] = os.fspath(
                Path(
                    #  `Optional[str]`.
                    self.fbsource_dir,
                    "third-party/rust/vendor",
                )
            )
            hash_data = get_fbsource_repo_data(self)
            env["FBSOURCE_HASH"] = hash_data.hash
            env["FBSOURCE_DATE"] = hash_data.date

        # reverse as we are prepending to the PATHs
        for m in reversed(dep_manifests):
            is_direct_dep = (
                manifest is not None and m.name in manifest.get_dependencies(ctx)
            )
            d = loader.get_project_install_dir(m)
            if Path(d).exists():
                self.add_prefix_to_env(
                    d,
                    env,
                    append=False,
                    is_direct_dep=is_direct_dep,
                )

        # Linux is always system openssl
        system_openssl: bool = self.is_linux()

        # For other systems lets see if package is requested
        if not system_openssl and manifest and manifest.resolved_system_packages:
            for _pkg_type, pkgs in manifest.resolved_system_packages.items():
                for p in pkgs:
                    if p.startswith("openssl") or p.startswith("libssl"):
                        system_openssl = True
                        break

        # Let openssl know to pick up the system certs if present
        if system_openssl or "OPENSSL_DIR" in env:
            for system_ssl_cfg in ["/etc/pki/tls", "/etc/ssl"]:
                cfg = Path(system_ssl_cfg)
                if cfg.is_dir():
                    cert_dir = cfg / "certs"
                    if cert_dir.is_dir():
                        env["SSL_CERT_DIR"] = os.fspath(cert_dir)
                    cert_file = cfg / "cert.pem"
                    if cert_file.is_file():
                        env["SSL_CERT_FILE"] = os.fspath(cert_file)

        return env

    def add_homebrew_package_to_env(self, package: str, env: Env) -> bool:
        prefix = homebrew_package_prefix(package)
        if prefix and Path(prefix).exists():
            return self.add_prefix_to_env(
                prefix, env, append=False, add_library_path=True
            )
        return False

    def add_prefix_to_env(
        self,
        d: str,
        env: Env,
        append: bool = True,
        add_library_path: bool = False,
        is_direct_dep: bool = False,
    ) -> bool:  # noqa: C901
        bindir: str = os.fspath(Path(d, "bin"))
        found: bool = False
        has_pkgconfig: bool = False
        pkgconfig: str = os.fspath(Path(d, "lib", "pkgconfig"))
        if Path(pkgconfig).exists():
            found = True
            has_pkgconfig = True
            add_path_entry(env, "PKG_CONFIG_PATH", pkgconfig, append=append)

        pkgconfig = os.fspath(Path(d, "lib64", "pkgconfig"))
        if Path(pkgconfig).exists():
            found = True
            has_pkgconfig = True
            add_path_entry(env, "PKG_CONFIG_PATH", pkgconfig, append=append)

        # For a library installed only as a static archive, like getdeps'
        # libcurl, tell Rust's pkg-config crate to also link the private
        # libraries in its .pc file, such as libcurl's nghttp2. A name that
        # isn't an identifier, like libusb-1.0's, can't be exported by a shell.
        for lib in ["lib", "lib64"]:
            for pc in Path(d, lib, "pkgconfig").glob("*.pc"):
                name = pc.stem[3:] if pc.stem.startswith("lib") else pc.stem
                files = [f.name for f in Path(d, lib).glob(f"lib{name}.*")]
                var = pc.stem.upper().replace("-", "_") + "_STATIC"
                if (
                    var.isidentifier()
                    and f"lib{name}.a" in files
                    and not any(".so" in f or f.endswith(".dylib") for f in files)
                ):
                    env[var] = "1"

        add_path_entry(env, "CMAKE_PREFIX_PATH", d, append=append)

        # Tell the thrift compiler about includes it needs to consider
        thriftdir: str = os.fspath(Path(d, "include", "thrift-files"))
        if Path(thriftdir).exists():
            found = True
            add_path_entry(env, "THRIFT_INCLUDE_PATH", thriftdir, append=append)

        # module detection for python is old fashioned and needs flags
        includedir: str = os.fspath(Path(d, "include"))
        if Path(includedir).exists():
            found = True
            ncursesincludedir: str = os.fspath(Path(d, "include", "ncurses"))
            if Path(ncursesincludedir).exists():
                add_path_entry(env, "C_INCLUDE_PATH", ncursesincludedir, append=append)
                add_flag(env, "CPPFLAGS", f"-I{includedir}", append=append)
                add_flag(env, "CPPFLAGS", f"-I{ncursesincludedir}", append=append)
            elif "/bz2-" in d:
                add_flag(env, "CPPFLAGS", f"-I{includedir}", append=append)
            # For non-pkgconfig projects Cabal has no way to find the includes or
            # libraries, so we provide a set of extra Cabal flags in the env
            if not has_pkgconfig and is_direct_dep:
                add_flag(
                    env,
                    "GETDEPS_CABAL_FLAGS",
                    f"--extra-include-dirs={includedir}",
                    append=append,
                )

            # The thrift compiler's built-in includes are installed directly to the include dir
            includethriftdir: str = os.fspath(Path(d, "include", "thrift"))
            if Path(includethriftdir).exists():
                add_path_entry(env, "THRIFT_INCLUDE_PATH", includedir, append=append)

        # Map from FB python manifests to PYTHONPATH
        pydir: str = os.fspath(Path(d, "lib", "fb-py-libs"))
        if Path(pydir).exists():
            found = True
            manifest_ext: str = ".manifest"
            pymanifestfiles: list[str] = [
                f.name
                for f in Path(pydir).iterdir()
                if f.name.endswith(manifest_ext) and f.is_file()
            ]
            for f in pymanifestfiles:
                subdir = f[: -len(manifest_ext)]
                add_path_entry(
                    env, "PYTHONPATH", os.fspath(Path(pydir, subdir)), append=append
                )

        # Allow resolving shared objects built earlier (eg: zstd
        # doesn't include the full path to the dylib in its linkage
        # so we need to give it an assist)
        if self.lib_path:
            for lib in ["lib", "lib64"]:
                libdir: str = os.fspath(Path(d, lib))
                if Path(libdir).exists():
                    found = True
                    #  `Optional[str]`.
                    add_path_entry(env, self.lib_path, libdir, append=append)
                    # module detection for python is old fashioned and needs flags
                    if "/ncurses-" in d:
                        add_flag(env, "LDFLAGS", f"-L{libdir}", append=append)
                    elif "/bz2-" in d:
                        add_flag(env, "LDFLAGS", f"-L{libdir}", append=append)
                    if add_library_path:
                        add_path_entry(env, "LIBRARY_PATH", libdir, append=append)
                    if not has_pkgconfig and is_direct_dep:
                        add_flag(
                            env,
                            "GETDEPS_CABAL_FLAGS",
                            f"--extra-lib-dirs={libdir}",
                            append=append,
                        )

        # Allow resolving binaries (eg: cmake, ninja) and dlls
        # built by earlier steps
        if Path(bindir).exists():
            found = True
            add_path_entry(env, "PATH", bindir, append=append)

        # If rustc is present in the `bin` directory, set RUSTC to prevent
        # cargo uses the rustc installed in the system.
        if self.is_windows():
            cargo_path: str = os.fspath(Path(bindir, "cargo.exe"))
            rustc_path: str = os.fspath(Path(bindir, "rustc.exe"))
            rustdoc_path: str = os.fspath(Path(bindir, "rustdoc.exe"))
        else:
            cargo_path = os.fspath(Path(bindir, "cargo"))
            rustc_path = os.fspath(Path(bindir, "rustc"))
            rustdoc_path = os.fspath(Path(bindir, "rustdoc"))

        if Path(rustc_path).is_file():
            env["CARGO_BIN"] = cargo_path
            env["RUSTC"] = rustc_path
            env["RUSTDOC"] = rustdoc_path

        openssl_include: str = os.fspath(Path(d, "include", "openssl"))
        if Path(openssl_include).is_dir() and any(
            Path(d, "lib", libcrypto).is_file()
            for libcrypto in ("libcrypto.lib", "libcrypto.so", "libcrypto.a")
        ):
            # This must be the openssl library, let Rust know about it
            env["OPENSSL_DIR"] = d

        return found


def list_win32_subst_letters() -> dict[str, str]:
    output = subprocess.check_output(["subst"]).decode("utf-8")
    # The output is a set of lines like: `F:\: => C:\open\some\where`
    lines = output.strip().split("\r\n")
    mapping: dict[str, str] = {}
    for line in lines:
        fields = line.split(": => ")
        if len(fields) != 2:
            continue
        letter = fields[0]
        path = fields[1]
        mapping[letter] = path

    return mapping


def find_existing_win32_subst_for_path(
    path: str,
    subst_mapping: Mapping[str, str],
) -> str | None:
    path = ntpath.normcase(ntpath.normpath(path))
    for letter, target in subst_mapping.items():
        if ntpath.normcase(target) == path:
            return letter
    return None


def find_unused_drive_letter() -> str | None:
    import ctypes

    buffer_len = 256
    blen = ctypes.c_uint(buffer_len)
    rv = ctypes.c_uint()
    bufs = ctypes.create_string_buffer(buffer_len)
    # pyre-fixme[16]: Module `ctypes` has no attribute `windll`.
    rv = ctypes.windll.kernel32.GetLogicalDriveStringsA(blen, bufs)
    if rv > buffer_len:
        raise Exception("GetLogicalDriveStringsA result too large for buffer")
    nul = "\x00".encode("ascii")

    used: list[str] = [
        drive.decode("ascii")[0] for drive in bufs.raw.strip(nul).split(nul)
    ]
    possible: list[str] = [c for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"]
    available: list[str] = sorted(list(set(possible) - set(used)))
    if len(available) == 0:
        return None
    # Prefer to assign later letters rather than earlier letters
    return available[-1]


def map_subst_path(path: str) -> str:
    """find a short drive letter mapping for a path"""
    for _attempt in range(0, 24):
        drive = find_existing_win32_subst_for_path(
            path, subst_mapping=list_win32_subst_letters()
        )
        if drive:
            return drive
        available = find_unused_drive_letter()
        if available is None:
            raise Exception(
                (
                    "unable to make shorter subst mapping for %s; "
                    "no available drive letters"
                )
                % path
            )

        # Try to set up a subst mapping; note that we may be racing with
        # other processes on the same host, so this may not succeed.
        try:
            subprocess.check_call(["subst", "%s:" % available, path])
            subst = "%s:\\" % available
            print("Mapped scratch dir %s -> %s" % (path, subst), file=sys.stderr)
            return subst
        except Exception:
            print("Failed to map %s -> %s" % (available, path), file=sys.stderr)

    raise Exception("failed to set up a subst path for %s" % path)


def _check_host_type(args: argparse.Namespace, host_type: HostType | None) -> HostType:
    if host_type is None:
        host_tuple_string: str | None = getattr(args, "host_type", None)
        if host_tuple_string:
            host_type = HostType.from_tuple_string(host_tuple_string)
        else:
            host_type = HostType()

    assert isinstance(host_type, HostType)
    return host_type


def setup_build_options(
    args: argparse.Namespace, host_type: HostType | None = None
) -> BuildOptions:
    """Create a BuildOptions object based on the arguments"""

    fbcode_builder_dir: str = os.fspath(Path(__file__).absolute().parent.parent)
    scratch_dir: str | None = args.scratch_path
    if not scratch_dir:
        # TODO: `mkscratch` doesn't currently know how best to place things on
        # sandcastle, so whip up something reasonable-ish
        if "SANDCASTLE" in os.environ:
            if "DISK_TEMP" not in os.environ:
                raise Exception(
                    (
                        "I need DISK_TEMP to be set in the sandcastle environment "
                        "so that I can store build products somewhere sane"
                    )
                )

            disk_temp: str = os.environ["DISK_TEMP"]
            if is_windows():
                # force use gitbash tmp dir for windows, as its less likely to have a tmp cleaner
                # that removes extracted prior dated source files
                Path(GITBASH_TMP).mkdir(parents=True, exist_ok=True)
                print(
                    f"Using {GITBASH_TMP} instead of DISK_TEMP {disk_temp} for scratch dir",
                    file=sys.stderr,
                )
                disk_temp = GITBASH_TMP

            scratch_dir = os.fspath(Path(disk_temp, "fbcode_builder_getdeps"))
        if not scratch_dir:
            try:
                scratch_dir = (
                    subprocess.check_output(
                        ["mkscratch", "path", "--subdir", "fbcode_builder_getdeps"]
                    )
                    .strip()
                    .decode("utf-8")
                )
            except OSError as exc:
                if exc.errno != errno.ENOENT:
                    # A legit failure; don't fall back, surface the error
                    raise
                # This system doesn't have mkscratch so we fall back to
                # something local.
                munged: str = fbcode_builder_dir.replace("Z", "zZ")
                for s in ["/", "\\", ":"]:
                    munged = munged.replace(s, "Z")

                if is_windows() and Path("c:/open").is_dir():
                    temp: str = "c:/open/scratch"
                else:
                    temp = tempfile.gettempdir()

                scratch_dir = os.fspath(
                    Path(temp, "fbcode_builder_getdeps-%s" % munged)
                )
                if not is_windows() and os.geteuid() == 0:
                    # Running as root; in the case where someone runs
                    # sudo getdeps.py install-system-deps
                    # and then runs as build without privs, we want to avoid creating
                    # a scratch dir that the second stage cannot write to.
                    # So we generate a different path if we are root.
                    scratch_dir += "-root"

        if not Path(scratch_dir).exists():
            Path(scratch_dir).mkdir(parents=True, exist_ok=True)

        if is_windows():
            subst = map_subst_path(scratch_dir)
            scratch_dir = subst
    else:
        if not Path(scratch_dir).exists():
            Path(scratch_dir).mkdir(parents=True, exist_ok=True)

    # Make sure we normalize the scratch path.  This path is used as part of the hash
    # computation for detecting if projects have been updated, so we need to always
    # use the exact same string to refer to a given directory.
    # But! realpath in some combinations of Windows/Python3 versions can expand the
    # drive substitutions on Windows, so avoid that!
    if not is_windows():
        scratch_dir = os.fspath(Path(scratch_dir).resolve())

    # Save these args passed by the user in an env variable, so it
    # can be used while hashing this build.
    os.environ["GETDEPS_CMAKE_DEFINES"] = getattr(args, "extra_cmake_defines", "") or ""

    host_type = _check_host_type(args, host_type)

    build_args: dict[str, object] = {
        k: v
        for (k, v) in vars(args).items()
        if k
        in {
            "num_jobs",
            "use_shipit",
            "vcvars_path",
            "allow_system_packages",
            "vendor_dir",
            "lfs_path",
            "shared_lib",
            "free_up_disk",
            "build_type",
        }
    }

    return BuildOptions(
        fbcode_builder_dir,
        scratch_dir,
        host_type,
        install_dir=args.install_prefix,
        facebook_internal=args.facebook_internal,
        # pyre-fixme[6]: For 6th argument expected `Optional[str]` but got `object`.
        # pyre-fixme[6]: For 6th argument expected `bool` but got `object`.
        # pyre-fixme[6]: For 6th argument expected `int` but got `object`.
        **build_args,
    )
