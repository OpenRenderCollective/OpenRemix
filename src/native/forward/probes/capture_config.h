#pragma once
#include <cstdint>

namespace remix::forward::probes {
  enum class CaptureTransport { SharedScene, StandaloneScene };
  // Selection/admission policy, separate from sampled radiance/cache identity.
  struct CapturePolicy {
    CaptureTransport transport = CaptureTransport::StandaloneScene;
    uint32_t automaticCount = 0;
    float detailTolerance = .5f; // metres
    bool authoredFallback = true;
    bool operator==(const CapturePolicy &) const = default;
  };
  struct CaptureConfig {
    bool enabled = true;
    uint32_t reflectionResolution = 128, textureResolution = 256;
    uint32_t samples = 256, bounces = 3, batch = 64;
    float gpuBudgetMs = 8;
    bool diskCache = true;
    bool operator==(const CaptureConfig &) const = default;
    bool sameLighting(const CaptureConfig &other) const {
      auto a = *this, b = other;
      a.batch = b.batch = 0;
      a.gpuBudgetMs = b.gpuBudgetMs = 0;
      a.diskCache = b.diskCache = false;
      return a == b;
    }
  };
}
