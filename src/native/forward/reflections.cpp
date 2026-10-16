#include "forward/reflections.h"
#include "forward/image_dump.h"
#include "forward/probes/capture_dump.h"
#include "forward/ray/reflections.h"
#include "forward/resources.h"
#include "forward/shaders/embedded.h"
#include "platform/gpu_frames.h"
#include "runtime/pipelines.h"
#include <algorithm>
#include <bit>
#include <vector>

namespace remix::forward {
  namespace {
    struct ReflectionFrame {
      Matrix camera, inverseCamera, view;
      Vec4 cameraPosition, trace;
      std::array<uint32_t, 4> config, target;
    };
    static_assert(sizeof(ReflectionFrame) == 256);

    VkDescriptorType descriptorType(uint32_t binding) {
      if (binding == 0 || binding == 14)
        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      if (binding == 13 || binding == 16)
        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      if (binding >= 8 && binding <= 11)
        return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    void imageBarrier(VkCommandBuffer command, VkImage image, uint32_t levels,
                      VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage,
                      VkAccessFlags sourceAccess, VkAccessFlags destinationAccess,
                      VkImageLayout oldLayout = VK_IMAGE_LAYOUT_GENERAL) {
      const VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                         .srcAccessMask = sourceAccess,
                                         .dstAccessMask = destinationAccess,
                                         .oldLayout = oldLayout,
                                         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .image = image,
                                         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1}};
      vkCmdPipelineBarrier(command, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
  } // namespace
  struct Reflections::Gpu {
    VkDevice device = VK_NULL_HANDLE;
    VkExtent2D extent{}, traceExtent{};
    uint32_t levels = 1, samples = 1, scale = 1;
    uint64_t imageBytes = 0, allocationBytes = 0;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline resolve = VK_NULL_HANDLE, reduce = VK_NULL_HANDLE, trace = VK_NULL_HANDLE,
               filter = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    std::unique_ptr<Image> normals, reflectance, depth, raw, filtered;
    std::vector<VkImageView> depthViews;
    VkImage hdr = VK_NULL_HANDLE;
    std::unique_ptr<ImageDump> dump;
    struct Frame {
      std::unique_ptr<Buffer> constants;
      VkDescriptorSet resolve = VK_NULL_HANDLE, trace = VK_NULL_HANDLE, filter = VK_NULL_HANDLE;
      std::vector<VkDescriptorSet> reduce;
    };
    std::array<Frame, gpuFrameCount> frames;
    void createImages(VkPhysicalDevice physical);
    void createLayouts();
    void createPipelines();
    void createDescriptors(VkPhysicalDevice physical, Inputs inputs);
    ~Gpu() {
      if (pool)
        vkDestroyDescriptorPool(device, pool, nullptr);
      for (auto handle : {resolve, reduce, trace, filter})
        if (handle)
          pipeline::destroyPipeline(device, handle, nullptr);
      if (layout)
        pipeline::destroyPipelineLayout(device, layout, nullptr);
      if (setLayout)
        pipeline::destroyDescriptorSetLayout(device, setLayout, nullptr);
      for (auto view : depthViews)
        vkDestroyImageView(device, view, nullptr);
      if (sampler)
        pipeline::destroySampler(device, sampler, nullptr);
    }
  };
  Reflections::Reflections() = default;
  Reflections::~Reflections() = default;

  void Reflections::initialize(VkPhysicalDevice physical, VkDevice device, VkExtent2D extent,
                               VkSampleCountFlagBits samples, uint32_t scale, Inputs inputs) {
    auto next = std::make_unique<Gpu>();
    auto &g = *next;
    g.device = device;
    g.extent = extent;
    g.samples = uint32_t(samples);
    g.scale = scale;
    g.traceExtent = {(extent.width + scale - 1) / scale, (extent.height + scale - 1) / scale};
    g.levels = std::bit_width(std::max(extent.width, extent.height));
    g.hdr = inputs.hdrImage;
    if (ImageDump::enabled(physical))
      g.dump = std::make_unique<ImageDump>(physical, device);
    g.createImages(physical);
    g.createLayouts();
    g.createPipelines();
    g.createDescriptors(physical, inputs);
    m_gpu = std::move(next);
  }
  void Reflections::Gpu::createImages(VkPhysicalDevice physical) {
    auto image = [&](VkExtent2D size, VkFormat format, uint32_t levels = 1) {
      auto result = std::make_unique<Image>();
      result->create(physical, device, size, format,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                         (dump ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0u),
                     VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 1, levels);
      imageBytes += result->bytes;
      return result;
    };
    normals = image(extent, VK_FORMAT_R16G16B16A16_SFLOAT);
    reflectance = image(extent, VK_FORMAT_R16G16B16A16_SFLOAT);
    // Traversal needs only the nearest depth per cell. R32F keeps the hierarchy
    // compact and requires no extended storage-image-format feature.
    depth = image(extent, VK_FORMAT_R32_SFLOAT, levels);
    raw = image(traceExtent, VK_FORMAT_R16G16B16A16_SFLOAT);
    filtered = image(extent, VK_FORMAT_R16G16B16A16_SFLOAT);
    for (uint32_t level = 0; level < levels; ++level) {
      const VkImageViewCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                       .image = depth->handle,
                                       .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                       .format = VK_FORMAT_R32_SFLOAT,
                                       .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1}};
      VkImageView view;
      check(vkCreateImageView(device, &info, nullptr, &view));
      depthViews.push_back(view);
    }
  }
  void Reflections::Gpu::createLayouts() {
    VkDescriptorSetLayoutBinding bindings[17];
    for (uint32_t i = 0; i < 17; ++i) {
      bindings[i] = {i, descriptorType(i), 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    const VkDescriptorSetLayoutCreateInfo setInfo{.sType =
                                                      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                  .bindingCount = 17,
                                                  .pBindings = bindings};
    check(pipeline::createDescriptorSetLayout(device, &setInfo, nullptr, &setLayout));
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
    const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                .setLayoutCount = 1,
                                                .pSetLayouts = &setLayout,
                                                .pushConstantRangeCount = 1,
                                                .pPushConstantRanges = &push};
    check(pipeline::createPipelineLayout(device, &layoutInfo, nullptr, &layout));
  }
  void Reflections::Gpu::createPipelines() {
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
    resolve = samples == VK_SAMPLE_COUNT_1_BIT
                  ? compute(ssrResolveSingleShader, sizeof(ssrResolveSingleShader))
                  : compute(ssrResolveShader, sizeof(ssrResolveShader));
    reduce = compute(ssrReduceShader, sizeof(ssrReduceShader));
    trace = compute(ssrTraceShader, sizeof(ssrTraceShader));
    filter = compute(ssrFilterShader, sizeof(ssrFilterShader));
    const VkSamplerCreateInfo samplerInfo{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                          .magFilter = VK_FILTER_NEAREST,
                                          .minFilter = VK_FILTER_NEAREST,
                                          .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                          .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                          .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                          .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                          .maxLod = float(levels)};
    check(pipeline::createSampler(device, &samplerInfo, nullptr, &sampler));
  }
  void Reflections::Gpu::createDescriptors(VkPhysicalDevice physical, Inputs inputs) {
    const uint32_t setCount = gpuFrameCount * (levels + 2);
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, setCount * 2},
                                          {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, setCount * 9},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, setCount * 4},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, setCount * 2}};
    const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              .maxSets = setCount,
                                              .poolSizeCount = 4,
                                              .pPoolSizes = sizes};
    check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool));
    for (auto &frame : frames) {
      frame.constants = std::make_unique<Buffer>();
      frame.constants->create(physical, device, sizeof(ReflectionFrame),
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                  (dump ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : 0u));
      allocationBytes += frame.constants->allocationBytes;
      const auto buffer = frame.constants->descriptor();
      auto descriptor = [&](VkImageView inputDepth, VkImageView outputDepth, VkImageView output) {
        VkDescriptorSet set;
        const VkDescriptorSetAllocateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                               .descriptorPool = pool,
                                               .descriptorSetCount = 1,
                                               .pSetLayouts = &setLayout};
        check(vkAllocateDescriptorSets(device, &info, &set));
        const VkDescriptorImageInfo images[] = {
            {sampler, inputs.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
            {sampler, inputs.normals, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {sampler, inputs.reflectance, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {sampler, inputs.hdr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {sampler, normals->view, VK_IMAGE_LAYOUT_GENERAL},
            {sampler, reflectance->view, VK_IMAGE_LAYOUT_GENERAL},
            {sampler, inputDepth, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, normals->view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, reflectance->view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, outputDepth, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, output, VK_IMAGE_LAYOUT_GENERAL},
            {sampler, raw->view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[13]{};
        writes[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                     .dstSet = set,
                     .dstBinding = 0,
                     .descriptorCount = 1,
                     .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                     .pBufferInfo = &buffer};
        for (uint32_t i = 1; i < 13; ++i)
          writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                       .dstSet = set,
                       .dstBinding = i,
                       .descriptorCount = 1,
                       .descriptorType = descriptorType(i),
                       .pImageInfo = &images[i - 1]};
        vkUpdateDescriptorSets(device, 13, writes, 0, nullptr);
        return set;
      };
      frame.resolve = descriptor(depth->view, depthViews[0], raw->view);
      for (uint32_t level = 1; level < levels; ++level)
        frame.reduce.push_back(descriptor(depthViews[level - 1], depthViews[level], raw->view));
      frame.trace = descriptor(depth->view, depthViews[0], raw->view);
      frame.filter = descriptor(depth->view, depthViews[0], filtered->view);
    }
  }
  void Reflections::record(VkCommandBuffer command, uint32_t slot, const FrameData &source,
                           const Options &options, VkDescriptorBufferInfo probes,
                           const Environment &environment, ray::Reflections *hardware) {
    auto &g = *m_gpu;
    auto &frame = g.frames.at(slot);
    const VkWriteDescriptorSet probeWrite{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                          .dstSet = frame.filter,
                                          .dstBinding = 13,
                                          .descriptorCount = 1,
                                          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                          .pBufferInfo = &probes};
    vkUpdateDescriptorSets(g.device, 1, &probeWrite, 0, nullptr);
    const VkWriteDescriptorSet environmentWrites[] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = frame.filter,
         .dstBinding = 14,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         .pBufferInfo = &environment.sky},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = frame.filter,
         .dstBinding = 15,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &environment.radiance},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = frame.filter,
         .dstBinding = 16,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &environment.irradiance}};
    vkUpdateDescriptorSets(g.device, 3, environmentWrites, 0, nullptr);
    const ReflectionFrame constants{
        source.camera,
        scene::inverse(source.camera),
        source.view,
        source.cameraPosition,
        {options.ssrDistance / options.sceneScale, options.ssrThickness / options.sceneScale,
         .002f / options.sceneScale, options.ssrRoughness},
        {g.samples, g.scale, options.ssr ? options.ssrSteps : 0u, g.levels},
        {g.extent.width, g.extent.height, g.traceExtent.width, g.traceExtent.height}};
    probes::dumpFrameData(".reflection-frame.bin", &constants, sizeof(constants));
    frame.constants->write(&constants, sizeof(constants));
    if (g.dump) {
      g.dump->recordBuffer(command, slot, "reflection-frame", frame.constants->descriptor());
      g.dump->recordBuffer(command, slot, "sky-frame", environment.sky);
      if (probes.range > sizeof(probes::Header))
        g.dump->recordBuffer(command, slot, "probes", probes);
      if (environment.irradiance.range > sizeof(bake::GridHeader))
        g.dump->recordBuffer(command, slot, "irradiance", environment.irradiance);
    }
    if (g.dump)
      g.dump->record(command, slot, "opaque", g.hdr, VK_FORMAT_R16G16B16A16_SFLOAT, g.extent,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    for (auto *image : {g.normals.get(), g.reflectance.get(), g.raw.get(), g.filtered.get()})
      imageBarrier(command, image->handle, 1, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    imageBarrier(command, g.depth->handle, g.levels, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                 VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    auto dispatch = [&](VkPipeline pipeline, VkDescriptorSet set, VkExtent2D size) {
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, g.layout, 0, 1, &set, 0, nullptr);
      vkCmdDispatch(command, (size.width + 7) / 8, (size.height + 7) / 8, 1);
    };
    auto readable = [&](Image &image, uint32_t levels = 1) {
      imageBarrier(command, image.handle, levels, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT);
    };
    dispatch(g.resolve, frame.resolve, g.extent);
    readable(*g.normals);
    readable(*g.reflectance);
    readable(*g.depth, g.levels);
    if (g.dump) {
      g.dump->record(command, slot, "normals", g.normals->handle, VK_FORMAT_R16G16B16A16_SFLOAT, g.extent,
                     VK_IMAGE_LAYOUT_GENERAL);
      g.dump->record(command, slot, "reflectance", g.reflectance->handle, VK_FORMAT_R16G16B16A16_SFLOAT,
                     g.extent, VK_IMAGE_LAYOUT_GENERAL);
      g.dump->record(command, slot, "depth", g.depth->handle, VK_FORMAT_R32_SFLOAT, g.extent,
                     VK_IMAGE_LAYOUT_GENERAL);
    }
    for (uint32_t level = 1; options.ssr && level < g.levels; ++level) {
      const VkExtent2D size{std::max(1u, g.extent.width >> level), std::max(1u, g.extent.height >> level)};
      dispatch(g.reduce, frame.reduce[level - 1], size);
      readable(*g.depth, g.levels);
      if (g.dump && g.dump->fullImages()) {
        const auto name = "depth-mip" + std::to_string(level);
        g.dump->record(command, slot, name.c_str(), g.depth->handle, VK_FORMAT_R32_SFLOAT, size,
                       VK_IMAGE_LAYOUT_GENERAL, level);
      }
    }
    dispatch(g.trace, frame.trace, g.traceExtent);
    if (g.dump)
      g.dump->record(command, slot, "ssr", g.raw->handle, VK_FORMAT_R16G16B16A16_SFLOAT, g.traceExtent,
                     VK_IMAGE_LAYOUT_GENERAL);
    readable(*g.raw);
    if (hardware)
      hardware->record(command, {g.normals->view, g.depthViews[0], g.raw->view, g.extent, g.traceExtent},
                       source, options);
    dispatch(g.filter, frame.filter, g.extent);
    if (g.dump)
      g.dump->record(command, slot, "filtered", g.filtered->handle, VK_FORMAT_R16G16B16A16_SFLOAT, g.extent,
                     VK_IMAGE_LAYOUT_GENERAL);
    imageBarrier(command, g.filtered->handle, 1, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_ACCESS_SHADER_READ_BIT);
  }
  VkDescriptorImageInfo Reflections::output() const {
    return {m_gpu->sampler, m_gpu->filtered->view, VK_IMAGE_LAYOUT_GENERAL};
  }
  void Reflections::selectFrame(uint32_t slot) {
    if (m_gpu->dump)
      m_gpu->dump->selectFrame(slot);
  }
  uint64_t Reflections::targetBytes() const { return m_gpu->imageBytes; }
  uint64_t Reflections::bufferBytes() const { return m_gpu->allocationBytes; }
} // namespace remix::forward
