# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


from __future__ import annotations

import unittest
from unittest.mock import MagicMock

from ..buildopts import BuildOptions
from ..getdeps_platform import HostType
from ..load import Loader, ManifestLoader
from ..manifest import ManifestContext, ManifestParser, parse_dep_spec


def _ctx_with_features(features: set[str] | None = None) -> ManifestContext:
    ctx = ManifestContext(
        {
            "os": "linux",
            "distro": None,
            "distro_vers": None,
            "distro_family": None,
            "fb": "off",
            "fbsource": "off",
            "shared_lib": "off",
            "test": "off",
        }
    )
    for f in features or set():
        ctx.set("feature_" + f, "on")
    return ctx


class _DictLoader(Loader):
    """Test loader that resolves manifest names from an in-memory dict."""

    def __init__(self, manifests: dict[str, str]) -> None:
        self._sources: dict[str, str] = manifests
        self._cache: dict[str, ManifestParser] = {}

    def load_project(self, build_opts, project_name):  # type: ignore[no-untyped-def]
        if project_name not in self._cache:
            self._cache[project_name] = ManifestParser(
                project_name, self._sources[project_name]
            )
        return self._cache[project_name]

    def load_all(self, build_opts):  # type: ignore[no-untyped-def]
        return {n: self.load_project(build_opts, n) for n in self._sources}


def _make_loader(manifests: dict[str, str]) -> ManifestLoader:
    build_opts = MagicMock(spec=BuildOptions)
    build_opts.host_type = HostType("linux", None, None)
    build_opts.facebook_internal = False
    build_opts.fbsource_dir = None
    build_opts.shared_lib = False
    build_opts.allow_system_packages = False
    # Hand-roll a ContextGenerator instead of pulling in BuildOptions.
    from ..manifest import ContextGenerator

    ctx_gen = ContextGenerator(
        {
            "os": "linux",
            "distro": None,
            "distro_vers": None,
            "distro_family": None,
            "fb": "off",
            "fbsource": "off",
            "shared_lib": "off",
            "test": "off",
        }
    )
    loader = ManifestLoader(build_opts, ctx_gen)
    loader._loader = _DictLoader(manifests)
    # Pre-populate so load_manifest works without scanning.
    for name in manifests:
        loader.manifests_by_name[name] = loader._loader.load_project(build_opts, name)
    return loader


class ParseDepSpecTest(unittest.TestCase):
    def test_empty(self) -> None:
        self.assertEqual(parse_dep_spec(None), (set(), set(), False))
        self.assertEqual(parse_dep_spec(""), (set(), set(), False))

    def test_positive_request(self) -> None:
        req, pro, od = parse_dep_spec("serialization")
        self.assertEqual(req, {"serialization"})
        self.assertEqual(pro, set())
        self.assertFalse(od)

    def test_multiple_requests(self) -> None:
        req, pro, od = parse_dep_spec("a, b, c")
        self.assertEqual(req, {"a", "b", "c"})
        self.assertEqual(pro, set())
        self.assertFalse(od)

    def test_opt_out_default(self) -> None:
        req, pro, od = parse_dep_spec("!default")
        self.assertEqual(req, set())
        self.assertEqual(pro, set())
        self.assertTrue(od)

    def test_prohibition(self) -> None:
        req, pro, od = parse_dep_spec("!rpc")
        self.assertEqual(req, set())
        self.assertEqual(pro, {"rpc"})
        self.assertFalse(od)

    def test_combined(self) -> None:
        req, pro, od = parse_dep_spec("!default, !rpc, serialization")
        self.assertEqual(req, {"serialization"})
        self.assertEqual(pro, {"rpc"})
        self.assertTrue(od)

    def test_whitespace_tolerant(self) -> None:
        req, pro, od = parse_dep_spec("  !default ,  serialization  ,  !rpc  ")
        self.assertEqual(req, {"serialization"})
        self.assertEqual(pro, {"rpc"})
        self.assertTrue(od)


class FeatureSectionTest(unittest.TestCase):
    def test_no_features_section(self) -> None:
        m = ManifestParser(
            "p",
            """
[manifest]
name = p
""",
        )
        self.assertEqual(m.declared_features, set())
        self.assertEqual(m.default_features, set())

    def test_declares_features_and_defaults(self) -> None:
        m = ManifestParser(
            "p",
            """
[manifest]
name = p

[features]
default = a, b
a =
b =
c =
""",
        )
        self.assertEqual(m.declared_features, {"a", "b", "c"})
        self.assertEqual(m.default_features, {"a", "b"})

    def test_default_must_reference_declared_feature(self) -> None:
        with self.assertRaisesRegex(Exception, "undeclared features"):
            ManifestParser(
                "p",
                """
[manifest]
name = p

[features]
default = nope
""",
            )

    def test_dependency_specs_round_trip(self) -> None:
        m = ManifestParser(
            "p",
            """
[manifest]
name = p

[dependencies]
a
b = serialization
c = !default, !rpc, serialization
""",
        )
        ctx = _ctx_with_features()
        specs = m.get_dependency_specs(ctx)
        self.assertEqual(
            specs,
            [
                ("a", set(), set(), False),
                ("b", {"serialization"}, set(), False),
                ("c", {"serialization"}, {"rpc"}, True),
            ],
        )


class FeatureGatedSectionsTest(unittest.TestCase):
    def test_dependencies_gate(self) -> None:
        m = ManifestParser(
            "p",
            """
[manifest]
name = p

[features]
default = rpc
rpc =

[dependencies]
always

[dependencies.feature_rpc=on]
rpconly
""",
        )
        # With rpc on, both deps appear.
        on_ctx = _ctx_with_features({"rpc"})
        self.assertEqual(
            sorted(d for d, *_ in m.get_dependency_specs(on_ctx)),
            ["always", "rpconly"],
        )
        # With rpc off, only the unconditional dep appears.
        off_ctx = _ctx_with_features()
        self.assertEqual(
            [d for d, *_ in m.get_dependency_specs(off_ctx)],
            ["always"],
        )


class ResolverTest(unittest.TestCase):
    def test_root_gets_default_features(self) -> None:
        loader = _make_loader(
            {
                "root": """
[manifest]
name = root

[features]
default = a
a =
b =
""",
            }
        )
        loader.resolve_features([loader.load_manifest("root")])
        ctx = loader.ctx_gen.get_context("root")
        self.assertEqual(ctx.features(), {"a"})

    def test_dep_inherits_dep_defaults(self) -> None:
        loader = _make_loader(
            {
                "root": """
[manifest]
name = root

[dependencies]
lib
""",
                "lib": """
[manifest]
name = lib

[features]
default = a
a =
b =
""",
            }
        )
        loader.resolve_features([loader.load_manifest("root")])
        self.assertEqual(loader.ctx_gen.get_context("lib").features(), {"a"})

    def test_consumer_can_request_extra_feature(self) -> None:
        loader = _make_loader(
            {
                "root": """
[manifest]
name = root

[dependencies]
lib = b
""",
                "lib": """
[manifest]
name = lib

[features]
default = a
a =
b =
""",
            }
        )
        loader.resolve_features([loader.load_manifest("root")])
        # default (a) plus requested (b).
        self.assertEqual(loader.ctx_gen.get_context("lib").features(), {"a", "b"})

    def test_opt_out_default(self) -> None:
        loader = _make_loader(
            {
                "root": """
[manifest]
name = root

[dependencies]
lib = !default
""",
                "lib": """
[manifest]
name = lib

[features]
default = a
a =
""",
            }
        )
        loader.resolve_features([loader.load_manifest("root")])
        self.assertEqual(loader.ctx_gen.get_context("lib").features(), set())

    def test_opt_out_default_then_request_other(self) -> None:
        loader = _make_loader(
            {
                "root": """
[manifest]
name = root

[dependencies]
lib = !default, b
""",
                "lib": """
[manifest]
name = lib

[features]
default = a
a =
b =
""",
            }
        )
        loader.resolve_features([loader.load_manifest("root")])
        self.assertEqual(loader.ctx_gen.get_context("lib").features(), {"b"})

    def test_features_compose_across_consumers(self) -> None:
        loader = _make_loader(
            {
                "root": """
[manifest]
name = root

[dependencies]
mid
direct = !default, b
""",
                "mid": """
[manifest]
name = mid

[dependencies]
direct = !default, a
""",
                "direct": """
[manifest]
name = direct

[features]
a =
b =
""",
            }
        )
        loader.resolve_features([loader.load_manifest("root")])
        # Union across edges from root and mid.
        self.assertEqual(loader.ctx_gen.get_context("direct").features(), {"a", "b"})

    def test_prohibition_conflict_detected(self) -> None:
        loader = _make_loader(
            {
                "root": """
[manifest]
name = root

[dependencies]
mid
strict
""",
                "mid": """
[manifest]
name = mid

[dependencies]
lib
""",
                "strict": """
[manifest]
name = strict

[dependencies]
lib = !default, !rpc
""",
                "lib": """
[manifest]
name = lib

[features]
default = rpc
rpc =
""",
            }
        )
        with self.assertRaisesRegex(Exception, "feature resolution conflicts"):
            loader.resolve_features([loader.load_manifest("root")])

    def test_no_conflict_when_only_one_consumer(self) -> None:
        loader = _make_loader(
            {
                "root": """
[manifest]
name = root

[dependencies]
lib = !default, !rpc, serialization
""",
                "lib": """
[manifest]
name = lib

[features]
default = rpc, serialization
rpc =
serialization =
""",
            }
        )
        loader.resolve_features([loader.load_manifest("root")])
        self.assertEqual(
            loader.ctx_gen.get_context("lib").features(), {"serialization"}
        )

    def test_feature_gated_dep_discovered_after_resolution(self) -> None:
        # Adding feature `b` to `lib` should pull in `extra` via the gated
        # dependencies section. The fixpoint loop must catch this.
        loader = _make_loader(
            {
                "root": """
[manifest]
name = root

[dependencies]
lib = b
""",
                "lib": """
[manifest]
name = lib

[features]
default =
b =

[dependencies.feature_b=on]
extra
""",
                "extra": """
[manifest]
name = extra
""",
            }
        )
        loader.resolve_features([loader.load_manifest("root")])
        order = [
            m.name
            for m in loader.manifests_in_dependency_order(loader.load_manifest("root"))
        ]
        self.assertIn("extra", order)
        self.assertLess(order.index("extra"), order.index("lib"))

    _LIB = """
[manifest]
name = lib

[features]
default =
b =
"""

    def test_features_from_rejects_test_only_request_for_a_plain_dep(self) -> None:
        loader = _make_loader(
            {
                "root": """
[manifest]
name = root

[dependencies.test=off]
lib

[dependencies.test=on]
lib = b
""",
                "lib": self._LIB,
            }
        )
        with self.assertRaisesRegex(ValueError, "only for test builds"):
            loader.resolve_features_from("root", "root")

    def test_features_from_rejects_request_from_a_test_only_dep(self) -> None:
        loader = _make_loader(
            {
                "root": """
[manifest]
name = root

[dependencies]
lib

[dependencies.test=on]
helper
""",
                "helper": """
[manifest]
name = helper

[dependencies]
lib = b
""",
                "lib": self._LIB,
            }
        )
        with self.assertRaisesRegex(ValueError, "only for test builds"):
            loader.resolve_features_from("root", "root")

    def test_features_from_accepts_a_test_only_dep_without_features(self) -> None:
        loader = _make_loader(
            {
                "root": """
[manifest]
name = root

[dependencies]
lib = b

[dependencies.test=on]
googletest
""",
                "googletest": """
[manifest]
name = googletest
""",
                "lib": self._LIB,
            }
        )
        loader.resolve_features_from("root", "lib")
        self.assertEqual(loader.ctx_gen.get_context("lib").features(), {"b"})
