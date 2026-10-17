#include "forward/mixed/mask_codec.h"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace remix::forward::mixed::mask {
  namespace {
    uint32_t word(std::span<const uint8_t> bytes, size_t offset) {
      if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::invalid_argument("Truncated visibility tile");
      return uint32_t(bytes[offset]) | uint32_t(bytes[offset + 1]) << 8 |
             uint32_t(bytes[offset + 2]) << 16 | uint32_t(bytes[offset + 3]) << 24;
    }
    void store(std::vector<uint8_t> &bytes, size_t offset, uint32_t value) {
      for (uint32_t c = 0; c < 4; ++c)
        bytes[offset + c] = uint8_t(value >> (c * 8));
    }
    size_t tileCount(uint32_t width, uint32_t height) {
      return size_t((uint64_t(width) + 7) / 8) * ((uint64_t(height) + 7) / 8);
    }
  }
  Encoded encode(std::span<const uint8_t> texels, uint32_t width, uint32_t height, std::stop_token cancelled) {
    const uint64_t count = uint64_t(width) * height;
    if (!count || count != texels.size() || count > (1u << 28))
      throw std::invalid_argument("Invalid visibility image");
    if (cancelled.stop_requested())
      throw std::runtime_error("Visibility encoding superseded");
    Encoded result;
    result.bytes.assign(texels.begin(), texels.end());
    result.bytes.resize((result.bytes.size() + 3) & ~size_t(3), 255);
    const size_t descriptors = tileCount(width, height) * 4;
    if (descriptors >= result.bytes.size())
      return result;
    std::vector<uint8_t> compact(descriptors);
    const uint32_t columns = (width + 7) / 8;
    for (uint32_t ty = 0; ty < (height + 7) / 8; ++ty) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Visibility encoding superseded");
      for (uint32_t tx = 0; tx < columns; ++tx) {
        std::array<uint8_t, 64> tile;
        bool constant = true, binary = true;
        for (uint32_t y = 0; y < 8; ++y)
          for (uint32_t x = 0; x < 8; ++x) {
            const auto px = std::min(tx * 8 + x, width - 1), py = std::min(ty * 8 + y, height - 1);
            const auto value = texels[size_t(py) * width + px];
            tile[y * 8 + x] = value;
            constant = constant && value == tile[0];
            binary = binary && (value == 0 || value == 255);
          }
        uint32_t descriptor = tile[0];
        if (!constant) {
          const auto offset = uint32_t(compact.size());
          descriptor = (binary ? 1u : 2u) << 30 | offset;
          if (binary) {
            compact.resize(compact.size() + 8);
            for (uint32_t i = 0; i < 64; ++i)
              compact[offset + i / 8] |= uint8_t((tile[i] != 0) << (i % 8));
          } else
            compact.insert(compact.end(), tile.begin(), tile.end());
        }
        store(compact, (size_t(ty) * columns + tx) * 4, descriptor);
        if (compact.size() >= result.bytes.size())
          return result; // Remaining tiles cannot make this encoding smaller.
      }
    }
    result.encoding = tiled;
    result.bytes = std::move(compact);
    return result;
  }
  size_t validate(std::span<const uint8_t> bytes, uint32_t width, uint32_t height, uint32_t encoding) {
    const uint64_t count = uint64_t(width) * height;
    if (!count || count > (1u << 28))
      throw std::invalid_argument("Invalid visibility image dimensions");
    size_t occupied = size_t((count + 3) & ~3ull);
    if (encoding == tiled) {
      const auto tiles = tileCount(width, height);
      occupied = tiles * 4;
      if (occupied > bytes.size())
        throw std::invalid_argument("Truncated visibility descriptors");
      for (size_t tile = 0; tile < tiles; ++tile) {
        const uint32_t descriptor = word(bytes, tile * 4), tag = descriptor >> 30;
        if (tag == 0) {
          if (descriptor > 255)
            throw std::invalid_argument("Invalid constant visibility tile");
        } else {
          if (tag == 3 || (descriptor & 0x3fffffffu) != occupied)
            throw std::invalid_argument("Invalid visibility tile payload offset");
          occupied += tag == 1 ? 8 : 64;
          if (occupied > bytes.size())
            throw std::invalid_argument("Truncated visibility payload");
        }
      }
    } else if (encoding != raw)
      throw std::invalid_argument("Unknown visibility encoding");
    if (occupied > bytes.size())
      throw std::invalid_argument("Truncated visibility image");
    return occupied;
  }
  uint8_t texel(std::span<const uint8_t> bytes, uint32_t width, uint32_t encoding, uint32_t x, uint32_t y) {
    if (encoding == raw)
      return bytes[size_t(y) * width + x];
    const uint32_t descriptor = word(bytes, (size_t(y / 8) * ((width + 7) / 8) + x / 8) * 4);
    const auto tag = descriptor >> 30, offset = descriptor & 0x3fffffffu, index = (y % 8) * 8 + x % 8;
    if (tag == 0)
      return uint8_t(descriptor);
    if (tag == 1)
      return (bytes[offset + index / 8] >> (index % 8) & 1) ? 255 : 0;
    return bytes[offset + index];
  }
} // namespace remix::forward::mixed::mask
