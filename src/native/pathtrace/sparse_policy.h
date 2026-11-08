#pragma once
#include "pathtrace/options.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace remix::trace {
  // Sparse rendering's sampling rate (the share of the pixels lit each frame)
  // at a render resolution of this many pixels; see TraceOptions.
  inline float sparseSamplingRate(const TraceOptions &options, uint64_t pixels) {
    constexpr float lowest = 1.f / 16, highest = 1;
    if (!options.sparseAdaptiveRate)
      return std::clamp(options.sparseSamplingRate, lowest, highest);
    // The render resolution of 4K output with DLSS Auto, where dxvk-remix
    // tuned the rates of its graphics presets.
    constexpr float reference = 2227.f * 1238.f;
    const float scaled =
        std::clamp(options.sparseSamplingRate * std::pow(reference / float(std::max<uint64_t>(pixels, 1)),
                                                         std::clamp(options.sparseResolutionExponent, 0.f, 1.f)),
                   lowest, highest);
    // Equal steps of quality are about equal steps in image quality.
    const float quality = std::clamp(options.sparseQuality, 0.f, 1.f);
    const float bound = quality >= .5f ? highest : lowest;
    const float rate = scaled * std::pow(bound / scaled, std::abs(2 * quality - 1));
    return std::clamp(rate, std::min(scaled, bound), std::max(scaled, bound));
  }
  // dxvk-remix's graphics presets: the rate at the reference resolution and
  // the exponent it scales by, tuned together.
  struct SparsePreset {
    const char *name;
    float samplingRate, resolutionExponent;
  };
  inline constexpr SparsePreset sparsePresets[] = {
      {"Ultra", .357f, .69f}, {"High", .228f, .83f}, {"Medium", .125f, .83f}, {"Low", .1f, .91f}};
  // The rate as the shader applies it: threshold levels passed of 256.
  inline uint32_t sparseLevels(float rate) { return uint32_t(std::clamp(std::lround(rate * 256), 1l, 256l)); }
} // namespace remix::trace
