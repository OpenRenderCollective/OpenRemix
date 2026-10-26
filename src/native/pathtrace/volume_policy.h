#pragma once
#include "pathtrace/options.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>

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
      if (fog.mode == 1 && fog.density > 0)
        medium.extinction = fog.density;
      else if (fog.mode == 2 && fog.density > 0)
        medium.extinction = fog.density * std::sqrt(reach);
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
  };
  inline VolumeState g_volumeState;
} // namespace remix::trace
