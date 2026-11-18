#pragma once
#include "pathtrace/options.h"
#include "scene/light_bvh.h"
#include "scene/volume_shaft.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <cstddef>
#include <utility>
#include <vector>

namespace remix::trace {
  // remixapi_DrawVolumeShaftsEXT: the shafts the traced view draws at most.
  inline constexpr uint32_t volumeShaftSlots = 32;

  // A shaft as the compose pass reads it (pathtrace.comp volumeShaftGlow), 5
  // vec4: origin and length; u and the near half extent along it; v and the
  // near half extent along it (oriented so cross(u, v) is the axis); radiance
  // and the far fade; the far half extents, softness and shape.
  inline void packVolumeShaft(const VolumeShaft &s, float (&out)[5][4]) {
    const auto &u = s.u, &a = s.axis;
    std::array<float, 3> v = s.v;
    const float cross[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    if (cross[0] * a[0] + cross[1] * a[1] + cross[2] * a[2] < 0)
      v = {-v[0], -v[1], -v[2]};
    const float rows[5][4] = {{s.origin[0], s.origin[1], s.origin[2], s.length},
                              {u[0], u[1], u[2], s.nearHalf[0]},
                              {v[0], v[1], v[2], s.nearHalf[1]},
                              {s.radiance[0], s.radiance[1], s.radiance[2], s.farFade},
                              {s.farHalf[0], s.farHalf[1], s.softness, float(s.shape)}};
    std::copy(&rows[0][0], &rows[0][0] + 20, &out[0][0]);
  }

  // The shafts drawn this frame: those not wholly behind the eye (along
  // `forward`), nearest first, at most volumeShaftSlots. Indices into `shafts`.
  inline std::vector<uint32_t> selectVolumeShafts(const std::vector<VolumeShaft> &shafts, const float eye[3],
                                                  const float forward[3]) {
    std::vector<std::pair<float, uint32_t>> ranked;
    ranked.reserve(shafts.size());
    for (uint32_t i = 0; i < shafts.size(); ++i) {
      const auto &s = shafts[i];
      // The bounding sphere: the centre of the axis, reaching the far corners.
      const float half = s.length * .5f;
      const float wide = std::max(std::hypot(s.nearHalf[0], s.nearHalf[1]), std::hypot(s.farHalf[0], s.farHalf[1]));
      const float radius = std::hypot(half, wide);
      float d[3];
      for (int c = 0; c < 3; ++c)
        d[c] = s.origin[c] + s.axis[c] * half - eye[c];
      if (d[0] * forward[0] + d[1] * forward[1] + d[2] * forward[2] < -radius)
        continue;
      ranked.emplace_back(std::max(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) - radius, 0.f), i);
    }
    const size_t keep = std::min<size_t>(ranked.size(), volumeShaftSlots);
    std::partial_sort(ranked.begin(), ranked.begin() + keep, ranked.end());
    std::vector<uint32_t> chosen;
    chosen.reserve(keep);
    for (size_t i = 0; i < keep; ++i)
      chosen.push_back(ranked[i].second);
    return chosen;
  }

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
    bool glowSun = false;    // raster fog: the sun glows this frame
    bool glowSky = false;    // raster fog: the sky glows this frame
  };
  inline VolumeState g_volumeState;

  // Each source's share of raster fog's glow (0 sun, 1 lights, 2 emissive
  // surfaces, 3 sky): its volumetric scale, 0 with its glow turned off.
  inline float rasterGlowScale(const TraceOptions &options, int source) {
    const bool on[4] = {options.volumetricRasterGlowSun, options.volumetricRasterGlowLights,
                        options.volumetricRasterGlowEmissive, options.volumetricRasterGlowSky};
    const float scale[4] = {options.volumetricSunScale, options.volumetricLightScale, options.volumetricEmissiveScale,
                            options.volumetricSkyScale};
    return on[source] ? std::max(scale[source], 0.f) : 0.f;
  }

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
    for (int i = 0; i < 4; ++i)
      lightScales[i] = rasterGlowScale(options, i);
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
  // rasterFogGlow). The buffer at Frame::volumeGrid starts with a
  // RasterGlowHeader, then each glowing light's cube of distances (six faces
  // of size^2 floats: the first `sharp` lights' of volumetricRasterShadowSize
  // texels a side, the rest's of volumetricRasterSoftSize), then the sun's
  // map (sunSize^2 floats), then the sky's maps (rasterSkyMaps of skySize^2).
  struct RasterGlowLight {
    float originReach[4] = {}; // the cube's centre and the light's reach (world units)
    // A sphere light: unused (its emitter has its radiance). Emissive
    // surfaces: their summed radiance times area (with their volumetric
    // scales), and 1 (one-sided) or 2 (two-sided).
    float intensityKind[4] = {};
    // Emissive surfaces: their area-weighted mean normal (unit) and its
    // length, how much they face one way (1 a flat panel, 0 all ways).
    float normalCoherence[4] = {};
    // A sphere light's emitter; where the cube's rays start; the radius that
    // softens 1 / d^2 to 1 / (d^2 + r^2).
    uint32_t emitter = 0;
    float minimum = 0, radius = 0, unused = 0;
  };
  static_assert(sizeof(RasterGlowLight) == 64);
  // An orthographic shadow map: square, centreExtent.w to each side of its
  // centre across `toward`, its rays starting rightHeight.w above the plane
  // and going against `toward`.
  struct RasterGlowMap {
    float centreExtent[4] = {}; // the map's centre, half its width (world units)
    float rightHeight[4] = {};  // its x axis; the rays' start above the centre
    float upUnused[4] = {};     // its y axis
    float toward[4] = {};       // towards the light (the sun, a part of the sky)
  };
  static_assert(sizeof(RasterGlowMap) == 64);
  // The sky as rasterSkyMaps parts of equal solid angle above the horizon: a
  // cap about the zenith (to 36.9 degrees from it) and four quarters of the
  // band below it. Each part's map looks along the part's mean direction.
  inline constexpr uint32_t rasterGlowSlots = 128, rasterSkyMaps = 5;
  struct RasterGlowHeader {
    RasterGlowLight lights[rasterGlowSlots];
    RasterGlowMap sun, sky[rasterSkyMaps];
    // Each sky part's mean radiance (volumetricSkyScale included): written
    // by the update pass (volume.glsl rasterSkyRadiance), not uploaded.
    float skyRadiance[rasterSkyMaps][4] = {};
    // The lights with a full-size cube (the first ones), the other lights'
    // cube size and samples, the sky maps' size (0: no sky glow).
    uint32_t sharp = 0, softSize = 0, softSteps = 0, skySize = 0;
    // How far camera rays take samples of the sun and the sky (world units);
    // 1: the sky is left clear of the sun's and the sky's glow
    // (volumetricRasterClearSky).
    float march = 0;
    uint32_t clearSky = 0, unused[2] = {};
  };
  // volume.glsl's offsets (in vec4s): the sun's map 512, the sky's 516, their
  // radiance 536, the counts 541; 543 in all.
  static_assert(offsetof(RasterGlowHeader, sun) == 512 * 16 && offsetof(RasterGlowHeader, sky) == 516 * 16 &&
                offsetof(RasterGlowHeader, skyRadiance) == 536 * 16 && offsetof(RasterGlowHeader, sharp) == 541 * 16 &&
                sizeof(RasterGlowHeader) == 543 * 16);

  // Emissive triangles as glowing lights: too many to give each a cube, so
  // those within one cell of a world grid (rasterClusterMeters) are summed into
  // one, at their area-weighted centre, facing their mean normal. Rebuilt
  // when the light tree changes.
  inline constexpr float rasterClusterMeters = 2;
  struct RasterGlowCluster {
    float centre[3] = {}, normal[3] = {}, intensity[3] = {};
    float area = 0, power = 0, coherence = 0;
    bool twoSided = false;
  };
  inline void rasterGlowClusters(const TraceOptions &options, const std::vector<scene::LightEmitter> &emitters,
                                 std::vector<RasterGlowCluster> &clusters) {
    clusters.clear();
    const float cell = rasterClusterMeters / options.sceneScale;
    struct Sum {
      double centre[3] = {}, normal[3] = {}, intensity[3] = {}, area = 0;
      bool twoSided = false;
    };
    std::vector<std::pair<uint64_t, uint32_t>> keys;
    std::vector<Sum> sums;
    for (const auto &e : emitters) {
      // Emissive surfaces store their volumetric scale negated less one.
      const float v = e.corners[1][3];
      if (e.radiance[3] == 0 || v >= -1)
        continue;
      const float scale = -1 - v;
      float edge1[3], edge2[3];
      for (int c = 0; c < 3; ++c) {
        edge1[c] = e.corners[1][c] - e.corners[0][c];
        edge2[c] = e.corners[2][c] - e.corners[0][c];
      }
      const float n[3] = {edge1[1] * edge2[2] - edge1[2] * edge2[1], edge1[2] * edge2[0] - edge1[0] * edge2[2],
                          edge1[0] * edge2[1] - edge1[1] * edge2[0]};
      const float twiceArea = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      if (!(twiceArea > 0) || !std::isfinite(twiceArea))
        continue;
      const float area = .5f * twiceArea;
      uint64_t key = 0;
      for (int c = 0; c < 3; ++c)
        key = key * 0x100000001b3ull ^ uint64_t(int64_t(std::floor(e.positionRadius[c] / cell)));
      keys.push_back({key, uint32_t(sums.size())});
      Sum sum;
      for (int c = 0; c < 3; ++c) {
        sum.centre[c] = double(e.positionRadius[c]) * area;
        sum.normal[c] = double(n[c]) * .5;
        sum.intensity[c] = double(e.radiance[c]) * area * scale;
      }
      sum.area = area;
      sum.twoSided = e.radiance[3] > 1;
      sums.push_back(sum);
    }
    // A cell's triangles are those of one key, gathered by sorting.
    std::sort(keys.begin(), keys.end());
    for (size_t i = 0; i < keys.size();) {
      Sum total;
      size_t j = i;
      for (; j < keys.size() && keys[j].first == keys[i].first; ++j) {
        const auto &t = sums[keys[j].second];
        for (int c = 0; c < 3; ++c) {
          total.centre[c] += t.centre[c];
          total.normal[c] += t.normal[c];
          total.intensity[c] += t.intensity[c];
        }
        total.area += t.area;
        total.twoSided |= t.twoSided;
      }
      i = j;
      RasterGlowCluster cluster;
      const double length = std::sqrt(total.normal[0] * total.normal[0] + total.normal[1] * total.normal[1] +
                                      total.normal[2] * total.normal[2]);
      for (int c = 0; c < 3; ++c) {
        cluster.centre[c] = float(total.centre[c] / total.area);
        cluster.normal[c] = length > 0 ? float(total.normal[c] / length) : 0;
        cluster.intensity[c] = float(total.intensity[c]);
      }
      cluster.area = float(total.area);
      cluster.coherence = total.twoSided ? 0 : float(std::min(length / total.area, 1.0));
      cluster.twoSided = total.twoSided;
      cluster.power = .2126f * cluster.intensity[0] + .7152f * cluster.intensity[1] + .0722f * cluster.intensity[2];
      if (cluster.power > 0 && std::isfinite(cluster.power))
        clusters.push_back(cluster);
    }
  }

  // The lights whose glow the camera sees most: each one's intensity (its
  // volumetric scales included) over its squared distance from the eye, among
  // those whose reach (a sphere light's range, the local light range, at most
  // volumetricMaxDistance) comes within volumetricMaxDistance of the eye and
  // in front of it. Sphere lights and emissive clusters (rasterGlowClusters)
  // compete for the same cubes. `scored` is scratch kept by the caller.
  inline uint32_t rasterGlowLights(const TraceOptions &options, const std::vector<scene::LightEmitter> &emitters,
                                   const std::vector<RasterGlowCluster> &clusters, const float eye[3],
                                   const float forward[3], uint32_t limit,
                                   std::vector<std::pair<float, uint32_t>> &scored, RasterGlowLight *out) {
    scored.clear();
    if (limit == 0)
      return 0;
    const float maxReach = std::max(options.volumetricMaxDistance, 1.f) / options.sceneScale;
    const float clusterReach = options.localRange > 0 ? std::min(maxReach, options.localRange) : maxReach;
    auto consider = [&](const float centre[3], float reach, float power, float radius, uint32_t index) {
      float distance2 = 0, ahead = 0;
      for (int c = 0; c < 3; ++c) {
        const float offset = centre[c] - eye[c];
        distance2 += offset * offset;
        ahead += offset * forward[c];
      }
      if (ahead < -reach || std::sqrt(distance2) - reach > maxReach)
        return;
      scored.push_back({power / std::max(distance2, radius * radius), index});
    };
    auto sphereReach = [&](const scene::LightEmitter &e) {
      float reach = maxReach;
      if (e.corners[1][0] > 0)
        reach = std::min(reach, e.corners[1][0]);
      if (options.localRange > 0)
        reach = std::min(reach, options.localRange);
      return reach;
    };
    if (const float lightScale = rasterGlowScale(options, 1); lightScale > 0)
      for (uint32_t i = 0; i < emitters.size(); ++i) {
        const auto &e = emitters[i];
        if (e.radiance[3] != 0 || e.corners[1][3] <= 0)
          continue;
        const float radius = std::max(e.positionRadius[3], 1e-3f);
        const float power = (.2126f * e.radiance[0] + .7152f * e.radiance[1] + .0722f * e.radiance[2]) * radius *
                            radius * 3.14159265f * e.corners[1][3] * lightScale;
        if (power > 0)
          consider(e.positionRadius, sphereReach(e), power, radius, i);
      }
    const uint32_t clusterFlag = 0x80000000u;
    if (const float emissiveScale = rasterGlowScale(options, 2); emissiveScale > 0)
      for (uint32_t i = 0; i < clusters.size(); ++i) {
        const auto &c = clusters[i];
        consider(c.centre, clusterReach, c.power * emissiveScale,
                 std::sqrt(c.area / 3.14159265f), i | clusterFlag);
      }
    const uint32_t count = uint32_t(std::min<size_t>(limit, scored.size()));
    std::partial_sort(scored.begin(), scored.begin() + count, scored.end(),
                      [](const auto &a, const auto &b) { return a.first > b.first; });
    const float cell = rasterClusterMeters / options.sceneScale;
    for (uint32_t i = 0; i < count; ++i) {
      auto &o = out[i];
      o = {};
      const uint32_t index = scored[i].second;
      if (index & clusterFlag) {
        const auto &c = clusters[index & ~clusterFlag];
        // A panel's cube starts just in front of it, so the panel itself and
        // the wall it is on do not hide it; surfaces facing all ways start at
        // their centre.
        const float lift = c.coherence > .5f ? .05f * cell : 0;
        for (int k = 0; k < 3; ++k) {
          o.originReach[k] = c.centre[k] + c.normal[k] * lift;
          o.intensityKind[k] = c.intensity[k];
          o.normalCoherence[k] = c.normal[k];
        }
        o.originReach[3] = clusterReach;
        o.intensityKind[3] = c.twoSided ? 2.f : 1.f;
        o.normalCoherence[3] = c.coherence;
        o.minimum = .01f * cell;
        o.radius = std::max(std::sqrt(c.area / 3.14159265f) * .5f, .01f * cell);
      } else {
        const auto &e = emitters[index];
        std::copy_n(e.positionRadius, 3, o.originReach);
        o.originReach[3] = sphereReach(e);
        o.emitter = index;
        o.radius = std::max(e.positionRadius[3], 1e-3f);
        // From the sphere's surface: a lamp's own bulb does not hide it.
        o.minimum = std::max(o.radius, 1e-4f * o.originReach[3]);
      }
    }
    return count;
  }

  // A shadow map for raster fog's glow (RasterGlowMap): `extent` to each side
  // of the eye across `to` (unit, towards the light), its texels fixed in the
  // world (the centre snapped to them) so shadow edges stay put as the camera
  // moves. Rays start 4 extents above the centre towards the light.
  inline void rasterGlowMap(const float eye[3], const float to[3], float extent, uint32_t size, RasterGlowMap &map) {
    // Any axis not along `to`, crossed with it.
    const bool alongY = std::abs(to[1]) > .9f;
    const float hint[3] = {alongY ? 1.f : 0.f, alongY ? 0.f : 1.f, 0};
    float right[3] = {hint[1] * to[2] - hint[2] * to[1], hint[2] * to[0] - hint[0] * to[2],
                      hint[0] * to[1] - hint[1] * to[0]};
    const float rl = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    for (auto &c : right)
      c /= rl;
    const float up[3] = {to[1] * right[2] - to[2] * right[1], to[2] * right[0] - to[0] * right[2],
                         to[0] * right[1] - to[1] * right[0]};
    const float texel = 2 * extent / float(size);
    float x = 0, y = 0;
    for (int c = 0; c < 3; ++c) {
      x += eye[c] * right[c];
      y += eye[c] * up[c];
    }
    const float dx = std::round(x / texel) * texel - x, dy = std::round(y / texel) * texel - y;
    map = {};
    for (int c = 0; c < 3; ++c) {
      map.centreExtent[c] = eye[c] + dx * right[c] + dy * up[c];
      map.rightHeight[c] = right[c];
      map.upUnused[c] = up[c];
      map.toward[c] = to[c];
    }
    map.centreExtent[3] = extent;
    map.rightHeight[3] = 4 * extent;
  }
  // The sun's map; false without a sun. `direction`: the way sunlight travels.
  inline bool rasterGlowSun(const TraceOptions &options, const float eye[3], const float direction[3],
                            const float radiance[4], uint32_t size, RasterGlowMap &sun) {
    if (rasterGlowScale(options, 0) <= 0 || radiance[3] <= 0 || !(radiance[0] + radiance[1] + radiance[2] > 0))
      return false;
    float to[3] = {-direction[0], -direction[1], -direction[2]};
    const float length = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
    if (!(length > 0) || !std::isfinite(length))
      return false;
    for (auto &c : to)
      c /= length;
    rasterGlowMap(eye, to, std::max(options.volumetricRasterSunExtent, 1.f) / options.sceneScale, size, sun);
    return true;
  }
  // The sky parts' directions (RasterGlowHeader): the zenith, then the band's
  // quarters about the axes after the up axis (+a, +b, -a, -b), at its mean
  // height (the cosine from the zenith 0.4, the middle of [0, 0.8]).
  inline void rasterSkyDirection(uint32_t part, uint32_t upAxis, float out[3]) {
    out[0] = out[1] = out[2] = 0;
    const uint32_t up = std::min(upAxis, 2u), a = (up + 1) % 3, b = (up + 2) % 3;
    if (part == 0) {
      out[up] = 1;
      return;
    }
    const float side = std::sqrt(1 - .4f * .4f);
    out[up] = .4f;
    out[part & 1 ? a : b] = part < 3 ? side : -side;
  }

  // Sizes of the glow's maps.
  inline uint32_t rasterGlowSize(const TraceOptions &options) {
    return std::clamp(options.volumetricRasterShadowSize, 16u, 256u);
  }
  inline uint32_t rasterSoftSize(const TraceOptions &options) {
    return std::clamp(options.volumetricRasterSoftSize, 4u, 256u);
  }
  inline uint32_t rasterSunSize(const TraceOptions &options) {
    return std::clamp(options.volumetricRasterSunSize, 64u, 2048u);
  }
  inline uint32_t rasterSkySize(const TraceOptions &options) {
    return std::clamp(options.volumetricRasterSkySize, 16u, 1024u);
  }
  // How many lights get a cube of each size, at most: the sharp, then soft.
  inline uint32_t rasterSharpCount(const TraceOptions &options) {
    return std::min(options.volumetricRasterLights, 64u);
  }
  inline uint32_t rasterSoftCount(const TraceOptions &options) {
    return std::min(options.volumetricRasterSoftLights, rasterGlowSlots - rasterSharpCount(options));
  }

  // A frame of raster fog's glow: what to upload, how big the buffer is and
  // how many threads the update pass takes (one a texel of every map, and
  // one a sky part for its radiance).
  struct RasterGlowFrame {
    uint32_t lights = 0, size = 0, sunSize = 0, steps = 0, sunSteps = 0;
    size_t bytes = 0;
    uint32_t threads = 0;
  };
  // The buffer is sized for the most lights allowed, not this frame's count:
  // the count changes as lights come into view.
  inline bool rasterGlowFrame(const TraceOptions &options, const std::vector<scene::LightEmitter> &emitters,
                              const std::vector<RasterGlowCluster> &clusters, bool localLights, const float eye[3],
                              const float forward[3], const float sunDirection[3], const float sunRadiance[4],
                              float march, std::vector<std::pair<float, uint32_t>> &scored,
                              RasterGlowHeader &header, RasterGlowFrame &out) {
    out = {};
    const uint32_t sharpLimit = rasterSharpCount(options), softLimit = rasterSoftCount(options);
    const uint32_t size = rasterGlowSize(options), softSize = rasterSoftSize(options);
    const uint32_t sunSize = rasterSunSize(options), skySize = rasterSkySize(options);
    const uint32_t count =
        localLights ? rasterGlowLights(options, emitters, clusters, eye, forward, sharpLimit + softLimit, scored,
                                       header.lights)
                    : 0;
    const bool sun = rasterGlowSun(options, eye, sunDirection, sunRadiance, sunSize, header.sun);
    const bool sky = rasterGlowScale(options, 3) > 0;
    if (sky) {
      const float extent = std::max(options.volumetricRasterSunExtent, 1.f) / options.sceneScale;
      for (uint32_t part = 0; part < rasterSkyMaps; ++part) {
        float to[3];
        rasterSkyDirection(part, options.volumetricUpAxis, to);
        rasterGlowMap(eye, to, extent, skySize, header.sky[part]);
      }
    }
    if (!count && !sun && !sky)
      return false;
    header.sharp = std::min(count, sharpLimit);
    header.softSize = softSize;
    header.softSteps = std::clamp(options.volumetricRasterSoftSteps, 1u, 64u);
    header.skySize = sky ? skySize : 0;
    header.march = march;
    header.clearSky = options.volumetricRasterClearSky;
    out.lights = count;
    out.size = size;
    out.sunSize = sun ? sunSize : 0;
    out.steps = std::clamp(options.volumetricRasterSteps, 1u, 64u);
    out.sunSteps = std::clamp(options.volumetricRasterSunSteps, 1u, 64u);
    const size_t sharpTexels = size_t(6) * size * size, softTexels = size_t(6) * softSize * softSize;
    const size_t mapTexels = size_t(sunSize) * sunSize + size_t(rasterSkyMaps) * skySize * skySize;
    out.bytes = sizeof(RasterGlowHeader) + (sharpLimit * sharpTexels + softLimit * softTexels + mapTexels) * 4;
    const size_t threads = header.sharp * sharpTexels + (count - header.sharp) * softTexels +
                           size_t(out.sunSize) * out.sunSize +
                           (sky ? size_t(rasterSkyMaps) * skySize * skySize + rasterSkyMaps : 0);
    out.threads = uint32_t(threads);
    return true;
  }
} // namespace remix::trace
