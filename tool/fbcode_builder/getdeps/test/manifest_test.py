# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import unittest
from unittest.mock import MagicMock

from ..buildopts import BuildOptions
from ..load import load_all_manifests, patch_loader
from ..manifest import ManifestParser


class ManifestTest(unittest.TestCase):
    def test_missing_section(self) -> None:
        with self.assertRaisesRegex(
            Exception, "manifest file test is missing required section manifest"
        ):
            ManifestParser("test", "")

    def test_missing_name(self) -> None:
        with self.assertRaisesRegex(
            Exception,
            "manifest file test section 'manifest' is missing required field 'name'",
        ):
            ManifestParser(
                "test",
                """
[manifest]
""",
            )

    def test_minimal(self) -> None:
        p = ManifestParser(
            "test",
            """
[manifest]
name = test
""",
        )
        self.assertEqual(p.name, "test")
        self.assertEqual(p.fbsource_path, None)

    def test_minimal_with_fbsource_path(self) -> None:
        p = ManifestParser(
            "test",
            """
[manifest]
name = test
fbsource_path = fbcode/wat
""",
        )
        self.assertEqual(p.name, "test")
        self.assertEqual(p.fbsource_path, "fbcode/wat")

    def test_unknown_field(self) -> None:
        with self.assertRaisesRegex(
            Exception,
            (
                "manifest file test section 'manifest' contains "
                "unknown field 'invalid.field'"
            ),
        ):
            ManifestParser(
                "test",
                """
[manifest]
name = test
invalid.field = woot
""",
            )

    def test_invalid_section_name(self) -> None:
        with self.assertRaisesRegex(
            Exception, "manifest file test contains unknown section 'invalid.section'"
        ):
            ManifestParser(
                "test",
                """
[manifest]
name = test

[invalid.section]
foo = bar
""",
            )

    def test_invalid_conditional_section_name(self) -> None:
        with self.assertRaisesRegex(
            Exception,
            (
                "manifest file test section 'dependencies.=' "
                "has invalid conditional: expected "
                "identifier found ="
            ),
        ):
            ManifestParser(
                "test",
                """
[manifest]
name = test

[dependencies.=]
""",
            )

    def test_section_as_args(self) -> None:
        p = ManifestParser(
            "test",
            """
[manifest]
name = test

[dependencies]
a
b
c

[dependencies.test=on]
foo
""",
        )
        self.assertEqual(p.get_section_as_args("dependencies"), ["a", "b", "c"])
        self.assertEqual(
            p.get_section_as_args("dependencies", {"test": "off"}), ["a", "b", "c"]
        )
        self.assertEqual(
            p.get_section_as_args("dependencies", {"test": "on"}),
            ["a", "b", "c", "foo"],
        )

        p2 = ManifestParser(
            "test",
            """
[manifest]
name = test

[autoconf.args]
--prefix=/foo
--with-woot
""",
        )
        self.assertEqual(
            p2.get_section_as_args("autoconf.args"), ["--prefix=/foo", "--with-woot"]
        )

    def test_github_actions_sccache_off_accepted(self) -> None:
        p = ManifestParser(
            "test",
            """
[manifest]
name = test

[github.actions]
sccache = off
""",
        )
        self.assertEqual(p.get("github.actions", "sccache"), "off")

    def test_github_actions_sccache_on_accepted(self) -> None:
        p = ManifestParser(
            "test",
            """
[manifest]
name = test

[github.actions]
sccache = on
""",
        )
        self.assertEqual(p.get("github.actions", "sccache"), "on")

    def test_github_actions_sccache_absent_returns_none(self) -> None:
        p = ManifestParser(
            "test",
            """
[manifest]
name = test
""",
        )
        self.assertIsNone(p.get("github.actions", "sccache"))

    def test_section_as_dict(self) -> None:
        p = ManifestParser(
            "test",
            """
[manifest]
name = test

[cmake.defines]
foo = bar

[cmake.defines.test=on]
foo = baz
""",
        )
        self.assertEqual(p.get_section_as_dict("cmake.defines", {}), {"foo": "bar"})
        self.assertEqual(
            p.get_section_as_dict("cmake.defines", {"test": "on"}), {"foo": "baz"}
        )

        p2 = ManifestParser(
            "test",
            """
[manifest]
name = test

[cmake.defines.test=on]
foo = baz

[cmake.defines]
foo = bar
""",
        )
        self.assertEqual(
            p2.get_section_as_dict("cmake.defines", {"test": "on"}),
            {"foo": "bar"},
            msg="sections cascade in the order they appear in the manifest",
        )

    def test_parse_common_manifests(self) -> None:
        patch_loader(__name__)
        # pyre-fixme[6]: For 1st argument expected `BuildOptions` but got `None`.
        manifests = load_all_manifests(None)
        self.assertNotEqual(0, len(manifests), msg="parsed some number of manifests")

    def test_shipit_project_has_one_fbcode_builder_layout(self) -> None:
        # ShipIt exports fbcode_builder once per GitHub repo, so the manifests
        # of one shipit_project must agree on where it goes.
        patch_loader(__name__)
        manifests = load_all_manifests(MagicMock(spec=BuildOptions))
        dirs_by_project: dict[str, set[str]] = {}
        for m in manifests.values():
            if m.shipit_project and m.shipit_fbcode_builder:
                dirs_by_project.setdefault(m.shipit_project, set()).add(
                    m.shipit_fbcode_builder_dir
                )
        self.assertEqual(dirs_by_project["folly"], {"tool/fbcode_builder"})
        self.assertEqual({p: d for p, d in dirs_by_project.items() if len(d) > 1}, {})

    def test_shared_folly_link_inputs(self) -> None:
        # Shared libfolly.so links third-party static archives. Those archives
        # must be position-independent, and libaio must install a shared
        # library, or the no-system-packages builds fail in the folly link.
        patch_loader(__name__)
        # The resource loader ignores build options; a mock only satisfies
        # the type signature.
        manifests = load_all_manifests(MagicMock(spec=BuildOptions))
        for name in ("zlib", "libevent", "libaio"):
            self.assertIn(name, manifests, msg="manifest still exists")
        for name in ("zlib", "libevent"):
            defines = manifests[name].get_section_as_dict("cmake.defines", {})
            self.assertEqual(
                defines.get("CMAKE_POSITION_INDEPENDENT_CODE"),
                "ON",
                msg=name + " archives stay linkable into shared libfolly.so",
            )
        libaio = manifests["libaio"]
        self.assertIn(
            "ENABLE_SHARED=1",
            libaio.get_section_as_args("make.build_args", {}),
            msg="source-built libaio provides libaio.so with version nodes",
        )
        self.assertIn(
            "ENABLE_SHARED=1",
            libaio.get_section_as_args("make.install_args", {}),
            msg="source-built libaio installs libaio.so with version nodes",
        )

    def test_shared_lib_builds_boost_from_source(self) -> None:
        # EL 10's boost-static archives are not position-independent, so a
        # --shared-lib build cannot link them into a shared library.
        patch_loader(__name__)
        manifests = load_all_manifests(MagicMock(spec=BuildOptions))
        el10 = {
            "os": "linux",
            "distro": "centos_stream",
            "distro_vers": "10",
            "distro_family": "rhel",
            "fb": "off",
            "fbsource": "off",
            "test": "off",
        }
        boost = manifests["boost"]
        self.assertEqual(
            boost.get_section_as_args("rpms", {**el10, "shared_lib": "off"}),
            ["boost-devel", "boost-static"],
        )
        self.assertEqual(
            boost.get_section_as_args("rpms", {**el10, "shared_lib": "on"}), []
        )

    def test_mismatch_name(self) -> None:
        with self.assertRaisesRegex(
            Exception,
            "filename of the manifest 'foo' does not match the manifest name 'bar'",
        ):
            ManifestParser(
                "foo",
                """
[manifest]
name = bar
""",
            )

    def test_duplicate_manifest(self) -> None:
        patch_loader(__name__, "fixtures/duplicate")

        with self.assertRaisesRegex(Exception, "found duplicate manifest 'foo'"):
            # pyre-fixme[6]: For 1st argument expected `BuildOptions` but got `None`.
            load_all_manifests(None)
