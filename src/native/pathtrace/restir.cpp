#include "pathtrace/gpu.h"

#include <unordered_map>

namespace remix {
using namespace trace::detail;

namespace {
  // Must match struct Reservoir in restir.glsl.
  constexpr VkDeviceSize kReservoirBytes = 48;
  struct PairHash {
    size_t operator()(const std::pair<uint64_t, uint32_t> &key) const {
      return std::hash<uint64_t>()(key.first * 0x9e3779b97f4a7c15ull ^ key.second);
    }
  };
} // namespace

// Per-frame ReSTIR DI state: reservoir buffers (this frame's initial and
// temporal result; the kept history ping-pong), the previous-to-current
// emitter index remap when the light set changed, and the frame constants.
void PathTracer::Gpu::configureRestir(Frame &frame, const TraceOptions &options, VkExtent2D extent) {
  auto &r = restir;
  const bool active = options.restir && r.initial && r.spatial && splitLightingActive && denoiser && postSplit &&
                      localIlluminationActive;
  frame.restirCurrent = frame.restirHistoryRead = frame.restirHistoryWrite = frame.restirRemap = 0;
  if (!active) {
    r.active = r.historyValid = false;
    r.keys.clear();
    return;
  }
  const VkDeviceSize bytes = VkDeviceSize(extent.width) * extent.height * kReservoirBytes;
  if (!r.current || r.width != extent.width || r.height != extent.height) {
    for (auto *b : {&r.current, &r.history[0], &r.history[1]}) {
      if (*b)
        retiredBuffers.push_back(std::move(*b));
      *b = buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    }
    r.width = extent.width;
    r.height = extent.height;
    r.historyValid = false;
  }
  if (history.reset())
    r.historyValid = false;
  // Emitter indices follow the sorted light list; when it changes, map the
  // history's indices to this frame's (none: the light is gone).
  const auto &lights = lightBvh.lights();
  bool same = r.keys.size() == lights.size();
  for (size_t i = 0; same && i < lights.size(); ++i)
    same = r.keys[i] == std::pair{lights[i].id, lights[i].triangle};
  if (!same) {
    if (r.historyValid && !r.keys.empty()) {
      std::unordered_map<std::pair<uint64_t, uint32_t>, uint32_t, PairHash> current;
      current.reserve(lights.size());
      for (size_t i = 0; i < lights.size(); ++i)
        current.emplace(std::pair{lights[i].id, lights[i].triangle}, uint32_t(i));
      std::vector<uint32_t> remap(r.keys.size(), 0xffffffffu);
      for (size_t i = 0; i < r.keys.size(); ++i)
        if (auto it = current.find(r.keys[i]); it != current.end())
          remap[i] = it->second;
      if (r.remap)
        retiredBuffers.push_back(std::move(r.remap));
      hostBuffer(r.remap, remap.data(), remap.size() * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      frame.restirRemap = r.remap->address;
    }
    r.keys.clear();
    r.keys.reserve(lights.size());
    for (const auto &light : lights)
      r.keys.emplace_back(light.id, light.triangle);
  }
  r.writeIndex ^= 1;
  frame.restirCurrent = r.current->address;
  frame.restirHistoryWrite = r.history[r.writeIndex]->address;
  frame.restirHistoryRead = r.historyValid ? r.history[r.writeIndex ^ 1]->address : 0;
  frame.restirConfig[0] = options.restirCandidates;
  frame.restirConfig[1] = std::min(options.restirSpatialSamples, 8u) | std::min(options.restirDisocclusionSamples, 8u) << 8;
  frame.restirConfig[2] = options.restirTemporalHistory;
  // Bit 2: re-test history samples for occluders that moved (local history
  // confidence, with the moved-light test of localHistoryConfidence).
  frame.restirConfig[3] = (options.restirInitialVisibility ? 1u : 0u) | (options.restirPermutation ? 2u : 0u) |
                          (options.localHistoryConfidence ? 4u : 0u);
  frame.restirParams[0] = options.restirSpatialRadius;
  frame.restirParams[1] = float(lights.size());
  frame.restirParams[2] = options.restirBoilingFilter;
  r.active = true;
  r.historyValid = true; // this frame writes it
}

// The split local pass's occlusion probes: each pixel's share of its local
// light that its shadow rays let through, compared the next frame to restart
// local history where an occluder moved. ReSTIR re-tests its own history
// samples instead.
void PathTracer::Gpu::configureLocalProbes(Frame &frame, const TraceOptions &options, VkExtent2D extent) {
  auto &p = localProbes;
  const bool active = options.localHistoryConfidence && splitLightingActive && denoiser && postSplit &&
                      localIlluminationActive && !restir.active;
  frame.localProbeRead = frame.localProbeWrite = 0;
  // Not probe settings; free slots.
  frame.localProbeParams[1] = options.shadowTerminatorSoften ? 1.f : 0.f;
  // The terminator lift's limits in world units (0 length: off).
  const float scale = std::max(options.sceneScale, 1e-6f), maxArea = options.shadowTerminatorMaxArea / (scale * scale);
  frame.localProbeParams[3] = options.shadowTerminatorLift ? options.shadowTerminatorMaxLength / scale : 0.f;
  std::memcpy(&frame.visibilityConfig[3], &maxArea, 4);
  frame.localProbeParams[2] = options.bounces > 0 ? std::min(options.emissiveSpecularRoughness, .45f) : 0.f;
  if (!active) {
    p.valid = false;
    return;
  }
  // Must match struct LocalProbe in pathtrace.comp.
  constexpr VkDeviceSize kProbeBytes = 32;
  if (!p.buffers[0] || p.width != extent.width || p.height != extent.height) {
    for (auto &b : p.buffers) {
      if (b)
        retiredBuffers.push_back(std::move(b));
      b = buffer(VkDeviceSize(extent.width) * extent.height * kProbeBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    }
    p.width = extent.width;
    p.height = extent.height;
    p.valid = false;
  }
  if (history.reset())
    p.valid = false;
  p.writeIndex ^= 1;
  frame.localProbeWrite = p.buffers[p.writeIndex]->address;
  frame.localProbeRead = p.valid ? p.buffers[p.writeIndex ^ 1]->address : 0;
  frame.localProbeParams[0] = options.movingShadowBlur;
  p.valid = true; // this frame writes it
}

// Replaces the split local-lighting dispatch: initial and temporal
// resampling, then spatial resampling and shading into the local signal.
void PathTracer::Gpu::recordRestir(VkCommandBuffer command, VkExtent2D extent) {
  const uint32_t x = (extent.width + 7) / 8, y = (extent.height + 7) / 8;
  {
    gpu::GpuScope scope(command, "ReSTIR DI: initial and temporal resampling", extent);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, restir.initial);
    vkCmdDispatch(command, x, y, 1);
  }
  barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  {
    gpu::GpuScope scope(command, "ReSTIR DI: spatial resampling and shading", extent);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, restir.spatial);
    vkCmdDispatch(command, x, y, 1);
  }
}
} // namespace remix
