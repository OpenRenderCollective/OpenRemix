#pragma once
#include "forward/bake/config.h"
#include "forward/bake/storage_options.h"
#include "forward/lighting_plan.h"
#include "forward/probes/capture_config.h"
#include <cstdint>

namespace remix::forward {
  // Numeric values match the renderer setting and GLSL LightingView.
  enum class LightingView : uint32_t {
    Shaded = 0, BakedIrradiance = 1, AnalyticDiffuse = 2, DirectSpecular = 3,
    Albedo = 4, IrradianceWithoutNormalMaps = 5, BakeCoverage = 6, TotalDiffuse = 7
  };

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
    uint32_t debugView = uint32_t(LightingView::Shaded);
    float lightCutoff = .001f;
    uint32_t clusterLights = 0; // zero adapts64..512; explicit1..1024 retains a bounded profiling override
    bool shadows = true;
    bool localShadows = true;
    bool depthPrepass = true; // reject hidden opaque fragments before expensive lighting
    bool opaqueOcclusionCulling = true; // use prepass depth to reject opaque draws in larger views
    bool staticDepthBatch = true; // bounded per-slot aggregation; unsupported sets use ordinary draws
    bool bicubicLightmaps = true; // chart-safe cubic B-spline reconstruction
    uint32_t localShadowLights = 4;
    uint32_t localShadowResolution = 512;
    float localShadowDistance = 100; // metres, independent of light influence
    float localShadowBias = .002f;   // metres, radial comparison bias
    float localShadowFade = 2;      // seconds per selection fade; zero selects immediately
    bool ssr = false;
    bool authoredReflectionFallback = true;
    bool rtReflections = false;
    bool rtGI = false;
    bool bakedDirectWithLiveGI = false; // explicit incoming-only composition; legacy rtGI replaces the whole field
    // Split Radiance Cascades diffuse: sparse world-space probes fed by one traced ray per sampled pixel.
    // Takes precedence over rtGI; needs ray queries in compute only.
    bool splitCascades = false;
    float cascadeSpacing = .05f;   // metres between the finest probes, nearest the camera
    uint32_t cascadeRayStride = 2; // one ray per stride x stride pixels
    // Finest probe cells per level of detail: a probe spans about 1 / (2 * cells) of a 90 degree view.
    // The build lowers it while its probe pools overflow.
    uint32_t cascadeLodCells = 32;
    bool cascadeAccumulation = true; // blend each probe with its previous builds
    // Trace a shadow ray for local lights that have no shadow map, instead of
    // leaving them unshadowed. Needs the cascades' ray scene and fragment ray queries.
    bool rtLocalShadows = false;
    uint32_t rtShadowLights = 8; // rays per shaded pixel; lights beyond it stay unshadowed
    // Trace the sun's shadow instead of sampling its cascades, on the same receivers.
    bool rtSunShadows = false;
    // The receivers that carry the ray scene; which shadows they trace can then change live.
    bool rayShadowReceivers() const { return rtLocalShadows || rtSunShadows; }
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
    // Scene-space offset shared by reflection sampling and probe mask padding.
    float reflectionSampleBias() const { return .002f / sceneScale; }
    lighting::Plan lightingPlan(bool liveGiActive) const {
      auto config = baking;
      config.modelLightmaps = bake::modelChartsRequested(config.modelLightmaps);
      config.directionalProbeDepth = bake::directionalProbeDepthRequested();
      return lighting::resolve(config, {bakedLighting, selectedBakedLights, bakedDirectWithLiveGI}, liveGiActive);
    }
    probes::CapturePolicy capturePolicy(probes::CaptureTransport transport) const {
      return {transport, baking.reflectionProbes, baking.reflectionDetailTolerance, authoredReflectionFallback};
    }
    probes::CaptureConfig captureConfiguration() const {
      return {.enabled = baking.reflectionProbes != 0,
              .reflectionResolution = baking.reflectionResolution,
              .textureResolution = baking.textureResolution,
              .samples = baking.samples, .bounces = baking.bounces,
              .batch = baking.batch, .gpuBudgetMs = baking.gpuBudgetMs, .diskCache = baking.diskCache};
    }
    bool operator==(const Options &) const = default;
    bool reflections() const { return ssr || rtReflections || baking.reflectionProbes != 0; }
    bool worldReflectionGuides() const {
      switch (LightingView(debugView)) {
      case LightingView::BakedIrradiance:
      case LightingView::Albedo:
      case LightingView::IrradianceWithoutNormalMaps:
      case LightingView::BakeCoverage: return false;
      default: return true;
      }
    }
    bool sameTargets(const Options &other) const {
      return debugView == other.debugView && staticDepthBatch == other.staticDepthBatch &&
             depthPrepass == other.depthPrepass &&
             (!depthPrepass || opaqueOcclusionCulling == other.opaqueOcclusionCulling) &&
             bicubicLightmaps == other.bicubicLightmaps &&
             autoExposure == other.autoExposure &&
             water == other.water && rtGI == other.rtGI && rtReflections == other.rtReflections &&
             splitCascades == other.splitCascades && rayShadowReceivers() == other.rayShadowReceivers() &&
             (!rtGI || lightingPlan(true).bakedReceivers == other.lightingPlan(true).bakedReceivers) &&
             localShadowLights == other.localShadowLights &&
             localShadowResolution == other.localShadowResolution && samples == other.samples &&
             shadowResolution == other.shadowResolution && ssr == other.ssr &&
             (!worldReflectionGuides() ||
              (reflections() == other.reflections() &&
               // A requested ray provider may be unsupported. Its cube fallback
               // can therefore change attachment requirements.
               (ssr || !rtReflections || bool(baking.reflectionProbes) == bool(other.baking.reflectionProbes)))) &&
             ssrScale == other.ssrScale;
    }
  };
} // namespace remix::forward
