#!/usr/bin/env python3
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

# pyre-unsafe

import array
import struct
import unittest

from folly.iobuf import IOBuf, WritableIOBuf
from folly.python.test.iobuf_helper import (
    get_empty_chain,
    get_empty_writable_chain,
    make_chain,
    to_uppercase_string,
    to_uppercase_string_heap,
    wrap_and_delayed_free,
)


class IOBufTests(unittest.TestCase):
    def test_empty_chain(self) -> None:
        ebuf = get_empty_chain()
        self.assertFalse(ebuf)
        self.assertTrue(ebuf.is_chained)
        self.assertEqual(len(ebuf), 0)
        self.assertEqual(ebuf.chain_size(), 0)
        self.assertEqual(ebuf.chain_count(), 8)
        self.assertEqual(b"".join(ebuf), b"")
        self.assertEqual(ebuf.chain_bytes(), b"")
        self.assertEqual(b"", bytes(ebuf))

    def test_chain(self) -> None:
        control = [b"facebook\x00", b"", bytes(range(256)), b"cython"]
        chain = make_chain([IOBuf(x) for x in control])
        self.assertTrue(chain.is_chained)
        self.assertTrue(chain)
        self.assertEqual(bytes(chain), control[0])
        self.assertEqual(len(chain), len(control[0]))
        self.assertEqual(chain.chain_size(), sum(len(x) for x in control))
        self.assertEqual(chain.chain_count(), len(control))
        # pyre-fixme[6]: For 1st argument expected `Buffer` but got `Optional[IOBuf]`.
        self.assertEqual(memoryview(chain.next), control[1])
        self.assertEqual(b"".join(chain), b"".join(control))
        self.assertEqual(chain.chain_bytes(), b"".join(control))

    def test_cyclic_chain(self) -> None:
        control = [b"aaa", b"aaaa"]
        chain = make_chain([IOBuf(x) for x in control])
        self.assertTrue(chain.is_chained)
        self.assertTrue(chain)
        self.assertEqual(bytes(chain), control[0])
        self.assertEqual(len(chain), len(control[0]))
        self.assertEqual(chain.chain_size(), sum(len(x) for x in control))
        self.assertEqual(chain.chain_count(), len(control))
        # pyre-fixme[6]: For 1st argument expected `Buffer` but got `Optional[IOBuf]`.
        self.assertEqual(memoryview(chain.next), control[1])
        self.assertEqual(b"".join(chain), b"".join(control))
        self.assertEqual(chain.chain_bytes(), b"".join(control))

    def test_hash(self) -> None:
        x = b"omg"
        y = b"wtf"
        xb = IOBuf(x)
        yb = IOBuf(y)
        hash(xb)
        self.assertNotEqual(hash(xb), hash(yb))
        self.assertEqual(hash(xb), hash(IOBuf(x)))

    def test_empty(self) -> None:
        x = b""
        xb = IOBuf(x)
        self.assertEqual(memoryview(xb), x)
        self.assertEqual(bytes(xb), x)
        self.assertFalse(xb)
        self.assertEqual(len(xb), len(x))

    def test_iter(self) -> None:
        x = b"testtest"
        xb = IOBuf(x)
        self.assertEqual(b"".join(iter(xb)), x)

    def test_bytes(self) -> None:
        x = b"omg\x00wtfbbq"
        xb = IOBuf(x)
        self.assertEqual(bytes(xb), x)
        self.assertEqual(xb.chain_bytes(), x)

    def test_cmp(self) -> None:
        x = IOBuf(b"abc")
        y = IOBuf(b"def")
        z = IOBuf(b"abc")
        self.assertEqual(x, z)
        self.assertNotEqual(x, y)
        self.assertLess(x, y)
        self.assertLessEqual(x, y)
        self.assertLessEqual(x, z)
        self.assertGreater(y, x)
        self.assertGreaterEqual(y, x)

    def test_typed(self) -> None:
        # pyre-fixme[6]: Expected `Union[IOBuf, bytearray, bytes, memoryview]` for
        #  1st param but got `array[int]`.
        x = IOBuf(array.array("l", [1, 2, 3, 4, 5]))
        self.assertEqual(x.chain_size(), 5 * struct.calcsize("l"))

    def test_unshaped(self) -> None:
        x = IOBuf(memoryview(b"a").cast("B", shape=[]))
        self.assertEqual(x.chain_size(), 1)

    def test_multidimensional(self) -> None:
        x = IOBuf(memoryview(b"abcdef").cast("B", shape=[3, 2]))
        self.assertEqual(x.chain_size(), 6)

    def test_conversion_from_python_to_cpp(self) -> None:
        iobuf = make_chain(
            [
                IOBuf(memoryview(b"abc")),
                IOBuf(memoryview(b"def")),
                IOBuf(memoryview(b"ghi")),
            ]
        )
        self.assertEqual(to_uppercase_string(iobuf), "ABCDEFGHI")
        self.assertEqual(to_uppercase_string_heap(iobuf), "ABCDEFGHI")

    def test_conversion_from_python_to_cpp_with_wrong_type(self) -> None:
        not_an_iobuf = [1, 2, 3]
        with self.assertRaises(TypeError):
            to_uppercase_string(not_an_iobuf)
        with self.assertRaises(TypeError):
            to_uppercase_string_heap(not_an_iobuf)

    def test_buffer_read_fail(self) -> None:
        finish = bytearray(b"1234567890123456")
        xb = IOBuf(finish)
        buf = memoryview(xb)

        try:
            self.assertEqual(bytes(buf[17]), None)
            self.fail("Expected exception for reading out of bounds")
        except IndexError as e:
            self.assertEqual(
                str(e),
                "index out of bounds on dimension 1",
            )

    def test_delayed_free_in_cpp(self) -> None:
        """
        Delays "giving back" the reference to memory view to Python, so that
        folly::IOBuf free function is executed after this test is done.
        What happens is that https://fburl.com/code/t38bo0vg is running on a
        C++ thread while Python interpreter is shutting down.
        I suspect that it causes https://fburl.com/code/zip8hjif check to succed,
        so that every thread thinks that it's holding GIL.
        """
        data = b"1234567890"
        wrap_and_delayed_free(mv=memoryview(data), free_delay_ms=1000)


class WritableIOBufTests(unittest.TestCase):
    def test_bytes_writable(self) -> None:
        x = bytearray(b"omgwtfbbq")
        xb = WritableIOBuf(x)
        self.assertEqual(bytes(xb), x)
        self.assertEqual(xb.writable(), True)

    def test_buffer_overwrite(self) -> None:
        start = b"omgwtfbbq"
        finish = b"123456789"
        x = bytearray(start)
        xb = WritableIOBuf(x)
        buf = memoryview(xb)

        self.assertEqual(bytes(buf), start)
        buf[:] = finish
        self.assertEqual(bytes(buf), finish)

        self.assertNotEqual(bytes(buf), start)

    def test_buffer_write_empty(self) -> None:
        start = bytearray(9)
        finish = b"123456789"
        xb = WritableIOBuf(bytearray(9))
        buf = memoryview(xb)

        self.assertEqual(bytes(buf), start)
        buf[:] = finish
        self.assertEqual(bytes(buf), finish)
        self.assertEqual(memoryview(xb), finish)

        self.assertNotEqual(bytes(buf), start)

    def test_buffer_update_in_place(self) -> None:
        x = bytearray(b"123")
        xb = WritableIOBuf(x)
        memoryview(xb)[:] = b"456"
        self.assertEqual(x, b"456")  # xb wrapped x, so by mutating xb we mutated x

    def test_buffer_write_out_of_bounds(self) -> None:
        start = bytearray(9)
        finish = b"1234567890"
        xb = WritableIOBuf(bytearray(9))
        buf = memoryview(xb)

        self.assertEqual(bytes(buf), start)
        try:
            buf[:] = finish
            self.fail("Expected exception for writing out of bounds")
        except ValueError as e:
            self.assertEqual(
                str(e),
                "memoryview assignment: lvalue and rvalue have different structures",
            )
        self.assertEqual(bytes(buf), start)

    def test_buffer_read_out_of_bounds(self) -> None:
        start = bytearray(b"1234567890")
        xb = WritableIOBuf(start)
        buf = memoryview(xb)

        self.assertEqual(bytes(buf), start)
        try:
            self.assertEqual(bytes(buf[10]), None)
            self.fail("Expected exception for reading out of bounds")
        except IndexError as e:
            self.assertEqual(
                str(e),
                "index out of bounds on dimension 1",
            )

    def test_buffer_write_empty_pieces(self) -> None:
        start = bytearray(9)
        builder = bytearray(9)
        piece_1 = b"123"
        piece_2 = b"456"
        piece_3 = b"789"
        xb = WritableIOBuf(start)

        buf = memoryview(xb)
        buf[0:3] = piece_1
        for i in range(3):
            builder[i] = piece_1[i]
        self.assertEqual(bytes(buf), builder)

        buf = memoryview(xb)
        buf[3:6] = piece_2
        for i in range(3):
            builder[i + 3] = piece_2[i]
        self.assertEqual(bytes(buf), builder)

        buf = memoryview(xb)
        buf[6:9] = piece_3
        for i in range(3):
            builder[i + 6] = piece_3[i]
        self.assertEqual(bytes(buf), builder)

    def test_empty_writable_chain(self) -> None:
        ebuf = get_empty_writable_chain()
        self.assertFalse(ebuf)
        self.assertTrue(ebuf.is_chained)
        self.assertEqual(len(ebuf), 0)
        self.assertEqual(ebuf.chain_size(), 0)
        self.assertEqual(ebuf.chain_count(), 8)
        self.assertEqual(b"".join(ebuf), b"")
        self.assertEqual(b"", bytes(ebuf))

    def test_appendable_writable_chain(self) -> None:
        x = bytearray(b"omgwtfbbq")
        xb = WritableIOBuf(x)
        self.assertFalse(xb.is_chained)

        y = bytearray(b"wtfbbqomg")
        yb = WritableIOBuf(y)
        xb.append_to_chain(yb)

        self.assertTrue(xb.is_chained)
        self.assertEqual(len(xb), 9)
        self.assertEqual(xb.chain_size(), 18)
        self.assertEqual(xb.chain_count(), 2)
        self.assertEqual(b"".join(xb), b"".join([x, y]))

    def test_appendable_writable_chain_overwrite(self) -> None:
        start = bytearray(9)
        x = bytearray(b"omgwtfbbq")
        for i in range(9):
            start[i] = x[i]

        xb = WritableIOBuf(x)
        self.assertFalse(xb.is_chained)

        y = bytearray(b"wtfbbqomg")
        yb = WritableIOBuf(y)
        xb.append_to_chain(yb)

        finish = b"123456789"
        buf = memoryview(xb)
        buf[:] = finish

        self.assertTrue(xb.is_chained)
        self.assertEqual(len(xb), 9)
        self.assertEqual(xb.chain_size(), 18)
        self.assertEqual(xb.chain_count(), 2)
        self.assertEqual(b"".join(xb), b"".join([x, y]))
        self.assertEqual(bytes(buf), x)

        self.assertNotEqual(b"".join(xb), b"".join([start, y]))

    def test_appendable_writable_chain_coalesce(self) -> None:
        x = bytearray(b"omgwtfbbq")
        xb = WritableIOBuf(x)
        self.assertFalse(xb.is_chained)

        y = bytearray(b"wtfbbqomg")
        yb = WritableIOBuf(y)
        xb.append_to_chain(yb)

        xb.coalesce()
        self.assertFalse(xb.is_chained)
        self.assertEqual(len(xb), 18)
        self.assertEqual(xb.chain_size(), 18)
        self.assertEqual(xb.chain_count(), 1)

        buf = memoryview(xb)
        self.assertEqual(bytes(buf), b"".join([x, y]))

    def test_appendable_writable_chain_coalesce_exception(self) -> None:
        x = bytearray(b"omgwtfbbq")
        xb = WritableIOBuf(x)
        self.assertFalse(xb.is_chained)

        y = bytearray(b"wtfbbqomg")
        yb = WritableIOBuf(y)
        xb.append_to_chain(yb)
        alias = xb

        with self.assertRaisesRegex(
            RuntimeError, "Cannot coalesce IOBuf with more than one reference"
        ):
            xb.coalesce()
        self.assertIs(alias, xb)
        self.assertTrue(xb.is_chained)
        self.assertEqual(b"".join(xb), x + y)

        del alias
        xb.coalesce()
        self.assertFalse(xb.is_chained)
        self.assertEqual(bytes(xb), x + y)

    def test_coalesce_with_exported_buffer(self) -> None:
        x = bytearray(b"abc")
        y = bytearray(b"def")
        xb = WritableIOBuf(x)
        xb.append_to_chain(WritableIOBuf(y))
        view = memoryview(xb)

        with self.assertRaisesRegex(
            RuntimeError, "Cannot coalesce IOBuf with more than one reference"
        ):
            xb.coalesce()
        self.assertTrue(xb.is_chained)
        self.assertEqual(bytes(view), x)
        self.assertEqual(b"".join(xb), x + y)

        view.release()
        xb.coalesce()
        self.assertFalse(xb.is_chained)
        self.assertEqual(bytes(xb), x + y)

    def test_buffer_creation_with_size_write(self) -> None:
        finish = bytearray(b"123456789012345")
        xb = WritableIOBuf.create_unitialized(17)
        buf = memoryview(xb)

        self.assertEqual(len(xb), 0)
        self.assertEqual(len(buf), 0)
        self.assertEqual(bytes(buf), b"")
        self.assertEqual(xb.length(), 0)
        xb.append(15)
        buf = memoryview(xb)
        self.assertNotEqual(bytes(buf), b"")
        buf[0:15] = finish
        self.assertEqual(len(xb), 15)
        self.assertEqual(len(memoryview(xb)), 15)
        self.assertEqual(memoryview(xb), finish)
        self.assertEqual(buf, finish)
        self.assertEqual(xb.length(), 15)
        self.assertEqual(bytes(xb), b"123456789012345")

    def test_buffer_creation_with_size_write_fail(self) -> None:
        finish = bytearray(b"12345678901234567")
        xb = WritableIOBuf.create_unitialized(16)
        xb.append(16)
        buf = memoryview(xb)

        self.assertEqual(xb.capacity(), 16)
        try:
            buf[:] = finish
            self.fail("Expected exception for writing out of bounds")
        except ValueError as e:
            self.assertEqual(
                str(e),
                "memoryview assignment: lvalue and rvalue have different structures",
            )

    def test_buffer_creation_with_size_read_fail(self) -> None:
        finish = bytearray(b"1234567890123456")
        xb = WritableIOBuf.create_unitialized(16)
        xb.append(16)
        buf = memoryview(xb)

        buf[:] = finish
        try:
            self.assertEqual(bytes(buf[17]), None)
            self.fail("Expected exception for reading out of bounds")
        except IndexError as e:
            self.assertEqual(
                str(e),
                "index out of bounds on dimension 1",
            )

    def test_buffer_creation_with_size_append(self) -> None:
        xb = WritableIOBuf.create_unitialized(16)
        self.assertEqual(xb.length(), 0)
        self.assertEqual(xb.capacity(), 16)
        xb.append(16)
        self.assertEqual(xb.length(), 16)

    def test_buffer_creation_with_size_append_fail(self) -> None:
        xb = WritableIOBuf.create_unitialized(16)
        self.assertEqual(xb.length(), 0)
        self.assertEqual(xb.capacity(), 16)
        try:
            xb.append(17)
            self.fail("Expected exception for appending more than capacity")
        except ValueError as e:
            self.assertEqual(str(e), "Cannot append more than capacity")

    def test_buffer_creation_with_size_append_fail_negative(self) -> None:
        xb = WritableIOBuf.create_unitialized(16)
        self.assertEqual(xb.length(), 0)
        self.assertEqual(xb.capacity(), 16)
        try:
            xb.append(-1)
            self.fail("Expected exception for negative amount")
        except ValueError as e:
            self.assertEqual(str(e), "Cannot append, amount must be positive")

    def test_buffer_creation_with_size_multiple_append(self) -> None:
        xb = WritableIOBuf.create_unitialized(16)
        self.assertEqual(xb.length(), 0)
        self.assertEqual(xb.capacity(), 16)
        xb.append(15)
        xb.append(1)
        self.assertEqual(xb.length(), 16)

    def test_buffer_creation_with_size_multiple_append_fail(self) -> None:
        xb = WritableIOBuf.create_unitialized(16)
        self.assertEqual(xb.length(), 0)
        self.assertEqual(xb.capacity(), 16)
        xb.append(16)
        try:
            xb.append(1)
            self.fail("Expected exception for appending more than capacity")
        except ValueError as e:
            self.assertEqual(str(e), "Cannot append more than capacity")

    def test_buffer_creation_with_size_trim_start(self) -> None:
        xb = WritableIOBuf.create_unitialized(16)
        self.assertEqual(xb.length(), 0)
        self.assertEqual(xb.capacity(), 16)
        xb.append(5)
        self.assertEqual(xb.length(), 5)
        xb.trim_start(5)
        self.assertEqual(xb.length(), 0)

    def test_buffer_creation_with_size_write_trim_start(self) -> None:
        finish = bytearray(b"1234567890123456")
        xb = WritableIOBuf.create_unitialized(16)
        xb.append(16)
        buf = memoryview(xb)

        buf[0:16] = finish
        self.assertEqual(memoryview(xb), finish)
        self.assertEqual(buf, finish)
        self.assertEqual(xb.length(), 16)
        self.assertEqual(bytes(xb), b"1234567890123456")
        xb.trim_start(6)
        self.assertEqual(bytes(xb), b"7890123456")
        self.assertEqual(xb.length(), 10)
        xb.trim_start(6)
        self.assertEqual(bytes(xb), b"3456")
        self.assertEqual(xb.length(), 4)

    def test_buffer_creation_with_size_trim_start_fail(self) -> None:
        xb = WritableIOBuf.create_unitialized(16)
        self.assertEqual(xb.length(), 0)
        self.assertEqual(xb.capacity(), 16)
        xb.append(5)
        self.assertEqual(xb.length(), 5)
        try:
            xb.trim_start(6)
            self.fail("Expected exception for trimming more than length")
        except ValueError as e:
            self.assertEqual(str(e), "Cannot trim more than length")

    def test_buffer_creation_with_size_trim_start_multiple(self) -> None:
        xb = WritableIOBuf.create_unitialized(16)
        self.assertEqual(xb.length(), 0)
        self.assertEqual(xb.capacity(), 16)
        xb.append(5)
        self.assertEqual(xb.length(), 5)
        xb.trim_start(4)
        xb.trim_start(1)
        self.assertEqual(xb.length(), 0)

    def test_buffer_creation_with_size_trim_start_fail_multiple(self) -> None:
        xb = WritableIOBuf.create_unitialized(16)
        self.assertEqual(xb.length(), 0)
        self.assertEqual(xb.capacity(), 16)
        xb.append(5)
        self.assertEqual(xb.length(), 5)
        xb.trim_start(5)
        try:
            xb.trim_start(1)
            self.fail("Expected exception for trimming more than length")
        except ValueError as e:
            self.assertEqual(str(e), "Cannot trim more than length")

    def test_buffer_creation_with_size_trim_start_fail_negative(self) -> None:
        xb = WritableIOBuf.create_unitialized(16)
        self.assertEqual(xb.length(), 0)
        self.assertEqual(xb.capacity(), 16)

        try:
            xb.trim_start(-1)
            self.fail("Expected exception for negative amount")
        except ValueError as e:
            self.assertEqual(str(e), "Cannot trim start, amount must be positive")

    def test_buffer_creation_with_size_trim_end(self) -> None:
        xb = WritableIOBuf.create_unitialized(16)
        self.assertEqual(xb.length(), 0)
        self.assertEqual(xb.capacity(), 16)
        xb.append(5)
        self.assertEqual(xb.length(), 5)
        xb.trim_end(5)
        self.assertEqual(xb.length(), 0)

    def test_buffer_creation_with_size_write_trim_end(self) -> None:
        finish = bytearray(b"1234567890123456")
        xb = WritableIOBuf.create_unitialized(16)
        xb.append(16)
        buf = memoryview(xb)

        buf[0:16] = finish
        self.assertEqual(memoryview(xb), finish)
        self.assertEqual(buf, finish)
        self.assertEqual(xb.length(), 16)
        self.assertEqual(bytes(xb), b"1234567890123456")
        xb.trim_end(6)
        self.assertEqual(bytes(xb), b"1234567890")
        self.assertEqual(xb.length(), 10)
        xb.trim_end(6)
        self.assertEqual(bytes(xb), b"1234")
        self.assertEqual(xb.length(), 4)

    def test_buffer_creation_with_size_trim_end_fail(self) -> None:
        xb = WritableIOBuf.create_unitialized(16)
        self.assertEqual(xb.length(), 0)
        self.assertEqual(xb.capacity(), 16)
        xb.append(5)
        self.assertEqual(xb.length(), 5)
        try:
            xb.trim_end(6)
            self.fail("Expected exception for trimming more than length")
        except ValueError as e:
            self.assertEqual(str(e), "Cannot trim more than length")

    def test_buffer_creation_with_size_trim_end_multiple(self) -> None:
        xb = WritableIOBuf.create_unitialized(16)
        self.assertEqual(xb.length(), 0)
        self.assertEqual(xb.capacity(), 16)
        xb.append(5)
        self.assertEqual(xb.length(), 5)
        xb.trim_end(4)
        xb.trim_end(1)
        self.assertEqual(xb.length(), 0)

    def test_buffer_creation_with_size_trim_end_fail_multiple(self) -> None:
        xb = WritableIOBuf.create_unitialized(16)
        self.assertEqual(xb.length(), 0)
        self.assertEqual(xb.capacity(), 16)
        xb.append(5)
        self.assertEqual(xb.length(), 5)
        xb.trim_end(5)
        try:
            xb.trim_end(1)
            self.fail("Expected exception for trimming more than length")
        except ValueError as e:
            self.assertEqual(str(e), "Cannot trim more than length")

    def test_buffer_creation_with_size_trim_end_fail_negative(self) -> None:
        xb = WritableIOBuf.create_unitialized(16)
        self.assertEqual(xb.length(), 0)
        self.assertEqual(xb.capacity(), 16)

        try:
            xb.trim_end(-1)
            self.fail("Expected exception for negative amount")
        except ValueError as e:
            self.assertEqual(str(e), "Cannot trim end, amount must be positive")

    def test_buffer_creation_with_size_prepend(self) -> None:
        xb = WritableIOBuf.create_unitialized(16)
        self.assertEqual(xb.length(), 0)
        self.assertEqual(xb.capacity(), 16)
        xb.append(5)
        self.assertEqual(xb.length(), 5)
        xb.trim_start(5)
        self.assertEqual(xb.length(), 0)
        xb.prepend(5)
        self.assertEqual(xb.length(), 5)

    def test_buffer_creation_with_size_write_prepend(self) -> None:
        finish = bytearray(b"1234567890123456")
        xb = WritableIOBuf.create_unitialized(16)
        xb.append(16)
        buf = memoryview(xb)

        buf[0:16] = finish
        self.assertEqual(memoryview(xb), finish)
        self.assertEqual(buf, finish)
        self.assertEqual(xb.length(), 16)
        self.assertEqual(bytes(xb), b"1234567890123456")
        xb.trim_start(6)
        self.assertEqual(bytes(xb), b"7890123456")
        self.assertEqual(xb.length(), 10)
        xb.prepend(6)
        self.assertEqual(bytes(xb), b"1234567890123456")
        self.assertEqual(xb.length(), 16)

    def test_buffer_creation_with_size_prepend_fail(self) -> None:
        xb = WritableIOBuf.create_unitialized(16)
        self.assertEqual(xb.length(), 0)
        self.assertEqual(xb.capacity(), 16)
        xb.append(5)
        self.assertEqual(xb.length(), 5)
        xb.trim_start(5)
        self.assertEqual(xb.length(), 0)
        try:
            xb.prepend(6)
            self.fail("Expected exception for prepending more than headroom")
        except ValueError as e:
            self.assertEqual(str(e), "Cannot prepend more than headroom")

    def test_buffer_creation_with_size_prepend_fail_multiple(self) -> None:
        xb = WritableIOBuf.create_unitialized(16)
        self.assertEqual(xb.length(), 0)
        self.assertEqual(xb.capacity(), 16)
        xb.append(5)
        self.assertEqual(xb.length(), 5)
        xb.trim_start(5)
        self.assertEqual(xb.length(), 0)
        xb.prepend(5)
        try:
            xb.prepend(1)
            self.fail("Expected exception for prepending more than headroom")
        except ValueError as e:
            self.assertEqual(str(e), "Cannot prepend more than headroom")

    def test_buffer_creation_with_size_prepend_fail_negative(self) -> None:
        xb = WritableIOBuf.create_unitialized(16)
        self.assertEqual(xb.length(), 0)
        self.assertEqual(xb.capacity(), 16)

        try:
            xb.prepend(-1)
            self.fail("Expected exception for negative amount")
        except ValueError as e:
            self.assertEqual(str(e), "Cannot prepend, amount must be positive")
