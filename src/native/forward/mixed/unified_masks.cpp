#include "forward/mixed/unified_masks.h"
#include "forward/half.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace remix::forward::mixed {
  std::optional<UnifiedMasks> unifyMasks(const Plan &plan, const lightmap::Layout &layout,
      std::span<const uint16_t> pixels, uint64_t byteBudget, std::stop_token cancelled) {
    auto check = [&] {
      if (cancelled.stop_requested()) throw std::runtime_error("Unified visibility superseded");
    };
    check();
    const uint64_t pageTexels = uint64_t(layout.pageSize) * layout.pageSize;
    if (!plan.lights.source || plan.charts.size() != layout.charts.size() ||
        pixels.size() != pageTexels * layout.pages * lightmap::atlasLayers * 4 ||
        byteBudget > Masks::maximumPublishedBytes)
      throw std::invalid_argument("Invalid unified visibility publication");
    UnifiedMasks result;
    std::vector<Masks::Entry> entries(plan.masks.entries().begin(), plan.masks.entries().end());
    std::vector<uint8_t> bytes(plan.masks.bytes().begin(), plan.masks.bytes().end());
    auto fits = [&](size_t count, size_t payload) {
      return uint64_t(layout.charts.size()) * 8 + uint64_t(count) * 16 + payload <= byteBudget;
    };
    if (!fits(entries.size(), bytes.size())) return {};
    for (uint32_t chart = 0; chart < plan.charts.size(); ++chart) {
      check();
      const auto &a = layout.charts[chart];
      if (!a.width || !a.height || a.page >= layout.pages || a.width > layout.pageSize ||
          a.height > layout.pageSize || a.x > layout.pageSize - a.width || a.y > layout.pageSize - a.height)
        throw std::invalid_argument("Invalid primary visibility rectangle");
      for (uint32_t channel = 0; channel < channels; ++channel) {
        const auto light = plan.charts[chart][channel];
        if (light == none) continue;
        if (light >= plan.lights.source->lights.size())
          throw std::invalid_argument("Invalid primary visibility identity");
        std::vector<uint8_t> texels(size_t(a.width) * a.height);
        for (uint32_t y = 0; y < a.height; ++y) {
          check();
          const size_t row = ((size_t(a.page) * lightmap::atlasLayers + lightmap::visibilityLayer) *
              layout.pageSize + a.y + y) * layout.pageSize + a.x;
          for (uint32_t x = 0; x < a.width; ++x) {
            const float value = half::decode(pixels[(row + x) * 4 + channel]);
            if (value < 0 || value > 1) throw std::invalid_argument("Invalid primary visibility value");
            const auto quantized = uint8_t(std::lround(value * 255));
            texels[size_t(y) * a.width + x] = quantized;
            result.maximumError = std::max(result.maximumError, std::abs(value - float(quantized) / 255));
          }
        }
        const auto encoded = mask::encode(texels, a.width, a.height, cancelled);
        if (!fits(entries.size() + 1, bytes.size() + encoded.bytes.size())) return {};
        entries.push_back({chart, light, uint32_t(bytes.size()), a.width, a.height, encoded.encoding});
        bytes.insert(bytes.end(), encoded.bytes.begin(), encoded.bytes.end());
        result.convertedTexels += texels.size();
      }
    }
    result.masks = Masks::restore(entries, bytes, layout, uint32_t(plan.lights.source->lights.size()), byteBudget);
    return result;
  }
}
