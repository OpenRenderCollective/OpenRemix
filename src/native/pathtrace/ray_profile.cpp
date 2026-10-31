#include "pathtrace/gpu.h"

namespace remix {
using namespace trace::detail;
void PathTracer::Gpu::beginRayProfile(VkCommandBuffer command, Frame &frame) {
  if (!rayProfile.path)
    return;
  if (!rayProfile.counters)
    rayProfile.counters = buffer(768,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 false);
  if (!rayProfile.readback)
    rayProfile.readback = buffer(768, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
  frame.reuseConfig[2] = uint32_t(rayProfile.counters->address);
  frame.reuseConfig[3] = uint32_t(rayProfile.counters->address >> 32);
  barrier(command,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  vkCmdFillBuffer(command, rayProfile.counters->handle, 0, 768, 0);
  barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}
void PathTracer::Gpu::finishRayProfile(VkCommandBuffer command) {
  if (!rayProfile.path)
    return;
  barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_TRANSFER_READ_BIT);
  VkBufferCopy copy{.size = 768};
  vkCmdCopyBuffer(command, rayProfile.counters->handle,
                  rayProfile.readback->handle, 1, &copy);
  barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
  rayProfile.pending = true;
}
void PathTracer::Gpu::completeRayProfile() {
  if (!rayProfile.pending)
    return;
  void *mapped{};
  check(vkMapMemory(device, rayProfile.readback->memory, 0, VK_WHOLE_SIZE, 0,
                    &mapped));
  VkMappedMemoryRange range{.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                            .memory = rayProfile.readback->memory,
                            .size = VK_WHOLE_SIZE};
  const auto result = vkInvalidateMappedMemoryRanges(device, 1, &range);
  uint32_t counts[192] = {};
  if (result == VK_SUCCESS)
    std::memcpy(counts, mapped, sizeof(counts));
  vkUnmapMemory(device, rayProfile.readback->memory);
  check(result);
  if (auto *file = std::fopen(rayProfile.path, "a")) {
    if (std::ftell(file) == 0) {
      std::fprintf(file, "time_ms");
      for (const char *pass : {"opaque", "direct", "transparency"})
        for (const char *ray : {"scene", "sun", "local"})
          for (const char *event :
               {"queries", "candidates", "alpha", "alpha_reject", "back_reject",
                "committed", "alpha_texture", "reserved"})
            std::fprintf(file, ",%s_%s_%s", pass, ray, event);
      for (const char *pass : {"opaque", "direct", "transparency"})
        for (const char *event : {"starts", "depth1", "depth2", "depth3", "depth4", "miss", "zero",
                                  "cache", "limit", "sheet_limit", "roulette_tests", "roulette_survivals", "roulette_terminated"})
          std::fprintf(file, ",%s_path_%s", pass, event);
      std::fprintf(file, "\n");
    }
    const auto time = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    std::fprintf(file, "%lld", static_cast<long long>(time));
    for (unsigned pass = 0; pass < 3; ++pass)
      for (unsigned i = 0; i < 24; ++i)
        std::fprintf(file, ",%u", counts[pass * 64 + i]);
    for (unsigned pass = 0; pass < 3; ++pass)
      for (unsigned i = 24; i < 37; ++i)
        std::fprintf(file, ",%u", counts[pass * 64 + i]);
    std::fprintf(file, "\n");
    std::fclose(file);
  }
  rayProfile.pending = false;
}
void PathTracer::Gpu::beginPathInspector(VkCommandBuffer command, Frame &frame) {
  auto &p = pathInspector;
  if (p.x < 0 || p.y < 0 || uint32_t(p.x) >= extent.width || uint32_t(p.y) >= extent.height)
    return;
  const VkDeviceSize size = 16 + VkDeviceSize(PathInspector::capacity) * 64;
  if (!p.records)
    p.records = buffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
  if (!p.readback)
    p.readback = buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
  frame.pathDebug[0] = uint32_t(p.records->address);
  frame.pathDebug[1] = uint32_t(p.records->address >> 32);
  frame.pathDebug[2] = uint32_t(p.x) | uint32_t(p.y) << 16;
  frame.pathDebug[3] = PathInspector::capacity;
  p.camera = frame.camera;
  p.width = extent.width;
  p.height = extent.height;
  barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_TRANSFER_WRITE_BIT);
  vkCmdFillBuffer(command, p.records->handle, 0, 16, 0);
  barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}
void PathTracer::Gpu::finishPathInspector(VkCommandBuffer command) {
  auto &p = pathInspector;
  if (p.x < 0 || !p.records || p.pending)
    return;
  barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_TRANSFER_READ_BIT);
  const VkBufferCopy copy{.size = p.records->size};
  vkCmdCopyBuffer(command, p.records->handle, p.readback->handle, 1, &copy);
  barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
          VK_ACCESS_HOST_READ_BIT);
  p.pending = true;
}
void PathTracer::Gpu::completePathInspector() {
  auto &p = pathInspector;
  if (!p.pending)
    return;
  p.pending = false;
  void *mapped{};
  check(vkMapMemory(device, p.readback->memory, 0, VK_WHOLE_SIZE, 0, &mapped));
  const VkMappedMemoryRange range{.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                  .memory = p.readback->memory, .size = VK_WHOLE_SIZE};
  const auto result = vkInvalidateMappedMemoryRanges(device, 1, &range);
  PathInspection inspection;
  if (result == VK_SUCCESS) {
    uint32_t count = 0;
    std::memcpy(&count, mapped, 4);
    inspection.records.resize(std::min(count, PathInspector::capacity));
    std::memcpy(inspection.records.data(), static_cast<const char *>(mapped) + 16,
                inspection.records.size() * sizeof(inspection.records[0]));
  }
  vkUnmapMemory(device, p.readback->memory);
  check(result);
  inspection.camera = p.camera;
  inspection.width = p.width;
  inspection.height = p.height;
  inspection.x = uint32_t(p.x);
  inspection.y = uint32_t(p.y);
  std::lock_guard lock(p.mutex);
  inspection.frame = p.last.frame + 1;
  p.last = std::move(inspection);
}
void PathTracer::inspectPixel(int x, int y) {
  auto &p = m_gpu->pathInspector;
  p.x = x;
  p.y = y;
  if (x < 0) {
    std::lock_guard lock(p.mutex);
    p.last = {};
  }
}
PathInspection PathTracer::pathInspection() const {
  std::lock_guard lock(m_gpu->pathInspector.mutex);
  return m_gpu->pathInspector.last;
}
} // namespace remix
