#include "forward/lightmap/geometry.h"
#include "forward/lightmap/decal.h"
#include <remix/remix_native_ext.h>
#include <cstdio>
namespace remix::forward::lightmap {
  Geometry::Geometry(VkPhysicalDevice physical, VkDevice device,
                     std::shared_ptr<const scene::BakeScene> source, const Layout &layout,
                     std::stop_token cancelled, std::span<const bake::UInt4> lightChannels,
                     std::span<const bake::UInt4> directChannels)
      : m_source(std::move(source)) {
    auto mapped = meshes(*m_source, layout, cancelled, lightChannels, directChannels);
    auto paint = decalMeshes(*m_source, mapped, cancelled);
    if (!paint.empty()) {
      size_t triangles = 0;
      for (const auto &mesh : paint) triangles += mesh.indices.size() / 3;
      std::fprintf(stderr, "Forward decal receivers: %zu overlays, %zu triangles, reused atlas charts\n",
                   paint.size(), triangles);
    }
    for (auto &mesh : paint) mapped.push_back(std::move(mesh));
    for (auto &mesh : mapped) {
      Entry entry;
      entry.draw = mesh.draw;
      entry.count = uint32_t(mesh.indices.size());
      entry.directOwners = directOwners(mesh);
      entry.vertices = std::make_unique<Buffer>();
      entry.indices = std::make_unique<Buffer>();
      entry.vertices->create(physical, device, mesh.vertices.size() * sizeof(Vertex),
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
      entry.indices->create(physical, device, mesh.indices.size() * sizeof(uint32_t),
                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
      entry.vertices->write(mesh.vertices.data(), mesh.vertices.size() * sizeof(Vertex));
      entry.indices->write(mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t));
      m_instances.emplace(m_source->draws[mesh.draw].instanceId, uint32_t(m_entries.size()));
      m_entries.push_back(std::move(entry));
    }
  }
  const Geometry::Entry *Geometry::find(const scene::Draw &draw) const {
    if (draw.classification != REMIXAPI_INSTANCE_STATIC_EXT)
      return nullptr;
    const auto [first, last] = m_instances.equal_range(draw.instanceId);
    for (auto found = first; found != last; ++found) {
      const auto &entry = m_entries[found->second];
      if (scene::matchesBakeDraw(*m_source, entry.draw, draw))
        return &entry;
    }
    return nullptr;
  }
  uint64_t Geometry::memoryBytes() const {
    uint64_t result = 0;
    for (const auto &entry : m_entries)
      result += entry.vertices->allocationBytes + entry.indices->allocationBytes;
    return result;
  }
} // namespace remix::forward::lightmap
