#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace remix::forward::mixed::mask {
  // Sorted R8 colours followed by 64 packed indices. Exact zero/one samples
  // get reserved colours; soft samples never select those endpoints.
  std::array<uint8_t, 20> fitPalette(std::span<const uint8_t, 64> tile);
  std::array<uint8_t, 32> fitPalette8(std::span<const uint8_t, 64> tile);
  std::array<uint8_t, 48> fitPalette16(std::span<const uint8_t, 64> tile);
  constexpr uint32_t maximumPaletteError = 8; // R8 units; larger errors retain raw texels.
  inline uint32_t paletteIndex(std::span<const uint8_t> bytes, uint32_t mode, uint32_t index) {
    const uint32_t colors = 4u << mode, bits = 2 + mode, bit = index * bits;
    uint32_t value = bytes[colors + bit / 8] >> (bit % 8);
    if (bit % 8 + bits > 8)
      value |= uint32_t(bytes[colors + bit / 8 + 1]) << (8 - bit % 8);
    return value & ((1u << bits) - 1);
  }
  struct Palette {
    std::array<uint8_t, 48> bytes{};
    uint32_t mode = 0, size = 0; // modes 0/1/2: 4/8/16 colours; size zero requests raw storage.
  };
  Palette fitBoundedPalette(std::span<const uint8_t, 64> tile);
}
