#pragma once
#include "forward/bake/config.h"
#include "forward/bake/storage_options.h"
#include <cstdint>

namespace remix::forward {
  struct Options {
    uint32_t samples = 4;
    uint32_t shadowResolution = 1024;
    float shadowDistance = 100; // metres; converted using sceneScale
    float shadowBias = .002f; // metres along the sun direction, independent of cascade depth span
    float sceneScale = 1;
    float ambient = .025f;
    float exposure = 1;
    bool prism = true, gamma22 = true; // shared path-traced display transform
    bool autoExposure = false; // runtime profiles enable it; low-level callers retain explicit manual output
    float autoExposureTarget = .18f, autoExposureSpeed = 2, autoExposureCentre = .75f;
    float autoExposureMinEv = -8, autoExposureMaxEv = 4;
    // 0 shaded, 1 baked irradiance, 2 analytic diffuse, 3 specular, 4 albedo,
    // 5 irradiance without normal maps, 6 bake coverage, 7 total diffuse.
    uint32_t debugView = 0;
    float lightCutoff = .001f;
    uint32_t clusterLights = 0; // zero adapts64..512; explicit1..1024 retains a bounded profiling override
    bool shadows = true;
    bool localShadows = true;
    bool depthPrepass = true; // reject hidden opaque fragments before expensive lighting
    bool bicubicLightmaps = true; // chart-safe cubic B-spline reconstruction
    uint32_t localShadowLights = 4;
    uint32_t localShadowResolution = 512;
    float localShadowDistance = 100; // metres, independent of light influence
    float localShadowBias = .002f;   // metres, radial comparison bias
    float localShadowFade = 2;      // seconds per selection fade; zero selects immediately
    bool ssr = true;
    bool authoredReflectionFallback = true;
    bool rtReflections = false;
    bool rtGI = false;
    bool selectedBakedLights = false; // experimental: aggregate lights use selected mixed-shadow slots
    bool bakedLighting = false; // runtime profiles default on; low-level callers retain explicit control
    uint32_t bakedHighlightLights = 16; // highlight budget independent of the mixed-shadow budget
    float bakedHighlightFade = 2;
    bool water = true; // measured refraction/reflections; snapshot images are allocated only on use
    float waterDistance = 50, waterThickness = .15f; // metres; refraction extent and depth tolerance
    uint32_t rtGISamples = 16, rtGIBounces = 2;
    float rtGIDistance = 50;         // metres
    float rtReflectionDistance = 50; // metres
    uint32_t rtReflectionBounces = 2, rtTextureResolution = 256;
    uint32_t ssrScale = 1;
    uint32_t ssrSteps = 128;
    float ssrDistance = 50;    // metres
    float ssrThickness = .15f; // metres, single-layer screen depth tolerance
    float ssrRoughness = .6f;
    bake::Config baking;
    bool bakedLightingActive(bool liveGiActive) const {
      return bakedLighting && baking.enabled && baking.directDiffuse && !liveGiActive;
    }
    bool selectBakedLights(bool liveGiActive) const {
      return !liveGiActive && (selectedBakedLights || bakedLightingActive(liveGiActive));
    }
    bake::Config bakeConfiguration(bool liveGiActive) const {
      auto config = baking;
      config.modelLightmaps = bake::modelChartsRequested(config.modelLightmaps);
      config.directionalProbeDepth = bake::directionalProbeDepthRequested();
      if (bakedLightingActive(liveGiActive)) {
        config.aggregateDiffuse = true;
        config.fullyBakedLights = false;
        config.bakedGeometryLights = true;
        config.groupedStyledDirect = true;
      }
      // Live GI replaces aggregate irradiance and evaluates primary lights
      // analytically. Preserve their attributed static visibility in that mode.
      if (liveGiActive) {
        config.groupedStyledDirect = false;
        config.aggregateDiffuse = false;
        config.fullyBakedLights = false;
        config.bakedGeometryLights = false;
      }
      return config;
    }
    bool operator==(const Options &) const = default;
    bool reflections() const { return ssr || rtReflections || baking.reflectionProbes != 0; }
    bool sameTargets(const Options &other) const {
      return debugView == other.debugView && water == other.water && rtGI == other.rtGI && rtReflections == other.rtReflections &&
             localShadowLights == other.localShadowLights &&
             localShadowResolution == other.localShadowResolution && samples == other.samples &&
             shadowResolution == other.shadowResolution && ssr == other.ssr &&
             reflections() == other.reflections() && ssrScale == other.ssrScale;
    }
  };
} // namespace remix::forward
