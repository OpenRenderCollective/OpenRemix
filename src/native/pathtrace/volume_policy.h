#pragma once
#include "pathtrace/options.h"
#include "scene/light_bvh.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace remix::trace {
  // The game's fog as remixapi_SetFogState last gave it (D3D fog: mode 0
  // none, 1 exp, 2 exp2, 3 linear; distances in world units).
  struct GameFog {
    uint32_t mode = 0;
    float color[3] = {};
    float scale = 0, end = 0, density = 0;
  };
  inline std::mutex g_gameFogLock;
  inline GameFog g_gameFog;
  inline void setGameFog(const GameFog &fog) {
    std::lock_guard lock(g_gameFogLock);
    g_gameFog = fog;
  }
  inline GameFog gameFog() {
    std::lock_guard lock(g_gameFogLock);
    return g_gameFog;
  }

  // The participating medium of a frame (volume.cpp): the game's fog, or the
  // override's when that is on. Extinction is per world unit.
  struct Medium {
    bool active = false;
    float extinction = 0;
    float albedo[3] = {1, 1, 1};
    const char *source = "none";
  };
  // Transmittance left at the "visibility distance" a medium is described by.
  inline constexpr float volumeVisibility = .05f;
  // A display-encoded value (gamma 2.2) to linear.
  inline float volumeLinear(float c) { return std::pow(c, 2.2f); }
  inline Medium volumeMedium(const TraceOptions &options, const GameFog &fog) {
    Medium medium;
    if (!options.volumetrics)
      return medium;
    const float reach = -std::log(volumeVisibility);
    if (options.volumetricOverride) {
      const float distance = std::max(options.volumetricDistance, .01f) / options.sceneScale;
      medium.extinction = reach / distance;
      std::copy_n(options.volumetricColor, 3, medium.albedo);
      medium.source = "override";
    } else {
      // exp: the density is the extinction. exp2 (exp(-(density d)^2)) and
      // linear (clear to `end`) have no exponential equal: the one that
      // leaves as much at the distance where they leave volumeVisibility.
      const float density = fog.density * std::max(options.volumetricFogDensityScale, 0.f);
      if (fog.mode == 1 && density > 0)
        medium.extinction = density;
      else if (fog.mode == 2 && density > 0)
        medium.extinction = density * std::sqrt(reach);
      else if (fog.mode == 3 && fog.end > 0)
        medium.extinction = reach / fog.end;
      else
        return medium;
      // The fog colour is what the game showed where the fog is thick; here
      // it tints what the medium scatters, and the lights give its level.
      float linear[3], brightest = 0;
      for (int c = 0; c < 3; ++c)
        brightest = std::max(brightest, linear[c] = volumeLinear(std::clamp(fog.color[c], 0.f, 1.f)));
      for (int c = 0; c < 3; ++c)
        medium.albedo[c] = brightest > 0 ? linear[c] / brightest : 1;
      medium.source = "game fog";
    }
    medium.active = medium.extinction > 0 && std::isfinite(medium.extinction);
    return medium;
  }
  // Cells per side of a level: even, within what the grids are sized for.
  inline uint32_t volumeCells(const TraceOptions &options) {
    return std::clamp(options.volumetricCells, 16u, 96u) & ~1u;
  }
  inline uint32_t volumeLevels(const TraceOptions &options) { return std::clamp(options.volumetricLevels, 1u, 6u); }

  // Quality presets: the grids' resolution and the samples of a camera ray.
  struct VolumeQuality {
    const char *name;
    float cellSize;
    uint32_t cells, levels, steps;
  };
  inline constexpr VolumeQuality volumeQualities[] = {
      {"Low", .5f, 32, 4, 8}, {"Medium", .35f, 40, 4, 10}, {"High", .25f, 48, 4, 12}, {"Ultra", .2f, 64, 4, 20}};
  // Responsiveness presets: how often a cell is relit and how many samples
  // it averages. A change of lighting settles in about interval x history
  // frames; a shorter interval traces more rays, a shorter history is noisier.
  struct VolumeResponse {
    const char *name;
    uint32_t interval, history;
  };
  inline constexpr VolumeResponse volumeResponses[] = {
      {"Smooth", 8, 16}, {"Balanced", 8, 8}, {"Fast", 4, 6}, {"Immediate", 2, 4}};

  // Volumetrics in the last traced frame (diagnostics).
  struct VolumeState {
    bool requested = false, active = false;
    const char *reason = "", *source = "none";
    uint32_t cells = 0, levels = 0;
    float visibilityMeters = 0, reachMeters = 0;
    bool raster = false; // TraceOptions::volumetricRaster
    uint32_t glowLights = 0; // raster fog: lights with a shadow cube this frame
  };
  inline VolumeState g_volumeState;

  // Raster fog (TraceOptions::volumetricRaster): the frame's volume fields
  // without grids. volumeConfig x 0 (no traced medium), y 1; volumeMedium
  // the linear fog colour times volumetricRasterBrightness; volumeParams:
  // start (world units), 1 / (end - start), density per world unit, mode
  // (D3D: 1 exp, 2 exp2, 3 linear); volumeMedium.w the scattering of the
  // glow around local lights: the traced medium's extinction (with
  // volumetricFogDensityScale) times volumetricScattering, so the glow is as
  // bright as the traced medium's would be. lightScales: volumeLightScales.
  // The game's linear fog comes as D3D's:
  // `scale` 1 / (end - start). The override is exponential fog that leaves
  // 5% at volumetricDistance. Fills `state`; false: no fog this frame.
  inline bool rasterFog(const TraceOptions &options, const GameFog &fog, uint32_t config[4], float params[4],
                        float medium[4], float lightScales[4], VolumeState &state) {
    state = {true, false, "the game sets no fog and the override is off", "none"};
    state.raster = true;
    float color[3], start = 0, scale = 0, density = 0, full = 0;
    uint32_t mode;
    const float reach = -std::log(volumeVisibility);
    if (options.volumetricOverride) {
      mode = 1;
      density = reach / (std::max(options.volumetricDistance, .01f) / options.sceneScale);
      std::copy_n(options.volumetricColor, 3, color);
      full = reach / density;
      state.source = "override";
    } else {
      mode = fog.mode;
      if ((mode == 1 || mode == 2) && fog.density > 0) {
        density = fog.density;
        full = (mode == 1 ? reach : std::sqrt(reach)) / density;
      } else if (mode == 3 && fog.end > 0) {
        scale = fog.scale > 0 ? fog.scale : 1 / fog.end;
        start = fog.end - 1 / scale;
        full = fog.end;
      } else
        return false;
      for (int c = 0; c < 3; ++c)
        color[c] = volumeLinear(std::clamp(fog.color[c], 0.f, 1.f));
      state.source = "game fog";
    }
    if (!std::isfinite(density) || !std::isfinite(scale) || !std::isfinite(start))
      return false;
    const float brightness = std::max(options.volumetricRasterBrightness, 0.f);
    std::fill_n(config, 4, 0u);
    config[1] = 1;
    for (int c = 0; c < 3; ++c)
      medium[c] = std::max(color[c], 0.f) * brightness;
    const auto lit = volumeMedium(options, fog);
    medium[3] = lit.active ? lit.extinction * std::max(options.volumetricScattering, 0.f) : 0;
    const float scales[4] = {options.volumetricSunScale, options.volumetricLightScale, options.volumetricEmissiveScale,
                             options.volumetricSkyScale};
    for (int i = 0; i < 4; ++i)
      lightScales[i] = std::max(scales[i], 0.f);
    params[0] = start;
    params[1] = scale;
    params[2] = density;
    params[3] = float(mode);
    state.active = true;
    state.reason = "";
    state.visibilityMeters = full * options.sceneScale;
    return true;
  }

  // Raster fog's shadowed glow (volume.glsl rasterShadowTexel, pathtrace.comp
  // rasterFogGlow). The buffer at Frame::volumeGrid starts with one of these
  // per glowing light (rasterGlowSlots of them), then each light's cube of
  // distances: six faces of size^2 floats.
  struct RasterGlowLight {
    float centreReach[4] = {}; // world units
    uint32_t emitter = 0, unused[3] = {};
  };
  static_assert(sizeof(RasterGlowLight) == 32);
  inline constexpr uint32_t rasterGlowSlots = 64;
  inline uint32_t rasterGlowCount(const TraceOptions &options) {
    return std::min(options.volumetricRasterLights, rasterGlowSlots);
  }
  inline uint32_t rasterGlowSize(const TraceOptions &options) {
    return std::clamp(options.volumetricRasterShadowSize, 16u, 256u);
  }
  inline size_t rasterGlowBytes(uint32_t lights, uint32_t size) {
    return sizeof(RasterGlowLight) * rasterGlowSlots + size_t(lights) * 6 * size * size * sizeof(float);
  }

  // The sphere lights whose glow the camera sees most: each one's intensity
  // (its volumetric scale included) over its squared distance from the eye,
  // among those whose reach (its range, the local light range, at most
  // volumetricMaxDistance) comes within volumetricMaxDistance of the eye and
  // in front of it. Emissive triangles have no glow: there are too many to
  // give each a cube. `scored` is scratch kept by the caller.
  inline uint32_t rasterGlowLights(const TraceOptions &options, const std::vector<scene::LightEmitter> &emitters,
                                   const float eye[3], const float forward[3], uint32_t limit,
                                   std::vector<std::pair<float, uint32_t>> &scored, RasterGlowLight *out) {
    scored.clear();
    if (limit == 0 || options.volumetricLightScale <= 0)
      return 0;
    const float maxReach = std::max(options.volumetricMaxDistance, 1.f) / options.sceneScale;
    for (uint32_t i = 0; i < emitters.size(); ++i) {
      const auto &e = emitters[i];
      if (e.radiance[3] != 0 || e.corners[1][3] <= 0)
        continue;
      const float radius = std::max(e.positionRadius[3], 1e-3f);
      const float power = (.2126f * e.radiance[0] + .7152f * e.radiance[1] + .0722f * e.radiance[2]) * radius *
                          radius * e.corners[1][3];
      if (!(power > 0))
        continue;
      float reach = maxReach;
      if (e.corners[1][0] > 0)
        reach = std::min(reach, e.corners[1][0]);
      if (options.localRange > 0)
        reach = std::min(reach, options.localRange);
      float offset[3], distance2 = 0, ahead = 0;
      for (int c = 0; c < 3; ++c) {
        offset[c] = e.positionRadius[c] - eye[c];
        distance2 += offset[c] * offset[c];
        ahead += offset[c] * forward[c];
      }
      if (ahead < -reach || std::sqrt(distance2) - reach > maxReach)
        continue;
      scored.push_back({power / std::max(distance2, radius * radius), i});
    }
    const uint32_t count = uint32_t(std::min<size_t>(limit, scored.size()));
    std::partial_sort(scored.begin(), scored.begin() + count, scored.end(),
                      [](const auto &a, const auto &b) { return a.first > b.first; });
    for (uint32_t i = 0; i < count; ++i) {
      const auto &e = emitters[scored[i].second];
      float reach = maxReach;
      if (e.corners[1][0] > 0)
        reach = std::min(reach, e.corners[1][0]);
      if (options.localRange > 0)
        reach = std::min(reach, options.localRange);
      out[i] = {};
      std::copy_n(e.positionRadius, 3, out[i].centreReach);
      out[i].centreReach[3] = reach;
      out[i].emitter = scored[i].second;
    }
    return count;
  }
} // namespace remix::trace
