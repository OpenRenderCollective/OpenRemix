#include "forward/ray/reflections.h"
#include "forward/bake/transport.h"
#include "forward/ray/shaders/embedded.h"
#include "forward/resources.h"
#include "platform/gpu_frames.h"
#include "runtime/pipelines.h"
#include <algorithm>

namespace remix::forward::ray {
  namespace {
    struct ScreenFrame {
      Matrix inverseCamera, view;
      Vec4 eye{}, trace{};
      std::array<uint32_t, 4> target{}, config{};
    };
    static_assert(sizeof(ScreenFrame) == 192);
    void computeBarrier(VkCommandBuffer command) {
      const VkMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                                    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
  } // namespace
  struct Reflections::Gpu {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline launch = VK_NULL_HANDLE, merge = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    uint32_t slot = 0;
    Statistics statistics;
    LiveScene *scene = nullptr;
    struct Frame {
      std::unique_ptr<Buffer> constants;
      VkDescriptorSet set = VK_NULL_HANDLE;
    };
    std::array<Frame, gpuFrameCount> frames;
    void createResources();
    ~Gpu() {
      if (pool)
        vkDestroyDescriptorPool(device, pool, nullptr);
      for (auto handle : {launch, merge})
        if (handle)
          pipeline::destroyPipeline(device, handle, nullptr);
      if (layout)
        pipeline::destroyPipelineLayout(device, layout, nullptr);
      if (setLayout)
        pipeline::destroyDescriptorSetLayout(device, setLayout, nullptr);
      if (sampler)
        pipeline::destroySampler(device, sampler, nullptr);
    }
  };
  Reflections::Reflections() = default;
  Reflections::~Reflections() = default;
  void Reflections::initialize(VkPhysicalDevice physical, VkDevice device, LiveScene &scene) {
    auto next = std::make_unique<Gpu>();
    next->physical = physical;
    next->device = device;
    next->statistics.available = true;
    next->scene = &scene;
    next->createResources();
    m_gpu = std::move(next);
  }
  void Reflections::Gpu::createResources() {
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    const VkDescriptorSetLayoutCreateInfo setInfo{.sType =
                                                      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                  .bindingCount = uint32_t(std::size(bindings)),
                                                  .pBindings = bindings};
    check(pipeline::createDescriptorSetLayout(device, &setInfo, nullptr, &setLayout));
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                .setLayoutCount = 1,
                                                .pSetLayouts = &setLayout,
                                                .pushConstantRangeCount = 1,
                                                .pPushConstantRanges = &push};
    check(pipeline::createPipelineLayout(device, &layoutInfo, nullptr, &layout));
    auto compute = [&](const uint32_t *code, size_t bytes) {
      const auto module = shaderModule(device, code, bytes);
      const VkComputePipelineCreateInfo info{
          .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
          .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module = module,
                    .pName = "main"},
          .layout = layout};
      VkPipeline result;
      const auto status =
          pipeline::createComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &result);
      pipeline::destroyShaderModule(device, module, nullptr);
      check(status);
      return result;
    };
    launch = compute(launchShader, sizeof(launchShader));
    merge = compute(mergeShader, sizeof(mergeShader));
    const VkSamplerCreateInfo sampleInfo{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                         .magFilter = VK_FILTER_NEAREST,
                                         .minFilter = VK_FILTER_NEAREST,
                                         .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                         .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                         .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                         .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    check(pipeline::createSampler(device, &sampleInfo, nullptr, &sampler));
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, gpuFrameCount},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, gpuFrameCount * 2},
                                          {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, gpuFrameCount * 2},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, gpuFrameCount}};
    const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              .maxSets = gpuFrameCount,
                                              .poolSizeCount = uint32_t(std::size(sizes)),
                                              .pPoolSizes = sizes};
    check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool));
    for (auto &frame : frames) {
      frame.constants = std::make_unique<Buffer>();
      frame.constants->create(physical, device, sizeof(ScreenFrame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
      const VkDescriptorSetAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                   .descriptorPool = pool,
                                                   .descriptorSetCount = 1,
                                                   .pSetLayouts = &setLayout};
      check(vkAllocateDescriptorSets(device, &allocation, &frame.set));
    }
  }
  void Reflections::selectFrame(uint32_t index) {
    m_gpu->slot = index;
  }
  void Reflections::record(VkCommandBuffer command, Inputs inputs, const FrameData &source,
                           const Options &options) {
    auto &g = *m_gpu;
    auto &frame = g.frames[g.slot];
    g.statistics = g.scene->statistics();
    auto *transport = g.scene->transport();
    if (!transport)
      return;
    const auto batch = transport->deviceBatch(g.slot);
    const ScreenFrame constants{
        scene::inverse(source.camera),
        source.view,
        source.cameraPosition,
        {options.rtReflectionDistance / options.sceneScale, .002f / options.sceneScale, options.ssrRoughness,
         0},
        {inputs.extent.width, inputs.extent.height, inputs.traceExtent.width, inputs.traceExtent.height},
        {options.ssrScale, batch.capacity, 0, 0}};
    frame.constants->write(&constants, sizeof(constants));
    const VkDescriptorBufferInfo buffers[] = {frame.constants->descriptor(), batch.points, batch.results};
    const VkDescriptorImageInfo images[] = {{g.sampler, inputs.normals, VK_IMAGE_LAYOUT_GENERAL},
                                            {g.sampler, inputs.depth, VK_IMAGE_LAYOUT_GENERAL},
                                            {VK_NULL_HANDLE, inputs.radiance, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[6]{};
    for (uint32_t i = 0; i < 6; ++i) {
      const bool buffer = i == 0 || i >= 4;
      writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                   .dstSet = frame.set,
                   .dstBinding = i,
                   .descriptorCount = 1,
                   .descriptorType = i == 0   ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                     : i >= 4 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                     : i == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                              : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                   .pImageInfo = buffer ? nullptr : &images[i - 1],
                   .pBufferInfo = buffer ? &buffers[i == 0 ? 0 : i - 3] : nullptr};
    }
    vkUpdateDescriptorSets(g.device, 6, writes, 0, nullptr);
    const uint64_t count64 = uint64_t(inputs.traceExtent.width) * inputs.traceExtent.height;
    if (count64 > UINT32_MAX)
      throw VK_ERROR_TOO_MANY_OBJECTS;
    const auto count = uint32_t(count64);
    g.statistics.rays = count;
    auto dispatch = [&](VkPipeline pipeline, uint32_t begin, uint32_t size, uint32_t invocations) {
      const uint32_t push[] = {begin, size};
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, g.layout, 0, 1, &frame.set, 0,
                              nullptr);
      vkCmdPushConstants(command, g.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
      vkCmdDispatch(command, (invocations + 63) / 64, 1, 1);
    };
    for (uint32_t begin = 0; begin < count;) {
      const auto size = std::min(batch.capacity, count - begin);
      // Each dispatch sees the same slot constants after submission. Pad the
      // last batch with inactive rays instead of rewriting its uniform count.
      dispatch(g.launch, begin, size, batch.capacity);
      computeBarrier(command);
      transport->recordDeviceRadiance(command, g.slot, batch.capacity, options.rtReflectionBounces);
      dispatch(g.merge, begin, size, size);
      computeBarrier(command);
      begin += size;
    }
  }
  const Statistics &Reflections::statistics() const {
    return m_gpu->statistics;
  }
  Reflections::Memory Reflections::memory() const {
    Memory memory;
    for (const auto &frame : m_gpu->frames)
      memory.buffers += frame.constants->allocationBytes;
    return memory;
  }
} // namespace remix::forward::ray
