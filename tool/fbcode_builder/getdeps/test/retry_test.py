# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import unittest
from unittest.mock import call, MagicMock, patch

from ..buildopts import BuildOptions
from ..errors import TransientFailure
from ..fetcher import ArchiveFetcher
from ..manifest import ManifestParser


class RetryTest(unittest.TestCase):
    def _get_build_opts(self) -> BuildOptions:
        mock_build_opts = MagicMock(spec=BuildOptions)
        mock_build_opts.scratch_dir = "/path/to/scratch_dir"
        return mock_build_opts

    def _get_manifest(self) -> ManifestParser:
        mock_manifest_parser = MagicMock(spec=ManifestParser)
        mock_manifest_parser.name = "mock_manifest_parser"
        return mock_manifest_parser

    def _get_archive_fetcher(self) -> ArchiveFetcher:
        return ArchiveFetcher(
            build_options=self._get_build_opts(),
            manifest=self._get_manifest(),
            url="https://github.com/systemd/systemd/archive/refs/tags/v256.7.tar.gz",
            sha256="896d76ff65c88f5fd9e42f90d152b0579049158a163431dd77cdc57748b1d7b0",
        )

    @patch("pathlib.Path.mkdir")
    @patch("os.environ.get")
    @patch("time.sleep")
    @patch("subprocess.run")
    def test_no_retries(
        self, mock_run, mock_sleep, mock_os_environ_get, mock_mkdir
    ) -> None:
        def custom_mkdir(*args, **kwargs):
            return None

        def custom_get(key, default=None):
            if key == "GETDEPS_USE_WGET":
                return "1"
            elif key == "GETDEPS_WGET_ARGS":
                return ""
            else:
                return None

        mock_mkdir.side_effect = custom_mkdir
        mock_os_environ_get.side_effect = custom_get
        mock_sleep.side_effect = None
        fetcher = self._get_archive_fetcher()
        fetcher._verify_hash = MagicMock(return_value=None)
        fetcher._download()
        mock_sleep.assert_has_calls([], any_order=False)
        mock_run.assert_called_once_with(
            [
                "wget",
                "-O",
                "/path/to/scratch_dir/downloads/mock_manifest_parser-v256.7.tar.gz",
                "https://github.com/systemd/systemd/archive/refs/tags/v256.7.tar.gz",
            ],
            capture_output=True,
        )

    @patch("random.random")
    @patch("pathlib.Path.mkdir")
    @patch("os.environ.get")
    @patch("time.sleep")
    @patch("subprocess.run")
    def test_retries(
        self, mock_run, mock_sleep, mock_os_environ_get, mock_mkdir, mock_random
    ) -> None:
        def custom_mkdir(*args, **kwargs):
            return None

        def custom_get(key, default=None):
            if key == "GETDEPS_USE_WGET":
                return "1"
            elif key == "GETDEPS_WGET_ARGS":
                return ""
            else:
                return None

        mock_random.return_value = 0

        mock_run.side_effect = [
            IOError("<urlopen error [Errno 104] Connection reset by peer>"),
            IOError("<urlopen error [Errno 104] Connection reset by peer>"),
            None,
        ]
        mock_mkdir.side_effect = custom_mkdir
        mock_os_environ_get.side_effect = custom_get
        mock_sleep.side_effect = None
        fetcher = self._get_archive_fetcher()
        fetcher._verify_hash = MagicMock(return_value=None)
        fetcher._download()
        mock_sleep.assert_has_calls([call(2), call(4)], any_order=False)
        calls = [
            call(
                [
                    "wget",
                    "-O",
                    "/path/to/scratch_dir/downloads/mock_manifest_parser-v256.7.tar.gz",
                    "https://github.com/systemd/systemd/archive/refs/tags/v256.7.tar.gz",
                ],
                capture_output=True,
            ),
        ] * 3

        mock_run.assert_has_calls(calls, any_order=False)

    @patch("random.random")
    @patch("pathlib.Path.mkdir")
    @patch("os.environ.get")
    @patch("time.sleep")
    @patch("subprocess.run")
    def test_all_retries(
        self, mock_run, mock_sleep, mock_os_environ_get, mock_mkdir, mock_random
    ) -> None:
        def custom_mkdir(*args, **kwargs):
            return None

        def custom_get(key, default=None):
            if key == "GETDEPS_USE_WGET":
                return "1"
            elif key == "GETDEPS_WGET_ARGS":
                return ""
            else:
                return None

        mock_random.return_value = 0

        mock_run.side_effect = IOError(
            "<urlopen error [Errno 104] Connection reset by peer>"
        )
        mock_mkdir.side_effect = custom_mkdir
        mock_os_environ_get.side_effect = custom_get
        mock_sleep.side_effect = None
        fetcher = self._get_archive_fetcher()
        fetcher._verify_hash = MagicMock(return_value=None)
        with self.assertRaises(TransientFailure):
            fetcher._download()
        mock_sleep.assert_has_calls(
            [call(2), call(4), call(8), call(10)], any_order=False
        )
        calls = [
            call(
                [
                    "wget",
                    "-O",
                    "/path/to/scratch_dir/downloads/mock_manifest_parser-v256.7.tar.gz",
                    "https://github.com/systemd/systemd/archive/refs/tags/v256.7.tar.gz",
                ],
                capture_output=True,
            ),
        ] * 5

        mock_run.assert_has_calls(calls, any_order=False)
