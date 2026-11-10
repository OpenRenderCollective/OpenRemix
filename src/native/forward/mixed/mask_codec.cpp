#include "forward/mixed/mask_codec.h"
#include "forward/mixed/mask_palette.h"
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
  static Encoded encodeImage(std::span<const uint8_t> texels, uint32_t width, uint32_t height,
                             std::stop_token cancelled, bool palettes) {
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
          const auto palette = palettes && !binary ? fitBoundedPalette(tile) : Palette{};
          descriptor = (binary ? 1u : palette.size ? 3u : 2u) << 30 | offset;
          if (binary) {
            compact.resize(compact.size() + 8);
            for (uint32_t i = 0; i < 64; ++i)
              compact[offset + i / 8] |= uint8_t((tile[i] != 0) << (i % 8));
          } else if (palette.size) {
            descriptor |= palette.mode; // Payloads are word-aligned; low two bits select palette size.
            compact.insert(compact.end(), palette.bytes.begin(), palette.bytes.begin() + palette.size);
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
  Encoded encode(std::span<const uint8_t> texels, uint32_t width, uint32_t height, std::stop_token cancelled) {
    return encodeImage(texels, width, height, cancelled, false);
  }
  Encoded encodeCompact(std::span<const uint8_t> texels, uint32_t width, uint32_t height, std::stop_token cancelled) {
    return encodeImage(texels, width, height, cancelled, true);
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
          const uint32_t mode = tag == 3 ? descriptor & 3u : 0u;
          if ((descriptor & (tag == 3 ? 0x3ffffffcu : 0x3fffffffu)) != occupied || mode == 3)
            throw std::invalid_argument("Invalid visibility tile payload offset");
          const auto start = occupied;
          const uint32_t colors = 4u << mode;
          occupied += tag == 1 ? 8 : tag == 3 ? colors + 8 * (2 + mode) : 64;
          if (occupied > bytes.size())
            throw std::invalid_argument("Truncated visibility payload");
          if (tag == 3) {
            if (!std::is_sorted(bytes.begin() + start, bytes.begin() + start + colors))
              throw std::invalid_argument("Unsorted visibility palette");
            for (uint32_t i = 0; i < 64; ++i) {
              const auto color = paletteIndex(bytes.subspan(start), mode, i);
              if (color && bytes[start + color] == bytes[start + color - 1])
                throw std::invalid_argument("Duplicate visibility palette index");
            }
          }
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
    if (tag == 3)
      return bytes[(offset & ~3u) + paletteIndex(bytes.subspan(offset & ~3u), descriptor & 3u, index)];
    return bytes[offset + index];
  }
  void Costs::observe(std::span<const uint8_t> values, uint32_t width, uint32_t height,
                      uint64_t encodedBytes, std::stop_token cancelled) {
    const uint64_t count = uint64_t(width) * height;
    if (!count || count != values.size())
      throw std::invalid_argument("Invalid visibility cost image");
    if (cancelled.stop_requested())
      throw std::runtime_error("Visibility cost audit superseded");
    auto quantize = [](uint32_t value, uint32_t levels) {
      if (value == 0 || value == 255)
        return value == 0 ? 0u : levels - 1;
      return std::clamp((value * (levels - 1) + 127) / 255, 1u, levels - 2);
    };
    auto padded = [](uint64_t bytes) -> uint64_t { return (bytes + 3) & ~uint64_t(3); };
    ++candidates;
    texels += count;
    rawBytes += padded(count);
    losslessBytes += encodedBytes;
    const auto compact = encodeCompact(values, width, height, cancelled);
    compactBytes += compact.bytes.size();
    std::vector<uint8_t> tileErrors(tileCount(width, height));
    for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x) {
        const int error = int(texel(compact.bytes, width, compact.encoding, x, y)) - values[size_t(y) * width + x];
        const auto magnitude = uint64_t(error < 0 ? -error : error);
        compactAbsoluteError += magnitude;
        compactSquaredError += magnitude * magnitude;
        compactMaximumError = std::max(compactMaximumError, magnitude);
        auto &maximum = tileErrors[size_t(y / 8) * ((width + 7) / 8) + x / 8];
        maximum = std::max(maximum, uint8_t(magnitude));
      }
    for (auto error : tileErrors)
      ++compactErrorHistogram[error];
    size_t sizeBin = 0;
    for (auto bytes = encodedBytes; bytes > 1 && sizeBin < 31; bytes >>= 1)
      ++sizeBin;
    ++maskSizeHistogram[sizeBin];
    for (uint8_t value : values) {
      blocked += value == 0;
      clear += value == 255;
      soft += value != 0 && value != 255;
      for (uint32_t mode = 0; mode < 2; ++mode) {
        const uint32_t levels = mode == 0 ? 4 : 16;
        const int error = int(quantize(value, levels) * (255 / (levels - 1))) - value;
        const auto magnitude = uint64_t(error < 0 ? -error : error);
        absoluteError[mode] += magnitude;
        squaredError[mode] += magnitude * magnitude;
        maximumError[mode] = std::max(maximumError[mode], magnitude);
      }
    }
    std::array<uint64_t, 2> tiledBytes{tileCount(width, height) * 4, tileCount(width, height) * 4};
    for (uint32_t ty = 0; ty < (height + 7) / 8; ++ty) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Visibility cost audit superseded");
      for (uint32_t tx = 0; tx < (width + 7) / 8; ++tx) {
        std::array<bool, 256> present{};
        std::array<bool, 2> constant{true, true};
        std::array<uint32_t, 2> first{};
        bool binary = true;
        for (uint32_t y = 0; y < 8; ++y)
          for (uint32_t x = 0; x < 8; ++x) {
            const auto px = std::min(tx * 8 + x, width - 1), py = std::min(ty * 8 + y, height - 1);
            const auto value = values[size_t(py) * width + px];
            present[value] = true;
            binary = binary && (value == 0 || value == 255);
            for (uint32_t mode = 0; mode < 2; ++mode) {
              const auto q = quantize(value, mode == 0 ? 4 : 16);
              if (x == 0 && y == 0)
                first[mode] = q;
              constant[mode] = constant[mode] && q == first[mode];
            }
          }
        const auto colors = size_t(std::count(present.begin(), present.end(), true));
        ++tileCardinality[colors];
        constantTiles += colors == 1;
        binaryTiles += colors > 1 && binary;
        softTiles += colors > 1 && !binary;
        for (uint32_t mode = 0; mode < 2; ++mode)
          tiledBytes[mode] += constant[mode] ? 0 : binary ? 8 : mode == 0 ? 16 : 32;
      }
    }
    twoBitBytes += std::min(padded((count * 2 + 7) / 8), tiledBytes[0]);
    fourBitBytes += std::min(padded((count * 4 + 7) / 8), tiledBytes[1]);
  }
} // namespace remix::forward::mixed::mask
