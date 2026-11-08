#pragma once
#include "platform/memory_release.h"
#include "pathtrace/block_costs.h"
#include <chrono>
#include <vulkan/vulkan.h>
#include <array>
#include <memory>
#include <optional>
#include <vector>
#include <algorithm>

namespace remix::trace::detail {
  // Buffers retain a range until their last GPU submission and idle-cache owner
  // release it. Sharing memory does not share a VkBuffer or overwrite live data.
  struct BufferMemory {
    VkDevice device = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    uint32_t type = 0;
    void *mapped = nullptr;
    std::vector<std::pair<VkDeviceSize, VkDeviceSize>> free;
    size_t liveRanges = 0;
    ~BufferMemory() {
      const auto began = std::chrono::steady_clock::now();
      MemoryRelease::free(device, memory); // (its mapping with it)
      remix::trace::g_slowestBlockFree =
          std::max(remix::trace::g_slowestBlockFree,
                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count());
    }
    std::optional<VkDeviceSize> take(VkDeviceSize bytes, VkDeviceSize alignment) {
      for (size_t i = 0; i < free.size(); ++i) {
        const auto begin = free[i].first, end = begin + free[i].second;
        const auto offset = (begin + alignment - 1) & ~(alignment - 1);
        if (offset > end || bytes > end - offset)
          continue;
        // Reserve at allocation time so releasing a live range cannot allocate
        // or throw, including from a Buffer destructor during error cleanup.
        const auto capacity = free.size() + liveRanges + 2;
        if (free.capacity() < capacity)
          free.reserve(std::max(capacity, free.capacity() * 2));
        const auto after = offset + bytes;
        if (offset > begin) {
          free[i].second = offset - begin;
          if (after < end)
            free.insert(free.begin() + i + 1, {after, end - after});
        } else if (after < end)
          free[i] = {after, end - after};
        else
          free.erase(free.begin() + i);
        ++liveRanges;
        return offset;
      }
      return {};
    }
    void release(VkDeviceSize offset, VkDeviceSize bytes) noexcept {
      auto next = std::lower_bound(free.begin(), free.end(), offset,
                                   [](const auto &range, VkDeviceSize at) { return range.first < at; });
      if (next != free.begin() && std::prev(next)->first + std::prev(next)->second == offset) {
        auto previous = std::prev(next);
        previous->second += bytes;
        if (next != free.end() && offset + bytes == next->first) {
          previous->second += next->second;
          free.erase(next);
        }
      } else if (next != free.end() && offset + bytes == next->first) {
        next->first = offset;
        next->second += bytes;
      } else
        free.insert(next, {offset, bytes});
      --liveRanges;
    }
  };
  struct BufferAllocation {
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    VkDeviceSize size = 0, allocationSize = 0;
    VkBufferUsageFlags usage = 0;
    bool host = false;
    void *mapped = nullptr;
    std::shared_ptr<BufferMemory> block;
    VkDeviceSize memoryOffset = 0, atomSize = 1;

    void destroy() const noexcept {
      const auto began = std::chrono::steady_clock::now();
      if (handle)
        vkDestroyBuffer(device, handle, nullptr);
      if (block)
        block->release(memoryOffset, allocationSize);
      else
        MemoryRelease::free(device, memory); // (its mapping with it)
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
      if (ms > remix::trace::g_slowestBufferDestroy) {
        remix::trace::g_slowestBufferDestroy = ms;
        remix::trace::g_slowestBufferDestroyBytes = allocationSize;
        remix::trace::g_slowestBufferDestroyOwn = !block && memory;
      }
    }
  };

  // Only upload storage participates. Buffer owners retain the usual submission-slot
  // references; an allocation becomes reusable when its final owner releases it.
  // Fixed storage keeps that release path allocation-free and caps idle memory.
  class BufferCache {
  public:
    static constexpr VkDeviceSize byteLimit = 64 * 1024 * 1024;
    static constexpr size_t entryLimit = 512;

    ~BufferCache() {
      for (size_t i = 0; i < m_count; ++i)
        m_entries[i].destroy();
    }
    BufferCache() = default;
    BufferCache(const BufferCache &) = delete;
    BufferCache &operator=(const BufferCache &) = delete;

    std::optional<BufferAllocation> take(VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
      size_t best = m_count;
      for (size_t i = 0; i < m_count; ++i) {
        const auto &entry = m_entries[i];
        if (entry.usage == usage && entry.host == host && entry.size >= size && entry.size - size <= size &&
            (best == m_count || entry.size < m_entries[best].size))
          best = i;
      }
      if (best == m_count)
        return {};
      const auto entry = m_entries[best];
      m_bytes -= entry.allocationSize;
      m_entries[best] = m_entries[--m_count];
      m_entries[m_count] = {};
      return entry;
    }

    // A few small cached ranges must not pin hundreds of otherwise empty
    // blocks. Bound physical idle block memory as well as logical buffer bytes.
    void trimMemory(const std::vector<std::shared_ptr<BufferMemory>> &blocks) noexcept {
      VkDeviceSize idle = 0;
      for (const auto &block : blocks) {
        size_t entries = 0;
        for (size_t i = 0; i < m_count; ++i)
          entries += m_entries[i].block == block;
        if (block.use_count() != long(entries + 1))
          continue; // A submitted/live buffer owns it.
        if (block->size <= byteLimit - idle) {
          idle += block->size;
          continue;
        }
        for (size_t i = 0; i < m_count;) {
          if (m_entries[i].block != block) {
            ++i;
            continue;
          }
          m_bytes -= m_entries[i].allocationSize;
          m_entries[i].destroy();
          m_entries[i] = m_entries[--m_count];
          m_entries[m_count] = {};
        }
      }
    }

    bool put(const BufferAllocation &entry) noexcept {
      if (m_count == entryLimit || entry.allocationSize > byteLimit - m_bytes)
        return false;
      m_entries[m_count++] = entry;
      m_bytes += entry.allocationSize;
      return true;
    }

  private:
    std::array<BufferAllocation, entryLimit> m_entries{};
    size_t m_count = 0;
    VkDeviceSize m_bytes = 0;
  };

  struct Buffer : BufferAllocation {
    std::shared_ptr<BufferCache> cache;
    Buffer() = default;
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    ~Buffer() {
      if (!cache || !cache->put(*this))
        destroy();
    }
  };
} // namespace remix::trace::detail
