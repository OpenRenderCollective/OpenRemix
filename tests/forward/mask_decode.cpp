#include "forward/mixed/masks.h"
#include "forward/mixed/mask_codec.h"
#include "forward/mixed/mask_pages.h"
#include "forward/mixed/map.h"
#include "gpu_fixture.h"
#include "mask_shaders/embedded.h"
#include <cmath>

using namespace remix;
using namespace remix::forward;
using namespace remix::forward::test;
namespace mask = remix::forward::mixed::mask;
int main() try {
  Fixture fixture(false); // Decoder requires only core Vulkan, not hardware RT.
  struct Gpu {
    VkDevice device;
    Buffer asset, input, output, staging;
    std::unique_ptr<mixed::OcclusionMap> pages;
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkDescriptorPool pool{};
    ~Gpu() {
      if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
      if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
      if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
      if (setLayout) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    }
  } gpu{fixture.device};
  lightmap::Layout layout;
  layout.charts = {{0, 0, 0, 32, 16}, {0, 0, 0, 17, 9}};
  struct Query { bake::UInt4 identity; Vec4 coordinate; };
  std::vector<Query> queries;
  std::vector<float> expected;
  std::vector<mixed::Masks::Entry> entries;
  std::vector<uint8_t> bytes;
  for (uint32_t mode = 0; mode < 9; ++mode) {
    const uint32_t chart = mode == 1 ? 1 : 0, light = mode >= 2 ? mode - 1 : 0;
    const auto &a = layout.charts[chart];
    std::vector<uint8_t> pixels(size_t(a.width) * a.height);
    for (uint32_t y = 0; y < a.height; ++y)
      for (uint32_t x = 0; x < a.width; ++x) {
        uint8_t value = 73;
        if (mode == 0) {
          value = x < 8 ? 0 : x < 16 ? ((x + y) & 1 ? 255 : 0) :
                  x < 24 ? uint8_t((x * 23 + y * 19) & 255) : 255;
        } else if (mode == 1)
          value = uint8_t((x * 123 + y * 29 + x * y * 17) & 255);
        else if (mode == 3) {
          constexpr uint8_t endpoints[] = {0, 1, 254, 255};
          constexpr uint8_t eightColors[] = {0, 1, 40, 80, 120, 160, 220, 255};
          value = x < 8 ? endpoints[(x + y) % 4] : x < 16 ? eightColors[(x + y) % 8] :
                  x < 24 ? uint8_t(1 + (x + y) % 8) : uint8_t(((x + y * 8) % 16) * 17);
        }
        pixels[size_t(y) * a.width + x] = value;
      }
    const auto packed = mode == 3 ? mask::encodeCompact(pixels, a.width, a.height)
                                  : mask::encode(pixels, a.width, a.height);
    require(packed.encoding == (mode == 1 ? mask::raw : mask::tiled), "GPU fixture exercises raw and tiled chunks");
    entries.push_back({chart, light, uint32_t(bytes.size()), a.width, a.height, packed.encoding});
    bytes.insert(bytes.end(), packed.bytes.begin(), packed.bytes.end());
    // Every centre, half-texel interpolation, tile boundary and chart edge.
    for (uint32_t y = 0; y < a.height * 2 - 1; ++y)
      for (uint32_t x = 0; x < a.width * 2 - 1; ++x) {
        queries.push_back({{chart, light + 1, 1, 0},
                           {float(x) / (2 * (a.width - 1)), float(y) / (2 * (a.height - 1)), 0, 1}});
        const uint32_t x0 = x / 2, x1 = std::min(x0 + 1, a.width - 1),
                       y0 = y / 2, y1 = std::min(y0 + 1, a.height - 1);
        const float fx = (x & 1) * .5f, fy = (y & 1) * .5f;
        const auto sample = [&](uint32_t px, uint32_t py) {
          return (mode == 3 ? mask::texel(packed.bytes, a.width, packed.encoding, px, py)
                            : pixels[size_t(py) * a.width + px]) / 255.f;
        };
        expected.push_back(std::lerp(std::lerp(sample(x0, y0), sample(x1, y0), fx),
                                    std::lerp(sample(x0, y1), sample(x1, y1), fx), fy));
      }
  }
  const auto masks = mixed::Masks::restore(entries, bytes, layout, 8);
  auto words = masks.words(2);
  words.insert(words.begin(), 0); // Tail base zero is reserved for absent masks.
  const auto pages = mixed::packMaskPages(masks, 32, 16, 1u << 20);
  require(pages && pages->layers > 4, "GPU fixture exercises many independent array layers");
  const uint32_t pageBase = uint32_t(words.size());
  const auto pageWords = pages->words(2);
  words.insert(words.end(), pageWords.begin(), pageWords.end());
  const size_t originalQueries = queries.size();
  for (size_t i = 0; i < originalQueries; ++i) {
    auto query = queries[i];
    query.identity[2] = pageBase;
    queries.push_back(query);
    expected.push_back(expected[i]);
  }
  const VkDescriptorSetLayoutBinding bindings[] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
  const VkDescriptorSetLayoutCreateInfo setInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                               .bindingCount = 4, .pBindings = bindings};
  check(vkCreateDescriptorSetLayout(fixture.device, &setInfo, nullptr, &gpu.setLayout));
  struct Push { bake::UInt4 occlusion; uint32_t count; } push{{0, 2, 0, 1}, uint32_t(queries.size())};
  const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
  const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1, .pSetLayouts = &gpu.setLayout, .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
  check(vkCreatePipelineLayout(fixture.device, &layoutInfo, nullptr, &gpu.layout));
  VkShaderModule module;
  const VkShaderModuleCreateInfo shaderInfo{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(mask_input::decodeShader), .pCode = mask_input::decodeShader};
  check(vkCreateShaderModule(fixture.device, &shaderInfo, nullptr, &module));
  const VkComputePipelineCreateInfo pipelineInfo{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = module, .pName = "main"}, .layout = gpu.layout};
  const auto result = vkCreateComputePipelines(fixture.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &gpu.pipeline);
  vkDestroyShaderModule(fixture.device, module, nullptr);
  check(result);
  gpu.asset.create(fixture.physical, fixture.device, words.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  gpu.asset.write(words.data(), words.size() * 4);
  gpu.input.create(fixture.physical, fixture.device, queries.size() * sizeof(Query), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  gpu.input.write(queries.data(), queries.size() * sizeof(Query));
  gpu.output.create(fixture.physical, fixture.device, queries.size() * sizeof(Vec4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  gpu.pages = std::make_unique<mixed::OcclusionMap>(fixture.physical, fixture.device,
      VkExtent2D{pages->size, pages->size}, VK_FORMAT_R8_UNORM, pages->layers);
  gpu.staging.create(fixture.physical, fixture.device, pages->texels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  gpu.staging.write(pages->texels.data(), pages->texels.size());
  struct Sampler {
    VkDevice device;
    VkSampler handle{};
    ~Sampler() { if (handle) vkDestroySampler(device, handle, nullptr); }
  } sampler{fixture.device};
  const VkSamplerCreateInfo samplerInfo{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
  check(vkCreateSampler(fixture.device, &samplerInfo, nullptr, &sampler.handle));
  const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
                                      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
  const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes};
  check(vkCreateDescriptorPool(fixture.device, &poolInfo, nullptr, &gpu.pool));
  VkDescriptorSet set;
  const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = gpu.pool, .descriptorSetCount = 1, .pSetLayouts = &gpu.setLayout};
  check(vkAllocateDescriptorSets(fixture.device, &allocate, &set));
  const VkDescriptorBufferInfo buffers[] = {gpu.asset.descriptor(), gpu.input.descriptor(), gpu.output.descriptor()};
  VkWriteDescriptorSet writes[3]{};
  for (uint32_t i = 0; i < 3; ++i)
    writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i,
      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffers[i]};
  vkUpdateDescriptorSets(fixture.device, 3, writes, 0, nullptr);
  auto pageImage = gpu.pages->descriptor();
  pageImage.sampler = sampler.handle;
  const VkWriteDescriptorSet pageWrite{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = set, .dstBinding = 3, .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &pageImage};
  vkUpdateDescriptorSets(fixture.device, 1, &pageWrite, 0, nullptr);
  fixture.begin();
  gpu.pages->upload(fixture.command, gpu.staging.handle, 0);
  vkCmdBindPipeline(fixture.command, VK_PIPELINE_BIND_POINT_COMPUTE, gpu.pipeline);
  vkCmdBindDescriptorSets(fixture.command, VK_PIPELINE_BIND_POINT_COMPUTE, gpu.layout, 0, 1, &set, 0, nullptr);
  vkCmdPushConstants(fixture.command, gpu.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
  vkCmdDispatch(fixture.command, (push.count + 63) / 64, 1, 1);
  const VkMemoryBarrier read{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
  vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                       0, 1, &read, 0, nullptr, 0, nullptr);
  fixture.finish();
  std::vector<Vec4> values(queries.size());
  gpu.output.read(values.data(), values.size() * sizeof(Vec4));
  for (size_t i = 0; i < values.size(); ++i)
    require(values[i][1] == 1 && std::abs(values[i][0] - expected[i]) < 2e-6f,
            "Production GPU mask decoder preserves R8 bilinear sampling across tile and image edges");
  std::printf("GPU visibility decoding: %zu raw/constant/binary/soft tile and bilinear samples passed\n", values.size());
} catch (VkResult error) {
  std::fprintf(stderr, "GPU mask decoder failed: %d\n", int(error));
  return 1;
}
