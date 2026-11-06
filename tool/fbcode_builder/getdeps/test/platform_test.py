# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import unittest

from ..getdeps_platform import HostType, parse_os_release


class PlatformTest(unittest.TestCase):
    def test_create(self) -> None:
        p = HostType()
        self.assertNotEqual(p.ostype, None, msg="probed and returned something")

        tuple_string = p.as_tuple_string()
        round_trip = HostType.from_tuple_string(tuple_string)
        self.assertEqual(round_trip, p)

    def test_rendering_of_none(self) -> None:
        p = HostType(ostype="foo")
        self.assertEqual(p.as_tuple_string(), "foo-none-none")

    def test_is_methods(self) -> None:
        p = HostType(ostype="windows")
        self.assertTrue(p.is_windows())
        self.assertFalse(p.is_darwin())
        self.assertFalse(p.is_linux())

        p = HostType(ostype="darwin")
        self.assertFalse(p.is_windows())
        self.assertTrue(p.is_darwin())
        self.assertFalse(p.is_linux())

        p = HostType(ostype="linux")
        self.assertFalse(p.is_windows())
        self.assertFalse(p.is_darwin())
        self.assertTrue(p.is_linux())


class OsReleaseTest(unittest.TestCase):
    def parse(self, name: str, version_id: str) -> tuple[str, str | None, str | None]:
        return parse_os_release(f'NAME="{name}"\nVERSION_ID="{version_id}"\n')

    def test_fedora(self) -> None:
        self.assertEqual(self.parse("Fedora Linux", "44"), ("linux", "fedora", "44"))
        self.assertEqual(HostType("linux", "fedora", "44").distro_family, "fedora")

    def test_centos_stream(self) -> None:
        self.assertEqual(
            self.parse("CentOS Stream", "9"), ("linux", "centos_stream", "9")
        )

    def test_rhel_gets_short_name_and_major_version(self) -> None:
        # EPEL buildroots run real RHEL, whose NAME would otherwise become
        # "red_hat_enterprise" and whose VERSION_ID carries a minor.
        ostype, distro, vers = self.parse("Red Hat Enterprise Linux", "9.8")
        self.assertEqual((ostype, distro, vers), ("linux", "rhel", "9"))
        host = HostType(ostype, distro, vers)
        self.assertEqual(host.distro_family, "rhel")
        self.assertEqual(host.get_package_manager(), "rpm")

    def test_el_rebuilds_share_family_and_major_version(self) -> None:
        self.assertEqual(self.parse("AlmaLinux", "9.6"), ("linux", "alma", "9"))
        self.assertEqual(self.parse("Rocky Linux", "9.6"), ("linux", "rocky", "9"))
        self.assertEqual(HostType("linux", "alma", "9").distro_family, "rhel")

    def test_debian_family_keeps_full_version(self) -> None:
        self.assertEqual(self.parse("Ubuntu", "22.04"), ("linux", "ubuntu", "22.04"))
        host = HostType("linux", "ubuntu", "22.04")
        self.assertEqual(host.distro_family, "debian")
        self.assertEqual(host.get_package_manager(), "deb")

    def test_bare_debian_maps_to_debian_family(self) -> None:
        # GNU/Linux is removed from NAME while preserving the Debian family.
        self.assertEqual(
            self.parse("Debian GNU/Linux", "12"), ("linux", "debian", "12")
        )
        host = HostType("linux", "debian", "12")
        self.assertEqual(host.distro_family, "debian")
        self.assertEqual(host.get_package_manager(), "deb")

    def test_kali(self) -> None:
        data = """\
NAME="Kali GNU/Linux"
VERSION_ID="2026.3"
ID=kali
ID_LIKE=debian
"""
        self.assertEqual(
            parse_os_release(data),
            ("linux", "kali", "2026.3"),
        )
        host = HostType("linux", "kali", "2026.3")
        self.assertEqual(host.distro_family, "debian")
        self.assertEqual(host.get_package_manager(), "deb")

    def test_name_preserves_existing_identifiers(self) -> None:
        for name, distro_id, expected, version, family, manager in (
            ("Pop!_OS", "pop", "pop!_os", "22.04", "debian", "deb"),
            ("CentOS Stream", "centos", "centos_stream", "9", "rhel", "rpm"),
        ):
            with self.subTest(name=name):
                data = f'NAME="{name}"\nID={distro_id}\nVERSION_ID="{version}"\n'
                result = parse_os_release(data)
                self.assertEqual(result, ("linux", expected, version))
                host = HostType(*result)
                self.assertEqual(host.distro_family, family)
                self.assertEqual(host.get_package_manager(), manager)

    def test_unknown_distro_has_no_family(self) -> None:
        host = HostType("linux", "gentoo", None)
        self.assertIsNone(host.distro_family)
        self.assertIsNone(host.get_package_manager())
