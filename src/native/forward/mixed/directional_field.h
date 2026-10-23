#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <vector>

namespace remix::forward::mixed::directional {
  using Field = std::array<uint16_t, 9>; // signed cosine-convolved unit-direct SH
  constexpr uint32_t raw = 0, tiled = 1, tiledSmall = 2, tileSide = 8;
  struct Encoded {
    uint32_t encoding = raw;
    std::vector<uint32_t> words;
  };
  // Lossless relative to finite half input. An 8x8 or 4x4 block is black, constant,
  // or raw. Descriptors use their high two bits for the mode and low 30 bits
  // for a chunk-relative word offset. Raw whole images win storage ties.
  // A word budget throws length_error as soon as neither layout can fit.
  Encoded encode(std::span<const Field> fields, uint32_t width, uint32_t height,
                 std::stop_token cancelled = {}, uint64_t maximumWords = ~uint64_t(0));
  // Validate one canonical chunk and return its occupied word count. The span
  // may include later chunks; validation never reads outside this chunk.
  size_t validate(std::span<const uint32_t> words, uint32_t width, uint32_t height, uint32_t encoding);
  // CPU reference for shader sampling; the chunk must already be validated.
  Field texel(std::span<const uint32_t> words, uint32_t width, uint32_t encoding, uint32_t x, uint32_t y);
}
