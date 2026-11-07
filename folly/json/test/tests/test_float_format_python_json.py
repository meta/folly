# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# pyre-strict

from __future__ import annotations

import json
import unittest

from ..folly_json_serialize import FloatFormat, serialize_double

# Doubles whose shortest repr has an integer mantissa in scientific notation.
# fmt's {:#} alternate form produces "3.e-06" for these — RFC 8259 §6 requires
# at least one digit after the decimal, so Python's json.loads() rejects them.
_SCIENTIFIC_INTEGER_MANTISSA: tuple[float, ...] = (3e-6, -3e-6, 1e20, 1e100, 2e10)

# Ordinary fractional values: the significand already contains a digit after the
# dot, so all modes handle these correctly.
_FRACTIONAL: tuple[float, ...] = (1.5, 4.1, -2.7, 1.5e-6)

# Integer-valued doubles: test that *_TRAILING_DOT_ZERO modes emit "45.0", not "45".
_INTEGER_VALUED: tuple[float, ...] = (0.0, 1.0, 45.0, -1.0, 1000.0)


def _assert_parseable(
    tc: unittest.TestCase,
    fmt: FloatFormat,
    num_digits: int,
    values: tuple[float, ...],
) -> None:
    for v in values:
        with tc.subTest(v=v):
            serialized = serialize_double(v, fmt, num_digits)
            try:
                json.loads(serialized)
            except json.JSONDecodeError as e:
                tc.fail(
                    f"json.loads({serialized!r}) failed for FloatFormat.{fmt.name}"
                    f" with value {v!r}: {e}"
                )


class ShortestJsonValidityTest(unittest.TestCase):
    """SHORTEST: valid JSON for all inputs; integers have no trailing dot (fine by RFC 8259)."""

    def test_fractional_values(self) -> None:
        _assert_parseable(self, FloatFormat.SHORTEST, 0, _FRACTIONAL)

    def test_integer_valued_doubles(self) -> None:
        _assert_parseable(self, FloatFormat.SHORTEST, 0, _INTEGER_VALUED)

    def test_scientific_integer_mantissa(self) -> None:
        _assert_parseable(self, FloatFormat.SHORTEST, 0, _SCIENTIFIC_INTEGER_MANTISSA)


class ShortestTrailingDotZeroJsonValidityTest(unittest.TestCase):
    """SHORTEST_TRAILING_DOT_ZERO must always emit at least one fractional digit.

    The {:#} fmt alternate form satisfies this for ordinary values but emits
    "3.e-06" (no digit after the dot) for any double whose shortest
    representation has an integer mantissa in scientific notation.  Python's
    json.loads() — and the RFC 8259 grammar — both reject that output.
    """

    def test_fractional_values(self) -> None:
        _assert_parseable(self, FloatFormat.SHORTEST_TRAILING_DOT_ZERO, 0, _FRACTIONAL)

    def test_integer_valued_doubles(self) -> None:
        _assert_parseable(
            self, FloatFormat.SHORTEST_TRAILING_DOT_ZERO, 0, _INTEGER_VALUED
        )

    def test_scientific_integer_mantissa(self) -> None:
        # This test FAILS with the {:#} implementation (produces "3.e-06").
        # A correct implementation must insert "0" after the dot, giving "3.0e-06".
        _assert_parseable(
            self,
            FloatFormat.SHORTEST_TRAILING_DOT_ZERO,
            0,
            _SCIENTIFIC_INTEGER_MANTISSA,
        )


class ShortestSingleTrailingDotZeroJsonValidityTest(unittest.TestCase):
    """SHORTEST_SINGLE_TRAILING_DOT_ZERO has the same {:#} footgun as the double variant."""

    def test_fractional_values(self) -> None:
        _assert_parseable(
            self, FloatFormat.SHORTEST_SINGLE_TRAILING_DOT_ZERO, 0, _FRACTIONAL
        )

    def test_scientific_integer_mantissa(self) -> None:
        _assert_parseable(
            self,
            FloatFormat.SHORTEST_SINGLE_TRAILING_DOT_ZERO,
            0,
            _SCIENTIFIC_INTEGER_MANTISSA,
        )


class FixedJsonValidityTest(unittest.TestCase):
    """FIXED: always fixed-point notation, always a decimal point — valid JSON for all inputs."""

    def test_scientific_integer_mantissa(self) -> None:
        _assert_parseable(self, FloatFormat.FIXED, 6, _SCIENTIFIC_INTEGER_MANTISSA)

    def test_integer_valued_doubles(self) -> None:
        _assert_parseable(self, FloatFormat.FIXED, 6, _INTEGER_VALUED)

    def test_fractional_values(self) -> None:
        _assert_parseable(self, FloatFormat.FIXED, 6, _FRACTIONAL)


class GeneralJsonValidityTest(unittest.TestCase):
    """GENERAL: %g semantics, strips trailing zeros — valid JSON for all inputs."""

    def test_scientific_integer_mantissa(self) -> None:
        _assert_parseable(self, FloatFormat.GENERAL, 6, _SCIENTIFIC_INTEGER_MANTISSA)

    def test_integer_valued_doubles(self) -> None:
        _assert_parseable(self, FloatFormat.GENERAL, 6, _INTEGER_VALUED)

    def test_fractional_values(self) -> None:
        _assert_parseable(self, FloatFormat.GENERAL, 6, _FRACTIONAL)


class ShortestTrailingDotZeroExactOutputTest(unittest.TestCase):
    """Spot-check exact serialized strings for the integer-mantissa edge cases."""

    def test_scientific_integer_mantissa_has_fractional_digit(self) -> None:
        # A correct SHORTEST_TRAILING_DOT_ZERO must insert ".0" before the exponent.
        self.assertEqual(
            serialize_double(3e-6, FloatFormat.SHORTEST_TRAILING_DOT_ZERO), "3.0e-06"
        )
        self.assertEqual(
            serialize_double(1e20, FloatFormat.SHORTEST_TRAILING_DOT_ZERO), "1.0e+20"
        )
        self.assertEqual(
            serialize_double(-3e-6, FloatFormat.SHORTEST_TRAILING_DOT_ZERO), "-3.0e-06"
        )

    def test_integer_valued_doubles_have_trailing_dot_zero(self) -> None:
        self.assertEqual(
            serialize_double(45.0, FloatFormat.SHORTEST_TRAILING_DOT_ZERO), "45.0"
        )
        self.assertEqual(
            serialize_double(0.0, FloatFormat.SHORTEST_TRAILING_DOT_ZERO), "0.0"
        )

    def test_already_fractional_values_are_unchanged(self) -> None:
        # Values that already have a digit after the dot must not be modified.
        self.assertEqual(
            serialize_double(1.5, FloatFormat.SHORTEST_TRAILING_DOT_ZERO), "1.5"
        )
        self.assertEqual(
            serialize_double(1.5e-6, FloatFormat.SHORTEST_TRAILING_DOT_ZERO), "1.5e-06"
        )
