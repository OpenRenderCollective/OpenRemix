#pragma once
#include <cstdint>
namespace remix::forward::bake {
  struct Config {
    bool enabled = true;
    bool directDiffuse = true;
    bool diskCache = true;
    bool denoiseLightmaps = true; // OIDN RTLightmap where available; unsupported builds retain raw transport
    bool relocateProbes = true; // bounded recovery of otherwise unused BSP-solid slots
    float gpuBudgetMs = 8; // zero selects fixed batch size; otherwise a measured GPU-time target
    float spacing = 2;     // metres; coarsened if necessary to respect maxProbes
    uint32_t maxProbes = 32768, samples = 256, bounces = 3, batch = 64, textureResolution = 256;
    uint32_t reflectionProbes = 16, reflectionResolution = 32;
    float reflectionDetailTolerance = .5f; // metres; zero requires entirely empty boxes
    uint32_t lightmapPageSize = 1024, lightmapTexels = 4194304;
    bool lightmapFitBudget = true;
    float lightmapLuxelSize = .125f; // manual spacing only when lightmapFitBudget is disabled
    uint32_t lightmapPageSizeForBudget() const {
      auto size = lightmapPageSize;
      while (size > 8 && uint64_t(size) * size > lightmapTexels)
        size /= 2;
      return size;
    }
    bool operator==(const Config &) const = default;
    bool sameLighting(const Config &other) const {
      auto a = *this, b = other;
      a.batch = b.batch = 0;
      a.gpuBudgetMs = b.gpuBudgetMs = 0;
      a.diskCache = b.diskCache = false;
      return a == b;
    }
  };
} // namespace remix::forward::bake
