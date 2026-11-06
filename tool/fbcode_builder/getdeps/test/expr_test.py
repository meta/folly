# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import unittest

from ..expr import parse_expr


class ExprTest(unittest.TestCase):
    def test_equal(self) -> None:
        valid_variables = {"foo", "some_var", "another_var"}
        e = parse_expr("foo=bar", valid_variables)
        self.assertTrue(e.eval({"foo": "bar"}))
        self.assertFalse(e.eval({"foo": "not-bar"}))
        self.assertFalse(e.eval({"not-foo": "bar"}))

    def test_not_equal(self) -> None:
        valid_variables = {"foo"}
        e = parse_expr("not(foo=bar)", valid_variables)
        self.assertFalse(e.eval({"foo": "bar"}))
        self.assertTrue(e.eval({"foo": "not-bar"}))

    def test_bad_not(self) -> None:
        valid_variables = {"foo"}
        with self.assertRaises(Exception):
            parse_expr("foo=not(bar)", valid_variables)

    def test_bad_variable(self) -> None:
        valid_variables = {"bar"}
        with self.assertRaises(Exception):
            parse_expr("foo=bar", valid_variables)

    def test_all(self) -> None:
        valid_variables = {"foo", "baz"}
        e = parse_expr("all(foo = bar, baz = qux)", valid_variables)
        self.assertTrue(e.eval({"foo": "bar", "baz": "qux"}))
        self.assertFalse(e.eval({"foo": "bar", "baz": "nope"}))
        self.assertFalse(e.eval({"foo": "nope", "baz": "nope"}))

    def test_any(self) -> None:
        valid_variables = {"foo", "baz"}
        e = parse_expr("any(foo = bar, baz = qux)", valid_variables)
        self.assertTrue(e.eval({"foo": "bar", "baz": "qux"}))
        self.assertTrue(e.eval({"foo": "bar", "baz": "nope"}))
        self.assertFalse(e.eval({"foo": "nope", "baz": "nope"}))
