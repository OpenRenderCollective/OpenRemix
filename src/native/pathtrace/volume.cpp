#include "pathtrace/gpu.h"
#include "pathtrace/volume_policy.h"

namespace remix {
  using namespace trace::detail;

  // Volumetrics (see TraceOptions::volumetrics and volume.glsl): the light the
  // medium scatters, in nested world-space grids around the camera. One buffer
  // holds every level's cells; an update pass relights a share of them each
  // frame and the transparency passes march camera rays through them.
  void PathTracer::Gpu::configureVolume(VkCommandBuffer command, Frame &frame, const TraceOptions &options) {
    auto &v = volume;
    frame.volumeGrid = 0;
    std::fill_n(frame.volumeConfig, 4, 0u);
    std::fill_n(frame.volumeFrame, 4, 0u);
    v.active = false;
    v.glowLights = 0;
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
    // (Re)allocates `cells` to `bytes`; false if that failed.
    auto allocate = [&](VkDeviceSize bytes) {
      try {
        // (Not the buffer's own size: that is rounded up, and one cell past a
        // round size the cells were allocated and emptied anew every frame.)
        if (!v.cells || v.bytes != bytes) {
          auto next = buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
          if (v.cells)
            retiredBuffers.push_back(std::move(v.cells));
          v.cells = std::move(next);
          v.bytes = bytes;
          v.valid = false;
          // Every cell starts without samples.
          vkCmdFillBuffer(command, v.cells->handle, 0, VK_WHOLE_SIZE, 0);
          barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        }
        return true;
      } catch (VkResult error) {
        if (error == VK_ERROR_DEVICE_LOST)
          throw;
        v.cells.reset();
        v.valid = false;
        return false;
      }
    };
    // Raster fog: constants (volume_policy.h rasterFog), and for its glow a
    // shadow cube per light, traced anew by the update pass every frame.
    if (options.volumetrics && options.volumetricRaster) {
      if (frame.splitLocal[3] || frame.sun[3] != 0 || options.sharcDebug || options.debugView) {
        trace::g_volumeState = {true, false, "a debug view is on", "none"};
        trace::g_volumeState.raster = true;
      } else if (trace::rasterFog(options, trace::gameFog(), frame.volumeConfig, frame.volumeParams,
                                  frame.volumeMedium, frame.volumeLightScales, trace::g_volumeState) &&
                 frame.volumeMedium[3] > 0 && frame.localConfig[0] && pipeline()) {
        trace::RasterGlowLight lights[trace::rasterGlowSlots];
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
        const uint32_t count = trace::rasterGlowLights(options, lightBvh.emitters(), frame.eye, forward,
                                                       trace::rasterGlowCount(options), v.glowScratch, lights);
        const uint32_t size = trace::rasterGlowSize(options);
        // Allocated for the most lights allowed, not this frame's count: the
        // count changes as lights come into view.
        if (count && allocate(trace::rasterGlowBytes(trace::rasterGlowCount(options), size))) {
          // Last frame's compose read the header; this frame's update reads
          // it after the copy.
          barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
          vkCmdUpdateBuffer(command, v.cells->handle, 0, sizeof(trace::RasterGlowLight) * count, lights);
          barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
          v.valid = false; // the buffer holds no traced medium's cells now
          v.glowLights = count;
          trace::g_volumeState.glowLights = count;
          v.glowSize = size;
          v.active = true;
          frame.volumeGrid = v.cells->address;
          frame.volumeConfig[2] = count;
          frame.volumeConfig[3] = size;
          frame.volumeFrame[0] = std::clamp(options.volumetricRasterSteps, 1u, 64u);
        }
      }
      v.inactiveReason = trace::g_volumeState.active ? "raster fog" : trace::g_volumeState.reason;
      return;
    }
    const auto medium = trace::volumeMedium(options, trace::gameFog());
    v.inactiveReason = !options.volumetrics ? "not requested"
                       : !medium.active     ? "the game sets no fog and the override is off"
                       : options.accumulate ? "accumulation is on"
                       : frame.splitLocal[3] || frame.sun[3] != 0 || options.sharcDebug || options.debugView
                           ? "a debug view is on"
                       : v.pipelineFailed ? "its pass failed to compile"
                                          : nullptr;
    const uint32_t cells = trace::volumeCells(options), levels = trace::volumeLevels(options);
    const float cellMeters = std::clamp(options.volumetricCellSize, .02f, 16.f);
    const float reachMeters = std::max(options.volumetricMaxDistance, cellMeters);
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
    const float cellUnits = cellMeters / options.sceneScale;
    // Every level's cells and the one for the medium beyond them, twice: what
    // they hold, and the environment's share of it (volume.glsl).
    const VkDeviceSize bytes = (VkDeviceSize(cells) * cells * cells * levels + 1) * 2 * 16;
    if (!allocate(bytes)) {
      trace::g_volumeState.reason = v.inactiveReason = "its cells could not be allocated";
      return;
    }
    // A buffer just allocated is empty (allocate clears `valid`).
    const bool reset = !v.valid || v.cellsPerSide != cells || v.levels != levels || v.cellUnits != cellUnits;
    v.cellsPerSide = cells;
    v.levels = levels;
    v.cellUnits = cellUnits;
    v.active = true;
    trace::g_volumeState.active = true;
    frame.volumeGrid = v.cells->address;
    frame.volumeConfig[0] = 1;
    frame.volumeConfig[1] = cells;
    frame.volumeConfig[2] = levels;
    frame.volumeConfig[3] = std::clamp(options.volumetricUpdateInterval, 1u, 64u);
    frame.volumeFrame[0] = v.counter++;
    frame.volumeFrame[1] = std::clamp(options.volumetricHistory, 1u, 256u);
    frame.volumeFrame[2] = std::clamp(options.volumetricSteps, 1u, 64u);
    frame.volumeFrame[3] = reset;
    frame.volumeParams[0] = cellUnits;
    frame.volumeParams[1] = reachMeters / options.sceneScale;
    frame.volumeParams[2] = std::max(options.volumetricScattering, 0.f);
    frame.volumeParams[3] = options.volumetricBaseHeight / options.sceneScale;
    const float scales[4] = {options.volumetricSunScale, options.volumetricLightScale, options.volumetricEmissiveScale,
                             options.volumetricSkyScale};
    for (unsigned i = 0; i < 4; ++i)
      frame.volumeLightScales[i] = std::max(scales[i], 0.f);
    std::copy_n(medium.albedo, 3, frame.volumeMedium);
    frame.volumeMedium[3] = medium.extinction;
    std::fill_n(frame.volumeHeight, 4, 0.f);
    frame.volumeHeight[std::min(options.volumetricUpAxis, 2u)] = 1;
    frame.volumeHeight[3] =
        options.volumetricHeightFalloff > 0 ? options.sceneScale / options.volumetricHeightFalloff : 0;
    std::copy_n(frame.eye, 3, frame.volumeEye);
    std::copy_n(reset ? frame.eye : v.eye, 3, frame.volumePrevious);
    frame.volumeEye[3] = frame.volumePrevious[3] = 0;
    std::copy_n(frame.eye, 3, v.eye);
    v.valid = true;
  }

  // After the light pools are presampled (the cells draw their local light
  // from them) and before any pass that marches the medium.
  void PathTracer::Gpu::recordVolume(VkCommandBuffer command) {
    auto &v = volume;
    if (!v.active)
      return;
    gpu::GpuScope scope(command, v.glowLights ? "Volumetrics: raster fog's shadow cubes"
                                              : "Volumetrics: light the medium's cells");
    // Queue-ordered across frame slots: last frame's marches have read the
    // cells before this frame's update writes them.
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    bindTrace(command, v.update, layout, defaultFeatures);
    // One thread a cell and one for the medium beyond them; or one a texel of
    // every glowing light's cube.
    const uint32_t count = v.glowLights ? v.glowLights * 6 * v.glowSize * v.glowSize
                                        : v.cellsPerSide * v.cellsPerSide * v.cellsPerSide * v.levels + 1;
    vkCmdDispatch(command, (count + 63) / 64, 1, 1);
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  }
} // namespace remix
