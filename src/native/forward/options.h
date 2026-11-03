#pragma once
#include "forward/bake/config.h"
#include "forward/bake/storage_options.h"
#include "forward/lighting_plan.h"
#include "forward/probes/capture_config.h"
#include <array>
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
    // 5 irradiance without normal maps, 6 bake coverage, 7 total diffuse,
    // 8 indirect diffuse only (material colour times the selected diffuse provider).
    uint32_t debugView = 0;
    float lightCutoff = .001f;
    uint32_t clusterLights = 0; // zero adapts64..512; explicit1..1024 retains a bounded profiling override
    bool shadows = true;
    bool localShadows = true;
    bool depthPrepass = true; // reject hidden opaque fragments before expensive lighting
    bool staticDepthBatch = true; // bounded per-slot aggregation; unsupported sets use ordinary draws
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
    // Radiance cascades diffuse provider (techniques::Diffuse::RadianceCascades):
    // screen-space probes traced against the live ray scene (forward/cascades).
    bool radianceCascades = false;
    uint32_t cascadeCount = 5;        // 2..6
    uint32_t cascadeDirections = 8;   // cascade-0 directions per octahedral side (4, 8, 16)
    float cascadeInterval = .25f;     // metres traced by cascade 0; each cascade traces four times further
    float cascadeDistance = 200;      // metres; the last cascade's reach before the sky
    float cascadeIntensity = 1;       // indirect diffuse scale, for inspection
    float cascadeHistory = .97f;      // history weight h: a running mean of up to 1 / (1 - h) frames
    bool cascadeBounces = true;       // screen-space feedback of the previous frame's irradiance
    // Analytic sky for scenes without an authored dome, scaled by the sun.
    bool proceduralSky = false; // opt in; preserve authored/host sky and Source 2 preset defaults
    float skyIntensity = 1;
    uint32_t skyUpAxis = 3; // 0 X, 1 Y, 2 Z, 3 automatic from the camera
    // Live light controls: replace the scene's distant light without rebaking.
    bool sunOverride = false;
    float sunAzimuth = 30, sunElevation = 55; // degrees around and above the horizon
    float sunIntensity = 9;                   // irradiance at normal incidence
    std::array<float, 3> sunColor{1, .94f, .84f};
    float sunAngularDiameter = .5f; // degrees
    bake::Config baking;
    bool bakedLightingActive(bool liveGiActive) const {
      return lighting::resolve(baking, {bakedLighting, selectedBakedLights}, liveGiActive).bakedReceivers;
    }
    bool selectBakedLights(bool liveGiActive) const {
      return lighting::resolve(baking, {bakedLighting, selectedBakedLights}, liveGiActive).selectedMixedLights;
    }
    lighting::Plan lightingPlan(bool liveGiActive) const {
      auto config = baking;
      config.modelLightmaps = bake::modelChartsRequested(config.modelLightmaps);
      config.directionalProbeDepth = bake::directionalProbeDepthRequested();
      return lighting::resolve(config, {bakedLighting, selectedBakedLights}, liveGiActive);
    }
    bake::Config bakeConfiguration(bool liveGiActive) const {
      return lightingPlan(liveGiActive).baking;
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
    bool sameTargets(const Options &other) const {
      return debugView == other.debugView && staticDepthBatch == other.staticDepthBatch &&
             water == other.water && rtGI == other.rtGI && rtReflections == other.rtReflections &&
             localShadowLights == other.localShadowLights &&
             localShadowResolution == other.localShadowResolution && samples == other.samples &&
             shadowResolution == other.shadowResolution && ssr == other.ssr &&
             reflections() == other.reflections() &&
             // A requested ray provider may be unsupported. Changing its cube
             // fallback can therefore change attachment requirements.
             (ssr || !rtReflections || bool(baking.reflectionProbes) == bool(other.baking.reflectionProbes)) &&
             ssrScale == other.ssrScale &&
             // Provider selection changes compiled world shaders and cascade targets.
             radianceCascades == other.radianceCascades && cascadeCount == other.cascadeCount &&
             cascadeDirections == other.cascadeDirections &&
             // Early cascade gathers require their own depth pass/framebuffer.
             (!radianceCascades || depthPrepass == other.depthPrepass);
    }
  };
} // namespace remix::forward
