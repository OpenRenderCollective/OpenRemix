#include "raster/gpu.h"
#include "runtime/pipelines.h"
#include "scene/images.h"
#include <remix/remix_native_ext.h>
#include <algorithm>

namespace remix {
  using namespace scene;
  using namespace raster::detail;

  namespace {
    // The CPU side accepts textures up to maxTextureDimension; past the
    // device's image limit a reduced copy is uploaded instead of failing the
    // frame: the first mip level that fits, the compressed preview, or a
    // single mean texel.
    Pixels oversizedFallback(const Pixels &p, uint32_t limit) {
      Pixels out{0, 0, p.srgb};
      out.twoChannel = p.twoChannel;
      if (p.compressed) {
        const auto &c = *p.compressed;
        out.width = c.previewWidth;
        out.height = c.previewHeight;
        out.mips = {c.preview};
        if (!c.hdrPreview.empty())
          out.hdrMips = {c.hdrPreview};
        return out;
      }
      uint32_t width = p.width, height = p.height;
      size_t first = 0;
      while ((width > limit || height > limit) && first + 1 < p.mips.size()) {
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
        ++first;
      }
      if (width <= limit && height <= limit) {
        out.width = width;
        out.height = height;
        out.mips.assign(p.mips.begin() + first, p.mips.end());
        if (first < p.hdrMips.size())
          out.hdrMips.assign(p.hdrMips.begin() + first, p.hdrMips.end());
        return out;
      }
      out.width = out.height = 1;
      const size_t texels = size_t(p.width) * p.height;
      double sum[4] = {};
      for (size_t i = 0; i < texels; ++i)
        for (int k = 0; k < 4; ++k)
          sum[k] += p.mips[0][4 * i + k];
      out.mips = {std::vector<uint8_t>(4)};
      for (int k = 0; k < 4; ++k)
        out.mips[0][k] = uint8_t(sum[k] / double(texels) + .5);
      if (!p.hdrMips.empty()) {
        std::vector<float> mean(4);
        for (size_t i = 0; i < texels; ++i)
          for (int k = 0; k < 4; ++k)
            mean[k] += p.hdrMips[0][4 * i + k] / float(texels);
        out.hdrMips = {std::move(mean)};
      }
      return out;
    }

    bool outOfMemory(VkResult result) {
      return result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    VkFormat blockFormat(uint32_t format) {
      switch (format) {
      case REMIXAPI_FORMAT_BC1_RGB_SRGB:
      case REMIXAPI_FORMAT_BC1_RGB_UNORM:
        return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
      case REMIXAPI_FORMAT_BC1_RGBA_SRGB_EXT:
      case REMIXAPI_FORMAT_BC1_RGBA_UNORM_EXT:
        return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
      case REMIXAPI_FORMAT_BC3_SRGB:
      case REMIXAPI_FORMAT_BC3_UNORM:
        return VK_FORMAT_BC3_UNORM_BLOCK;
      case REMIXAPI_FORMAT_BC4_UNORM_EXT:
        return VK_FORMAT_BC4_UNORM_BLOCK;
      case REMIXAPI_FORMAT_BC5_UNORM:
        return VK_FORMAT_BC5_UNORM_BLOCK;
      case REMIXAPI_FORMAT_BC6H_UFLOAT_EXT:
        return VK_FORMAT_BC6H_UFLOAT_BLOCK;
      case REMIXAPI_FORMAT_BC6H_SFLOAT_EXT:
        return VK_FORMAT_BC6H_SFLOAT_BLOCK;
      case REMIXAPI_FORMAT_BC7_SRGB:
      case REMIXAPI_FORMAT_BC7_UNORM:
        return VK_FORMAT_BC7_UNORM_BLOCK;
      default:
        return VK_FORMAT_UNDEFINED;
      }
    }
    // sRGB-encoded block formats: stored as UNORM like the rest, the shaders
    // decode them (pow 2.2).
    bool srgbBlock(uint32_t format) {
      return format == REMIXAPI_FORMAT_BC1_RGB_SRGB || format == REMIXAPI_FORMAT_BC1_RGBA_SRGB_EXT ||
             format == REMIXAPI_FORMAT_BC3_SRGB || format == REMIXAPI_FORMAT_BC7_SRGB;
    }
  } // namespace

  RasterRenderer::Gpu::MeshGpu &RasterRenderer::Gpu::mesh(const std::shared_ptr<Surface> &source,
                                                          VkCommandBuffer command) {
    auto it = meshes.find(source.get());
    if (it != meshes.end()) {
      return it->second;
    }
    const auto begin = std::chrono::steady_clock::now();
    struct Timed {
      double &total;
      std::chrono::steady_clock::time_point begin;
      ~Timed() { total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count(); }
    } timed{createMeshMs, begin};
    const size_t offset = source->vertices.size() * sizeof(Vertex);
    // Sprite vertices (remixapi_MeshInfoSpriteVerticesEXT) follow the indices.
    const size_t spriteOffset = offset + source->indices.size() * sizeof(uint32_t);
    std::vector<uint8_t> data(spriteOffset + source->sprite.size() * sizeof(SpriteVertex));
    std::memcpy(data.data(), source->vertices.data(), offset);
    std::memcpy(data.data() + offset, source->indices.data(), source->indices.size() * sizeof(uint32_t));
    if (!source->sprite.empty())
      std::memcpy(data.data() + spriteOffset, source->sprite.data(), source->sprite.size() * sizeof(SpriteVertex));
    MeshGpu value{source,
                  geometryBuffer(data.data(), data.size(),
                                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                 command),
                  offset, spriteOffset};
    return meshes.emplace(source.get(), std::move(value)).first->second;
  }

  RasterRenderer::Gpu::TextureGpu &RasterRenderer::Gpu::texture(const std::shared_ptr<Texture> &source,
                                                                VkCommandBuffer command) {
    auto it = textures.find(source.get());
    if (it != textures.end()) {
      if (source->updates)
        refreshTexture(it->second, *source, command);
      return it->second;
    }
    struct Timed {
      double &total;
      std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
      ~Timed() { total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count(); }
    } timed{createTextureMs};
    ++texturesCreated;
    if (!source->updates)
      return createTexture(source, command);
    // An updatable texture's texels change under its lock: the image starts
    // from a consistent state at the current revision.
    auto &updates = *source->updates;
    std::lock_guard lock(updates.lock);
    auto &value = createTexture(source, command);
    value.revision = updates.revision;
    value.updatable = true;
    std::fill_n(updates.dirty, 4, 0u);
    return value;
  }

  void RasterRenderer::Gpu::refreshTexture(TextureGpu &value, const Texture &source, VkCommandBuffer command) {
    auto &updates = *source.updates;
    std::lock_guard lock(updates.lock);
    if (updates.revision == value.revision)
      return;
    value.revision = updates.revision;
    const uint32_t x0 = updates.dirty[0], y0 = updates.dirty[1], x1 = updates.dirty[2], y1 = updates.dirty[3];
    std::fill_n(updates.dirty, 4, 0u);
    const auto &p = source.pixels;
    // Region updates cover plain RGBA8 images at full size. One with mips
    // (generated again by every update) is uploaded whole, every level.
    if (x1 <= x0 || y1 <= y0 || !value.data || p.compressed || !p.hdrMips.empty() ||
        value.data->width != p.width || value.data->height != p.height || x1 > p.width || y1 > p.height)
      return;
    if (p.mips.size() > 1) {
      refreshLevels(value, source, command);
      return;
    }
    const uint32_t width = x1 - x0, height = y1 - y0;
    CounterTicks time{textureCounters.uploadTicks};
    dynamicTextureUploads += source.dynamic;
    textureCounters.uploads.fetch_add(1, std::memory_order_relaxed);
    textureCounters.uploadBytes.fetch_add(uint64_t(width) * height * 4, std::memory_order_relaxed);
    uploads.push_back(buffer(size_t(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, [&](uint8_t *mapped) {
      for (uint32_t y = 0; y < height; ++y)
        std::memcpy(mapped + size_t(y) * width * 4, p.mips[0].data() + (size_t(y0 + y) * p.width + x0) * 4,
                    size_t(width) * 4);
    }));
    // Earlier frames on this queue may still sample the image: wait for
    // their reads, keep the untouched texels (no UNDEFINED transition).
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
                                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = value.data->image,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
    const VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageOffset = {int32_t(x0), int32_t(y0), 0},
                                    .imageExtent = {width, height, 1}};
    vkCmdCopyBufferToImage(command, uploads.back()->buffer, value.data->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
  }

  void RasterRenderer::Gpu::refreshLevels(TextureGpu &value, const Texture &source, VkCommandBuffer command) {
    const auto &p = source.pixels;
    size_t total = 0;
    for (const auto &level : p.mips)
      total += level.size();
    CounterTicks time{textureCounters.uploadTicks};
    dynamicTextureUploads += source.dynamic;
    textureCounters.uploads.fetch_add(1, std::memory_order_relaxed);
    textureCounters.uploadBytes.fetch_add(total, std::memory_order_relaxed);
    uploads.push_back(buffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, [&](uint8_t *mapped) {
      for (const auto &level : p.mips) {
        std::memcpy(mapped, level.data(), level.size());
        mapped += level.size();
      }
    }));
    const uint32_t levels = uint32_t(p.mips.size());
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
                                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = value.data->image,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
    std::vector<VkBufferImageCopy> copies;
    VkDeviceSize offset = 0;
    for (uint32_t level = 0; level < levels; ++level) {
      const uint32_t w = std::max(1u, p.width >> level), h = std::max(1u, p.height >> level);
      copies.push_back({.bufferOffset = offset,
                        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1},
                        .imageExtent = {w, h, 1}});
      offset += p.mips[level].size();
    }
    vkCmdCopyBufferToImage(command, uploads.back()->buffer, value.data->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           uint32_t(copies.size()), copies.data());
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
  }

  RasterRenderer::Gpu::TextureGpu &RasterRenderer::Gpu::createTexture(const std::shared_ptr<Texture> &source,
                                                                      VkCommandBuffer command) {
    CounterTicks time{textureCounters.gpuCreateTicks};
    textureCounters.gpuTexturesCreated.fetch_add(1, std::memory_order_relaxed);
    struct Staged {
      const uint64_t &bytes, before = bytes;
      ~Staged() { textureCounters.gpuCreateBytes.fetch_add(bytes - before, std::memory_order_relaxed); }
    } staged{uploadBytes};
    const uint32_t limit = properties.limits.maxImageDimension2D;
    if (source->pixels.width > limit || source->pixels.height > limit)
      return textures.emplace(source.get(), uploadPixels(source, oversizedFallback(source->pixels, limit), command, 0))
          .first->second;
    if (source->pixels.compressed)
      return compressedTexture(source, command);
    return textures.emplace(source.get(), uploadPixels(source, source->pixels, command, 0)).first->second;
  }

  uint32_t RasterRenderer::Gpu::textureMipSkip(uint32_t width, uint32_t height, uint32_t levels) const {
    if (!memoryBudget || levels <= 1 || std::max(width, height) <= 1024)
      return 0;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
                                                    .pNext = &budget};
    vkGetPhysicalDeviceMemoryProperties2(physical, &properties);
    double used = 0;
    for (uint32_t i = 0; i < properties.memoryProperties.memoryHeapCount; ++i)
      if ((properties.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && budget.heapBudget[i])
        used = std::max(used, double(budget.heapUsage[i]) / double(budget.heapBudget[i]));
    uint32_t skip = used > .9 ? 2 : used > .75 ? 1 : 0;
    while (skip && std::max(width, height) >> skip < 512)
      --skip;
    return std::min(skip, levels - 1);
  }

  RasterRenderer::Gpu::TextureGpu RasterRenderer::Gpu::uploadPixels(const std::shared_ptr<Texture> &source,
                                                                    const Pixels &p, VkCommandBuffer command,
                                                                    VkImageCreateFlags flags) {
    const uint32_t available = uint32_t(p.mips.size());
    // sRGB texels stay encoded: the shaders decode them (pow 2.2).
    const VkFormat format = !p.hdrMips.empty() ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    const auto queryBegin = std::chrono::steady_clock::now();
    uint32_t skip = textureMipSkip(p.width, p.height, available);
    textureCosts.query +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - queryBegin).count();
    std::unique_ptr<Image> allocated;
    for (;;) {
      try {
        allocated = image(std::max(1u, p.width >> skip), std::max(1u, p.height >> skip), available - skip, format,
                          false, flags);
        break;
      } catch (VkResult result) {
        if (!outOfMemory(result) || skip + 1 >= available)
          throw;
        ++skip;
      }
    }
    const uint32_t levels = available - skip;
    TextureGpu value{source, std::move(allocated)};
    value.view = value.data->view;
    value.srgb = p.srgb && p.hdrMips.empty();
    // Level bytes go straight into the staging buffer.
    auto level = [&](uint32_t i) -> std::pair<const uint8_t *, size_t> {
      i += skip;
      if (p.hdrMips.empty())
        return {p.mips[i].data(), p.mips[i].size()};
      return {reinterpret_cast<const uint8_t *>(p.hdrMips[i].data()), p.hdrMips[i].size() * sizeof(float)};
    };
    std::vector<VkBufferImageCopy> copies;
    size_t total = 0;
    uint32_t width = std::max(1u, p.width >> skip), height = std::max(1u, p.height >> skip);
    for (uint32_t i = 0; i < levels; ++i) {
      copies.push_back({.bufferOffset = total,
                        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1},
                        .imageExtent = {width, height, 1}});
      total += level(i).second;
      width = std::max(1u, width / 2);
      height = std::max(1u, height / 2);
    }
    const auto stageBegin = std::chrono::steady_clock::now();
    uploads.push_back(buffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, [&](uint8_t *mapped) {
      for (uint32_t i = 0; i < levels; ++i) {
        const auto [bytes, size] = level(i);
        if (size)
          std::memcpy(mapped + copies[i].bufferOffset, bytes, size);
      }
    }));
    textureCosts.stage +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stageBegin).count();
    copyLevels(command, value.data->image, levels, copies);
    return value;
  }

  void RasterRenderer::Gpu::copyLevels(VkCommandBuffer command, VkImage image, uint32_t levels,
                                       const std::vector<VkBufferImageCopy> &copies) {
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = image,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
    vkCmdCopyBufferToImage(command, uploads.back()->buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels,
                           copies.data());
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
  }

  RasterRenderer::Gpu::TextureGpu &RasterRenderer::Gpu::compressedTexture(const std::shared_ptr<Texture> &source,
                                                                          VkCommandBuffer command) {
    const auto &p = source->pixels;
    auto &c = *p.compressed;
    // A linear data map of an sRGB compressed texture (dataTexturePath) shares
    // the source's image, read without the decode: no second copy.
    if (const auto &base = source->encodedSource; base && base->pixels.compressed == p.compressed) {
      const auto view = texture(base, command).view;
      return textures.emplace(source.get(), TextureGpu{source, nullptr, view}).first->second;
    }
    const VkImageCreateFlags flags = 0;
    // Kept for USD captures while scene::retainCompressedUploads is set.
    auto release = [&] {
      if (!scene::retainCompressedUploads.load(std::memory_order_relaxed))
        std::vector<uint8_t>().swap(c.upload);
    };
    const VkFormat format = textureCompression ? blockFormat(c.format) : VK_FORMAT_UNDEFINED;
    if (c.upload.empty() || format == VK_FORMAT_UNDEFINED) {
      // Without BC sampling, decode once from the payload. A released payload
      // (never expected: images live as long as their texture) degrades to the
      // small preview level instead of failing the frame.
      Pixels fallback;
      if (!c.upload.empty()) {
        fallback = decodeCompressed(p);
      } else {
        fallback = {c.previewWidth, c.previewHeight, p.srgb, {c.preview}};
        if (!c.hdrPreview.empty())
          fallback.hdrMips = {c.hdrPreview};
      }
      auto value = uploadPixels(source, fallback, command, flags);
      release();
      return textures.emplace(source.get(), std::move(value)).first->second;
    }
    // BC4 is sampled as (r, 0, 0, 1); the decoded path replicates r into rgb.
    const VkComponentMapping components =
        c.block == 4 ? VkComponentMapping{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                                          VK_COMPONENT_SWIZZLE_ONE}
                     : VkComponentMapping{};
    const VkDeviceSize blockSize = (c.block == 1 || c.block == 4) ? 8 : 16;
    auto levelBytes = [&](uint32_t level) {
      const uint32_t w = std::max(1u, p.width >> level), h = std::max(1u, p.height >> level);
      return VkDeviceSize((w + 3) / 4) * ((h + 3) / 4) * blockSize;
    };
    VkDeviceSize total = 0;
    for (uint32_t i = 0; i < c.levels; ++i)
      total += levelBytes(i);
    if (total != c.upload.size())
      throw VK_ERROR_INITIALIZATION_FAILED;
    // Budgeted top-mip skipping; out of memory drops further levels and, at
    // the last one, falls back to the preview instead of failing the frame.
    uint32_t skip = textureMipSkip(p.width, p.height, c.levels);
    std::unique_ptr<Image> allocated;
    for (;;) {
      try {
        allocated = image(std::max(1u, p.width >> skip), std::max(1u, p.height >> skip), c.levels - skip, format,
                          false, flags, components);
        break;
      } catch (VkResult result) {
        if (!outOfMemory(result))
          throw;
        if (skip + 1 < c.levels) {
          ++skip;
          continue;
        }
        Pixels preview = {c.previewWidth, c.previewHeight, p.srgb, {c.preview}};
        if (!c.hdrPreview.empty())
          preview.hdrMips = {c.hdrPreview};
        auto value = uploadPixels(source, preview, command, flags);
        release();
        return textures.emplace(source.get(), std::move(value)).first->second;
      }
    }
    TextureGpu value{source, std::move(allocated)};
    value.view = value.data->view;
    value.srgb = srgbBlock(c.format);
    std::vector<VkBufferImageCopy> copies;
    VkDeviceSize first = 0, offset = 0;
    for (uint32_t i = 0; i < skip; ++i)
      first += levelBytes(i);
    for (uint32_t i = skip; i < c.levels; ++i) {
      const uint32_t w = std::max(1u, p.width >> i), h = std::max(1u, p.height >> i);
      copies.push_back({.bufferOffset = offset,
                        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - skip, 0, 1},
                        .imageExtent = {w, h, 1}});
      offset += levelBytes(i);
    }
    uploads.push_back(buffer(c.upload.data() + first, c.upload.size() - first, VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
    copyLevels(command, value.data->image, c.levels - skip, copies);
    // The staging buffer and then the image are now the only copies.
    release();
    return textures.emplace(source.get(), std::move(value)).first->second;
  }

  VkDescriptorSet RasterRenderer::Gpu::material(const std::shared_ptr<Material> &source,
                                                VkCommandBuffer command) {
    if (auto it = materials.find(source.get()); it != materials.end()) {
      return it->second->set;
    }
    struct Timed {
      double &total;
      std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
      ~Timed() { total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count(); }
    } timed{createMaterialMs};
    auto &tex = texture(source->texture, command);
    CounterTicks time{textureCounters.gpuCreateTicks};
    textureCounters.gpuMaterialsCreated.fetch_add(1, std::memory_order_relaxed);
    auto value = std::make_unique<MaterialGpu>();
    value->device = device;
    value->pool = pool;
    value->owner = source;
    auto address = [](uint8_t mode) {
      // Material::wrapU/wrapV (Remix's MDL encoding, scene/types.h).
      switch (mode) {
      case 0:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      case 2:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
      case 3:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
      default:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
      }
    };
    const VkSamplerCreateInfo sampler = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                         .magFilter = source->filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                         .minFilter = source->filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                         .mipmapMode = source->filter ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                                      : VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                         .addressModeU = address(source->wrapU),
                                         .addressModeV = address(source->wrapV),
                                         .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                         .anisotropyEnable = source->filter && maxAnisotropy > 1,
                                         .maxAnisotropy = std::max(1.f, maxAnisotropy),
                                         .maxLod = float(source->texture->pixels.levels() - 1),
                                         .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
    check(pipeline::createSampler(device, &sampler, nullptr, &value->sampler));
    const VkDescriptorSetAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                    .descriptorPool = pool,
                                                    .descriptorSetCount = 1,
                                                    .pSetLayouts = &setLayout};
    check(vkAllocateDescriptorSets(device, &allocation, &value->set));
    const VkDescriptorImageInfo imageInfo = {value->sampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                        .dstSet = value->set,
                                        .dstBinding = 0,
                                        .descriptorCount = 1,
                                        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                        .pImageInfo = &imageInfo};
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    const auto set = value->set;
    materials.emplace(source.get(), std::move(value));
    return set;
  }

  uint64_t RasterRenderer::dynamicTextureGeneration() const { return m_gpu ? m_gpu->dynamicTextureUploads : 0; }

  VkDescriptorImageInfo RasterRenderer::sampledMaterial(const std::shared_ptr<Material> &source,
                                                        VkCommandBuffer command) {
    m_gpu->material(source, command);
    const auto &mat = *m_gpu->materials.at(source.get());
    return {mat.sampler, m_gpu->textures.at(source->texture.get()).view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  }

  VkDescriptorImageInfo RasterRenderer::sampledEmission(const std::shared_ptr<Material> &source,
                                                        VkCommandBuffer command) {
    return sampledTexture(source, source->emissionTexture, command);
  }

  VkDescriptorImageInfo RasterRenderer::sampledTexture(const std::shared_ptr<Material> &source,
                                                       const std::shared_ptr<Texture> &image,
                                                       VkCommandBuffer command) {
    m_gpu->material(source, command);
    auto &texture = m_gpu->texture(image, command);
    return {m_gpu->materials.at(source.get())->sampler, texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  }

  bool RasterRenderer::textureSrgb(const scene::Texture *texture) const {
    if (!m_gpu || !texture)
      return false;
    auto it = m_gpu->textures.find(texture);
    return it != m_gpu->textures.end() && it->second.srgb;
  }

  VkImageView RasterRenderer::textureView(const scene::Texture *texture) const {
    if (!m_gpu || !texture)
      return VK_NULL_HANDLE;
    auto it = m_gpu->textures.find(texture);
    if (it != m_gpu->textures.end())
      return it->second.view;
    return VK_NULL_HANDLE;
  }
} // namespace remix
