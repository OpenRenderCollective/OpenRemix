#include "pathtrace/gpu.h"
#include "pathtrace/volume_policy.h"

#include <cstring>

namespace remix {
  using namespace trace::detail;

  // Volumetrics (see TraceOptions::volumetrics and volume.glsl): the light the
  // medium scatters, in nested world-space grids around the camera. One buffer
  // holds every level's cells; an update pass relights a share of them each
  // frame and the transparency passes march camera rays through them.
  void PathTracer::Gpu::configureVolume(VkCommandBuffer command, Frame &frame, const TraceOptions &options,
                                        bool litOverlays) {
    auto &v = volume;
    overlay.volumeCells = VK_NULL_HANDLE;
    frame.volumeGrid = 0;
    std::fill_n(frame.volumeConfig, 4, 0u);
    std::fill_n(frame.volumeFrame, 4, 0u);
    v.active = false;
    v.glowThreads = 0;
    v.spriteThreads = 0;
    // The update pass, compiled in the background: false until it is ready.
    auto pipeline = [&] {
      if (!v.update && !v.pipelineFailed) {
        try {
          createLightingPipelines({{trace::Stage::volumeUpdate, layout, &v.update, false, false, false, false}});
        } catch (VkResult error) {
          // Still compiling in the background: retry on a later frame.
          if (error == VK_ERROR_DEVICE_LOST)
            throw;
          if (error != VK_NOT_READY)
            v.pipelineFailed = true;
        }
      }
      return v.update != VK_NULL_HANDLE;
    };
    // (Re)allocates `cells` (requested `size`, holding valid cells `valid`)
    // to `bytes`; false if that failed.
    auto allocateIn = [&](std::unique_ptr<Buffer> &cells, VkDeviceSize &size, bool &valid, VkDeviceSize bytes) {
      try {
        // (Not the buffer's own size: that is rounded up, and one cell past a
        // round size the cells were allocated and emptied anew every frame.)
        if (!cells || size != bytes) {
          auto next = buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
          if (cells)
            retiredBuffers.push_back(std::move(cells));
          cells = std::move(next);
          size = bytes;
          valid = false;
          // Every cell starts without samples.
          vkCmdFillBuffer(command, cells->handle, 0, VK_WHOLE_SIZE, 0);
          barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        }
        return true;
      } catch (VkResult error) {
        if (error == VK_ERROR_DEVICE_LOST)
          throw;
        cells.reset();
        valid = false;
        return false;
      }
    };
    auto allocate = [&](VkDeviceSize bytes) { return allocateIn(v.cells, v.bytes, v.valid, bytes); };
    // The cells' sizes: for overlays alone few, large and seldom relit (a
    // sixth of a megabyte, some thousands of samples a frame): a puff of
    // smoke needs the light of the place it is in, not the medium's detail.
    auto cellsPerSide = [&](bool overlaysOnly) { return overlaysOnly ? 32u : trace::volumeCells(options); };
    auto cellLevels = [&](bool overlaysOnly) { return overlaysOnly ? 4u : trace::volumeLevels(options); };
    auto cellMetersOf = [&](bool overlaysOnly) {
      return std::clamp(overlaysOnly ? std::max(options.volumetricCellSize, .5f) : options.volumetricCellSize, .02f,
                        16.f);
    };
    // Lights a set of cells, held in `cells`, which `target`'s volume fields
    // then describe (marched: the medium's; else for LIT sprite overlays
    // alone); the overlays read them. False if they could not be allocated.
    auto lightCells = [&](Frame &target, std::unique_ptr<Buffer> &cellBuffer, VkDeviceSize &size, bool &valid,
                          bool marched, const auto &medium) {
      const bool overlaysOnly = !marched;
      const uint32_t cells = cellsPerSide(overlaysOnly), levels = cellLevels(overlaysOnly);
      const float cellMeters = cellMetersOf(overlaysOnly);
      const float reachMeters = std::max(options.volumetricMaxDistance, cellMeters);
      const float cellUnits = cellMeters / options.sceneScale;
      // Every level's cells and the one for the medium beyond them, twice:
      // what they hold, and the environment's share of it (volume.glsl).
      const VkDeviceSize bytes = (VkDeviceSize(cells) * cells * cells * levels + 1) * 2 * 16;
      if (!allocateIn(cellBuffer, size, valid, bytes))
        return false;
      // A buffer just allocated is empty (allocateIn clears `valid`).
      const bool reset = !valid || v.cellsPerSide != cells || v.levels != levels || v.cellUnits != cellUnits;
      v.cellsPerSide = cells;
      v.levels = levels;
      v.cellUnits = cellUnits;
      target.volumeGrid = cellBuffer->address;
      // Bit 0: a medium is marched; bit 1: the cells are lit for overlays alone.
      // Bit 2: the cells take reflected light (volumetricIndirect).
      target.volumeConfig[0] = (marched ? 1 : 2) | (options.volumetricIndirect ? 4 : 0);
      overlay.volumeCells = cellBuffer->handle;
      std::copy_n(frame.eye, 3, overlay.volumeEye);
      overlay.volumeCellUnits = cellUnits;
      overlay.volumeCellsPerSide = cells;
      overlay.volumeLevels = levels;
      target.volumeConfig[1] = cells;
      target.volumeConfig[2] = levels;
      target.volumeConfig[3] = overlaysOnly ? 16u : std::clamp(options.volumetricUpdateInterval, 1u, 64u);
      target.volumeFrame[0] = v.counter++;
      target.volumeFrame[1] = overlaysOnly ? 8u : std::clamp(options.volumetricHistory, 1u, 256u);
      target.volumeFrame[2] = std::clamp(options.volumetricSteps, 1u, 64u);
      target.volumeFrame[3] = reset;
      target.volumeParams[0] = cellUnits;
      target.volumeParams[1] = reachMeters / options.sceneScale;
      target.volumeParams[2] = std::max(options.volumetricScattering, 0.f);
      target.volumeParams[3] = options.volumetricBaseHeight / options.sceneScale;
      const float scales[4] = {options.volumetricSunScale, options.volumetricLightScale,
                               options.volumetricEmissiveScale, options.volumetricSkyScale};
      for (unsigned i = 0; i < 4; ++i)
        target.volumeLightScales[i] = std::max(scales[i], 0.f);
      std::copy_n(medium.albedo, 3, target.volumeMedium);
      target.volumeMedium[3] = medium.extinction;
      std::fill_n(target.volumeHeight, 4, 0.f);
      target.volumeHeight[std::min(options.volumetricUpAxis, 2u)] = 1;
      target.volumeHeight[3] =
          options.volumetricHeightFalloff > 0 ? options.sceneScale / options.volumetricHeightFalloff : 0;
      std::copy_n(frame.eye, 3, target.volumeEye);
      std::copy_n(reset ? frame.eye : v.eye, 3, target.volumePrevious);
      target.volumeEye[3] = target.volumePrevious[3] = 0;
      std::copy_n(frame.eye, 3, v.eye);
      valid = true;
      return true;
    };
    // Raster fog: constants (volume_policy.h rasterFog), and for its glow a
    // shadow cube per light, traced anew by the update pass every frame.
    if (options.volumetrics && options.volumetricRaster) {
      if (frame.splitLocal[3] || frame.sun[3] != 0 || options.sharcDebug || options.debugView) {
        trace::g_volumeState = {true, false, "a debug view is on", "none"};
        trace::g_volumeState.raster = true;
      } else if (trace::rasterFog(options, trace::gameFog(), frame.volumeConfig, frame.volumeParams,
                                  frame.volumeMedium, frame.volumeLightScales, trace::g_volumeState) &&
                 frame.volumeMedium[3] > 0 && pipeline()) {
        // The camera's axis: any depth of the screen's centre lies on it.
        const auto &m = frame.inverseCameraRelative;
        float forward[3], length = 0;
        for (int r = 0; r < 3; ++r) {
          forward[r] = (m[8 + r] * .5f + m[12 + r]) / (m[11] * .5f + m[15]);
          length += forward[r] * forward[r];
        }
        length = std::sqrt(length);
        if (length > 0 && std::isfinite(length))
          for (auto &f : forward)
            f /= length;
        if (frame.localConfig[0] &&
            (v.glowClustersInstance != lightBvh.instance() || v.glowClustersGeneration != lightBvh.generation)) {
          trace::rasterGlowClusters(options, lightBvh.emitters(), v.glowClusters);
          v.glowClustersInstance = lightBvh.instance();
          v.glowClustersGeneration = lightBvh.generation;
        }
        // Sun and sky samples reach as far as the fog leaves 5% of the light.
        trace::RasterGlowFrame glow;
        if (trace::rasterGlowFrame(options, lightBvh.emitters(), v.glowClusters, frame.localConfig[0] != 0,
                                   frame.eye, forward, frame.direction, frame.radiance,
                                   trace::g_volumeState.visibilityMeters / options.sceneScale, v.glowScratch,
                                   v.glowHeader, glow) &&
            allocate(glow.bytes)) {
          // Last frame's compose read the header; this frame's update reads
          // it after the copy. The sky's radiance is the update's to write.
          barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
          if (glow.lights)
            vkCmdUpdateBuffer(command, v.cells->handle, 0, sizeof(trace::RasterGlowLight) * glow.lights,
                              v.glowHeader.lights);
          constexpr size_t maps = offsetof(trace::RasterGlowHeader, sun),
                           radiance = offsetof(trace::RasterGlowHeader, skyRadiance),
                           counts = offsetof(trace::RasterGlowHeader, sharp);
          vkCmdUpdateBuffer(command, v.cells->handle, maps, radiance - maps, &v.glowHeader.sun);
          vkCmdUpdateBuffer(command, v.cells->handle, counts, sizeof(trace::RasterGlowHeader) - counts,
                            &v.glowHeader.sharp);
          barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
          v.valid = false; // the buffer holds no traced medium's cells now
          v.glowThreads = glow.threads;
          v.active = true;
          trace::g_volumeState.glowLights = glow.lights;
          trace::g_volumeState.glowSun = glow.sunSize != 0;
          trace::g_volumeState.glowSky = v.glowHeader.skySize != 0;
          frame.volumeGrid = v.cells->address;
          frame.volumeConfig[2] = glow.lights;
          frame.volumeConfig[3] = glow.size;
          frame.volumeFrame[0] = glow.steps;
          frame.volumeFrame[1] = glow.sunSize;
          frame.volumeFrame[2] = glow.sunSteps;
        }
      }
      v.inactiveReason = trace::g_volumeState.active ? "raster fog" : trace::g_volumeState.reason;
      // LIT sprite overlays take their light from cells, which raster fog
      // does not keep (its buffer holds its glow's maps and the frame's
      // volume fields its constants): theirs are lit apart, by a second
      // dispatch that sees their fields (recordVolume). Not with a debug view
      // or accumulation, as the medium's cells.
      const bool fogFields = frame.volumeConfig[0] == 0 && frame.volumeConfig[1] == 1;
      if (litOverlays && !options.accumulate && !frame.splitLocal[3] && frame.sun[3] == 0 && !options.sharcDebug &&
          !options.debugView && pipeline()) {
        const auto medium = trace::volumeMedium(options, trace::gameFog());
        if (!fogFields) {
          // Raster fog draws nothing this frame (no fog): the cells as without it.
          if (lightCells(frame, v.spriteCells, v.spriteBytes, v.spriteValid, false, medium)) {
            v.active = true;
            v.spriteThreads = 0;
          }
        } else {
          Frame sprites = frame;
          if (lightCells(sprites, v.spriteCells, v.spriteBytes, v.spriteValid, false, medium)) {
            constexpr size_t first = offsetof(Frame, volumeGrid);
            static_assert(offsetof(Frame, volumePrevious) + sizeof(Frame::volumePrevious) - first ==
                          sizeof(Volume::spriteFields));
            std::memcpy(v.spriteFields, reinterpret_cast<const uint8_t *>(&sprites) + first, sizeof v.spriteFields);
            std::memcpy(v.fogFields, reinterpret_cast<const uint8_t *>(&frame) + first, sizeof v.fogFields);
            std::copy_n(sprites.volumeLightScales, 4, v.spriteScales);
            std::copy_n(frame.volumeLightScales, 4, v.fogScales);
            v.spriteThreads = v.cellsPerSide * v.cellsPerSide * v.cellsPerSide * v.levels + 1;
            v.active = true;
          }
        }
      }
      return;
    }
    const auto medium = trace::volumeMedium(options, trace::gameFog());
    // Without a medium the cells are still lit while sprite overlays take
    // their light from them (REMIXAPI_SPRITE_LIT_EXT): nothing marches them.
    const bool marched = options.volumetrics && medium.active;
    v.inactiveReason = !marched && !litOverlays
                           ? (!options.volumetrics ? "not requested" : "the game sets no fog and the override is off")
                       : options.accumulate ? "accumulation is on"
                       : frame.splitLocal[3] || frame.sun[3] != 0 || options.sharcDebug || options.debugView
                           ? "a debug view is on"
                       : v.pipelineFailed ? "its pass failed to compile"
                                          : nullptr;
    const bool overlaysOnly = !marched;
    const uint32_t cells = cellsPerSide(overlaysOnly), levels = cellLevels(overlaysOnly);
    const float reachMeters = std::max(options.volumetricMaxDistance, cellMetersOf(overlaysOnly));
    trace::g_volumeState = {options.volumetrics,
                            false,
                            v.inactiveReason ? v.inactiveReason : "",
                            medium.source,
                            cells,
                            levels,
                            medium.active ? -std::log(trace::volumeVisibility) / medium.extinction * options.sceneScale
                                          : 0,
                            reachMeters};
    if (v.inactiveReason)
      return;
    if (!pipeline()) {
      trace::g_volumeState.reason = v.inactiveReason =
          v.pipelineFailed ? "its pass failed to compile" : "its pass is compiling";
      return;
    }
    if (!lightCells(frame, v.cells, v.bytes, v.valid, marched, medium)) {
      trace::g_volumeState.reason = v.inactiveReason = "its cells could not be allocated";
      return;
    }
    v.active = true;
    trace::g_volumeState.active = true;
  }

  // After the light pools are presampled (the cells draw their local light
  // from them) and before any pass that marches the medium.
  void PathTracer::Gpu::recordVolume(VkCommandBuffer command) {
    auto &v = volume;
    if (!v.active)
      return;
    gpu::GpuScope scope(command, v.glowThreads ? "Volumetrics: raster fog's shadow maps"
                                                                : "Volumetrics: light the medium's cells");
    // Queue-ordered across frame slots: last frame's marches have read the
    // cells before this frame's update writes them.
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    bindTrace(command, v.update, layout, defaultFeatures);
    auto dispatch = [&](uint32_t count) {
      const uint32_t groups = (count + 63) / 64;
      vkCmdDispatch(command, std::min(groups, 32768u), (groups + 32767) / 32768, 1);
    };
    // One a texel of every map of raster fog's glow (trace::rasterGlowFrame);
    // else one thread a cell and one for the medium beyond them.
    if (v.glowThreads)
      dispatch(v.glowThreads);
    else if (!v.spriteThreads)
      dispatch(v.cellsPerSide * v.cellsPerSide * v.cellsPerSide * v.levels + 1);
    if (v.spriteThreads) {
      // The sprites' cells under raster fog: the frame's volume fields are
      // theirs for this dispatch, raster fog's again after it.
      constexpr VkDeviceSize fields = offsetof(Frame, volumeGrid), scales = offsetof(Frame, volumeLightScales);
      auto swap = [&](const uint8_t *block, const float *lightScales) {
        barrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_UNIFORM_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdUpdateBuffer(command, uniform->handle, fields, sizeof v.spriteFields, block);
        vkCmdUpdateBuffer(command, uniform->handle, scales, sizeof v.spriteScales, lightScales);
        barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_UNIFORM_READ_BIT);
      };
      swap(v.spriteFields, v.spriteScales);
      dispatch(v.spriteThreads);
      swap(v.fogFields, v.fogScales);
    }
    // (The vertex stage: LIT sprite overlays read the cells.)
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  }
} // namespace remix
