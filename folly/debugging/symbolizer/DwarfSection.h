/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cstdint>
#include <span>

#include <folly/Optional.h>
#include <folly/debugging/symbolizer/DwarfCursor.h>
#include <folly/portability/Config.h>

#if FOLLY_HAVE_DWARF && FOLLY_HAVE_ELF

namespace folly {
namespace symbolizer {

/**
 * DWARF section made up of chunks, each prefixed with a length header. The
 * length indicates whether the chunk is DWARF-32 or DWARF-64, which guides
 * interpretation of "section offset" records. (yes, DWARF-32 and DWARF-64
 * sections may coexist in the same file).
 */
class DwarfSection {
 public:
  DwarfSection() : is64Bit_(false) {}

  explicit DwarfSection(std::span<const uint8_t> data);

  /**
   * Return next chunk, if any; the 4- or 12-byte length was already
   * parsed and isn't part of the chunk.
   */
  DwarfParseResult<folly::Optional<std::span<const uint8_t>>> next();

  /** Is the current chunk 64 bit? */
  bool is64Bit() const { return is64Bit_; }

 private:
  // Yes, 32- and 64- bit sections may coexist.  Yikes!
  bool is64Bit_;
  std::span<const uint8_t> data_;
  size_t offset_ = 0;
};

} // namespace symbolizer
} // namespace folly

#endif
