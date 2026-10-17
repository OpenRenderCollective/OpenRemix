#include "forward/bake/transport.h"
#include "forward/gpu.h"
#include "forward/image_dump.h"
#include "runtime/pipelines.h"
#include <algorithm>

namespace remix::forward {
  namespace {
    constexpr VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    constexpr VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    VkSampleCountFlags supportedSamples(VkPhysicalDevice physical, VkFormat format, VkImageUsageFlags usage) {
      VkImageFormatProperties properties;
      check(vkGetPhysicalDeviceImageFormatProperties(physical, format, VK_IMAGE_TYPE_2D,
                                                     VK_IMAGE_TILING_OPTIMAL, usage, 0, &properties));
      return properties.sampleCounts;
    }
  } // namespace
  void Renderer::Gpu::createTargets() {
    const auto colorUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const auto depthUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    auto supported = supportedSamples(physical, colorFormat, colorUsage) &
                     supportedSamples(physical, depthFormat, depthUsage);
    if (options.reflections())
      supported &= supportedSamples(physical, VK_FORMAT_R8G8B8A8_UNORM, colorUsage);
    for (auto count : {VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_2_BIT, VK_SAMPLE_COUNT_1_BIT})
      if (uint32_t(count) <= options.samples && (supported & count)) {
        samples = count;
        break;
      }
    statistics.samples = uint32_t(samples);
    auto image = [&](VkExtent2D size, VkFormat format, VkImageUsageFlags usage, VkSampleCountFlagBits count,
                     VkImageAspectFlags aspect, uint32_t layers = 1) {
      auto result = std::make_unique<Image>();
      result->create(physical, device, size, format, usage, count, aspect, layers);
      statistics.targetBytes += result->bytes;
      return result;
    };
    const auto debugUsage = ImageDump::enabled(physical) ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0u;
    color = image(extent, colorFormat, colorUsage | (samples == VK_SAMPLE_COUNT_1_BIT ? debugUsage : 0u),
                  samples, VK_IMAGE_ASPECT_COLOR_BIT);
    depth = image(extent, depthFormat, depthUsage, samples, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (samples != VK_SAMPLE_COUNT_1_BIT)
      resolved = image(extent, colorFormat, colorUsage | debugUsage, VK_SAMPLE_COUNT_1_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
    if (options.reflections()) {
      normalGuide = image(extent, colorFormat, colorUsage, samples, VK_IMAGE_ASPECT_COLOR_BIT);
      reflectanceGuide =
          image(extent, VK_FORMAT_R8G8B8A8_UNORM, colorUsage, samples, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    const uint32_t resolveIndex = options.reflections() ? 4 : 2;
    const uint32_t attachmentCount = resolveIndex + uint32_t(bool(resolved));
    VkAttachmentDescription attachments[5]{};
    attachments[0] = {.format = colorFormat,
                      .samples = samples,
                      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                      .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    attachments[1] = attachments[0];
    attachments[1].format = depthFormat;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    if (options.reflections()) {
      attachments[2] = attachments[0];
      attachments[3] = attachments[0];
      attachments[3].format = VK_FORMAT_R8G8B8A8_UNORM;
    }
    attachments[resolveIndex] = attachments[0];
    attachments[resolveIndex].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[resolveIndex].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    const VkAttachmentReference colorReferences[] = {{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                                     {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                                     {3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
    const VkAttachmentReference depthReference{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference resolveReferences[] = {
        {resolveIndex, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
    const VkSubpassDescription subpass{.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                       .colorAttachmentCount = options.reflections() ? 3u : 1u,
                                       .pColorAttachments = colorReferences,
                                       .pResolveAttachments = resolved ? resolveReferences : nullptr,
                                       .pDepthStencilAttachment = &depthReference};
    // Targets are shared between slots on one queue. The incoming dependency
    // includes the previous frame's composite reads, not just attachment writes.
    const auto attachmentStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkSubpassDependency dependencies[] = {
        {.srcSubpass = VK_SUBPASS_EXTERNAL,
         .dstSubpass = 0,
         .srcStageMask =
             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | attachmentStages,
         .dstStageMask = attachmentStages,
         .srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
         .dstAccessMask =
             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT},
        {.srcSubpass = 0,
         .dstSubpass = VK_SUBPASS_EXTERNAL,
         .srcStageMask = attachmentStages,
         .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
         .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_SHADER_READ_BIT}};
    const VkRenderPassCreateInfo info{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                      .attachmentCount = attachmentCount,
                                      .pAttachments = attachments,
                                      .subpassCount = 1,
                                      .pSubpasses = &subpass,
                                      .dependencyCount = 2,
                                      .pDependencies = dependencies};
    check(pipeline::createRenderPass(device, &info, nullptr, &worldPass));
    VkImageView views[5] = {color->view, depth->view};
    if (options.reflections()) {
      views[2] = normalGuide->view;
      views[3] = reflectanceGuide->view;
    }
    if (resolved)
      views[resolveIndex] = resolved->view;
    const VkFramebufferCreateInfo framebuffer{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                              .renderPass = worldPass,
                                              .attachmentCount = attachmentCount,
                                              .pAttachments = views,
                                              .width = extent.width,
                                              .height = extent.height,
                                              .layers = 1};
    check(vkCreateFramebuffer(device, &framebuffer, nullptr, &worldFramebuffer));

    const VkExtent2D shadowSize{options.shadowResolution, options.shadowResolution};
    shadow = image(shadowSize, depthFormat, depthUsage, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                   sunShadowLayers);
    const VkAttachmentDescription shadowAttachment{.format = depthFormat,
                                                   .samples = VK_SAMPLE_COUNT_1_BIT,
                                                   .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                                   .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                                   .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                                   .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                                   .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                                   .finalLayout =
                                                       VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    const VkAttachmentReference shadowReference{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription shadowSubpass{.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                             .pDepthStencilAttachment = &shadowReference};
    const VkRenderPassCreateInfo shadowInfo{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                            .attachmentCount = 1,
                                            .pAttachments = &shadowAttachment,
                                            .subpassCount = 1,
                                            .pSubpasses = &shadowSubpass,
                                            .dependencyCount = 2,
                                            .pDependencies = dependencies};
    check(pipeline::createRenderPass(device, &shadowInfo, nullptr, &shadowPass));
    for (uint32_t c = 0; c < sunShadowLayers; ++c) {
      const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                       .image = shadow->handle,
                                       .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                       .format = depthFormat,
                                       .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, c, 1}};
      check(vkCreateImageView(device, &view, nullptr, &shadowViews[c]));
      const VkFramebufferCreateInfo target{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                           .renderPass = shadowPass,
                                           .attachmentCount = 1,
                                           .pAttachments = &shadowViews[c],
                                           .width = shadowSize.width,
                                           .height = shadowSize.height,
                                           .layers = 1};
      check(vkCreateFramebuffer(device, &target, nullptr, &shadowFramebuffers[c]));
    }
  }
  void Renderer::Gpu::createLayouts() {
    const auto stages =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    const VkDescriptorSetLayoutBinding sceneBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    const VkDescriptorSetLayoutCreateInfo sceneInfo{.sType =
                                                        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                    .bindingCount = uint32_t(std::size(sceneBindings)),
                                                    .pBindings = sceneBindings};
    check(pipeline::createDescriptorSetLayout(device, &sceneInfo, nullptr, &sceneLayout));
    VkDescriptorSetLayoutBinding materialBindings[5];
    for (uint32_t i = 0; i < 5; ++i)
      materialBindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                             nullptr};
    const VkDescriptorSetLayoutCreateInfo materialInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 5,
        .pBindings = materialBindings};
    check(pipeline::createDescriptorSetLayout(device, &materialInfo, nullptr, &materialLayout));
    const VkDescriptorSetLayoutBinding compositeBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    const VkDescriptorSetLayoutCreateInfo compositeInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = options.water ? 6u : 5u,
        .pBindings = compositeBindings};
    check(pipeline::createDescriptorSetLayout(device, &compositeInfo, nullptr, &compositeSetLayout));
    const VkDescriptorSetLayout sets[] = {sceneLayout, materialLayout};
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8};
    const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                .setLayoutCount = 2,
                                                .pSetLayouts = sets,
                                                .pushConstantRangeCount = 1,
                                                .pPushConstantRanges = &push};
    check(pipeline::createPipelineLayout(device, &layoutInfo, nullptr, &layout));
    if (giSupported) {
      giSceneLayout = bake::Transport::createSceneLayout(device);
      const VkDescriptorSetLayout giSets[] = {sceneLayout, materialLayout, giSceneLayout};
      auto info = layoutInfo;
      info.setLayoutCount = 3;
      info.pSetLayouts = giSets;
      check(pipeline::createPipelineLayout(device, &info, nullptr, &giLayout));
    }
    const VkPushConstantRange compositePush{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 20};
    const VkPipelineLayoutCreateInfo compositeLayoutInfo{.sType =
                                                             VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                         .setLayoutCount = 1,
                                                         .pSetLayouts = &compositeSetLayout,
                                                         .pushConstantRangeCount = 1,
                                                         .pPushConstantRanges = &compositePush};
    check(pipeline::createPipelineLayout(device, &compositeLayoutInfo, nullptr, &compositeLayout));
    VkSamplerCreateInfo samplerInfo{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                    .magFilter = VK_FILTER_NEAREST,
                                    .minFilter = VK_FILTER_NEAREST,
                                    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                    .maxLod = 0};
    check(pipeline::createSampler(device, &samplerInfo, nullptr, &sampler));
    // Explicit chart bounds isolate mip-zero coefficient/mask filtering. Do
    // not inherit the nearest sampler used for exact depth/composite reads.
    VkFormatProperties format;
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R16G16B16A16_SFLOAT, &format);
    if (format.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
      samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR;
    check(pipeline::createSampler(device, &samplerInfo, nullptr, &surfaceSampler));
    samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    check(pipeline::createSampler(device, &samplerInfo, nullptr, &skySampler));
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.compareEnable = VK_TRUE;
    samplerInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    check(pipeline::createSampler(device, &samplerInfo, nullptr, &shadowSampler));
  }
} // namespace remix::forward
