#include "forward/mixed/mask_palette.h"
#include <algorithm>
#include <limits>

namespace remix::forward::mixed::mask {
  template <uint32_t Colors, uint32_t Bits>
  static std::array<uint8_t, Colors + 64 * Bits / 8> fit(std::span<const uint8_t, 64> tile) {
    std::array<uint32_t, 256> histogram{};
    for (auto value : tile)
      ++histogram[value];
    std::array<uint8_t, Colors + 64 * Bits / 8> result{};
    uint32_t colors = 0;
    for (uint32_t value = 0; value < 256; ++value)
      if (histogram[value]) {
        if (colors < Colors)
          result[colors] = uint8_t(value);
        ++colors;
      }
    if (colors > Colors) {
      colors = 0;
      if (histogram[0]) result[colors++] = 0;
      if (histogram[255]) result[colors++] = 255;
      const uint32_t slots = Colors - colors;
      std::array<uint32_t, 64> values{};
      uint32_t count = 0, weight = 0, sum = 0;
      for (uint32_t value = 1; value < 255; ++value)
        if (histogram[value]) {
          values[count++] = value;
          weight += histogram[value];
          sum += value * histogram[value];
        }
      std::array<uint32_t, Colors> centers{};
      centers[0] = (sum + weight / 2) / weight;
      auto nearest = [&](uint32_t value, uint32_t available) {
        uint32_t best = 0;
        int distance = std::numeric_limits<int>::max();
        for (uint32_t c = 0; c < available; ++c) {
          const int delta = int(value) - int(centers[c]);
          if (delta * delta < distance) { best = c; distance = delta * delta; }
        }
        return best;
      };
      // Seed distant weighted values, then fit only the soft population.
      // Endpoint colours are fixed and cannot absorb a faint soft sample.
      for (uint32_t c = 1; c < slots; ++c) {
        uint32_t best = 0, largest = 0;
        for (uint32_t i = 0; i < count; ++i) {
          const auto value = values[i];
          const int delta = int(value) - int(centers[nearest(value, c)]);
          const auto error = uint32_t(delta * delta) * histogram[value];
          if (error > largest) { best = value; largest = error; }
        }
        centers[c] = best;
      }
      for (uint32_t iteration = 0; iteration < 8; ++iteration) {
        std::array<uint32_t, Colors> sums{}, weights{};
        for (uint32_t i = 0; i < count; ++i) {
          const auto value = values[i], c = nearest(value, slots);
          sums[c] += value * histogram[value];
          weights[c] += histogram[value];
        }
        bool changed = false;
        for (uint32_t c = 0; c < slots; ++c)
          if (weights[c]) {
            const auto next = (sums[c] + weights[c] / 2) / weights[c];
            changed = changed || centers[c] != next;
            centers[c] = next;
          }
        if (!changed) break;
      }
      for (uint32_t c = 0; c < slots; ++c)
        result[colors++] = uint8_t(centers[c]);
      std::sort(result.begin(), result.begin() + Colors);
      colors = uint32_t(std::unique(result.begin(), result.begin() + Colors) - result.begin());
    }
    std::fill(result.begin() + colors, result.begin() + Colors, result[colors - 1]);
    for (uint32_t i = 0; i < 64; ++i) {
      uint32_t best = 0;
      int distance = std::numeric_limits<int>::max();
      for (uint32_t c = 0; c < colors; ++c) {
        if (tile[i] != 0 && tile[i] != 255 && (result[c] == 0 || result[c] == 255))
          continue;
        const int delta = int(tile[i]) - int(result[c]);
        if (delta * delta < distance) { best = c; distance = delta * delta; }
      }
      for (uint32_t bit = 0; bit < Bits; ++bit)
        result[Colors + (i * Bits + bit) / 8] |= uint8_t(((best >> bit) & 1u) << ((i * Bits + bit) % 8));
    }
    return result;
  }
  std::array<uint8_t, 20> fitPalette(std::span<const uint8_t, 64> tile) { return fit<4, 2>(tile); }
  std::array<uint8_t, 32> fitPalette8(std::span<const uint8_t, 64> tile) { return fit<8, 3>(tile); }
  std::array<uint8_t, 48> fitPalette16(std::span<const uint8_t, 64> tile) { return fit<16, 4>(tile); }
  Palette fitBoundedPalette(std::span<const uint8_t, 64> tile) {
    Palette result;
    auto accept = [&](std::span<const uint8_t> bytes, uint32_t mode) {
      for (uint32_t i = 0; i < 64; ++i) {
        const int delta = int(bytes[paletteIndex(bytes, mode, i)]) - tile[i];
        if (uint32_t(delta < 0 ? -delta : delta) > maximumPaletteError)
          return false;
      }
      std::copy(bytes.begin(), bytes.end(), result.bytes.begin());
      result.mode = mode;
      result.size = uint32_t(bytes.size());
      return true;
    };
    if (accept(fitPalette(tile), 0) || accept(fitPalette8(tile), 1))
      return result;
    accept(fitPalette16(tile), 2);
    return result;
  }
}
