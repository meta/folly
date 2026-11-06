# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


from __future__ import annotations

import argparse
from collections.abc import Callable


class SubCmd:
    NAME: str | None = None
    HELP: str | None = None

    def run(self, args: argparse.Namespace) -> int:
        """perform the command"""
        return 0

    def setup_parser(self, parser: argparse.ArgumentParser) -> None:
        # Subclasses should override setup_parser() if they have any
        # command line options or arguments.
        pass


CmdTable: list[type[SubCmd]] = []


def add_subcommands(
    parser: argparse._SubParsersAction[argparse.ArgumentParser],
    common_args: argparse.ArgumentParser,
    cmd_table: list[type[SubCmd]] = CmdTable,
) -> None:
    """Register parsers for the defined commands with the provided parser"""
    for cls in cmd_table:
        command = cls()
        command_parser = parser.add_parser(
            # pyre-fixme[6]: For 1st argument expected `str` but got `Optional[str]`.
            command.NAME,
            help=command.HELP,
            parents=[common_args],
        )
        command.setup_parser(command_parser)
        command_parser.set_defaults(func=command.run)


def cmd(
    name: str,
    help: str | None = None,
    cmd_table: list[type[SubCmd]] = CmdTable,
) -> Callable[[type[SubCmd]], type[SubCmd]]:
    """
    @cmd() is a decorator that can be used to help define Subcmd instances

    Example usage:

        @subcmd('list', 'Show the result list')
        class ListCmd(Subcmd):
            def run(self, args):
                # Perform the command actions here...
                pass
    """

    def wrapper(cls: type[SubCmd]) -> type[SubCmd]:
        class SubclassedCmd(cls):
            NAME = name
            HELP = help

        #  `Type[SubclassedCmd]`.
        cmd_table.append(SubclassedCmd)
        return SubclassedCmd

    return wrapper
