#include "pathtrace/gpu.h"

namespace remix {
  using namespace scene;
  using namespace trace::detail;

  uint32_t PathTracer::Gpu::memoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags)
        return i;
    throw VK_ERROR_FEATURE_NOT_PRESENT;
  }

  std::unique_ptr<Buffer> PathTracer::Gpu::buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host,
                                                  bool reusable) {
    ScopedMilliseconds time{performance.allocationMilliseconds};
    auto out = std::make_unique<Buffer>();
    out->device = device;
    out->size = align(std::max(size, VkDeviceSize(16)), 4096);
    out->usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    out->host = host;
    if (reusable) {
      if (auto cached = bufferCache->take(out->size, out->usage, host)) {
        ++performance.bufferCacheHits;
        static_cast<BufferAllocation &>(*out) = *cached;
        out->cache = bufferCache;
        return out;
      }
    }
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = out->size,
                               .usage = out->usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    ++buffersCreated;
    {
      const auto began = std::chrono::steady_clock::now();
      check(vkCreateBuffer(device, &info, nullptr, &out->handle));
      trace::g_blockCosts.slowestCreate =
          std::max(trace::g_blockCosts.slowestCreate,
                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count());
    }
    VkMemoryDedicatedRequirements dedicated{.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 query{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dedicated};
    VkBufferMemoryRequirementsInfo2 request{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
                                            .buffer = out->handle};
    vkGetBufferMemoryRequirements2(device, &request, &query);
    const auto &requirements = query.memoryRequirements;
    out->allocationSize = requirements.size;
    ++performance.bufferAllocations;
    performance.allocationBytes += requirements.size;
    VkMemoryAllocateFlagsInfo flags = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                       .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &flags,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memoryType(requirements.memoryTypeBits, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                                                        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    // Device-local buffers are parts of a block, the idle cache's (reusable)
    // or not: a memory allocation of its own cost each new acceleration
    // structure and table the driver's allocation time. Host-visible buffers
    // outside the cache keep memory of their own, which their readers map.
    bool firstInBlock = false;
    if (!dedicated.requiresDedicatedAllocation && (reusable || !host)) {
      const VkDeviceSize alignment =
          std::max(requirements.alignment, host ? properties.limits.nonCoherentAtomSize : VkDeviceSize(1));
      out->allocationSize = align(requirements.size, alignment);
      out->atomSize = host ? properties.limits.nonCoherentAtomSize : 1;
      for (const auto &block : bufferMemory)
        if (block->type == allocate.memoryTypeIndex) {
          if (const auto offset = block->take(out->allocationSize, alignment)) {
            out->block = block;
            out->memoryOffset = *offset;
            break;
          }
        }
      if (!out->block) {
        const uint32_t type = allocate.memoryTypeIndex;
        std::shared_ptr<BufferMemory> block;
        const bool standard = out->allocationSize <= bufferBlockSize;
        const auto began = std::chrono::steady_clock::now();
        uint32_t how = standard ? 2 : 3;
        if (standard)
          for (auto it = spareBufferMemory.begin(); it != spareBufferMemory.end(); ++it)
            if (it->first == type) {
              how = it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready ? 0 : 1;
              block = it->second.get(); // Waits only if the spare is still being allocated.
              spareBufferMemory.erase(it);
              break;
            }
        const auto noteBlock = [&] {
          auto &c = trace::g_blockCosts;
          ++(how == 0 ? c.ready : how == 1 ? c.awaited : how == 2 ? c.none : c.large);
          const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
          if (ms > c.slowest) {
            c.slowest = ms;
            c.slowestBuffer = out->allocationSize;
            c.slowestType = type;
            c.slowestHow = how;
            c.slowestHost = host;
            c.slowestBlocks = 0;
            for (const auto &other : bufferMemory)
              c.slowestBlocks += other->type == type;
          }
        };
        if (!block) {
          block = allocateBufferMemory(device, std::max(bufferBlockSize, out->allocationSize), type, host, false);
          if (!block)
            throw VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        noteBlock();
        if (standard && std::none_of(sparesWanted.begin(), sparesWanted.end(),
                                     [&](const SpareWanted &wanted) { return wanted.type == type; }))
          sparesWanted.push_back({type, host, 0});
        ++performance.memoryAllocations;
        performance.memoryAllocationBytes += block->size;
        const auto offset = block->take(out->allocationSize, alignment);
        out->block = block;
        out->memoryOffset = *offset;
        firstInBlock = true;
        bufferMemory.push_back(std::move(block));
      }
      out->memory = out->block->memory;
      if (host) {
        if (!out->block->mapped) {
          const auto began = std::chrono::steady_clock::now();
          check(vkMapMemory(device, out->memory, 0, VK_WHOLE_SIZE, 0, &out->block->mapped));
          trace::g_blockCosts.slowestMap =
              std::max(trace::g_blockCosts.slowestMap,
                       std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count());
        }
        out->mapped = static_cast<char *>(out->block->mapped) + out->memoryOffset;
      }
    } else {
      VkMemoryDedicatedAllocateInfo dedicatedInfo{.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                                                  .buffer = out->handle};
      if (dedicated.requiresDedicatedAllocation)
        flags.pNext = &dedicatedInfo;
      check(vkAllocateMemory(device, &allocate, nullptr, &out->memory));
      ++performance.memoryAllocations;
      performance.memoryAllocationBytes += allocate.allocationSize;
    }
    {
      const auto began = std::chrono::steady_clock::now();
      check(vkBindBufferMemory(device, out->handle, out->memory, out->memoryOffset));
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
      if (ms > trace::g_blockCosts.slowestBind) {
        trace::g_blockCosts.slowestBind = ms;
        trace::g_blockCosts.bindFirst = firstInBlock;
      }
    }
    VkBufferDeviceAddressInfo address = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = out->handle};
    out->address = getBufferAddress(device, &address);
    // Failed or partially initialized allocations always take the ordinary destruction path.
    if (reusable)
      out->cache = bufferCache;
    return out;
  }

  void PathTracer::Gpu::launchSpares() {
    const bool quiet = buffersCreated == buffersSeen;
    buffersSeen = buffersCreated;
    std::erase_if(sparesWanted, [&](SpareWanted &wanted) {
      // (Asked for in the last frame at the earliest: never in the frame that took the block.)
      if (!(++wanted.frames > 1 && quiet) && wanted.frames < 120)
        return false;
      spareBufferMemory.emplace_back(wanted.type, std::async(std::launch::async, allocateBufferMemory, device,
                                                             bufferBlockSize, wanted.type, wanted.host, true));
      return true;
    });
  }

  std::shared_ptr<BufferMemory> PathTracer::Gpu::allocateBufferMemory(VkDevice device, VkDeviceSize size, uint32_t type,
                                                                      bool host, bool warm) {
    auto block = std::make_shared<BufferMemory>();
    block->device = device;
    block->type = type;
    block->size = size;
    const VkMemoryAllocateFlagsInfo flags = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                             .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    const VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                           .pNext = &flags,
                                           .allocationSize = size,
                                           .memoryTypeIndex = type};
    if (vkAllocateMemory(device, &allocate, nullptr, &block->memory) != VK_SUCCESS)
      return nullptr;
    block->free.emplace_back(0, size);
    // The driver finishes a memory allocation at its first use: the first
    // buffer bound into a new block took ~20 ms (and a host block's mapping
    // its own time), in the frame that needed the block. A spare, allocated
    // on another thread ahead of its use, pays that there: a buffer bound and
    // destroyed again, and the mapping its buffers will share.
    if (warm) {
      const VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = 65536,
                                       .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                       .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
      VkBuffer first = VK_NULL_HANDLE;
      if (vkCreateBuffer(device, &info, nullptr, &first) == VK_SUCCESS) {
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, first, &requirements);
        if ((requirements.memoryTypeBits >> type & 1) && requirements.size <= size)
          vkBindBufferMemory(device, first, block->memory, 0);
        vkDestroyBuffer(device, first, nullptr);
      }
      if (host && vkMapMemory(device, block->memory, 0, VK_WHOLE_SIZE, 0, &block->mapped) != VK_SUCCESS)
        block->mapped = nullptr;
    }
    return block;
  }

  void PathTracer::Gpu::write(Buffer &buffer, const void *data, size_t size) {
    if (!size)
      return;
    ScopedMilliseconds time{performance.uploadMilliseconds};
    performance.uploadBytes += size;
    // Slot ownership already protects host writes. Keep reusable staging mapped
    // through its lifetime, including time in the bounded idle allocation cache.
    if (!buffer.mapped)
      check(vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped));
    std::memcpy(buffer.mapped, data, size);
    VkMappedMemoryRange flush = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                 .memory = buffer.memory,
                                 .offset = buffer.memoryOffset,
                                 .size = buffer.block ? align(size, buffer.atomSize) : VK_WHOLE_SIZE};
    const auto result = vkFlushMappedMemoryRanges(device, 1, &flush);
    check(result);
  }

  void PathTracer::Gpu::write(Buffer &buffer, std::initializer_list<std::pair<const void *, size_t>> parts,
                              size_t offset) {
    size_t size = 0;
    for (const auto &part : parts)
      size += part.second;
    if (size <= offset)
      return;
    ScopedMilliseconds time{performance.uploadMilliseconds};
    performance.uploadBytes += size - offset;
    if (!buffer.mapped)
      check(vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped));
    size_t position = 0;
    for (const auto &[data, bytes] : parts) {
      const size_t skip = offset > position ? std::min(offset - position, bytes) : 0;
      if (bytes > skip)
        std::memcpy(static_cast<char *>(buffer.mapped) + position + skip, static_cast<const char *>(data) + skip,
                    bytes - skip);
      position += bytes;
    }
    VkMappedMemoryRange flush = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                 .memory = buffer.memory,
                                 .offset = buffer.memoryOffset,
                                 .size = buffer.block ? align(size, buffer.atomSize) : VK_WHOLE_SIZE};
    check(vkFlushMappedMemoryRanges(device, 1, &flush));
  }

  void PathTracer::Gpu::hostBuffer(std::unique_ptr<Buffer> &out, const void *data, size_t size,
                                   VkBufferUsageFlags usage) {
    // Grown with room to spare, as part of a block of host memory: a buffer
    // with memory of its own cost the driver's allocation time (20 ms and
    // more) in the frame whose instance, light or remap count passed the
    // last. The one it replaces may still be read by a frame in flight.
    if (!out || out->size < size) {
      if (out)
        retiredBuffers.push_back(std::move(out));
      out = buffer(size + size / 2, usage, true, true);
    }
    write(*out, data, size);
  }

  VkDeviceSize PathTracer::Gpu::stage(const void *data, size_t size) {
    VkDeviceSize offset = align(staging.used, 16);
    if (!staging.buffer || offset + size > staging.buffer->size) {
      // Copies recorded from the full buffer keep it until this slot's fence.
      const VkDeviceSize capacity = std::max<VkDeviceSize>(
          {VkDeviceSize(size), staging.buffer ? staging.buffer->size * 2 : 0, VkDeviceSize(4 * 1024 * 1024)});
      if (staging.buffer)
        uploads.push_back(std::move(staging.buffer));
      staging.buffer = buffer(capacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, true);
      offset = 0;
    }
    ScopedMilliseconds time{performance.uploadMilliseconds};
    performance.uploadBytes += size;
    auto &target = *staging.buffer;
    if (!target.mapped)
      check(vkMapMemory(device, target.memory, 0, VK_WHOLE_SIZE, 0, &target.mapped));
    std::memcpy(static_cast<char *>(target.mapped) + offset, data, size);
    const VkDeviceSize atom = std::max<VkDeviceSize>(1, properties.limits.nonCoherentAtomSize);
    const VkDeviceSize begin = offset / atom * atom, end = align(offset + size, atom);
    VkMappedMemoryRange flush = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                 .memory = target.memory,
                                 .offset = target.memoryOffset + begin,
                                 .size = target.block                  ? std::min(end, target.allocationSize) - begin
                                         : end > target.allocationSize ? VK_WHOLE_SIZE
                                                                       : end - begin};
    check(vkFlushMappedMemoryRanges(device, 1, &flush));
    staging.used = offset + size;
    return offset;
  }

  std::unique_ptr<Buffer> PathTracer::Gpu::upload(VkCommandBuffer command, const void *data, size_t size,
                                                  VkBufferUsageFlags usage) {
    const VkDeviceSize offset = stage(data, size);
    auto out = buffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, true);
    VkBufferCopy copy = {.srcOffset = offset, .size = size};
    vkCmdCopyBuffer(command, staging.buffer->handle, out->handle, 1, &copy);
    return out;
  }

  std::shared_ptr<Accel> PathTracer::Gpu::acceleration(VkDeviceSize size, VkAccelerationStructureTypeKHR type) {
    size = align(size, 256);
    std::shared_ptr<Arena> arena;
    VkDeviceSize offset = 0;
    for (auto &candidate : arenas) {
      auto range = std::find_if(candidate->free.begin(), candidate->free.end(),
                                [&](auto value) { return value.second >= size; });
      if (range == candidate->free.end())
        continue;
      arena = candidate;
      offset = range->first;
      auto remaining = range->second - size;
      candidate->free.erase(range);
      if (remaining)
        candidate->free.emplace(offset + size, remaining);
      break;
    }
    if (!arena) {
      arena = std::make_shared<Arena>();
      arena->buffer = buffer(std::max(size, VkDeviceSize(16 * 1024 * 1024)),
                             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
      if (arena->buffer->size > size)
        arena->free.emplace(size, arena->buffer->size - size);
      arenas.push_back(arena);
    }
    auto out = std::make_shared<Accel>();
    out->device = device;
    out->destroy = destroyAs;
    out->arena = arena;
    out->offset = offset;
    out->size = size;
    VkAccelerationStructureCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                                 .buffer = arena->buffer->handle,
                                                 .offset = offset,
                                                 .size = size,
                                                 .type = type};
    {
      ScopedMilliseconds time{trace::g_accelNow.create};
      ++trace::g_accelNow.created;
      check(createAs(device, &info, nullptr, &out->handle));
    }
    VkAccelerationStructureDeviceAddressInfoKHR address = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
        .accelerationStructure = out->handle};
    out->address = getAsAddress(device, &address);
    return out;
  }

} // namespace remix
