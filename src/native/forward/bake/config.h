#pragma once
#include <cstdint>
namespace remix::forward::bake {
  struct Config {
    bool enabled = true;
    bool directDiffuse = true;
    float gpuBudgetMs = 2; // zero selects fixed batch size; otherwise a measured GPU-time target
    float spacing = 2;     // metres; coarsened if necessary to respect maxProbes
    uint32_t maxProbes = 32768, samples = 256, bounces = 3, batch = 64, textureResolution = 256;
    uint32_t reflectionProbes = 4, reflectionResolution = 32;
    float reflectionDetailTolerance = .5f; // metres; zero requires entirely empty boxes
    uint32_t lightmapPageSize = 512, lightmapTexels = 1048576;
    float lightmapLuxelSize = .25f; // metres; measured on world surfaces before budget coarsening
    bool operator==(const Config &) const = default;
    bool sameLighting(const Config &other) const {
      auto a = *this, b = other;
      a.batch = b.batch = 0;
      a.gpuBudgetMs = b.gpuBudgetMs = 0;
      return a == b;
    }
  };
} // namespace remix::forward::bake
