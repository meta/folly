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

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <folly/compression/Compression.h>
#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>
#include <folly/lang/Bits.h>

/**
 * Helper functions for compression codecs.
 */
namespace folly {
namespace compression {
namespace detail {

/**
 * Reads sizeof(T) bytes, and returns false if not enough bytes are available.
 * Returns true if the first n bytes are equal to prefix when interpreted as
 * a little endian T.
 */
template <typename T>
  requires std::is_unsigned_v<T>
bool dataStartsWithLE(const IOBuf* data, T prefix, uint64_t n = sizeof(T)) {
  DCHECK_GT(n, 0);
  DCHECK_LE(n, sizeof(T));
  T value;
  io::Cursor cursor{data};
  if (!cursor.tryReadLE(value)) {
    return false;
  }
  const T mask = n == sizeof(T) ? T(-1) : (T(1) << (8 * n)) - 1;
  return prefix == (value & mask);
}

template <typename T>
  requires std::is_arithmetic_v<T>
std::string prefixToStringLE(T prefix, uint64_t n = sizeof(T)) {
  DCHECK_GT(n, 0);
  DCHECK_LE(n, sizeof(T));
  prefix = Endian::little(prefix);
  std::string result;
  result.resize(n);
  memcpy(&result[0], &prefix, n);
  return result;
}

/**
 * Calls @p streamFn in a loop, and guarantees that @p streamFn is never called
 * with `input.size() > chunkSize` or `output.size() > chunkSize`. It behaves
 * as-if streamFn(input, output, flushOp) was called.
 *
 * This function relies only on the rules defined by
 * StreamCodec::compressStream() and StreamCodec::uncompressStream() for
 * correctness.
 */
template <typename StreamFn>
bool chunkedStream(
    size_t chunkSize,
    ByteRange& input,
    MutableByteRange& output,
    StreamCodec::FlushOp flushOp,
    StreamFn&& streamFn) {
  for (;;) {
    const auto chunkInputSize = std::min<size_t>(input.size(), chunkSize);
    const auto chunkOutputSize = std::min<size_t>(output.size(), chunkSize);
    auto chunkInput = input.subpiece(0, chunkInputSize);
    auto chunkOutput = output.subpiece(0, chunkOutputSize);

    // If we're presenting the entire suffix of the input in this call, use the
    // users flush op, otherwise use NONE.
    const StreamCodec::FlushOp chunkFlushOp =
        (chunkInput.size() == input.size())
        ? flushOp
        : StreamCodec::FlushOp::NONE;

    const bool finished = streamFn(chunkInput, chunkOutput, chunkFlushOp);

    // Update input / output buffers
    const size_t inputConsumed = chunkInputSize - chunkInput.size();
    const size_t outputProduced = chunkOutputSize - chunkOutput.size();
    input.advance(inputConsumed);
    output.advance(outputProduced);

    // If the underlying streaming function returns true, we want to forward
    // that to the caller.
    // Compression: If flushOp == NONE, this is guaranteed not to happen.
    // Decompression: This signals the end of a frame, and we must forward that
    //                signal to the caller without consuming any more input.
    if (finished) {
      return true;
    }

    // We've consumed the entire input, which means we fulfilled our as-if
    // guarantee.
    if (input.empty()) {
      DCHECK(!finished);
      return false;
    }

    // Compression: Presenting more input bytes guarantees that there must be
    //              more output bytes to produce. Therefore we've made maximal
    //              forward progress.
    // Decompression: The only flushOp that guarantees maximal forward progress
    //                is FLUSH. Its signal that the flush is complete is
    //                !output.empty(). So we can safely return if
    //                output.empty().
    if (output.empty()) {
      DCHECK(!input.empty() && !finished);
      return false;
    }

    // If we've failed to make forward progress, return to the caller.
    // The underlying streaming function is always required to make some forward
    // progress if any forward progress is possible. So forward progress must be
    // impossible.
    // This preserves StreamCodec's strong guarantee of forward progress, and
    // protects us from infinite loops.
    if (inputConsumed == 0 && outputProduced == 0) {
      DCHECK(!output.empty() && !input.empty() && !finished);
      return false;
    }
  }
}

// Some codecs use uint32_t for sizes so we need to limit the chunk size to 4GB.
// Rather than allow 4GB inputs / outputs, which nearly never happens, limit to
// 4MB, so we don't have complex logic that almost never runs.
constexpr size_t kDefaultChunkSizeFor32BitSizes = size_t(4) << 20;

inline uint64_t computeBufferLength(
    uint64_t const compressedLength, uint64_t const blockSize) {
  uint64_t constexpr kMaxBufferLength = uint64_t(4) << 20; // 4 MiB
  uint64_t const goodBufferSize = 4 * std::max(blockSize, compressedLength);
  return std::min(goodBufferSize, kMaxBufferLength);
}

inline std::unique_ptr<IOBuf> addOutputBuffer(
    MutableByteRange& output, uint64_t size) {
  DCHECK(output.empty());
  auto buffer = IOBuf::create(size);
  buffer->append(buffer->capacity());
  output = {buffer->writableData(), buffer->length()};
  return buffer;
}

template <typename Output>
void appendOutputBuffer(Output& output, std::unique_ptr<IOBuf> buffer) {
  if constexpr (std::is_same_v<Output, std::unique_ptr<IOBuf>>) {
    if (output) {
      output->prependChain(std::move(buffer));
    } else {
      output = std::move(buffer);
    }
  } else if (!buffer->empty()) {
    output.append(std::move(buffer));
  }
}

template <typename Cursor, typename Output>
void uncompressStream(
    StreamCodec& codec,
    Cursor& cursor,
    uint64_t compressedLength,
    Optional<uint64_t> uncompressedLength,
    Output& result) {
  auto constexpr kMaxSingleStepLength = uint64_t(64) << 20; // 64 MB
  auto constexpr kBlockSize = uint64_t(128) << 10;
  auto const defaultBufferLength =
      computeBufferLength(compressedLength, kBlockSize);

  codec.resetStream(uncompressedLength);

  MutableByteRange output;
  auto buffer = addOutputBuffer(
      output,
      uncompressedLength && *uncompressedLength <= kMaxSingleStepLength
          ? *uncompressedLength
          : defaultBufferLength);

  ByteRange input;
  StreamCodec::FlushOp flushOp = StreamCodec::FlushOp::NONE;
  bool done = false;
  while (!done) {
    input = cursor.peekBytes();
    const auto inputLength = input.size();
    if (cursor.getCurrentPosition() + inputLength == compressedLength) {
      flushOp = StreamCodec::FlushOp::END;
    }
    if (output.empty()) {
      appendOutputBuffer(result, std::move(buffer));
      buffer = addOutputBuffer(output, defaultBufferLength);
    }
    done = codec.uncompressStream(input, output, flushOp);
    cursor.skipNoAdvance(inputLength - input.size());
  }
  if (!input.empty()) {
    throw std::runtime_error("Codec: Junk after end of data");
  }

  buffer->trimEnd(output.size());
  appendOutputBuffer(result, std::move(buffer));
}

} // namespace detail
} // namespace compression
} // namespace folly
