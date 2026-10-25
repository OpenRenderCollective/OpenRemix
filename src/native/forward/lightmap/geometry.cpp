#include "forward/lightmap/geometry.h"
#include "forward/lightmap/decal.h"
#include <remix/remix_native_ext.h>
#include <cstdio>
#include <cstring>
namespace remix::forward::lightmap {
  Geometry::Geometry(VkPhysicalDevice physical, VkDevice device,
                     std::shared_ptr<const scene::BakeScene> source, const Layout &layout,
                     std::stop_token cancelled, std::span<const bake::UInt4> lightChannels,
                     std::span<const bake::UInt4> directChannels)
      : m_physical(physical), m_device(device), m_source(std::move(source)) {
    auto mapped = meshes(*m_source, layout, cancelled, lightChannels, directChannels);
    auto paint = decalMeshes(*m_source, mapped, cancelled);
    if (!paint.empty()) {
      size_t triangles = 0;
      for (const auto &mesh : paint) triangles += mesh.indices.size() / 3;
      std::fprintf(stderr, "Forward decal receivers: %zu overlays, %zu triangles, reused atlas charts\n",
                   paint.size(), triangles);
    }
    for (auto &mesh : paint) mapped.push_back(std::move(mesh));
    uint64_t directoryBytes = 0;
    for (auto &mesh : mapped) {
      Entry entry;
      entry.draw = mesh.draw;
      entry.count = uint32_t(mesh.indices.size());
      entry.fullyMapped = fullyMapped(mesh);
      if (!entry.fullyMapped) entry.receiverRuns = receiverRuns(mesh, cancelled);
      entry.directOwners = directOwners(mesh);
      entry.vertices = std::make_unique<Buffer>();
      entry.indices = std::make_unique<Buffer>();
      entry.vertices->create(physical, device, mesh.vertices.size() * sizeof(Vertex),
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
      entry.indices->create(physical, device, mesh.indices.size() * sizeof(uint32_t),
                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
      entry.vertices->write(mesh.vertices.data(), mesh.vertices.size() * sizeof(Vertex));
      entry.indices->write(mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t));
      // Conservative CPU directory estimate, including tree nodes and keys.
      const uint64_t directoryEstimate = (mesh.indices.size() / 3) * 512ull;
      if (m_source->receiverOnly(mesh.draw) && mesh.indices.size() <= ReceiverTriangles::maximumIndices &&
          directoryEstimate <= (32ull << 20) - directoryBytes) {
        entry.receiver = std::make_unique<ReceiverTriangles>(mesh, cancelled);
        directoryBytes += directoryEstimate;
      }
      m_instances.emplace(m_source->draws[mesh.draw].instanceId, uint32_t(m_entries.size()));
      m_entries.push_back(std::move(entry));
    }
  }
  const Geometry::Entry *Geometry::find(const scene::Draw &draw) const {
    const auto [first, last] = m_instances.equal_range(draw.instanceId);
    const Entry *exact = nullptr;
    for (auto found = first; found != last; ++found) {
      const auto &entry = m_entries[found->second];
      if (!scene::matchesBakeDraw(*m_source, entry.draw, draw)) continue;
      if (!m_source->receiverOnly(entry.draw)) return &entry;
      if (exact) return nullptr;
      exact = &entry;
    }
    if (exact) return exact;
    const Entry *owner = nullptr;
    std::optional<Mesh> mapped;
    for (auto found = first; found != last; ++found) {
      const auto &entry = m_entries[found->second];
      if (!entry.receiver) continue;
      auto candidate = entry.receiver->map(m_source->draws[entry.draw], draw, m_source->sceneScale);
      if (!candidate) continue;
      // Reject duplicate source ownership before allocating any derived data.
      // Cache capacity must never decide which coincident chart gets selected.
      if (mapped) return nullptr;
      mapped = std::move(candidate);
      owner = &entry;
    }
    return mapped ? receiverSubset(*owner, std::move(*mapped)) : nullptr;
  }
  const Geometry::Entry *Geometry::receiverSubset(const Entry &source, Mesh mapped) const {
    const uint64_t vertexBytes = mapped.vertices.size() * sizeof(Vertex);
    const uint64_t indexBytes = mapped.indices.size() * sizeof(uint32_t);
    auto hash = scene::fastHash(mapped.vertices.data(), vertexBytes, source.draw);
    hash = scene::fastHash(mapped.indices.data(), indexBytes, hash);
    const auto [first, last] = m_receivers.equal_range(hash);
    for (auto found = first; found != last; ++found) {
      const auto &cached = *found->second;
      if (cached.mesh.draw == mapped.draw && cached.mesh.vertices.size() == mapped.vertices.size() &&
          cached.mesh.indices == mapped.indices &&
          std::memcmp(cached.mesh.vertices.data(), mapped.vertices.data(), vertexBytes) == 0)
        return &cached.entry;
    }
    constexpr uint64_t budget = 32ull << 20;
    if (m_receiverAllocationFailed || m_receivers.size() >= 128 || vertexBytes + indexBytes > budget - m_receiverBytes)
      return nullptr;
    try {
      auto cached = std::make_unique<DerivedReceiver>();
      auto &entry = cached->entry;
      entry.draw = mapped.draw;
      entry.count = uint32_t(mapped.indices.size());
      entry.fullyMapped = true;
      entry.directOwners = directOwners(mapped);
      entry.vertices = std::make_unique<Buffer>();
      entry.indices = std::make_unique<Buffer>();
      entry.vertices->create(m_physical, m_device, vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
      entry.indices->create(m_physical, m_device, indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
      const uint64_t allocated = entry.vertices->allocationBytes + entry.indices->allocationBytes;
      if (allocated > budget - m_receiverBytes) return nullptr;
      entry.vertices->write(mapped.vertices.data(), vertexBytes);
      entry.indices->write(mapped.indices.data(), indexBytes);
      cached->mesh = std::move(mapped);
      const auto result = &entry;
      m_receivers.emplace(hash, std::move(cached));
      m_receiverBytes += allocated;
      return result;
    } catch (VkResult) {
      m_receiverAllocationFailed = true;
      return nullptr;
    }
  }
  uint64_t Geometry::memoryBytes() const {
    uint64_t result = 0;
    for (const auto &entry : m_entries)
      result += entry.vertices->allocationBytes + entry.indices->allocationBytes;
    result += m_receiverBytes;
    return result;
  }
} // namespace remix::forward::lightmap
