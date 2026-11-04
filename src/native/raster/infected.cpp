#include "raster/gpu.h"
#include "raster/shaders/embedded.h"
#include "runtime/pipelines.h"

// remixapi_MaterialGradientMapEXT and remixapi_InstanceInfoWoundCutEXT in the
// raster backend: Left 4 Dead 2's Infected shader. Such draws take the raster
// stages with INFECTED (shaders/infected.glsl): the vertex stage evaluates the
// wound at the mesh's own positions (the bind pose, binding 1: the mesh's
// vertices whether or not binding 0 holds skinned ones), the fragment stage
// cuts the wound out and composites the gradient-mapped albedo. Set 1 holds
// the gradient, detail, cut and burn textures (each through its sampler material)
// and the draw's parameter block, a part of one uniform buffer per frame; the
// sets come from a pool per frame slot. Nothing here exists until a frame
// draws such a material or instance; the objects go with the target.
namespace remix {
  using namespace scene;
  using namespace raster::detail;
  using namespace raster;

  void RasterRenderer::Gpu::destroyInfected() {
    auto &s = infected;
    for (auto &pool : s.pools) {
      if (pool)
        vkDestroyDescriptorPool(device, pool, nullptr);
      pool = VK_NULL_HANDLE;
    }
    std::fill(std::begin(s.capacity), std::end(s.capacity), 0u);
    for (auto *shader : {&s.vertex, &s.fragment}) {
      if (*shader)
        pipeline::destroyShaderModule(device, *shader, nullptr);
      *shader = VK_NULL_HANDLE;
    }
    if (s.layout)
      pipeline::destroyPipelineLayout(device, s.layout, nullptr);
    s.layout = VK_NULL_HANDLE;
    if (s.setLayout)
      pipeline::destroyDescriptorSetLayout(device, s.setLayout, nullptr);
    s.setLayout = VK_NULL_HANDLE;
    s.failed = false;
    s.parameters.clear();
    s.images.clear();
  }

  bool RasterRenderer::Gpu::prepareInfected() {
    auto &s = infected;
    if (s.failed || !pass)
      return false;
    if (s.layout && s.vertex && s.fragment)
      return true;
    if (!s.setLayout) {
      constexpr auto image = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      const VkDescriptorSetLayoutBinding bindings[] = {
          {0, image, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
          {1, image, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
          {2, image, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
          {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
           nullptr},
          {4, image, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
      const VkDescriptorSetLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                    .bindingCount = 5,
                                                    .pBindings = bindings};
      check(pipeline::createDescriptorSetLayout(device, &info, nullptr, &s.setLayout));
    }
    if (!s.layout) {
      const VkDescriptorSetLayout sets[] = {setLayout, s.setLayout};
      const VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
      const VkPipelineLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                               .setLayoutCount = 2,
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
    module(s.vertex, infectedVertShader, sizeof(infectedVertShader));
    module(s.fragment, infectedFragShader, sizeof(infectedFragShader));
    return true;
  }

  void RasterRenderer::Gpu::infectedParameters(const Draw &draw, Command &out, VkCommandBuffer command) {
    auto &s = infected;
    // shaders/infected.glsl Infected, std140: 15 vec4.
    struct Block {
      float ellipsoid[6][4], projection[2][4], cut[2][4], wound[4], rows[4], sprite[4], detail[4], blood[4];
    } block = {};
    static_assert(sizeof(Block) <= Infected::blockSize);
    // A texture through a sampler material: its descriptor (the albedo's
    // where the draw has none).
    const auto sampled = [&](const std::shared_ptr<Material> &sampler) {
      const auto &source = sampler ? sampler : draw.material;
      material(source, command);
      return VkDescriptorImageInfo{materials.at(source.get())->sampler, textures.at(source->texture.get()).view,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    };
    uint32_t flags = 0;
    std::array<VkDescriptorImageInfo, 4> images; // bindings 0-2 and 4
    if (const auto &w = draw.wound) {
      flags |= 1;
      for (unsigned e = 0; e < 2; ++e)
        for (unsigned axis = 0; axis < 3; ++axis) {
          const float *a = w->ellipsoids[e][1 + axis], *c = w->ellipsoids[e][0];
          auto *row = block.ellipsoid[3 * e + axis];
          std::copy_n(a, 3, row);
          row[3] = -(a[0] * c[0] + a[1] * c[1] + a[2] * c[2]);
        }
      std::copy_n(w->projectionScale, 4, block.projection[0]);
      std::copy_n(w->projectionBias, 4, block.projection[1]);
      std::copy_n(w->cutScaleBias[0], 4, block.cut[0]);
      std::copy_n(w->cutScaleBias[1], 4, block.cut[1]);
      block.wound[0] = w->mappingScale;
      block.wound[1] = w->secondBlend;
      block.wound[2] = w->threshold;
      images[2] = sampled(w->sampler);
    } else
      images[2] = sampled(nullptr);
    if (const auto &g = draw.material->gradient) {
      flags |= 2;
      images[0] = sampled(g->gradientSampler);
      if (textures.at(g->gradient.get()).srgb)
        flags |= 8;
      if (g->detailSampler) {
        flags |= 4;
        images[1] = sampled(g->detailSampler);
      } else
        images[1] = sampled(nullptr);
      block.rows[0] = g->skinRow;
      block.rows[1] = g->clothRow;
      std::copy_n(g->spriteOffset, 2, block.sprite);
      std::copy_n(g->bloodRange, 2, block.sprite + 2);
      std::copy_n(g->detailU, 2, block.detail);
      std::copy_n(g->detailV, 2, block.detail + 2);
      std::copy_n(g->bloodColor, 3, block.blood);
      // The burn (skipped at strength 0, where it changes nothing).
      if (g->burnSampler && g->burnStrength > 0) {
        flags |= 16;
        images[3] = sampled(g->burnSampler);
        if (textures.at(g->burn.get()).srgb)
          flags |= 32;
        block.rows[2] = g->burnStrength;
      } else
        images[3] = sampled(nullptr);
    } else
      images[0] = images[1] = images[3] = sampled(nullptr);
    std::memcpy(&block.wound[3], &flags, sizeof(flags));
    out.infected = int32_t(s.images.size());
    const size_t at = s.parameters.size();
    s.parameters.resize(at + Infected::blockSize);
    std::memcpy(s.parameters.data() + at, &block, sizeof(block));
    s.images.push_back(images);
  }

  void RasterRenderer::Gpu::finishInfected() {
    auto &s = infected;
    const auto count = uint32_t(s.images.size());
    if (!count) {
      s.parameters.clear();
      return;
    }
    // This slot's last frame has completed (prepare runs after its fence).
    auto &pool = s.pools[frameIndex];
    if (!pool || s.capacity[frameIndex] < count) {
      if (pool)
        vkDestroyDescriptorPool(device, pool, nullptr);
      pool = VK_NULL_HANDLE;
      const uint32_t capacity = std::max(64u, count + count / 2);
      const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * capacity},
                                            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, capacity}};
      const VkDescriptorPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                               .maxSets = capacity,
                                               .poolSizeCount = 2,
                                               .pPoolSizes = sizes};
      check(vkCreateDescriptorPool(device, &info, nullptr, &pool));
      s.capacity[frameIndex] = capacity;
    } else
      check(vkResetDescriptorPool(device, pool, 0));
    auto parameters = buffer(s.parameters.data(), s.parameters.size(), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    std::vector<VkDescriptorSetLayout> layouts(count, s.setLayout);
    std::vector<VkDescriptorSet> sets(count);
    const VkDescriptorSetAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                    .descriptorPool = pool,
                                                    .descriptorSetCount = count,
                                                    .pSetLayouts = layouts.data()};
    check(vkAllocateDescriptorSets(device, &allocation, sets.data()));
    std::vector<VkDescriptorBufferInfo> blocks(count);
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(size_t(count) * 5);
    for (uint32_t i = 0; i < count; ++i) {
      blocks[i] = {parameters->buffer, parameters->offset + VkDeviceSize(i) * Infected::blockSize,
                   Infected::blockSize};
      for (uint32_t image = 0; image < 4; ++image)
        writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                          .dstSet = sets[i],
                          .dstBinding = image < 3 ? image : 4,
                          .descriptorCount = 1,
                          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                          .pImageInfo = &s.images[i][image]});
      writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstSet = sets[i],
                        .dstBinding = 3,
                        .descriptorCount = 1,
                        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                        .pBufferInfo = &blocks[i]});
    }
    vkUpdateDescriptorSets(device, uint32_t(writes.size()), writes.data(), 0, nullptr);
    for (auto &command : commands)
      if (command.infected >= 0)
        command.infectedSet = sets[size_t(command.infected)];
    uploads.push_back(std::move(parameters));
    s.parameters.clear();
    s.images.clear();
  }
} // namespace remix
