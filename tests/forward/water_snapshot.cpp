#include "forward/lighting.h"
#include "forward/resources.h"
#include "forward/water/snapshot.h"
#include "gpu_fixture.h"
#include "platform/gpu_frames.h"
#include "runtime/pipelines.h"
#include "water_shaders/embedded.h"
#include "water_snapshot_queries.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace remix;
using namespace remix::forward;
using remix::forward::test::require;
namespace {
  constexpr VkExtent2D extent{17, 11}; // exercise partial compute workgroups
  VkShaderModule module(VkDevice device, const uint32_t *code, size_t size) {
    const VkShaderModuleCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule result;
    check(vkCreateShaderModule(device, &info, nullptr, &result));
    return result;
  }
  struct Inputs {
    VkDevice device;
    VkSampleCountFlagBits samples;
    Image color, depth, resolved, reflections;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool reflectionReady = false;
    VkImageLayout previousReflectionLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    explicit Inputs(VkDevice target, VkSampleCountFlagBits count) : device(target), samples(count) {}
    ~Inputs() {
      if (pipeline)
        vkDestroyPipeline(device, pipeline, nullptr);
      if (layout)
        vkDestroyPipelineLayout(device, layout, nullptr);
      if (framebuffer)
        vkDestroyFramebuffer(device, framebuffer, nullptr);
      if (pass)
        vkDestroyRenderPass(device, pass, nullptr);
    }
    VkImageView hdr() const { return samples == VK_SAMPLE_COUNT_1_BIT ? color.view : resolved.view; }
    VkImage hdrImage() const { return samples == VK_SAMPLE_COUNT_1_BIT ? color.handle : resolved.handle; }
    void initialize(test::Fixture &f) {
      const auto colorUsage =
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      color.create(f.physical, device, extent, VK_FORMAT_R16G16B16A16_SFLOAT, colorUsage, samples,
                   VK_IMAGE_ASPECT_COLOR_BIT);
      depth.create(f.physical, device, extent, VK_FORMAT_D32_SFLOAT,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, samples,
                   VK_IMAGE_ASPECT_DEPTH_BIT);
      if (samples != VK_SAMPLE_COUNT_1_BIT)
        resolved.create(f.physical, device, extent, VK_FORMAT_R16G16B16A16_SFLOAT, colorUsage,
                        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
      reflections.create(f.physical, device, extent, VK_FORMAT_R16G16B16A16_SFLOAT,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_SAMPLE_COUNT_1_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT);
      VkAttachmentDescription attachments[3]{};
      attachments[0] = {.format = VK_FORMAT_R16G16B16A16_SFLOAT,
                        .samples = samples,
                        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                        .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      attachments[1] = attachments[0];
      attachments[1].format = VK_FORMAT_D32_SFLOAT;
      attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
      attachments[2] = attachments[0];
      attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
      attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      const VkAttachmentReference c{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
          d{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL},
          r{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      const VkSubpassDescription subpass{.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                         .colorAttachmentCount = 1,
                                         .pColorAttachments = &c,
                                         .pResolveAttachments =
                                             samples == VK_SAMPLE_COUNT_1_BIT ? nullptr : &r,
                                         .pDepthStencilAttachment = &d};
      const auto stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
      const VkSubpassDependency dependencies[] = {
          {.srcSubpass = VK_SUBPASS_EXTERNAL,
           .dstSubpass = 0,
           .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
           .dstStageMask = stages,
           .srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
           .dstAccessMask =
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT},
          {.srcSubpass = 0,
           .dstSubpass = VK_SUBPASS_EXTERNAL,
           .srcStageMask = stages,
           .dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
           .srcAccessMask =
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
           .dstAccessMask = VK_ACCESS_SHADER_READ_BIT}};
      const VkRenderPassCreateInfo passInfo{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                            .attachmentCount = samples == VK_SAMPLE_COUNT_1_BIT ? 2u : 3u,
                                            .pAttachments = attachments,
                                            .subpassCount = 1,
                                            .pSubpasses = &subpass,
                                            .dependencyCount = 2,
                                            .pDependencies = dependencies};
      check(vkCreateRenderPass(device, &passInfo, nullptr, &pass));
      const VkImageView views[] = {color.view, depth.view, resolved.view};
      const VkFramebufferCreateInfo framebufferInfo{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                                    .renderPass = pass,
                                                    .attachmentCount =
                                                        samples == VK_SAMPLE_COUNT_1_BIT ? 2u : 3u,
                                                    .pAttachments = views,
                                                    .width = extent.width,
                                                    .height = extent.height,
                                                    .layers = 1};
      check(vkCreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffer));
      const VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
      const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                  .pushConstantRangeCount = 1,
                                                  .pPushConstantRanges = &range};
      check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout));
      const auto vertex =
          module(device, test::water_input::vertShader, sizeof(test::water_input::vertShader));
      const auto fragment =
          module(device, test::water_input::fragShader, sizeof(test::water_input::fragShader));
      const VkPipelineShaderStageCreateInfo shaders[] = {
          {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
           .stage = VK_SHADER_STAGE_VERTEX_BIT,
           .module = vertex,
           .pName = "main"},
          {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
           .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
           .module = fragment,
           .pName = "main"}};
      const VkPipelineVertexInputStateCreateInfo vertices{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      const VkPipelineInputAssemblyStateCreateInfo assembly{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
          .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
      const VkViewport viewport{0, 0, float(extent.width), float(extent.height), 0, 1};
      const VkRect2D scissor{{0, 0}, extent};
      const VkPipelineViewportStateCreateInfo viewportInfo{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
          .viewportCount = 1,
          .pViewports = &viewport,
          .scissorCount = 1,
          .pScissors = &scissor};
      const VkPipelineRasterizationStateCreateInfo raster{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
          .polygonMode = VK_POLYGON_MODE_FILL,
          .cullMode = VK_CULL_MODE_NONE,
          .lineWidth = 1};
      const VkPipelineMultisampleStateCreateInfo msaa{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = samples};
      const VkPipelineDepthStencilStateCreateInfo depthState{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
          .depthTestEnable = VK_TRUE,
          .depthWriteEnable = VK_TRUE,
          .depthCompareOp = VK_COMPARE_OP_LESS};
      const VkPipelineColorBlendAttachmentState blend{.colorWriteMask = 15};
      const VkPipelineColorBlendStateCreateInfo colorState{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
          .attachmentCount = 1,
          .pAttachments = &blend};
      const VkGraphicsPipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                              .stageCount = 2,
                                              .pStages = shaders,
                                              .pVertexInputState = &vertices,
                                              .pInputAssemblyState = &assembly,
                                              .pViewportState = &viewportInfo,
                                              .pRasterizationState = &raster,
                                              .pMultisampleState = &msaa,
                                              .pDepthStencilState = &depthState,
                                              .pColorBlendState = &colorState,
                                              .layout = layout,
                                              .renderPass = pass};
      const auto status = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
      vkDestroyShaderModule(device, fragment, nullptr);
      vkDestroyShaderModule(device, vertex, nullptr);
      check(status);
    }
    void record(VkCommandBuffer command, Vec4 source, Vec4 reflection, bool empty,
                VkImageLayout reflectionLayout = VK_IMAGE_LAYOUT_GENERAL, bool partialCoverage = false) {
      const auto clearColor = partialCoverage ? Vec4{} : source;
      const VkClearValue clear[] = {{.color = {{clearColor[0], clearColor[1], clearColor[2], clearColor[3]}}},
                                    {.depthStencil = {1, 0}}};
      const VkRenderPassBeginInfo begin{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                        .renderPass = pass,
                                        .framebuffer = framebuffer,
                                        .renderArea = {{0, 0}, extent},
                                        .clearValueCount = 2,
                                        .pClearValues = clear};
      vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
      if (!empty) {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        struct Push {
          float depth;
          uint32_t mask, leftOnly, width;
          Vec4 color;
        };
        const Push draws[] = {
            {.7f, partialCoverage ? 1u << (uint32_t(samples) - 1) : (1u << uint32_t(samples)) - 1, 0,
             extent.width, source},
            {.2f, partialCoverage ? 1u : 1u << (uint32_t(samples) - 1), 1, extent.width, source}};
        for (const auto &draw : draws) {
          vkCmdPushConstants(command, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(draw), &draw);
          vkCmdDraw(command, 3, 1, 0, 0);
        }
      }
      vkCmdEndRenderPass(command);
      VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                   .srcAccessMask = reflectionReady ? VK_ACCESS_SHADER_READ_BIT : 0u,
                                   .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                   .oldLayout =
                                       reflectionReady ? previousReflectionLayout : VK_IMAGE_LAYOUT_UNDEFINED,
                                   .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = reflections.handle,
                                   .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &barrier);
      const VkClearColorValue value{{reflection[0], reflection[1], reflection[2], reflection[3]}};
      vkCmdClearColorImage(command, reflections.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1,
                           &barrier.subresourceRange);
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.newLayout = reflectionLayout;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                           0, nullptr, 0, nullptr, 1, &barrier);
      reflectionReady = true;
      previousReflectionLayout = reflectionLayout;
    }
  };
  void copy(VkCommandBuffer command, VkImage image, Buffer &buffer, VkDeviceSize offset = 0) {
    VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                 .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                                                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                 .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                                 .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .image = image,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
    const VkBufferImageCopy region{.bufferOffset = offset,
                                   .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                   .imageExtent = {extent.width, extent.height, 1}};
    vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.handle, 1, &region);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
  }
  float half(uint16_t value) {
    const int exponent = value >> 10 & 31, fraction = value & 1023;
    return std::ldexp(exponent ? 1.f + float(fraction) / 1024 : float(fraction) / 1024,
                      exponent ? exponent - 15 : -14) *
           (value & 0x8000u ? -1.f : 1.f);
  }
} // namespace
int main() try {
  {
    test::Fixture fixture(false, true); // core Vulkan device; no optional RT features/extensions
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(fixture.physical, &properties);
    pipeline::Service pipelines(fixture.device, properties, false);
    for (auto samples : {VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_2_BIT, VK_SAMPLE_COUNT_4_BIT}) {
      Inputs inputs(fixture.device, samples);
      inputs.initialize(fixture);
      water::Snapshot snapshot;
      auto preparation = pipelines.prepare("water-snapshot-" + std::to_string(uint32_t(samples)), [&] {
        snapshot.initialize(fixture.physical, fixture.device, extent, samples);
      });
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
      while (!preparation->ready() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      require(preparation->ready(), "Water snapshot worker preparation completed");
      preparation->check();
      if (test::validationErrors)
        throw std::runtime_error("Water snapshot pipeline validation failed");
      require(snapshot.targetBytes() == 0, "Pipeline preparation leaves refraction images unallocated");
      snapshot.prepareImages();
      const auto allocated = snapshot.targetBytes();
      require(allocated > 0, "First water use allocates independent snapshot images");
      snapshot.prepareImages();
      require(snapshot.targetBytes() == allocated, "Repeated preparation reuses the snapshot allocation");
      const auto output = snapshot.output();
      test::SnapshotQueries queries(fixture.device);
      queries.initialize(fixture, extent, output);
      require(output.hdr.imageView != inputs.hdr() && output.depth.imageView != inputs.depth.view,
              "Refraction snapshots never alias their opaque render attachments");
      bool feedbackRefused = false;
      try {
        snapshot.record(VK_NULL_HANDLE, 0, {output.hdr.imageView, inputs.depth.view, {}});
      } catch (const std::invalid_argument &) {
        feedbackRefused = true;
      }
      require(feedbackRefused, "Direct snapshot output/input feedback is rejected before recording commands");
      Buffer hdr, depth, original;
      hdr.create(fixture.physical, fixture.device, extent.width * extent.height * 8,
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buffer::Memory::Readback);
      depth.create(fixture.physical, fixture.device, extent.width * extent.height * 4,
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buffer::Memory::Readback);
      original.create(fixture.physical, fixture.device, extent.width * extent.height * 8,
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buffer::Memory::Readback);
      for (uint32_t frame = 0; frame < 7; ++frame) {
        const bool reflected = frame % 2 == 0, sky = frame == 5, bright = frame == 6,
                   partialCoverage = frame == 4;
        const Vec4 source = sky      ? Vec4{}
                            : bright ? Vec4{60000, 60000, 60000, 1}
                                     : Vec4{.25f, .5f, 1, partialCoverage ? 1.f : .5f};
        const Vec4 reflection = bright ? Vec4{60000, 60000, 60000, 1} : Vec4{.1f, .2f, .3f, 1};
        fixture.begin();
        const auto reflectionLayout =
            frame == 2 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
        inputs.record(fixture.command, source, reflection, sky, reflectionLayout, partialCoverage);
        snapshot.record(
            fixture.command, frame % gpuFrameCount,
            {inputs.hdr(), inputs.depth.view,
             reflected ? VkDescriptorImageInfo{VK_NULL_HANDLE, inputs.reflections.view, reflectionLayout}
                       : VkDescriptorImageInfo{}});
        queries.record(fixture.command);
        copy(fixture.command, output.hdrImage, hdr);
        copy(fixture.command, output.depthImage, depth);
        copy(fixture.command, inputs.hdrImage(), original);
        const VkMemoryBarrier host{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                   .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                   .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                             1, &host, 0, nullptr, 0, nullptr);
        fixture.finish();
        std::vector<uint16_t> colors(extent.width * extent.height * 4), sources(colors.size());
        std::vector<float> depths(extent.width * extent.height);
        const auto refracted = queries.read();
        hdr.read(colors.data(), colors.size() * sizeof(uint16_t));
        original.read(sources.data(), sources.size() * sizeof(uint16_t));
        depth.read(depths.data(), depths.size() * sizeof(float));
        for (uint32_t y = 0; y < extent.height; ++y)
          for (uint32_t x = 0; x < extent.width; ++x) {
            const size_t pixel = y * extent.width + x;
            const float coverage = !partialCoverage || samples == VK_SAMPLE_COUNT_1_BIT
                                       ? 1.f
                                       : float(x < extent.width / 2 ? 2 : 1) / uint32_t(samples);
            const float sourceAlpha = source[3] * coverage;
            const float distance = sky ? -1 : x < extent.width / 2 ? .1f : .6f;
            require(std::abs(refracted[pixel][3] - distance) < .00001f,
                    "Water refraction queries consume real MSAA snapshots without a CPU round trip");
            require(std::abs(depths[pixel] - (sky                    ? 1
                                              : x < extent.width / 2 ? .2f
                                                                     : .7f)) < .00001f,
                    "Depth snapshots select the nearest real MSAA sample and preserve empty sky");
            for (uint32_t channel = 0; channel < 4; ++channel) {
              const float expected =
                  channel == 3 ? sourceAlpha
                               : std::min(65000.f, source[channel] * coverage +
                                                       (reflected ? reflection[channel] * sourceAlpha : 0));
              const float value = half(colors[pixel * 4 + channel]);
              require(std::isfinite(value) && std::abs(value - expected) < (bright ? 32.f : .002f),
                      "HDR snapshots preserve coverage and add opaque reflections once before tone mapping");
              if (channel < 3) {
                const float transmission = sky ? 0
                                               : std::pow(channel == 0   ? .5f
                                                          : channel == 1 ? .25f
                                                                         : 1.f,
                                                          distance);
                const float through = value / std::max(sourceAlpha, 1e-6f) * transmission;
                require(std::isfinite(refracted[pixel][channel]) &&
                            std::abs(refracted[pixel][channel] - through) < (bright ? 1.f : .0001f),
                        "Absorption uses the GPU snapshot path length and scene-linear HDR");
              }
              require(std::abs(half(sources[pixel * 4 + channel]) - source[channel] * coverage) <
                          (bright ? 32.f : .001f),
                      "Snapshot creation leaves the source HDR unchanged");
            }
          }
      }
      // Exercise shared targets and distinct descriptor slots in one submission,
      // without a queue idle between their read/write uses.
      const VkDeviceSize frameBytes = extent.width * extent.height * 8;
      Buffer packed;
      packed.create(fixture.physical, fixture.device, frameBytes * gpuFrameCount,
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buffer::Memory::Readback);
      fixture.begin();
      for (uint32_t slot = 0; slot < gpuFrameCount; ++slot) {
        const float scale = float(slot + 1);
        inputs.record(fixture.command, {.25f * scale, .5f * scale, scale, 1}, {.1f, .2f, .3f, 1}, false);
        snapshot.record(fixture.command, slot,
                        {inputs.hdr(),
                         inputs.depth.view,
                         {VK_NULL_HANDLE, inputs.reflections.view, VK_IMAGE_LAYOUT_GENERAL}});
        copy(fixture.command, output.hdrImage, packed, frameBytes * slot);
      }
      const VkMemoryBarrier packedHost{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                       .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                       .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
      vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                           &packedHost, 0, nullptr, 0, nullptr);
      fixture.finish();
      std::vector<uint16_t> packedColors(size_t(frameBytes * gpuFrameCount / sizeof(uint16_t)));
      packed.read(packedColors.data(), packedColors.size() * sizeof(uint16_t));
      for (uint32_t slot = 0; slot < gpuFrameCount; ++slot)
        for (uint32_t pixel = 0; pixel < extent.width * extent.height; ++pixel)
          require(std::abs(half(packedColors[(size_t(slot) * extent.width * extent.height + pixel) * 4]) -
                           (.25f * float(slot + 1) + .1f)) < .002f,
                  "Snapshot read/write barriers preserve each slot's HDR inside one submission");
      std::printf("Water snapshot: %ux real MSAA, seven frame/slot reuses, partial groups, "
                  "sky/HDR/reflections and finite highlights passed\n",
                  uint32_t(samples));
    }
  }
  require(test::validationErrors == 0, "Water snapshots are Vulkan synchronization/validation clean");
} catch (VkResult result) {
  std::fprintf(stderr, "Water snapshot Vulkan failure %d\n", int(result));
  return 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "Water snapshot failure: %s\n", error.what());
  return 1;
}
