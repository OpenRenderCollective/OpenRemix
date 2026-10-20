#pragma once
#include "forward/lightmap/data.h"
#include "forward/resources.h"
#include <unordered_map>
namespace remix::forward::lightmap {
  class Geometry {
  public:
    struct Entry {
      uint32_t draw = 0, count = 0;
      bool fullyMapped = false;
      std::vector<uint32_t> directOwners;
      std::unique_ptr<Buffer> vertices, indices;
    };
    Geometry(VkPhysicalDevice physical, VkDevice device, std::shared_ptr<const scene::BakeScene> source,
             const Layout &layout, std::stop_token cancelled = {},
             std::span<const bake::UInt4> lightChannels = {},
             std::span<const bake::UInt4> directChannels = {});
    const Entry *find(const scene::Draw &draw) const;
    uint64_t memoryBytes() const;

  private:
    std::shared_ptr<const scene::BakeScene> m_source;
    std::vector<Entry> m_entries;
    std::unordered_multimap<uint64_t, uint32_t> m_instances;
  };
} // namespace remix::forward::lightmap
