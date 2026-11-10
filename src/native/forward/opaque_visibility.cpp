#include "forward/gpu.h"
#include "forward/shaders/embedded.h"
#include "platform/gpu_timers.h"
#include "runtime/pipelines.h"
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <algorithm>

namespace remix::forward {
  namespace {
    struct VisibilitySettings {
      Matrix camera;
      VkExtent2D extent;
      uint32_t boundCount;
    };
    static_assert(sizeof(VisibilitySettings) == 76);
    constexpr uint32_t tileSize = 32;
    constexpr uint32_t cullGroupSize = 64;
    uint32_t groups(uint32_t count, uint32_t size) { return (count + size - 1) / size; }
  }

  void Renderer::Gpu::createOpaqueVisibility() {
    VkDescriptorSetLayoutBinding bindings[4]{};
    for (uint32_t i = 0; i < 4; ++i)
      bindings[i] = {i, i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
          VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    const VkDescriptorSetLayoutCreateInfo setInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 4, .pBindings = bindings};
    check(pipeline::createDescriptorSetLayout(device, &setInfo, nullptr, &visibilitySetLayout));
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VisibilitySettings)};
    const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &visibilitySetLayout, .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push};
    check(pipeline::createPipelineLayout(device, &layoutInfo, nullptr, &visibilityLayout));
    auto compute = [&](const uint32_t *code, size_t bytes, const VkSpecializationInfo *specialization = nullptr) {
      const auto shader = shaderModule(device, code, bytes);
      const VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
          .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main",
                    .pSpecializationInfo = specialization}, .layout = visibilityLayout};
      VkPipeline result = VK_NULL_HANDLE;
      const auto status = pipeline::createComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &result);
      pipeline::destroyShaderModule(device, shader, nullptr);
      check(status);
      return result;
    };
    const uint32_t count = uint32_t(samples);
    const VkSpecializationMapEntry sampleEntry{0, 0, sizeof(count)};
    const VkSpecializationInfo sampleInfo{1, &sampleEntry, sizeof(count), &count};
    tileDepthPipeline = samples == VK_SAMPLE_COUNT_1_BIT
        ? compute(opaqueTilesSingleShader, sizeof(opaqueTilesSingleShader))
        : compute(opaqueTilesShader, sizeof(opaqueTilesShader), &sampleInfo);
    visibilityPipeline = compute(opaqueCullShader, sizeof(opaqueCullShader));
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, gpuFrameCount},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, gpuFrameCount * 3}};
    const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = gpuFrameCount, .poolSizeCount = 2, .pPoolSizes = sizes};
    check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &visibilityPool));
    const VkDescriptorSetAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = visibilityPool, .descriptorSetCount = 1, .pSetLayouts = &visibilitySetLayout};
    for (auto &frame : frames)
      check(vkAllocateDescriptorSets(device, &allocation, &frame.visibilitySet));
  }

  uint32_t Renderer::Gpu::prepareOpaqueVisibility() {
    auto &frame = frames[slot];
    // Small submissions use direct draws and need no indirect command list.
    constexpr size_t minimumVisibilityDraws = 256;
    // At 4K with measured water, direct submission avoids an extra
    // depth-to-color split before the later water pass reads those attachments.
    static const bool keepVisibility = std::getenv("REMIX_DEBUG_KEEP_WATER_OPAQUE_VISIBILITY") != nullptr;
    if (!keepVisibility && samples == VK_SAMPLE_COUNT_4_BIT && largeOpaqueTarget() && waterPass &&
        std::any_of(frame.waterCommands.begin(), frame.waterCommands.end(),
                    [](const auto &draw) { return draw.cameraVisible; }))
      return 0;
    if (!opaqueResumePass || frame.rayInputs.diffuse || frame.visibleOpaqueRanges < minimumVisibilityDraws ||
        !frame.visibleOpaqueBounds ||
        frame.visibleOpaqueRanges > maxStorageBufferRange / sizeof(VkDrawIndexedIndirectCommand))
      return 0;
    const size_t rangeCount = frame.visibleOpaqueRanges;
    const auto columns = groups(extent.width, tileSize), rows = groups(extent.height, tileSize);
    const VkDeviceSize tilesBytes = VkDeviceSize(columns) * rows * sizeof(float);
    const auto boundsBytes = sizeof(VisibilityBounds) * frame.visibleOpaqueBounds;
    const auto commandsBytes = sizeof(VkDrawIndexedIndirectCommand) * rangeCount;
    // Oversized submissions can still use ordinary depth and color draws.
    if (tilesBytes > maxStorageBufferRange || boundsBytes > maxStorageBufferRange ||
        frame.visibleOpaqueBounds > uint64_t(maxComputeGroupsX) * cullGroupSize)
      return 0;
    const auto ensureVisibilityBuffer = [&](std::unique_ptr<Buffer> &buffer, VkDeviceSize bytes,
                                            VkBufferUsageFlags usage, Buffer::Memory memory) {
      if (buffer && buffer->capacity >= bytes) return;
      frame.visibilityDescriptorsCurrent = false;
      ensure(buffer, bytes, usage, memory);
    };
    ensureVisibilityBuffer(frame.visibilityTiles, tilesBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           Buffer::Memory::Device);
    ensureVisibilityBuffer(frame.visibilityBounds, boundsBytes,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Buffer::Memory::Host);
    ensureVisibilityBuffer(frame.indirectDraws, commandsBytes,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                           Buffer::Memory::Host);
    // The frame slot fence has retired. Seed commands again because the GPU
    // can zero index counts in place, then flush each mapping once.
    auto *bounds = static_cast<std::byte *>(frame.visibilityBounds->mapped);
    auto *commands = static_cast<std::byte *>(frame.indirectDraws->mapped);
    uint32_t boundCount = 0, commandCount = 0;
    for (const auto &draw : frame.commands) {
      if (!draw.cameraVisible) continue;
      const auto firstCommand = commandCount;
      draw.forEachIndexRange(true, [&](const VkDrawIndexedIndirectCommand &range, auto) {
        assert(commandCount < rangeCount);
        std::memcpy(commands + size_t(commandCount++) * sizeof(range), &range, sizeof(range));
      });
      // Receiver runs occupy a contiguous range under one conservative bound.
      // A draw without a bound still emits commands and stays visible.
      const auto &box = draw.worldBounds;
      if (box.valid) {
        const VisibilityBounds bound{box.minimum, firstCommand, box.maximum, commandCount - firstCommand};
        assert(boundCount < frame.visibleOpaqueBounds);
        std::memcpy(bounds + size_t(boundCount++) * sizeof(bound), &bound, sizeof(bound));
      }
    }
    assert(boundCount == frame.visibleOpaqueBounds && commandCount == rangeCount);
    frame.visibilityBounds->flush();
    frame.indirectDraws->flush();
    // The depth target and sampler live as long as this renderer. Buffer
    // contents change per frame, but descriptors only change on allocation.
    if (frame.visibilityDescriptorsCurrent) return boundCount;
    const VkDescriptorImageInfo depthInfo{sampler, depth->view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo buffers[] = {frame.visibilityTiles->descriptor(),
        frame.visibilityBounds->descriptor(), frame.indirectDraws->descriptor()};
    VkWriteDescriptorSet writes[4] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = frame.visibilitySet, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &depthInfo}};
    for (uint32_t i = 1; i < 4; ++i)
      writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = frame.visibilitySet, .dstBinding = i, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffers[i - 1]};
    vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);
    frame.visibilityDescriptorsCurrent = true;
    return boundCount;
  }

  void Renderer::Gpu::recordOpaqueVisibility(VkCommandBuffer command, uint32_t boundCount) {
    const auto &frame = frames[slot];
    gpu::GpuScope time(command, "Forward+ opaque visibility", extent);
    const VisibilitySettings push{frame.data.camera, extent, boundCount};
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, visibilityLayout, 0, 1,
                            &frame.visibilitySet, 0, nullptr);
    vkCmdPushConstants(command, visibilityLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, tileDepthPipeline);
    vkCmdDispatch(command, groups(extent.width, tileSize), groups(extent.height, tileSize), 1);
    VkMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, visibilityPipeline);
    vkCmdDispatch(command, groups(push.boundCount, cullGroupSize), 1, 1);
    barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
  }
}
