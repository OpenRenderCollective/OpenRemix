#include "pathtrace/buffer_cache.h"
#include <cassert>
#include <cstdio>
#include <unordered_set>

using namespace remix::trace::detail;

namespace {
  std::unordered_set<VkBuffer> destroyed;
  std::unordered_set<VkDeviceMemory> freed;
  std::unordered_set<VkDeviceMemory> unmapped;
  uintptr_t nextHandle = 1;

  std::shared_ptr<Buffer> allocation(const std::shared_ptr<BufferCache> &cache, VkDeviceSize size,
                                     VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                     bool host = true) {
    auto result = std::make_shared<Buffer>();
    result->handle = reinterpret_cast<VkBuffer>(nextHandle);
    result->memory = reinterpret_cast<VkDeviceMemory>(nextHandle++);
    result->size = result->allocationSize = size;
    result->usage = usage;
    result->host = host;
    result->cache = cache;
    return result;
  }

  std::shared_ptr<Buffer> restore(std::shared_ptr<BufferCache> cache, BufferAllocation entry) {
    auto result = std::make_shared<Buffer>();
    static_cast<BufferAllocation &>(*result) = entry;
    result->cache = std::move(cache);
    return result;
  }
}

// Mock only destruction: these tests exercise ownership and matching without a Vulkan device.
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice, VkBuffer buffer,
                                                       const VkAllocationCallbacks *) {
  assert(destroyed.insert(buffer).second);
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice, VkDeviceMemory memory,
                                                   const VkAllocationCallbacks *) {
  assert(freed.insert(memory).second);
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice, VkDeviceMemory memory) {
  assert(!freed.count(memory) && unmapped.insert(memory).second);
}

int main() {
  {
    BufferMemory block;
    block.size = 4096; block.free.emplace_back(0,4096);
    auto a = block.take(257,256), b = block.take(512,256), c = block.take(1024,512);
    assert(a == 0 && b == 512 && c == 1024);
    assert(!block.take(4096,256));
    block.release(*b,512); block.release(*a,257); block.release(*c,1024);
    assert(block.free.size() == 1 && block.free.begin()->first == 0 && block.free.begin()->second == 4096);
  }
  {
    auto cache = std::make_shared<BufferCache>();
    auto buffer = allocation(cache, 4096);
    auto pendingFrame = buffer;
    const auto handle = buffer->handle;
    const auto memory = buffer->memory;
    buffer->mapped = reinterpret_cast<void*>(uintptr_t(0x1000));
    buffer.reset();
    assert(!cache->take(4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
    pendingFrame.reset();
    assert(!destroyed.count(handle));
    auto reused = cache->take(4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    assert(reused && reused->handle == handle);
    assert(reused->mapped && !unmapped.count(memory));
    assert(!cache->take(4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
    auto owner = restore(cache, *reused);
    cache.reset(); // The last outstanding buffer also keeps the cache alive.
    assert(!destroyed.count(handle));
    owner.reset();
    assert(destroyed.count(handle));
    remix::MemoryRelease::drain(); // memory is freed on its own thread; the mapping ends with it
    assert(freed.count(memory) && !unmapped.count(memory));
  }
  {
    auto cache = std::make_shared<BufferCache>();
    allocation(cache, 16384).reset();
    auto smaller = allocation(cache, 8192);
    const auto handle = smaller->handle;
    smaller.reset();
    assert(!cache->take(4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false));
    assert(!cache->take(4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
    assert(!cache->take(1024, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
    auto best = cache->take(4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    assert(best && best->handle == handle);
    restore(cache, *best).reset();
    assert(!cache->take(32768, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
  }
  {
    auto cache = std::make_shared<BufferCache>();
    allocation(cache, BufferCache::byteLimit).reset();
    auto overflow = allocation(cache, 4096);
    const auto handle = overflow->handle;
    overflow.reset();
    assert(destroyed.count(handle));
    auto large = cache->take(BufferCache::byteLimit, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    assert(large);
    large->destroy();
    auto smaller = allocation(cache, 4096);
    const auto smallerHandle = smaller->handle;
    smaller.reset();
    assert(!destroyed.count(smallerHandle)); // Taking an entry restores the byte allowance.
  }
  {
    auto cache = std::make_shared<BufferCache>();
    for (size_t i = 0; i < BufferCache::entryLimit; ++i)
      allocation(cache, 4096).reset();
    auto overflow = allocation(cache, 4096);
    const auto handle = overflow->handle;
    overflow.reset();
    assert(destroyed.count(handle));
  }
  remix::MemoryRelease::drain();
  assert(destroyed.size() == nextHandle - 1 && freed.size() == destroyed.size());
  {
    auto cache = std::make_shared<BufferCache>();
    std::vector<std::shared_ptr<BufferMemory>> blocks;
    for (size_t i = 0; i < 9; ++i) {
      auto block = std::make_shared<BufferMemory>();
      block->size = 16*1024*1024; block->memory = reinterpret_cast<VkDeviceMemory>(nextHandle++);
      block->free.emplace_back(0,block->size);
      auto buffer = std::make_shared<Buffer>();
      buffer->handle = reinterpret_cast<VkBuffer>(nextHandle++);
      buffer->memory = block->memory; buffer->block = block;
      buffer->size = buffer->allocationSize = 4096;
      buffer->memoryOffset = *block->take(4096,256); buffer->cache = cache;
      blocks.push_back(std::move(block));
    }
    cache->trimMemory(blocks);
    std::erase_if(blocks,[](const auto &block) { return block.use_count() == 1; });
    assert(blocks.size() == 4); // 36 KiB of cached ranges cannot pin 144 MiB.
    cache.reset();
    for (const auto &block : blocks) assert(block->liveRanges == 0 && block->free.size() == 1);
  }
  remix::MemoryRelease::drain();
  std::puts("Upload buffer cache ownership, compatibility, limits and cleanup passed");
}
