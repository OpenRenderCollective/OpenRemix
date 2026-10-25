// Metal builds' copy of ui/lighting.cpp: the same tab, with what is Vulkan's read through
// the runtime's ui* queries (metal/ui/status.h). Port upstream changes to ui/lighting.cpp here.
#include "pathtrace/volume_policy.h"
#include "metal/runtime.h"
#include "ui/widgets.h"
#include <algorithm>
#include <iterator>
#include <cmath>
#include <imgui.h>
#include <utility>

namespace remix {
  void RemixNativeRuntime::drawLightingTab(config::Settings &settings) {
    auto &options = settings.trace;
    const bool rrActive = m_pathTracer && metal::denoises(m_pathTracer->upscalingStatistics().backend);
    if (ImGui::CollapsingHeader("Sun and sky", ImGuiTreeNodeFlags_DefaultOpen)) {
      ui::Checkbox("Override daylight", &options.overrideLight);
      if (options.overrideLight) {
        ui::SliderFloat3("Light direction", options.lightDirection, -1, 1);
        ui::ColorEdit3("Light color", options.lightColor);
        ui::SliderFloat("Light intensity", &options.lightIntensity, 0, 20);
      }
      bool overrideSize = options.sunAngularDiameter >= 0;
      if (ui::Checkbox("Override sun size", &overrideSize))
        options.sunAngularDiameter = overrideSize ? .533f : -1.f;
      if (overrideSize)
        ui::SliderFloat("Sun diameter (degrees)", &options.sunAngularDiameter, 0.f, 3.f, "%.3f");
      ui::Checkbox("Sky illumination", &options.sky);
      if (options.sky) {
        int rays = int(options.skySamples);
        if (ui::SliderInt("Sky rays per pixel", &rays, 1, 16))
          options.skySamples = uint32_t(rays);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Sky visibility rays at each pixel's surface, averaged. Indoors few directions\n"
                            "reach an opening, so one ray finds the sky rarely and brightly and the light\n"
                            "boils; more rays steady it. Each is one shadow ray.");
        int sheetRays = int(std::min(options.sheetSkySamples, options.skySamples));
        if (ui::SliderInt("Sky rays on translucent layers", &sheetRays, 1, int(options.skySamples)))
          options.sheetSkySamples = uint32_t(sheetRays);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Sky rays at glass and water surfaces, at most the rays above. Each lit layer\n"
                            "casts them on top of the surface behind it, so fewer keep glass cheap.");
        ImGui::BeginDisabled(options.skySamples < 2);
        ui::Checkbox("Sky guiding", &options.skyGuiding);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Each area learns which directions reach the sky (openings, windows) and the\n"
                            "extra sky rays aim there. Diffuse and legacy surfaces; needs 2 or more rays.");
        if (options.skyGuiding) {
          ui::SliderFloat("Guided share", &options.skyGuideShare, 0.f, .95f, "%.2f");
          ui::DragFloat("Sky guide cell size", &options.skyGuideCellSize, .01f, .05f, 100, "%.2f m");
          options.skyGuideCellSize = std::clamp(options.skyGuideCellSize, .05f, 100.f);
        }
        ImGui::EndDisabled();
      }
    }
    if (ImGui::CollapsingHeader("Volumetrics")) {
      ui::Checkbox("Volumetric fog", &options.volumetrics);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Fog and haze lit by the sun, the sky and every local light. Uses the game's fog\n"
                          "unless the override below is on.");
      if (options.volumetrics) {
        ui::Checkbox("Override the game's fog", &options.volumetricOverride);
        if (options.volumetricOverride) {
          ui::SliderFloat("Sight distance (m)", &options.volumetricDistance, 1.f, 2000.f, "%.0f",
                          ImGuiSliderFlags_Logarithmic);
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How far one sees through the medium: 5%% of the light is left at this distance.");
          ui::ColorEdit3("Scattering color", options.volumetricColor);
        }
        ui::SliderFloat("Scattered light", &options.volumetricScattering, 0.f, 8.f, "%.2f");
        ui::SliderFloat("Height falloff (m)", &options.volumetricHeightFalloff, 0.f, 500.f, "%.1f");
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Above the base height the medium thins to 37%% over this height. 0: uniform.");
        if (options.volumetricHeightFalloff > 0) {
          ui::DragFloat("Base height (m)", &options.volumetricBaseHeight, .25f, -100000, 100000, "%.2f");
          int axis = int(std::min(options.volumetricUpAxis, 2u));
          if (ui::Combo("Up axis", &axis, "X\0Y\0Z\0"))
            options.volumetricUpAxis = uint32_t(axis);
        }
        ui::SliderFloat("March distance (m)", &options.volumetricMaxDistance, 10.f, 2000.f, "%.0f",
                        ImGuiSliderFlags_Logarithmic);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("The medium fills at most this much of a camera ray, the sky's included.");
        {
          const int count = int(std::size(trace::volumeQualities));
          int preset = count;
          for (int i = 0; i < count; ++i) {
            const auto &q = trace::volumeQualities[i];
            if (std::abs(options.volumetricCellSize - q.cellSize) < 1e-4f && options.volumetricCells == q.cells &&
                options.volumetricLevels == q.levels && options.volumetricSteps == q.steps)
              preset = i;
          }
          if (ui::Combo("Volumetric quality", &preset, "Low\0Medium\0High\0Ultra\0Custom\0") && preset < count) {
            const auto &q = trace::volumeQualities[preset];
            options.volumetricCellSize = q.cellSize;
            options.volumetricCells = q.cells;
            options.volumetricLevels = q.levels;
            options.volumetricSteps = q.steps;
          }
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How finely the medium's light is stored and sampled: sharper shadows in the fog and\n"
                              "smoother gradients from Low to Ultra, at more memory and GPU time.");
        }
        {
          const int count = int(std::size(trace::volumeResponses));
          int preset = count;
          for (int i = 0; i < count; ++i)
            if (options.volumetricUpdateInterval == trace::volumeResponses[i].interval &&
                options.volumetricHistory == trace::volumeResponses[i].history)
              preset = i;
          if (ui::Combo("Volumetric response", &preset, "Smooth\0Balanced\0Fast\0Immediate\0Custom\0") &&
              preset < count) {
            options.volumetricUpdateInterval = trace::volumeResponses[preset].interval;
            options.volumetricHistory = trace::volumeResponses[preset].history;
          }
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How quickly the fog follows a change of lighting. Faster settings trace more rays\n"
                              "each frame and flicker more.");
        }
        if (ImGui::TreeNode("Volumetric detail")) {
          ui::SliderFloat("Cell size (m)", &options.volumetricCellSize, .05f, 2.f, "%.2f");
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The finest grid's cells: shadows in the fog are as sharp as these.");
          int cells = int(options.volumetricCells), levels = int(options.volumetricLevels);
          if (ui::SliderInt("Cells per side", &cells, 16, 96))
            options.volumetricCells = uint32_t(cells) & ~1u;
          if (ui::SliderInt("Grid levels", &levels, 1, 6))
            options.volumetricLevels = uint32_t(levels);
          int steps = int(options.volumetricSteps), interval = int(options.volumetricUpdateInterval),
              history = int(options.volumetricHistory);
          if (ui::SliderInt("Samples per camera ray", &steps, 1, 64))
            options.volumetricSteps = uint32_t(steps);
          if (ui::SliderInt("Frames between a cell's updates", &interval, 1, 64))
            options.volumetricUpdateInterval = uint32_t(interval);
          if (ui::SliderInt("Samples a cell averages", &history, 1, 256))
            options.volumetricHistory = uint32_t(history);
          const float cellSize = std::clamp(options.volumetricCellSize, .02f, 16.f);
          const uint32_t side = trace::volumeCells(options), count = trace::volumeLevels(options);
          ImGui::TextWrapped("Grids span %.0f m to %.0f m; %.1f MiB; lighting settles in about %u frames",
                             cellSize * side, cellSize * side * float(1u << (count - 1)),
                             double(side) * side * side * count * 32 / (1024 * 1024),
                             std::clamp(options.volumetricUpdateInterval, 1u, 64u) *
                                 std::clamp(options.volumetricHistory, 1u, 256u));
          ImGui::TreePop();
        }
        const auto volume = trace::g_volumeState;
        if (volume.active)
          ImGui::TextWrapped("Volumetrics: on (%s), sight %.1f m", volume.source, volume.visibilityMeters);
        else
          ImGui::TextWrapped("Volumetrics: off (%s)", volume.reason);
      }
    }
    if (ImGui::CollapsingHeader("Local lighting")) {
      ui::Checkbox("Local lights", &options.localLights);
      ui::Checkbox("Torch lights", &options.torchLights);
      ui::Checkbox("Emissive lighting", &options.emissiveLights);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Controls light cast onto the world. Emissive textures remain visible.");
      {
        int cluster = int(std::clamp(options.emissiveClusterSize, 1u, 64u));
        if (ui::SliderInt("Emissive triangles per light cluster", &cluster, 1, 64))
          options.emissiveClusterSize = uint32_t(cluster);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Neighbouring glowing triangles of a surface are grouped (1: none). Each keeps its\n"
                            "shape and color; larger groups make the light tree cheaper to update and walk, at\n"
                            "some noise close to large glowing surfaces.");
      }
      ui::Checkbox("Adaptive local-light sampling", &options.adaptiveLocalSampling);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Smoothly raises the local-light sample budget toward four near important light "
                          "clusters. Configured sample and emissive candidate counts remain minimums.");
      ui::Checkbox("Stabilize animated emissive lighting", &options.stabilizeEmissiveLighting);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Experimental: smooths animation-driven cast light over half a second. Visible emissive textures "
            "keep animating.\nCombined lighting and the shadow-ray budget are preserved.");
      ui::Checkbox("Improve nearby emissive lighting", &options.emissiveSolidAngleSampling);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Experimental: samples nearby area lights with the same shadow-ray count.\nAdds "
                          "some GPU work; disable to compare performance.");
      int candidates = int(options.emissiveCandidates);
      if (ui::SliderInt("Emissive candidates", &candidates, 1, 8))
        options.emissiveCandidates = uint32_t(candidates);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Compares unshadowed candidates at opaque primary surfaces in larger emissive light sets.\nKeeps "
            "one shadow ray per local light sample; 1 restores basic BVH sampling.");
      int sphereCandidates = int(options.sphereCandidates);
      if (ui::SliderInt("Light candidates (no emissives)", &sphereCandidates, 1, 8))
        options.sphereCandidates = uint32_t(sphereCandidates);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Scenes lit only by API lights: compares unshadowed candidates (spot cones included) before the\n"
            "one shadow ray. Helps a dominant nearby light (a flashlight) among many distant ones.");
      ImGui::BeginDisabled(rrActive);
      ui::Checkbox("Blue noise local lights", &options.blueNoiseLocalLights);
      ui::Checkbox("Blue noise bounce and sky directions", &options.blueNoiseSky);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Each pixel's first bounce and sky rays take directions from a blue-noise sequence:\n"
                          "neighbouring pixels and frames cover the sky evenly, which steadies the light\n"
                          "after denoising. No extra rays.");
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Uses blue noise for primary torch and emissive sample positions with local "
                          "denoising.\nPreserves candidate and shadow-ray budgets.");
      ImGui::BeginDisabled(rrActive);
      int separation = int(options.separateLocalLighting);
      const char *separationModes[] = {"Combined", "Separate - shared ray budget",
                                       "Separate - samples per family", "Combined - separate pass"};
      if (ui::Combo("Torch / emissive lighting", &separation, separationModes, 4))
        options.separateLocalLighting = uint32_t(separation);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Separate families use distinct direct sampling and denoiser histories.\nShared budget keeps the "
            "shadow-ray budget. Per-family mode can trace twice as many.\nCombined separate pass retains the "
            "combined sampler and budget in a dedicated dispatch.\nIndirect lighting remains combined.");
      ImGui::EndDisabled();
      if (rrActive)
        ImGui::TextWrapped("%s", "RR samples each pixel; shared family budgets use combined sampling.");
      int samples = int(options.localSamples);
      if (ui::SliderInt("Local light samples", &samples, 1, 16))
        options.localSamples = uint32_t(samples);
      ui::SliderFloat("Local light strength", &options.localIntensity, 0.f, 4.f);
      ui::SliderFloat("Light reach (0 = unlimited)", &options.localRange, 0.f, 64.f, "%.1f");
      if (options.localRange > 0 && options.localRange < .01f)
        options.localRange = .01f;
      ui::Checkbox("Per-frame radiance cache (ORCA)", &options.orca);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Traces full indirect paths for one pixel per tile and caches their first-bounce radiance\n"
                          "in voxels rebuilt every frame; other pixels read it after one ray. No lighting lag from\n"
                          "the cache itself. Combine with a shorter indirect history below.");
      if (options.orca) {
        int tile = int(options.orcaTile), levels = int(options.orcaLevels), minimum = int(options.orcaMinSamples);
        if (ui::SliderInt("ORCA tile (one path per tile)", &tile, 2, 8))
          options.orcaTile = uint32_t(tile);
        ui::SliderFloat("ORCA smallest voxel", &options.orcaCellSize, .01f, 1.f, "%.3f m");
        if (ui::SliderInt("ORCA voxel sizes", &levels, 1, 8))
          options.orcaLevels = uint32_t(levels);
        if (ui::SliderInt("ORCA minimum samples", &minimum, 1, 16))
          options.orcaMinSamples = uint32_t(minimum);
        ui::SliderFloat("ORCA maximum voxel error (0: any)", &options.orcaMaxError, 0.f, 2.f, "%.2f");
      }
      {
        int history = int(options.indirectHistoryFrames);
        if (ui::SliderInt("Indirect denoiser history (0 = default)", &history, 0, 63))
          options.indirectHistoryFrames = uint32_t(history);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Frames REBLUR averages bounce light over. The default is half a second of frames.\n"
                            "Longer hides more boiling; lighting changes take longer to settle.");
      }
      ui::Checkbox("Presampled light cells (ReGIR)", &options.regir);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Each world cell gets a pool of light samples drawn once per frame. Bounce lighting,\n"
                          "the SHARC update and ReSTIR candidates pick from it instead of walking the light tree.");
      if (options.regir)
        ui::SliderFloat("Light cell size", &options.regirCellSize, .125f, 16.f, "%.2f m");
      ui::Checkbox("ReSTIR DI (local lights)", &options.restir);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Reservoir resampling of primary local lighting: light samples are reused across\n"
                          "frames and neighboring pixels, and occluded ones discarded. Needs the NRD denoiser\n"
                          "(not used with DLSS Ray Reconstruction). Uses the separate local-lighting pass.");
      if (options.restir) {
        int candidates = int(options.restirCandidates), spatial = int(options.restirSpatialSamples),
            history = int(options.restirTemporalHistory);
        if (ui::SliderInt("ReSTIR candidates", &candidates, 1, 32))
          options.restirCandidates = uint32_t(candidates);
        if (ui::SliderInt("ReSTIR spatial samples", &spatial, 0, 8))
          options.restirSpatialSamples = uint32_t(spatial);
        ui::SliderFloat("ReSTIR spatial radius", &options.restirSpatialRadius, 1, 100, "%.0f px");
        if (ui::SliderInt("ReSTIR temporal history", &history, 0, 64))
          options.restirTemporalHistory = uint32_t(history);
        ui::Checkbox("ReSTIR initial visibility", &options.restirInitialVisibility);
        ui::Checkbox("ReSTIR permutation sampling", &options.restirPermutation);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Reuses a shuffled neighbor's history each frame: less temporally\n"
                            "correlated (frozen) noise for the denoiser.");
      }
      ui::Checkbox("Cull lights by visibility sets", &options.lightVisibilityCulling);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("When the host supplies its map's potentially visible sets, lights in rooms that\n"
                          "cannot be seen from a surface are never sampled for it.");
      ui::Checkbox("Guide by recent visibility", &options.localGuiding);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Guides opaque direct-light samples while retaining exploration.\nOne active light needs no "
            "visibility history.\nEmissive reservoirs use visible groups while checking shadows afresh.");
      ImGui::BeginDisabled(!options.localGuiding);
      ui::DragFloat("Guide cell size", &options.localGuideCellSize, .01f, .05f, 100, "%.2f m");
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Size of a visibility cell in meters (converted with the scene scale).");
      options.localGuideCellSize = std::clamp(options.localGuideCellSize, .05f, 100.f);
      const auto guiding = localLightGuidingStatistics();
      if (guiding.active)
        ImGui::TextWrapped("Guided %u / %u observed samples | %.1f MiB history", guiding.sampledGuided,
                           guiding.sampledQueries, double(guiding.memoryBytes) / (1024 * 1024));
      if (guiding.unavailableReason[0])
        ImGui::TextWrapped("%s", guiding.unavailableReason);
      const auto local = localLightStatistics();
      ImGui::TextWrapped("%u active / %u lights | tree depth %u", local.activeLights, local.lightCount,
                         local.treeDepth);
      ImGui::TextWrapped("BVH builds %llu / refits %llu | %.3f ms CPU", (unsigned long long)local.builds,
                         (unsigned long long)local.refits, local.cpuUpdateMilliseconds);
      const auto set = localLightSetStatistics();
      ImGui::TextWrapped("Static %u (+%u unlit slots) / dynamic %u (%u of %u dynamic instances) | dynamic part "
                         "rebuilt %llu times",
                         set.staticLights, set.retiredSlots, set.dynamicLights, set.dynamicInstanceLights,
                         set.dynamicEmittingInstances, (unsigned long long)set.dynamicBuilds);
      if (set.tableBuilds)
        ImGui::TextWrapped("Visibility table made %llu times, last %.3f ms | %.1f KiB",
                           (unsigned long long)set.tableBuilds, set.tableMilliseconds, double(set.tableBytes) / 1024);
      if (local.denoiserActive && local.gpuTimingsAvailable)
        ImGui::TextWrapped("Local light filter %.3f ms", local.gpuDenoiseMilliseconds);
      if (local.unavailableReason[0])
        ImGui::TextWrapped("%s", local.unavailableReason);
    }
    if (ImGui::CollapsingHeader("SHARC radiance cache")) {
      ui::Checkbox("SHARC", &options.sharc);
      ui::Checkbox("Show cached radiance (raw)", &options.sharcDebug);
      if (ImGui::Button("Reset cache"))
        ++m_sharcReset;
      int scale = int(options.sharcSceneScale), downscale = int(options.sharcDownscale);
      if (ui::SliderInt("Cache scene scale", &scale, 1, 200))
        options.sharcSceneScale = uint32_t(scale);
      if (ui::SliderInt("Update downscale", &downscale, 1, 16))
        options.sharcDownscale = uint32_t(downscale);
      int paths = int(options.sharcMaxUpdatePaths), motion = int(options.sharcMotionFrames);
      if (ui::SliderInt("Most update paths (0: no limit)", &paths, 0, 1000000))
        options.sharcMaxUpdatePaths = uint32_t(paths);
      if (ui::SliderInt("Frames averaged while objects move", &motion, 1, 64))
        options.sharcMotionFrames = uint32_t(motion);
      ui::SliderFloat("Update sample limit (x cell mean, 0: none)", &options.sharcFireflyClamp, 0, 100);
      ui::Checkbox("Cache on moving surfaces", &options.sharcDynamicSurfaces);
      ui::Checkbox("Cache on glossy surfaces", &options.sharcGlossySurfaces);
      ui::Checkbox("Footprint test on cache reads", &options.sharcFootprintTest);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("On: a path reads cached lighting only where it has spread wider than a cache cell.\n"
                          "Off: faster (fewer traced bounces), but coarse distant cells can read too dark.");
      auto cache = sharcStatistics();
      ImGui::TextWrapped("%s | %u / %u entries | %.1f MiB",
                         cache.active ? (cache.updatePaths ? "Caching" : "Enabled (idle)") : "Inactive",
                         cache.occupiedEntries, cache.capacity, double(cache.memoryBytes) / (1024 * 1024));
      ImGui::TextWrapped("Update paths %u | sampled query hits %.1f%% | resets %llu", cache.updatePaths,
                         cache.queryAttempts ? 100.0 * cache.queryHits / cache.queryAttempts : 0.0,
                         (unsigned long long)cache.resets);
      if (cache.gpuTimingsAvailable)
        ImGui::TextWrapped("Update %.3f / resolve %.3f ms", cache.gpuUpdateMilliseconds,
                           cache.gpuResolveMilliseconds);
      if (cache.unavailableReason[0])
        ImGui::TextWrapped("%s", cache.unavailableReason);
      if (cache.active && !cache.updatePaths)
        ImGui::TextWrapped("No cache update paths. Requires opaque surfaces the settings above let hold entries.");
      ImGui::TextWrapped("%s", options.sharcGlossySurfaces
                                   ? "Glossy PBR surfaces hold cached lighting as rough ones; paths that are still sharp trace on."
                                   : "Sharp PBR surfaces continue tracing; rough PBR surfaces can reuse cached lighting.");
      if (options.accumulate)
        ImGui::TextWrapped("%s", "Progressive reference rendering bypasses SHARC.");
    }
  }
} // namespace remix
