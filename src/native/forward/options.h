#pragma once
#include "forward/bake/config.h"
#include <cstdint>

namespace remix::forward {
  struct Options {
    uint32_t samples = 4;
    uint32_t shadowResolution = 1024;
    float shadowDistance = 100; // metres; converted using sceneScale
    float sceneScale = 1;
    float ambient = .025f;
    float exposure = 1;
    float lightCutoff = .001f;
    bool shadows = true;
    bool localShadows = true;
    uint32_t localShadowLights = 4;
    uint32_t localShadowResolution = 512;
    float localShadowDistance = 100; // metres, independent of light influence
    float localShadowBias = .002f;   // metres, radial comparison bias
    bool ssr = true;
    bool rtReflections = false;
    bool rtGI = false;
    uint32_t rtGISamples = 16, rtGIBounces = 2;
    float rtGIDistance = 50;         // metres
    float rtReflectionDistance = 50; // metres
    uint32_t rtReflectionBounces = 2, rtTextureResolution = 256;
    uint32_t ssrScale = 2;
    uint32_t ssrSteps = 128;
    float ssrDistance = 50;    // metres
    float ssrThickness = .15f; // metres, single-layer screen depth tolerance
    float ssrRoughness = .6f;
    bake::Config baking;
    bool operator==(const Options &) const = default;
    bool reflections() const { return ssr || rtReflections || baking.reflectionProbes != 0; }
    bool sameTargets(const Options &other) const {
      return rtGI == other.rtGI && rtReflections == other.rtReflections &&
             localShadowLights == other.localShadowLights &&
             localShadowResolution == other.localShadowResolution && samples == other.samples &&
             shadowResolution == other.shadowResolution && ssr == other.ssr &&
             reflections() == other.reflections() && ssrScale == other.ssrScale;
    }
  };
} // namespace remix::forward
