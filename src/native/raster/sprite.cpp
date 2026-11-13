#include "raster/gpu.h"
#include "raster/shaders/embedded.h"
#include "runtime/pipelines.h"

// Sprite overlays (remixapi_InstanceInfoSpriteEXT): blended raster-only draws
// with the sprite stages of the raster shaders (raster.vert / raster.frag with
// SPRITE), which blend sheet frames and sequences from the mesh's sprite
// vertices (a second vertex binding) and fade the draw out where it meets the
// scene. Their layout is the raster layout plus, as set 1, the frame's opaque
// depth: a copy of the target's depth attachment that the frame takes
// (captureSceneDepth) between the pass that wrote the scene's depth and the
// one that draws the overlays, so it is whatever the overlays are depth-tested
// against, at target resolution, for every way the scene's depth got there.
// Nothing here exists until a frame submits a sprite instance; the objects go
// with the target.
namespace remix {
  using namespace scene;
  using namespace raster::detail;
  using namespace raster;

  void RasterRenderer::Gpu::destroySprite() {
    auto &s = sprite;
    if (s.set)
      vkFreeDescriptorSets(device, pool, 1, &s.set);
    s.set = VK_NULL_HANDLE;
    s.depth.reset();
    if (s.sampler)
      pipeline::destroySampler(device, s.sampler, nullptr);
    s.sampler = VK_NULL_HANDLE;
    for (auto *shader : {&s.vertex, &s.fragment}) {
      if (*shader)
        pipeline::destroyShaderModule(device, *shader, nullptr);
      *shader = VK_NULL_HANDLE;
    }
    if (s.layout)
      pipeline::destroyPipelineLayout(device, s.layout, nullptr);
    s.layout = VK_NULL_HANDLE;
    for (auto &pool : s.lightPools) {
      if (pool)
        vkDestroyDescriptorPool(device, pool, nullptr);
      pool = VK_NULL_HANDLE;
    }
    s.lightSet = VK_NULL_HANDLE;
    if (s.lightLayout)
      pipeline::destroyDescriptorSetLayout(device, s.lightLayout, nullptr);
    s.lightLayout = VK_NULL_HANDLE;
    s.failed = s.captured = false;
    s.soft = 0;
    s.commands = 0;
  }

  bool RasterRenderer::Gpu::prepareSprite() {
    auto &s = sprite;
    if (s.failed || !pass)
      return false;
    if (s.layout && s.vertex && s.fragment)
      return true;
    if (!s.lightLayout) {
      const VkDescriptorSetLayoutBinding bindings[] = {
          {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
          {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
      const VkDescriptorSetLayoutCreateInfo info = {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings};
      check(pipeline::createDescriptorSetLayout(device, &info, nullptr, &s.lightLayout));
    }
    if (!s.layout) {
      // Set 0 and the push constants as the raster layout; set 1: the depth;
      // set 2: the lighting (finishSprite).
      const VkDescriptorSetLayout sets[] = {setLayout, setLayout, s.lightLayout};
      const VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
      const VkPipelineLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                               .setLayoutCount = 3,
                                               .pSetLayouts = sets,
                                               .pushConstantRangeCount = 1,
                                               .pPushConstantRanges = &push};
      check(pipeline::createPipelineLayout(device, &info, nullptr, &s.layout));
    }
    const auto module = [&](VkShaderModule &out, const uint32_t *code, size_t bytes) {
      if (out)
        return;
      const VkShaderModuleCreateInfo info = {
          .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
      check(pipeline::createShaderModule(device, &info, nullptr, &out));
    };
    module(s.vertex, spriteVertShader, sizeof(spriteVertShader));
    module(s.fragment, spriteFragShader, sizeof(spriteFragShader));
    return true;
  }

  // The frame's lighting set for its sprite commands, unlit: the frame's
  // lighting comes after the commands are built (setSpriteLighting, once the
  // path tracer has prepared its cells) and is written into the same set.
  void RasterRenderer::Gpu::finishSprite() {
    auto &s = sprite;
    s.lightSet = VK_NULL_HANDLE;
    if (!s.commands || !s.lightLayout)
      return;
    s.commands = 0;
    // This slot's last frame has completed (prepare runs after its fence).
    auto &pool = s.lightPools[frameIndex];
    if (!pool) {
      const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
      const VkDescriptorPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                               .maxSets = 1,
                                               .poolSizeCount = 2,
                                               .pPoolSizes = sizes};
      check(vkCreateDescriptorPool(device, &info, nullptr, &pool));
    } else
      check(vkResetDescriptorPool(device, pool, 0));
    const VkDescriptorSetAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                    .descriptorPool = pool,
                                                    .descriptorSetCount = 1,
                                                    .pSetLayouts = &s.lightLayout};
    check(vkAllocateDescriptorSets(device, &allocation, &s.lightSet));
    writeSpriteLighting({});
  }

  // The block, and the cells it names (the block again while it names none:
  // the shader reads them only when the block says so).
  void RasterRenderer::Gpu::writeSpriteLighting(const SpriteLighting &l) {
    auto &s = sprite;
    if (!s.lightSet)
      return;
    struct Block {
      float worldFromClip[16];
      float eye[4];       // the cells' eye; w: a cell of the first level (world units)
      uint32_t config[4]; // lit, cells a side, levels, display bits (1: HDR10)
      float display[4];   // exposure, peak nits, paper white nits
    } block = {};
    const bool lit = l.cells != VK_NULL_HANDLE && l.cellsPerSide && l.levels;
    std::copy_n(l.worldFromClip.data(), 16, block.worldFromClip);
    std::copy_n(l.eye, 3, block.eye);
    block.eye[3] = l.cellUnits;
    block.config[0] = lit;
    block.config[1] = l.cellsPerSide;
    block.config[2] = l.levels;
    block.config[3] = l.hdr10;
    block.display[0] = l.exposure;
    block.display[1] = l.peakNits;
    block.display[2] = l.paperWhiteNits;
    auto parameters =
        buffer(&block, sizeof(block), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const VkDescriptorBufferInfo uniform = {parameters->buffer, parameters->offset, sizeof(block)};
    const VkDescriptorBufferInfo cells =
        lit ? VkDescriptorBufferInfo{l.cells, 0, VK_WHOLE_SIZE} : uniform;
    const VkWriteDescriptorSet writes[] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                            .dstSet = s.lightSet,
                                            .dstBinding = 0,
                                            .descriptorCount = 1,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                            .pBufferInfo = &uniform},
                                           {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                            .dstSet = s.lightSet,
                                            .dstBinding = 1,
                                            .descriptorCount = 1,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                            .pBufferInfo = &cells}};
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    uploads.push_back(std::move(parameters));
  }

  bool RasterRenderer::litOverlays() const {
    for (const auto &draw : draws())
      if ((draw.spriteFlags & REMIXAPI_SPRITE_LIT_EXT) && draw.blend.enabled)
        return true;
    return false;
  }

  void RasterRenderer::setSpriteLighting(const SpriteLighting &lighting) {
    if (m_gpu)
      m_gpu->writeSpriteLighting(lighting);
  }

  bool RasterRenderer::wantsSceneDepth() const { return m_gpu && (m_gpu->sprite.soft != 0 || m_gpu->sprite.requested); }

  void RasterRenderer::requestSceneDepth() {
    if (m_gpu)
      m_gpu->sprite.requested = true;
  }

  VkDescriptorImageInfo RasterRenderer::sceneDepth() const {
    if (!m_gpu || !m_gpu->sprite.captured || !m_gpu->sprite.depth)
      return {};
    return {m_gpu->sprite.sampler, m_gpu->sprite.depth->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  }

  void RasterRenderer::captureSceneDepth(VkCommandBuffer command) {
    auto &g = *m_gpu;
    auto &s = g.sprite;
    if ((!s.soft && !s.requested) || s.failed || !g.depth)
      return;
    try {
      if (!s.depth) {
        // Sampled as a texture: its red is the depth.
        VkFormatProperties properties;
        vkGetPhysicalDeviceFormatProperties(g.physical, g.depthFormat, &properties);
        constexpr VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((properties.optimalTilingFeatures & needed) != needed)
          throw VK_ERROR_FORMAT_NOT_SUPPORTED;
        s.depth = g.image(g.extent.width, g.extent.height, 1, g.depthFormat, true);
      }
      if (!s.sampler) {
        const VkSamplerCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                          .magFilter = VK_FILTER_NEAREST,
                                          .minFilter = VK_FILTER_NEAREST,
                                          .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                          .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                          .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                          .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        check(pipeline::createSampler(g.device, &info, nullptr, &s.sampler));
      }
      if (!s.set) {
        const VkDescriptorSetAllocateInfo sets = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                  .descriptorPool = g.pool,
                                                  .descriptorSetCount = 1,
                                                  .pSetLayouts = &g.setLayout};
        check(vkAllocateDescriptorSets(g.device, &sets, &s.set));
        const VkDescriptorImageInfo image = {s.sampler, s.depth->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                            .dstSet = s.set,
                                            .dstBinding = 0,
                                            .descriptorCount = 1,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                            .pImageInfo = &image};
        vkUpdateDescriptorSets(g.device, 1, &write, 0, nullptr);
      }
    } catch (VkResult result) {
      if (result == VK_ERROR_DEVICE_LOST)
        throw;
      s.failed = true; // sprites stay hard-edged
      return;
    }
    // The target's depth attachment, left in its attachment layout by the
    // pass that just ended, to the copy.
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier barriers[2] = {{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                         .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                                         .oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .image = g.depth->image,
                                         .subresourceRange = range},
                                        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                         .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
                                         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .image = s.depth->image,
                                         .subresourceRange = range}};
    vkCmdPipelineBarrier(command,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
    const VkImageCopy copy = {.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                              .dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                              .extent = {g.extent.width, g.extent.height, 1}};
    vkCmdCopyImage(command, g.depth->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.depth->image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].dstAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, barriers);
    s.captured = true;
  }
} // namespace remix
