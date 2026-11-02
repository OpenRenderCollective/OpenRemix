#pragma once
#include <cstdint>

namespace remix::forward::probes {
  struct CaptureConfig {
    bool enabled = true;
    uint32_t reflectionResolution = 128, textureResolution = 256;
    uint32_t samples = 256, bounces = 3, batch = 64;
    float gpuBudgetMs = 8;
    bool operator==(const CaptureConfig &) const = default;
    bool sameLighting(const CaptureConfig &other) const {
      auto a = *this, b = other;
      a.batch = b.batch = 0;
      a.gpuBudgetMs = b.gpuBudgetMs = 0;
      return a == b;
    }
  };
}
