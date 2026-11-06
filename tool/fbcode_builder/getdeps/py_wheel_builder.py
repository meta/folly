# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import annotations

import codecs
import collections
import email
import email.message
import os
import re
import stat
from pathlib import Path

from .builder import BuilderBase, CMakeBuilder


WheelNameInfo = collections.namedtuple(
    "WheelNameInfo", ("distribution", "version", "build", "python", "abi", "platform")
)

CMAKE_HEADER = """
cmake_minimum_required(VERSION 3.8)

project("{manifest_name}" LANGUAGES C)

set(CMAKE_MODULE_PATH
  "{cmake_dir}"
  ${{CMAKE_MODULE_PATH}}
)
include(FBPythonBinary)

set(CMAKE_INSTALL_DIR lib/cmake/{manifest_name} CACHE STRING
    "The subdirectory where CMake package config files should be installed")
"""

CMAKE_FOOTER = """
install_fb_python_library({lib_name} EXPORT all)
install(
  EXPORT all
  FILE {manifest_name}-targets.cmake
  NAMESPACE {namespace}::
  DESTINATION ${{CMAKE_INSTALL_DIR}}
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
  ${{CMAKE_BINARY_DIR}}/{manifest_name}-config.cmake.in
  {manifest_name}-config.cmake
  INSTALL_DESTINATION ${{CMAKE_INSTALL_DIR}}
  PATH_VARS
    CMAKE_INSTALL_DIR
)
install(
  FILES ${{CMAKE_CURRENT_BINARY_DIR}}/{manifest_name}-config.cmake
  DESTINATION ${{CMAKE_INSTALL_DIR}}
)
"""

CMAKE_CONFIG_FILE = """
@PACKAGE_INIT@

include(CMakeFindDependencyMacro)

set_and_check({upper_name}_CMAKE_DIR "@PACKAGE_CMAKE_INSTALL_DIR@")

if (NOT TARGET {namespace}::{lib_name})
  include("${{{upper_name}_CMAKE_DIR}}/{manifest_name}-targets.cmake")
endif()

set({upper_name}_LIBRARIES {namespace}::{lib_name})

{find_dependency_lines}

if (NOT {manifest_name}_FIND_QUIETLY)
  message(STATUS "Found {manifest_name}: ${{PACKAGE_PREFIX_DIR}}")
endif()
"""


# Note: for now we are manually manipulating the wheel packet contents.
# The wheel format is documented here:
# https://www.python.org/dev/peps/pep-0491/#file-format
#
# We currently aren't particularly smart about correctly handling the full wheel
# functionality, but this is good enough to handle simple pure-python wheels,
# which is the main thing we care about right now.
#
# We could potentially use pip to install the wheel to a temporary location and
# then copy its "installed" files, but this has its own set of complications.
# This would require pip to already be installed and available, and we would
# need to correctly find the right version of pip or pip3 to use.
# If we did ever want to go down that path, we would probably want to use
# something like the following pip3 command:
#   pip3 --isolated install --no-cache-dir --no-index --system \
#       --target <install_dir> <wheel_file>
class PythonWheelBuilder(BuilderBase):
    """This Builder can take Python wheel archives and install them as python libraries
    that can be used by add_fb_python_library()/add_fb_python_executable() CMake rules.
    """

    dist_info_dir: str
    template_format_dict: dict[str, str]

    def _build(self, reconfigure: bool) -> None:
        # When we are invoked, self.src_dir contains the unpacked wheel contents.
        #
        # Since a wheel file is just a zip file, the Fetcher code recognizes it as such
        # and goes ahead and unpacks it.  (We could disable that Fetcher behavior in the
        # future if we ever wanted to, say if we wanted to call pip here.)
        wheel_name = self._parse_wheel_name()
        name_version_prefix = "-".join((wheel_name.distribution, wheel_name.version))
        dist_info_name = name_version_prefix + ".dist-info"
        data_dir_name = name_version_prefix + ".data"
        self.dist_info_dir = os.fspath(Path(self.src_dir, dist_info_name))
        wheel_metadata = self._read_wheel_metadata(wheel_name)

        # Check that we can understand the wheel version.
        # We don't really care about wheel_metadata["Root-Is-Purelib"] since
        # we are generating our own standalone python archives rather than installing
        # into site-packages.
        version = wheel_metadata["Wheel-Version"]
        if not version.startswith("1."):
            raise Exception("unsupported wheel version %s" % (version,))

        # Add a find_dependency() call for each of our dependencies.
        # The dependencies are also listed in the wheel METADATA file, but it is simpler
        # to pull this directly from the getdeps manifest.
        dep_list = sorted(
            self.manifest.get_section_as_dict("dependencies", self.ctx).keys()
        )
        find_dependency_lines = ["find_dependency({})".format(dep) for dep in dep_list]

        getdeps_cmake_dir = os.fspath(Path(__file__).parent.parent / "CMake")
        self.template_format_dict = {
            # Note that CMake files always uses forward slash separators in path names,
            # even on Windows.  Therefore replace path separators here.
            "cmake_dir": _to_cmake_path(getdeps_cmake_dir),
            "lib_name": self.manifest.name,
            "manifest_name": self.manifest.name,
            "namespace": self.manifest.name,
            "upper_name": self.manifest.name.upper().replace("-", "_"),
            "find_dependency_lines": "\n".join(find_dependency_lines),
        }

        # Find sources from the root directory
        path_mapping: dict[str, str] = {}
        for entry in os.listdir(self.src_dir):
            if entry == data_dir_name:
                continue
            self._add_sources(path_mapping, os.fspath(Path(self.src_dir, entry)), entry)

        # Files under the .data directory also need to be installed in the correct
        # locations
        if Path(data_dir_name).exists():
            # TODO: process the subdirectories of data_dir_name
            # This isn't implemented yet since for now we have only needed dependencies
            # on some simple pure Python wheels, so I haven't tested against wheels with
            # additional files in the .data directory.
            raise Exception(
                "handling of the subdirectories inside %s is not implemented yet"
                % data_dir_name
            )

        # Emit CMake files
        self._write_cmakelists(path_mapping, dep_list)
        self._write_cmake_config_template()

        # Run the build
        self._run_cmake_build(reconfigure)

    def _run_cmake_build(self, reconfigure: bool) -> None:
        cmake_builder = CMakeBuilder(
            loader=self.loader,
            dep_manifests=self.dep_manifests,
            build_opts=self.build_opts,
            ctx=self.ctx,
            manifest=self.manifest,
            # Note that we intentionally supply src_dir=build_dir,
            # since we wrote out our generated CMakeLists.txt in the build directory
            src_dir=self.build_dir,
            build_dir=self.build_dir,
            inst_dir=self.inst_dir,
            defines={},
            final_install_prefix=None,
        )
        cmake_builder.build(reconfigure=reconfigure)

    def _write_cmakelists(
        self, path_mapping: dict[str, str], dependencies: list[str]
    ) -> None:
        cmake_path = os.fspath(Path(self.build_dir, "CMakeLists.txt"))
        with open(cmake_path, "w") as f:
            f.write(CMAKE_HEADER.format(**self.template_format_dict))
            for dep in dependencies:
                f.write("find_package({0} REQUIRED)\n".format(dep))

            f.write(
                "add_fb_python_library({lib_name}\n".format(**self.template_format_dict)
            )
            f.write('  BASE_DIR "%s"\n' % _to_cmake_path(self.src_dir))
            f.write("  SOURCES\n")
            for src_path, install_path in path_mapping.items():
                f.write(
                    '    "%s=%s"\n'
                    % (_to_cmake_path(src_path), _to_cmake_path(install_path))
                )
            if dependencies:
                f.write("  DEPENDS\n")
                for dep in dependencies:
                    f.write('    "{0}::{0}"\n'.format(dep))
            f.write(")\n")

            f.write(CMAKE_FOOTER.format(**self.template_format_dict))

    def _write_cmake_config_template(self) -> None:
        config_path_name = self.manifest.name + "-config.cmake.in"
        output_path = os.fspath(Path(self.build_dir, config_path_name))

        with open(output_path, "w") as f:
            f.write(CMAKE_CONFIG_FILE.format(**self.template_format_dict))

    def _add_sources(
        self, path_mapping: dict[str, str], src_path: str, install_path: str
    ) -> None:
        s = os.lstat(src_path)
        if not stat.S_ISDIR(s.st_mode):
            path_mapping[src_path] = install_path
            return

        for entry in os.listdir(src_path):
            self._add_sources(
                path_mapping,
                os.fspath(Path(src_path, entry)),
                os.fspath(Path(install_path, entry)),
            )

    def _parse_wheel_name(self) -> WheelNameInfo:
        # The ArchiveFetcher prepends "manifest_name-", so strip that off first.
        wheel_name = Path(self.src_dir).name
        prefix = self.manifest.name + "-"
        if not wheel_name.startswith(prefix):
            raise Exception(
                "expected wheel source directory to be of the form %s-NAME.whl"
                % (prefix,)
            )
        wheel_name = wheel_name[len(prefix) :]

        wheel_name_re = re.compile(
            r"(?P<distribution>[^-]+)"
            r"-(?P<version>\d+[^-]*)"
            r"(-(?P<build>\d+[^-]*))?"
            r"-(?P<python>\w+\d+(\.\w+\d+)*)"
            r"-(?P<abi>\w+)"
            r"-(?P<platform>\w+(\.\w+)*)"
            r"\.whl"
        )
        match = wheel_name_re.match(wheel_name)
        if not match:
            raise Exception(
                "bad python wheel name %s: expected to have the form "
                "DISTRIBUTION-VERSION-[-BUILD]-PYTAG-ABI-PLATFORM"
            )

        return WheelNameInfo(
            distribution=match.group("distribution"),
            version=match.group("version"),
            build=match.group("build"),
            python=match.group("python"),
            abi=match.group("abi"),
            platform=match.group("platform"),
        )

    def _read_wheel_metadata(self, wheel_name: WheelNameInfo) -> email.message.Message:
        metadata_path = os.fspath(Path(self.dist_info_dir, "WHEEL"))
        with codecs.open(metadata_path, "r", encoding="utf-8") as f:
            return email.message_from_file(f)


def _to_cmake_path(path: str) -> str:
    # CMake always uses forward slashes to separate paths in CMakeLists.txt files,
    # even on Windows.  It treats backslashes as character escapes, so using
    # backslashes in the path will cause problems.  Therefore replace all path
    # separators with forward slashes to make sure the paths are correct on Windows.
    # e.g. "C:\foo\bar.txt" becomes "C:/foo/bar.txt"
    return path.replace(os.path.sep, "/")
