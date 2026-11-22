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

from typing import List

from folly.iobuf import IOBuf, WritableIOBuf

def get_empty_chain() -> IOBuf: ...
def get_empty_writable_chain() -> WritableIOBuf: ...
def make_chain(data: List[IOBuf]) -> IOBuf: ...
def make_writable_chain(data: List[WritableIOBuf]) -> WritableIOBuf: ...
def to_uppercase_string(iobuf: object) -> str: ...
def to_uppercase_string_heap(iobuf: object) -> str: ...
def wrap_and_delayed_free(mv: memoryview, free_delay_ms: int) -> None: ...
