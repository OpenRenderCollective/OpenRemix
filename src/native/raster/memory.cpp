#include "raster/gpu.h"

namespace remix {
  using namespace scene;
  using namespace raster::detail;

  void RasterRenderer::Gpu::recycle(std::unique_ptr<Buffer> buffer) {
    // Called only after the frame fence completes. The buffer just used is the
    // one most likely to be asked for again (a frame's staging, skinned
    // vertices and transient meshes come back every frame), so it is always
    // kept and what the pool has held longest goes when it is over its bound.
    // A full pool that turned the newcomer away instead kept leftovers of one
    // large upload forever and freed and allocated device memory every frame,
    // which costs milliseconds a buffer (Garry's Mod, raster backend: 7-25 ms
    // of a frame's preparation). One eviction a call: a pool far over its
    // bound empties over some frames, not in one.
    if (!buffer || buffer->arena)
      return; // A part of an arena goes back to it.
    constexpr uint64_t bound = 64u * 1024u * 1024u;
    reusableBytes += buffer->allocationSize;
    reusableBuffers.push_back(std::move(buffer));
    if (reusableBytes > bound && reusableBuffers.size() > 1) {
      reusableBytes -= reusableBuffers.front()->allocationSize;
      reusableBuffers.erase(reusableBuffers.begin());
    }
  }

  uint32_t RasterRenderer::Gpu::memoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
      if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) {
        return i;
      }
    }
    throw VK_ERROR_FEATURE_NOT_PRESENT;
  }

  std::unique_ptr<Buffer> RasterRenderer::Gpu::allocateBuffer(size_t size, VkBufferUsageFlags usage,
                                                              VkMemoryPropertyFlags flags) {
    if (!size) {
      throw VK_ERROR_INITIALIZATION_FAILED;
    }
    auto best = reusableBuffers.end();
    for (auto it = reusableBuffers.begin(); it != reusableBuffers.end(); ++it) {
      if ((*it)->usage == usage && (*it)->flags == flags && (*it)->capacity >= size &&
          (best == reusableBuffers.end() || (*it)->capacity < (*best)->capacity))
        best = it;
    }
    if (best != reusableBuffers.end()) {
      auto out = std::move(*best);
      reusableBytes -= out->allocationSize;
      reusableBuffers.erase(best);
      return out;
    }
    // Capacities in a few sizes, so a buffer fits the next request of about
    // its size (a frame's meshes and vertex counts vary): powers of two up to
    // 1 MiB, 64 KiB steps above (large buffers are mostly kept, not recycled).
    if (size <= 1024u * 1024u) {
      size_t capacity = 4096;
      while (capacity < size)
        capacity *= 2;
      size = capacity;
    } else
      size = (size + 65535) & ~size_t(65535);
    auto out = std::make_unique<Buffer>();
    out->device = device;
    out->capacity = size;
    out->usage = usage;
    out->flags = flags;
    const VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                     .size = size,
                                     .usage = usage,
                                     .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    check(vkCreateBuffer(device, &info, nullptr, &out->buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, out->buffer, &requirements);
    out->allocationSize = requirements.size;
    const VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                             .allocationSize = requirements.size,
                                             .memoryTypeIndex = memoryType(requirements.memoryTypeBits, flags)};
    if (flags == VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      stagingMemoryFlags = memory.memoryTypes[allocation.memoryTypeIndex].propertyFlags;
    check(vkAllocateMemory(device, &allocation, nullptr, &out->memory));
    check(vkBindBufferMemory(device, out->buffer, out->memory, 0));
    return out;
  }

  std::unique_ptr<Buffer> RasterRenderer::Gpu::buffer(const void *data, size_t size, VkBufferUsageFlags usage) {
    return buffer(size, usage, [&](uint8_t *mapped) { std::memcpy(mapped, data, size); });
  }

  std::unique_ptr<Buffer> RasterRenderer::Gpu::buffer(size_t size, VkBufferUsageFlags usage,
                                                      const std::function<void(uint8_t *)> &write) {
    const auto begin = std::chrono::steady_clock::now();
    const size_t pooled = reusableBuffers.size();
    auto out = allocateBuffer(size, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    stagingAllocated += reusableBuffers.size() == pooled; // not taken from the pool
    ++stagingBuffers;
    uploadBytes += size;
    void *mapped;
    check(vkMapMemory(device, out->memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    const auto mappedAt = std::chrono::steady_clock::now();
    stagingAllocateMs += std::chrono::duration<double, std::milli>(mappedAt - begin).count();
    write(static_cast<uint8_t *>(mapped));
    stagingWriteMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mappedAt).count();
    const VkMappedMemoryRange flush = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = out->memory, .offset = 0, .size = VK_WHOLE_SIZE};
    const auto result = vkFlushMappedMemoryRanges(device, 1, &flush);
    vkUnmapMemory(device, out->memory);
    check(result);
    return out;
  }

  void RasterRenderer::Gpu::recycle(std::unique_ptr<Image> image) {
    // Called after the image's last-use fence. Only large images are worth
    // keeping; a few suffice for a per-frame overlay in flight.
    if (!image || image->bytes < 1024 * 1024)
      return;
    reusableImages.push_back(std::move(image));
    if (reusableImages.size() > 4)
      reusableImages.erase(reusableImages.begin());
  }

  void RasterRenderer::Gpu::launchSpareImageBlock() {
    const bool quiet = imagesCreated == imagesSeen;
    imagesSeen = imagesCreated;
    if (!spareImageWanted || (!(++spareImageFrames > 1 && quiet) && spareImageFrames < 120))
      return;
    spareImageBlock = std::async(std::launch::async, allocateBlock, device, imageBlockSize, *spareImageWanted, true);
    spareImageWanted.reset();
  }

  std::shared_ptr<MemoryBlock> RasterRenderer::Gpu::allocateBlock(VkDevice device, VkDeviceSize size, uint32_t type,
                                                                  bool warm) {
    auto block = std::make_shared<MemoryBlock>();
    block->device = device;
    block->size = size;
    block->type = type;
    const VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = size, .memoryTypeIndex = type};
    // Out of memory: no block, and the image gets memory of its own (or its
    // caller's smaller mip chain).
    if (vkAllocateMemory(device, &allocation, nullptr, &block->memory) != VK_SUCCESS)
      return nullptr;
    block->free.emplace(0, size);
    // The driver finishes a memory allocation at its first use: the first
    // image bound into a new block took ~15 ms, in the frame that created the
    // texture. A spare, allocated on another thread ahead of its use, pays
    // that there: an image bound and destroyed again.
    if (warm) {
      const VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                      .imageType = VK_IMAGE_TYPE_2D,
                                      .format = VK_FORMAT_R8G8B8A8_UNORM,
                                      .extent = {64, 64, 1},
                                      .mipLevels = 1,
                                      .arrayLayers = 1,
                                      .samples = VK_SAMPLE_COUNT_1_BIT,
                                      .tiling = VK_IMAGE_TILING_OPTIMAL,
                                      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
      VkImage first = VK_NULL_HANDLE;
      if (vkCreateImage(device, &info, nullptr, &first) == VK_SUCCESS) {
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(device, first, &requirements);
        if ((requirements.memoryTypeBits >> type & 1) && requirements.size <= size)
          vkBindImageMemory(device, first, block->memory, 0);
        vkDestroyImage(device, first, nullptr);
      }
    }
    return block;
  }

  std::unique_ptr<Buffer> RasterRenderer::Gpu::part(std::vector<std::shared_ptr<Arena>> &arenas, VkDeviceSize size,
                                                    VkBufferUsageFlags usage, VkMemoryPropertyFlags flags,
                                                    VkDeviceSize arenaSize) {
    size = (size + 15) & ~VkDeviceSize(15);
    std::shared_ptr<Arena> from;
    VkDeviceSize offset = 0;
    for (auto &arena : arenas)
      if (arena->take(size, offset)) {
        from = arena;
        break;
      }
    if (!from) {
      // An arena nothing is part of any more goes before another is made: the
      // list stays as long as the most that was ever in use at once.
      for (auto it = arenas.begin(); it != arenas.end();)
        it = (*it)->empty() && it->use_count() == 1 && arenas.size() > 1 ? arenas.erase(it) : std::next(it);
      auto arena = std::make_shared<Arena>();
      arena->device = device;
      arena->size = arenaSize;
      const VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = arenaSize,
                                       .usage = usage,
                                       .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
      check(vkCreateBuffer(device, &info, nullptr, &arena->buffer));
      VkMemoryRequirements requirements;
      vkGetBufferMemoryRequirements(device, arena->buffer, &requirements);
      const uint32_t type = memoryType(requirements.memoryTypeBits, flags);
      const VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                               .allocationSize = requirements.size,
                                               .memoryTypeIndex = type};
      check(vkAllocateMemory(device, &allocation, nullptr, &arena->memory));
      check(vkBindBufferMemory(device, arena->buffer, arena->memory, 0));
      if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        void *mapped;
        check(vkMapMemory(device, arena->memory, 0, VK_WHOLE_SIZE, 0, &mapped));
        arena->mapped = static_cast<uint8_t *>(mapped);
        arena->coherent = (memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
      }
      arena->free.emplace(0, arenaSize);
      arena->take(size, offset);
      arenas.push_back(arena);
      from = std::move(arena);
    }
    auto out = std::make_unique<Buffer>();
    out->device = device;
    out->buffer = from->buffer;
    out->capacity = size;
    out->allocationSize = size;
    out->usage = usage;
    out->flags = flags;
    out->offset = offset;
    out->arena = std::move(from);
    return out;
  }

  std::unique_ptr<Buffer> RasterRenderer::Gpu::geometryBuffer(const void *data, size_t size, VkBufferUsageFlags usage,
                                                              VkCommandBuffer command) {
    // Geometry stays on the GPU, including devices without CPU-visible local memory.
    // Small geometry (a frame's effects and skinned vertices, most meshes) is
    // a part of an arena, and so is its staging copy; large geometry keeps a
    // buffer of its own.
    constexpr VkDeviceSize smallGeometry = 512u * 1024u, arenaSize = 8u * 1024u * 1024u;
    constexpr VkBufferUsageFlags geometryUsage =
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const bool shared = size && size <= smallGeometry && !(usage & ~geometryUsage);
    auto out =
        shared ? part(geometryArenas, size, geometryUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, arenaSize)
               : allocateBuffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (shared) {
      auto staged = part(stagingArenas[frameIndex], size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, arenaSize);
      std::memcpy(staged->arena->mapped + staged->offset, data, size);
      if (!staged->arena->coherent) {
        const VkDeviceSize atom = std::max<VkDeviceSize>(properties.limits.nonCoherentAtomSize, 1);
        const VkDeviceSize begin = staged->offset / atom * atom;
        const VkMappedMemoryRange flush = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = staged->arena->memory,
            .offset = begin,
            .size = std::min((staged->offset + size - begin + atom - 1) / atom * atom, staged->arena->size - begin)};
        check(vkFlushMappedMemoryRanges(device, 1, &flush));
      }
      uploadBytes += size;
      ++stagingBuffers;
      uploads.push_back(std::move(staged));
    } else
      uploads.push_back(buffer(data, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
    const VkBufferCopy copy = {.srcOffset = uploads.back()->offset, .dstOffset = out->offset, .size = size};
    vkCmdCopyBuffer(command, uploads.back()->buffer, out->buffer, 1, &copy);
    VkAccessFlags reads = 0;
    if (usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) {
      reads |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    }
    if (usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) {
      reads |= VK_ACCESS_INDEX_READ_BIT;
    }
    const VkBufferMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                           .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                           .dstAccessMask = reads,
                                           .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                           .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                           .buffer = out->buffer,
                                           .offset = out->offset,
                                           .size = out->arena ? VkDeviceSize(size) : VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr, 1,
                         &barrier, 0, nullptr);
    // prepare() releases staging buffers only after the previous frame fence completes.
    return out;
  }

  std::unique_ptr<Image> RasterRenderer::Gpu::image(uint32_t width, uint32_t height, uint32_t levels, VkFormat format,
                                                    bool isDepth, VkImageCreateFlags flags,
                                                    VkComponentMapping components) {
    if (width > properties.limits.maxImageDimension2D || height > properties.limits.maxImageDimension2D) {
      throw VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    if (!isDepth)
      for (auto it = reusableImages.begin(); it != reusableImages.end(); ++it) {
        const auto &image = **it;
        if (image.width == width && image.height == height && image.levels == levels && image.format == format &&
            image.flags == flags && std::memcmp(&image.components, &components, sizeof(components)) == 0) {
          auto out = std::move(*it);
          reusableImages.erase(it);
          return out;
        }
      }
    auto out = std::make_unique<Image>();
    out->width = width;
    out->height = height;
    out->flags = flags;
    out->device = device;
    out->format = format;
    out->levels = levels;
    out->components = components;
    const VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = flags,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = levels,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        // A depth image is the target's attachment or the sampled copy sprite
        // overlays take of it (captureSceneDepth).
        .usage = isDepth
                     ? VkImageUsageFlags(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
                     : VkImageUsageFlags(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::time_point a, Clock::time_point b) {
      return std::chrono::duration<double, std::milli>(b - a).count();
    };
    const auto t0 = Clock::now();
    check(vkCreateImage(device, &info, nullptr, &out->image));
    const auto t1 = Clock::now();
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, out->image, &requirements);
    const VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    // A texture's image is a part of a block; a large one, and a depth image
    // (few, long-lived), has memory of its own.
    constexpr VkDeviceSize blockSize = imageBlockSize;
    VkDeviceSize offset = 0;
    if (!isDepth && requirements.size <= blockSize / 4) {
      const uint32_t type = allocation.memoryTypeIndex;
      for (auto &block : imageBlocks)
        if (block->type == type && block->take(requirements.size, requirements.alignment, offset)) {
          out->block = block;
          break;
        }
      if (!out->block) {
        // The block allocated ahead, if it is of this type; else one now.
        std::shared_ptr<MemoryBlock> block;
        spareImageWanted.reset();
        if (spareImageBlock.valid()) {
          block = spareImageBlock.get();
          if (block && block->type != type)
            block.reset();
        }
        if (!block)
          block = allocateBlock(device, blockSize, type, false);
        if (block && block->take(requirements.size, requirements.alignment, offset)) {
          imageBlocks.push_back(block);
          out->block = std::move(block);
          spareImageWanted = type;
          spareImageFrames = 0;
        }
      }
    }
    if (out->block) {
      out->blockOffset = offset;
      out->blockBytes = requirements.size;
    } else
      check(vkAllocateMemory(device, &allocation, nullptr, &out->memory));
    const auto t2 = Clock::now();
    out->bytes = requirements.size;
    check(vkBindImageMemory(device, out->image, out->block ? out->block->memory : out->memory, offset));
    const auto t3 = Clock::now();
    const VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = out->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components = components,
        .subresourceRange = {VkImageAspectFlags(isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0,
                             levels, 0, 1}};
    check(vkCreateImageView(device, &view, nullptr, &out->view));
    if (!isDepth) {
      const auto t4 = Clock::now();
      ++textureCosts.images;
      ++imagesCreated;
      textureCosts.bytes += requirements.size;
      textureCosts.create += ms(t0, t1);
      textureCosts.allocate += ms(t1, t2);
      textureCosts.bind += ms(t2, t3);
      textureCosts.view += ms(t3, t4);
      textureCosts.slowest = std::max(textureCosts.slowest, ms(t0, t4));
    }
    return out;
  }

} // namespace remix
