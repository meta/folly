# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import annotations

import os
import platform
import re
import shlex
import sys


def is_windows() -> bool:
    """Returns true if the system we are currently running on
    is a Windows system"""
    return sys.platform.startswith("win")


# Distribution families, after Chef's platform_family: distros that share a
# package namespace and release cadence for the purposes of manifests.
# Fedora is deliberately its own family (unlike Ansible's os_family, which
# folds it into RedHat): its base repos carry far more of our dependencies
# than the EL family's do, and manifests already select on that difference.
DISTRO_FAMILIES: dict[str, tuple[str, ...]] = {
    "rhel": ("rhel", "centos", "centos_stream", "alma", "rocky"),
    "fedora": ("fedora",),
    "debian": ("debian", "ubuntu", "pop!_os", "mint", "kali"),
    "arch": ("arch",),
}


def distro_family(distro: str | None) -> str | None:
    if distro is None:
        return None
    for family, members in DISTRO_FAMILIES.items():
        if distro in members:
            return family
    # Fall back to prefix matching: distro names are normalized from free
    # text, so an exact table miss can still belong to a known family
    # (for example, a distribution name with a release-specific suffix).
    # This preserves the pre-refactor startswith tolerance.
    for family, members in DISTRO_FAMILIES.items():
        if distro.startswith(members):
            return family
    return None


def get_linux_type() -> tuple[str | None, str | None, str | None]:
    try:
        with open("/etc/os-release") as f:
            data = f.read()
    except EnvironmentError:
        return (None, None, None)
    return parse_os_release(data)


def parse_os_release(data: str) -> tuple[str, str | None, str | None]:
    """Derive (ostype, distro, distrovers) from the contents of /etc/os-release."""
    os_vars: dict[str, str] = {}
    for line in data.splitlines():
        parts = line.split("=", 1)
        if len(parts) != 2:
            continue
        key = parts[0].strip()
        value_parts = shlex.split(parts[1].strip())
        if not value_parts:
            value = ""
        else:
            value = value_parts[0]
        os_vars[key] = value

    name = os_vars.get("NAME")
    if name:
        name = name.lower()
        name = re.sub(r"(?:gnu/)?linux", "", name)
        name = name.strip().replace(" ", "_")

    version_id = os_vars.get("VERSION_ID")
    if version_id:
        version_id = version_id.lower()

    # Red Hat Enterprise Linux's NAME would otherwise become
    # "red_hat_enterprise"; use the same short name as its os-release ID.
    if name == "red_hat_enterprise":
        name = "rhel"

    # The EL family is versioned by major release for packaging purposes,
    # and manifests select on distro_vers=9; RHEL, Alma and Rocky report
    # a minor too ("9.8") which would never match.
    if version_id and distro_family(name) == "rhel":
        version_id = version_id.split(".")[0]

    return "linux", name, version_id


# Ideally we'd use a common library like `psutil` to read system information,
# but getdeps can't take third-party dependencies.


def _get_available_ram_linux() -> int:
    # TODO: Ideally, this function would inspect the current cgroup for any
    # limits, rather than solely relying on system RAM.

    meminfo_path = "/proc/meminfo"
    try:
        with open(meminfo_path) as f:
            for line in f:
                try:
                    key, value = line.split(":", 1)
                except ValueError:
                    continue
                suffix = " kB\n"
                if key == "MemAvailable" and value.endswith(suffix):
                    value = value[: -len(suffix)]
                    try:
                        return int(value) // 1024
                    except ValueError:
                        continue
    except OSError:
        print("error opening {}".format(meminfo_path), end="", file=sys.stderr)
    else:
        print(
            "{} had no valid MemAvailable".format(meminfo_path), end="", file=sys.stderr
        )

    guess = 8
    print(", guessing {} GiB".format(guess), file=sys.stderr)
    return guess * 1024


def _get_available_ram_macos() -> int:
    import ctypes.util

    libc = ctypes.CDLL(ctypes.util.find_library("libc"), use_errno=True)
    sysctlbyname = libc.sysctlbyname
    sysctlbyname.restype = ctypes.c_int
    sysctlbyname.argtypes = [
        ctypes.c_char_p,
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.c_void_p,
        ctypes.c_size_t,
    ]
    # TODO: There may be some way to approximate an availability
    # metric, but just use total RAM for now.
    memsize = ctypes.c_int64()
    memsizesize = ctypes.c_size_t(8)
    res = sysctlbyname(
        b"hw.memsize", ctypes.byref(memsize), ctypes.byref(memsizesize), None, 0
    )
    if res != 0:
        raise NotImplementedError(
            f"failed to retrieve hw.memsize sysctl: {ctypes.get_errno()}"
        )
    return memsize.value // (1024 * 1024)


def _get_available_ram_windows() -> int:
    import ctypes

    DWORD = ctypes.c_uint32
    QWORD = ctypes.c_uint64

    class MEMORYSTATUSEX(ctypes.Structure):
        _fields_ = [
            ("dwLength", DWORD),
            ("dwMemoryLoad", DWORD),
            ("ullTotalPhys", QWORD),
            ("ullAvailPhys", QWORD),
            ("ullTotalPageFile", QWORD),
            ("ullAvailPageFile", QWORD),
            ("ullTotalVirtual", QWORD),
            ("ullAvailVirtual", QWORD),
            ("ullExtendedVirtual", QWORD),
        ]

    ms = MEMORYSTATUSEX()
    ms.dwLength = ctypes.sizeof(ms)
    # pyre-ignore[16]
    res = ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(ms))
    if res == 0:
        raise NotImplementedError("error calling GlobalMemoryStatusEx")

    # This is fuzzy, but AvailPhys is too conservative, and AvailTotal is too
    # aggressive, so average the two. It's okay for builds to use some swap.
    return (ms.ullAvailPhys + ms.ullTotalPhys) // (2 * 1024 * 1024)


def _get_available_ram_freebsd() -> int:
    import ctypes.util

    libc = ctypes.CDLL(ctypes.util.find_library("libc"), use_errno=True)
    sysctlbyname = libc.sysctlbyname
    sysctlbyname.restype = ctypes.c_int
    sysctlbyname.argtypes = [
        ctypes.c_char_p,
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.c_void_p,
        ctypes.c_size_t,
    ]
    # hw.usermem is pretty close to what we want.
    memsize = ctypes.c_int64()
    memsizesize = ctypes.c_size_t(8)
    res = sysctlbyname(
        b"hw.usermem", ctypes.byref(memsize), ctypes.byref(memsizesize), None, 0
    )
    if res != 0:
        raise NotImplementedError(
            f"failed to retrieve hw.memsize sysctl: {ctypes.get_errno()}"
        )
    return memsize.value // (1024 * 1024)


def get_available_ram() -> int:
    """
    Returns a platform-appropriate available RAM metric in MiB.
    """
    if sys.platform == "linux":
        return _get_available_ram_linux()
    elif sys.platform == "darwin":
        return _get_available_ram_macos()
    elif sys.platform == "win32":
        return _get_available_ram_windows()
    elif sys.platform.startswith("freebsd"):
        return _get_available_ram_freebsd()
    else:
        raise NotImplementedError(
            f"platform {sys.platform} does not have an implementation of get_available_ram"
        )


def is_current_host_arm() -> bool:
    if sys.platform.startswith("darwin"):
        # platform.machine() can be fooled by rosetta for python < 3.9.2
        return "ARM64" in os.uname().version
    else:
        machine = platform.machine().lower()
        return "arm" in machine or "aarch" in machine


class HostType:
    def __init__(
        self,
        ostype: str | None = None,
        distro: str | None = None,
        distrovers: str | None = None,
    ) -> None:
        # Maybe we should allow callers to indicate whether this machine uses
        # an ARM architecture, but we need to change HostType serialization
        # and deserialization in that case and hunt down anywhere that is
        # persisting that serialized data.
        isarm = False

        if ostype is None:
            distro = None
            distrovers = None
            if sys.platform.startswith("linux"):
                ostype, distro, distrovers = get_linux_type()
            elif sys.platform.startswith("darwin"):
                ostype = "darwin"
            elif is_windows():
                ostype = "windows"
                distrovers = str(sys.getwindowsversion().major)
            elif sys.platform.startswith("freebsd"):
                ostype = "freebsd"
            else:
                ostype = sys.platform

            isarm = is_current_host_arm()

        # The operating system type
        self.ostype: str | None = ostype
        # The distribution, if applicable
        self.distro: str | None = distro
        # The OS/distro version if known
        self.distrovers: str | None = distrovers
        # Does the CPU use an ARM architecture? ARM includes Apple Silicon
        # Macs as well as other ARM systems that might be running Linux or
        # something.
        self.isarm: bool = isarm

    def is_windows(self) -> bool:
        return self.ostype == "windows"

    # is_arm is kinda half implemented at the moment. This method is only
    # intended to be used when HostType represents information about the
    # current machine we are running on.
    # When HostType is being used to enumerate platform types (represent
    # information about machine types that we may or may not be running on)
    # the result could be nonsense (under the current implementation its always
    # false.)
    def is_arm(self) -> bool:
        return self.isarm

    def is_darwin(self) -> bool:
        return self.ostype == "darwin"

    def is_linux(self) -> bool:
        return self.ostype == "linux"

    def is_freebsd(self) -> bool:
        return self.ostype == "freebsd"

    @property
    def distro_family(self) -> str | None:
        return distro_family(self.distro)

    def as_tuple_string(self) -> str:
        return "%s-%s-%s" % (
            self.ostype,
            self.distro or "none",
            self.distrovers or "none",
        )

    def get_package_manager(self) -> str | None:
        if not self.is_linux() and not self.is_darwin():
            return None
        if self.is_darwin():
            return "homebrew"
        family = self.distro_family
        if family in ("fedora", "rhel"):
            return "rpm"
        if family == "debian":
            return "deb"
        if family == "arch":
            return "pacman-package"
        return None

    @staticmethod
    def from_tuple_string(s: str) -> HostType:
        ostype, distro, distrovers = s.split("-")
        return HostType(ostype=ostype, distro=distro, distrovers=distrovers)

    def __eq__(self, b: object) -> bool:
        if not isinstance(b, HostType):
            return False
        return (
            self.ostype == b.ostype
            and self.distro == b.distro
            and self.distrovers == b.distrovers
        )
