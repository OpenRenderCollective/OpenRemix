#pragma once
#include <cstdint>
#include <array>
#include <span>
#include <stop_token>
#include <vector>

namespace remix::forward::mixed::mask {
  constexpr uint32_t raw = 0, tiled = 1, tileSize = 8;
  struct Encoded {
    uint32_t encoding = raw;
    std::vector<uint8_t> bytes;
  };
  // Lossless relative to R8 input. Each 8x8 tile has a four-byte descriptor:
  // tag 0 = constant byte, 1 = 64 binary bits, 2 = 64 raw bytes. Payload
  // offsets are chunk-relative bytes in the lower 30 bits. Raw wins ties.
  Encoded encode(std::span<const uint8_t> texels, uint32_t width, uint32_t height,
                 std::stop_token cancelled = {});
  // Validate canonical payload offsets/tags and return the occupied byte count.
  // The span may include subsequent chunks; nothing beyond this chunk is read.
  size_t validate(std::span<const uint8_t> bytes, uint32_t width, uint32_t height, uint32_t encoding);
  // CPU reference for the shader decoder. Call only on a validated chunk.
  uint8_t texel(std::span<const uint8_t> bytes, uint32_t width, uint32_t encoding, uint32_t x, uint32_t y);
  // Optional diagnostic accounting, including candidates rejected by the mask
  // budget. Low-bit costs are estimates for endpoint-preserving quantization,
  // not formats accepted by the production decoder. All byte costs are padded.
  struct Costs {
    uint64_t candidates = 0, texels = 0, blocked = 0, clear = 0, soft = 0;
    uint64_t rawBytes = 0, losslessBytes = 0, twoBitBytes = 0, fourBitBytes = 0;
    uint64_t constantTiles = 0, binaryTiles = 0, softTiles = 0;
    std::array<uint64_t, 65> tileCardinality{};
    std::array<uint64_t, 32> maskSizeHistogram{}; // floor(log2(lossless payload bytes))
    std::array<uint64_t, 2> absoluteError{}, squaredError{}, maximumError{}; // R8 units, 2/4 bits
    void observe(std::span<const uint8_t> values, uint32_t width, uint32_t height,
                 uint64_t encodedBytes, std::stop_token cancelled = {});
  };
} // namespace remix::forward::mixed::mask
