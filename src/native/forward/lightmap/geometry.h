#pragma once
#include "forward/lightmap/data.h"
#include "forward/lightmap/receiver_triangles.h"
#include "forward/resources.h"
#include <unordered_map>
namespace remix::forward::lightmap {
  class Geometry {
  public:
    struct Entry {
      uint32_t draw = 0, count = 0;
      bool fullyMapped = false;
      std::vector<ReceiverRun> receiverRuns;
      std::vector<uint32_t> directOwners;
      std::unique_ptr<Buffer> vertices, indices;
      std::unique_ptr<ReceiverTriangles> receiver;
    };
    Geometry(VkPhysicalDevice physical, VkDevice device, std::shared_ptr<const scene::BakeScene> source,
             const Layout &layout, std::stop_token cancelled = {},
             std::span<const bake::UInt4> lightChannels = {},
             std::span<const bake::UInt4> directChannels = {});
    const Entry *find(const scene::Draw &draw) const;
    uint64_t memoryBytes() const;

  private:
    struct DerivedReceiver {
      Mesh mesh;
      Entry entry;
    };
    const Entry *receiverSubset(const Entry &entry, Mesh mapped) const;
    VkPhysicalDevice m_physical;
    VkDevice m_device;
    std::shared_ptr<const scene::BakeScene> m_source;
    std::vector<Entry> m_entries;
    std::unordered_multimap<uint64_t, uint32_t> m_instances;
    // Entries never move or retire while a frame can reference this volume.
    // Bounded content reuse handles fresh dynamic index-buffer mesh objects.
    mutable std::unordered_multimap<uint64_t, std::unique_ptr<DerivedReceiver>> m_receivers;
    mutable uint64_t m_receiverBytes = 0;
    mutable bool m_receiverAllocationFailed = false;
  };
} // namespace remix::forward::lightmap
