#include "forward/probes/image_data.h"
#include "gpu_fixture.h"
#include <bit>
#include <cstring>

using namespace remix::forward;
using namespace remix::forward::test;

void uploadCase(uint32_t count, uint32_t size) {
  Fixture fixture(false); // No optional cube-array or descriptor-indexing features.
  probes::Data source;
  const uint32_t levels = std::bit_width(size), stride = probes::levelOffset(size, levels);
  source.header.parameters = {count, size, levels, stride};
  source.texels.resize(size_t(count) * stride);
  for (size_t i = 0; i < source.texels.size(); ++i)
    source.texels[i] = {float(i % 1000) / 16, float(i % 31), .001f, 1000000};
  const auto packed = probes::prepareImage(source);
  const auto bytes = packed.pixels.size() * sizeof(uint16_t);
  Buffer upload, readback;
  upload.create(fixture.physical, fixture.device, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  upload.write(packed.pixels.data(), bytes);
  readback.create(fixture.physical, fixture.device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buffer::Memory::Readback);
  Image image;
  image.create(fixture.physical, fixture.device, {size, 6*size}, VK_FORMAT_R16G16B16A16_SFLOAT,
               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
               VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, count, levels, VK_IMAGE_VIEW_TYPE_2D_ARRAY);
  std::vector<VkBufferImageCopy> copies;
  for (uint32_t level = 0; level < levels; ++level)
    copies.push_back({.bufferOffset = packed.levels[level].byteOffset,
                      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, count},
                      .imageExtent = {size >> level, 6*(size >> level), 1}});
  fixture.begin();
  VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image.handle,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, count}};
  vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &barrier);
  vkCmdCopyBufferToImage(fixture.command, upload.handle, image.handle,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()), copies.data());
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &barrier);
  vkCmdCopyImageToBuffer(fixture.command, image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         readback.handle, uint32_t(copies.size()), copies.data());
  const VkMemoryBarrier host{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
  vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                       0, 1, &host, 0, nullptr, 0, nullptr);
  fixture.finish();
  std::vector<uint16_t> actual(packed.pixels.size());
  readback.read(actual.data(), bytes);
  require(actual == packed.pixels, "Every reflection layer, face and mip survives GPU image upload/readback");
  require(validationErrors == 0, "Reflection image upload is Vulkan validation clean");
  std::printf("Reflection image: %u layers, %u-square faces, %u mips, %zu bytes passed\n",
              count, size, levels, bytes);
}
int main() try {
  uploadCase(64, 8);
  uploadCase(1, 256);
  uploadCase(1, 512);
} catch (VkResult error) {
  std::fprintf(stderr, "Reflection image upload failed: %d\n", int(error));
  return 1;
}
