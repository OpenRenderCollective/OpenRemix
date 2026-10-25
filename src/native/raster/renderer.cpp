#include "raster/gpu.h"
#include "raster/shaders/embedded.h"
#include "runtime/pipelines.h"

namespace remix {
  using namespace scene;
  using namespace raster::detail;
  using namespace raster;

  RasterRenderer::Gpu::~Gpu() {
    destroyTarget();
    commands.clear();
    for (auto &frame : frames)
      frame = {};
    materials.clear();
    textures.clear();
    meshes.clear();
    uploads.clear();
    reusableImages.clear();
    skin.reset();
    if (vertex) {
      pipeline::destroyShaderModule(device, vertex, nullptr);
    }
    if (fragment) {
      pipeline::destroyShaderModule(device, fragment, nullptr);
    }
    if (layout) {
      pipeline::destroyPipelineLayout(device, layout, nullptr);
    }
    if (pool) {
      vkDestroyDescriptorPool(device, pool, nullptr);
    }
    if (setLayout) {
      pipeline::destroyDescriptorSetLayout(device, setLayout, nullptr);
    }
  }

  void RasterRenderer::Gpu::destroyTarget() {
    for (auto [key, pipeline] : pipelines) {
      pipeline::destroyPipeline(device, pipeline, nullptr);
    }
    pipelines.clear();
    destroyMask();
    destroyHalo();
    destroySprite();
    destroySkyComposite();
    depth.reset();
    pass = VK_NULL_HANDLE;
    extent = {};
  }

  RasterRenderer::RasterRenderer() { reset(); }

  RasterRenderer::~RasterRenderer() = default;

  void RasterRenderer::selectFrame(uint32_t slot) {
    auto &g = *m_gpu;
    auto &previous = g.frames[g.frameIndex];
    previous.uploads = std::move(g.uploads);
    previous.skin = std::move(g.skin);
    g.frameIndex = slot;
    auto &next = g.frames[slot];
    for (auto &buffer : next.retiredBuffers)
      g.recycle(std::move(buffer));
    next.retiredBuffers.clear();
    if (!next.retiredMaterials.empty() || !next.retiredImages.empty()) {
      // Samplers, descriptor sets, images and their memory of materials and
      // textures the host destroyed (remixapi_TextureStatisticsEXT).
      CounterTicks time{textureCounters.gpuDestroyTicks};
      textureCounters.gpuMaterialsDestroyed.fetch_add(next.retiredMaterials.size(), std::memory_order_relaxed);
      next.retiredMaterials.clear();
      for (auto &image : next.retiredImages) {
        textureCounters.gpuTexturesDestroyed.fetch_add(image != nullptr, std::memory_order_relaxed);
        g.recycle(std::move(image));
      }
      next.retiredImages.clear();
    }
    g.uploads = std::move(next.uploads);
    g.skin = std::move(next.skin);
  }

  RasterRenderer::TextureCosts RasterRenderer::takeTextureCosts() {
    const auto costs = m_gpu->textureCosts;
    m_gpu->textureCosts = {};
    return costs;
  }

  RasterRenderer::MemoryStatistics RasterRenderer::memoryStatistics() const {
    MemoryStatistics result;
    if (!m_gpu)
      return result;
    for (const auto &[texture, entry] : m_gpu->textures)
      if (entry.data) {
        result.textureBytes += entry.data->bytes;
        ++result.textureCount;
        if (entry.data->format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && entry.data->format <= VK_FORMAT_BC7_SRGB_BLOCK)
          result.compressedTextureBytes += entry.data->bytes;
      }
    for (const auto &[surface, entry] : m_gpu->meshes)
      if (entry.data)
        result.geometryBytes += entry.data->allocationSize;
    result.pooledBytes = m_gpu->reusableBytes;
    return result;
  }

  void RasterRenderer::initializeGpu(VkPhysicalDevice physical, VkDevice device, bool textureCompression,
                                     bool memoryBudget) {
    m_gpu = std::make_unique<Gpu>();
    auto &g = *m_gpu;
    g.device = device;
    g.physical = physical;
    g.textureCompression = textureCompression;
    g.memoryBudget = memoryBudget;
    vkGetPhysicalDeviceMemoryProperties(physical, &g.memory);
    vkGetPhysicalDeviceProperties(physical, &g.properties);
    {
      // The runtime enables samplerAnisotropy whenever the device supports it.
      VkPhysicalDeviceFeatures features{};
      vkGetPhysicalDeviceFeatures(physical, &features);
      if (features.samplerAnisotropy)
        g.maxAnisotropy = std::min(16.f, g.properties.limits.maxSamplerAnisotropy);
      // Enabled with it (wireframe instances).
      g.fillModeNonSolid = features.fillModeNonSolid == VK_TRUE;
    }
    for (auto format : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM}) {
      VkFormatProperties p;
      vkGetPhysicalDeviceFormatProperties(physical, format, &p);
      if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
        g.depthFormat = format;
        break;
      }
    }
    if (g.depthFormat == VK_FORMAT_UNDEFINED) {
      throw VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    const VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                  VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    const VkDescriptorSetLayoutCreateInfo set = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                 .bindingCount = 1,
                                                 .pBindings = &binding};
    check(pipeline::createDescriptorSetLayout(device, &set, nullptr,
                                              &g.setLayout));
    const VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384};
    const VkDescriptorPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                             .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                             .maxSets = 16384,
                                             .poolSizeCount = 1,
                                             .pPoolSizes = &size};
    check(vkCreateDescriptorPool(device, &pool, nullptr, &g.pool));
    const VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                      sizeof(Push)};
    const VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                               .setLayoutCount = 1,
                                               .pSetLayouts = &g.setLayout,
                                               .pushConstantRangeCount = 1,
                                               .pPushConstantRanges = &push};
    check(pipeline::createPipelineLayout(device, &layout, nullptr, &g.layout));
    VkShaderModuleCreateInfo shader = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                       .codeSize = sizeof(vertShader),
                                       .pCode = vertShader};
    check(pipeline::createShaderModule(device, &shader, nullptr, &g.vertex));
    shader.codeSize = sizeof(fragShader);
    shader.pCode = fragShader;
    check(pipeline::createShaderModule(device, &shader, nullptr, &g.fragment));
  }

  void RasterRenderer::adoptGpu(RasterRenderer &other) {
    if (&other == this)
      return;
    m_gpu = std::move(other.m_gpu);
    m_profile = std::exchange(other.m_profile, {});
  }

  void RasterRenderer::configureTarget(VkRenderPass pass, VkExtent2D extent, bool encodeOutput,
                                       bool linearTarget) {
    auto &g = *m_gpu;
    g.destroyTarget();
    g.pass = pass;
    g.skyPass = VK_NULL_HANDLE;
    g.extent = extent;
    g.encodeOutput = encodeOutput;
    g.linearTarget = linearTarget;
    g.depth = g.image(extent.width, extent.height, 1, g.depthFormat, true);
  }

  void RasterRenderer::setSkyTarget(VkRenderPass pass) {
    auto &g = *m_gpu;
    if (g.skyPass == pass)
      return;
    for (auto [key, pipeline] : g.pipelines)
      pipeline::destroyPipeline(g.device, pipeline, nullptr);
    g.pipelines.clear();
    g.skyPass = pass;
  }
  void RasterRenderer::destroyTarget() {
    if (m_gpu) {
      m_gpu->destroyTarget();
    }
  }

  void RasterRenderer::shutdownGpu() {
    m_gpu.reset();
    reset();
  }

  VkFormat RasterRenderer::depthFormat() const { return m_gpu->depthFormat; }

  std::shared_ptr<void> RasterRenderer::depthLifetime() const {
    return m_gpu->depth;
  }

  VkImageView RasterRenderer::depthView() const { return m_gpu->depth->view; }

} // namespace remix
