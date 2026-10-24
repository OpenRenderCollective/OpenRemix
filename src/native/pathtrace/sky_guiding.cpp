#include "pathtrace/gpu.h"

namespace remix {
using namespace trace::detail;

// Sky guiding (sky_guiding.glsl). Only the extra sky rays of a path's first
// surface sample the learned directions, so it is active with skySamples
// above one, a sky, and the denoiser (a learned proposal changes no
// expectation, but a progressive reference keeps plain BRDF sampling).
void PathTracer::Gpu::configureSkyGuiding(Frame &frame, const TraceOptions &options) {
  auto &g = skyGuiding;
  g.active = false;
  frame.skyGuideRead = frame.skyGuideWrite = 0;
  std::fill_n(frame.skyGuideConfig, 4, 0u);
  if (!options.skyGuiding || options.skySamples < 2 || frame.skyRadiance[3] == 0 || !options.sky ||
      !denoiser || options.accumulate)
    return;
  if (!g.attempted) {
    g.attempted = true;
    try {
      const auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      const VkDeviceSize bytes = VkDeviceSize(SkyGuiding::capacity) * SkyGuiding::stride * 4;
      auto first = buffer(bytes, usage);
      auto second = buffer(bytes, usage);
      g.history[0] = std::move(first);
      g.history[1] = std::move(second);
    } catch (VkResult error) {
      if (error == VK_ERROR_DEVICE_LOST)
        throw;
      std::fprintf(stderr, "Sky guiding unavailable (%d); using BRDF sampling\n", int(error));
    }
  }
  if (!g.history[0])
    return;
  const float cellUnits = options.skyGuideCellSize / options.sceneScale;
  if (history.reset() || g.cellSize != cellUnits)
    g.valid = false;
  g.cellSize = cellUnits;
  frame.skyGuideRead = g.valid ? g.history[g.writeIndex ^ 1]->address : 0;
  frame.skyGuideWrite = g.history[g.writeIndex]->address;
  const float scale = 1.f / cellUnits, share = std::clamp(options.skyGuideShare, 0.f, .95f);
  frame.skyGuideConfig[0] = SkyGuiding::capacity;
  std::memcpy(&frame.skyGuideConfig[1], &scale, 4);
  std::memcpy(&frame.skyGuideConfig[2], &share, 4);
  frame.skyGuideConfig[3] = 1;
  g.active = true;
}

void PathTracer::Gpu::beginSkyGuiding(VkCommandBuffer command) {
  if (!skyGuiding.active)
    return;
  // As beginLightGuiding: the read bank stays immutable for the frame; the
  // next bank is cleared after the prior submission's use, before training.
  barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  vkCmdFillBuffer(command, skyGuiding.history[skyGuiding.writeIndex]->handle, 0, VK_WHOLE_SIZE, 0);
  barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}
} // namespace remix
