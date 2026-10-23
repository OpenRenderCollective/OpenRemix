#include "forward/bake/sun_depth.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
namespace remix::forward::bake {
  std::shared_ptr<SunDepth> SunDepth::prepare(VkPhysicalDevice physical, VkDevice device,
      const scene::BakeScene &source, const lightmap::Layout &receivers, std::stop_token cancelled) {
    if (!source.sun.isDistant) return {};
    auto result = std::shared_ptr<SunDepth>(new SunDepth);
    const auto z = normalized({source.sun.direction[0], source.sun.direction[1], source.sun.direction[2]});
    const auto x = normalized(cross(std::abs(z[2]) < .99f ? Vec3{0,0,1} : Vec3{0,1,0}, z));
    result->m_basis = {x, cross(z,x), z};
    const std::array<ReceiverTileOptions, 1> trials{{{4, 1}}};
    auto projected = projectReceivers(source, receivers, result->m_basis, cancelled, trials);
    if (projected.depthLayouts.empty() || !projected.depthLayouts[0] || projected.depthLayouts[0]->tiles.empty()) {
      std::fprintf(stderr, "Forward sparse sun depth declined: empty or over-budget coverage\n");
      return {};
    }
    result->m_layout = std::move(*projected.depthLayouts[0]);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    const uint64_t tableBytes = sizeof(SunDepthHeader) + result->m_layout.directory.size() * sizeof(uint32_t);
    if (result->m_layout.depthBytes > properties.limits.maxStorageBufferRange ||
        tableBytes > properties.limits.maxStorageBufferRange ||
        result->m_layout.depthBytes > result->m_layout.options.maximumBytes ||
        tableBytes > result->m_layout.options.maximumBytes - result->m_layout.depthBytes) {
      std::fprintf(stderr, "Forward sparse sun depth declined: device storage range\n");
      return {};
    }
    Bounds casters;
    for (const auto &draw : source.draws) {
      if (cancelled.stop_requested()) throw std::runtime_error("Sun depth preparation superseded");
      for (const auto &vertex : draw.surface->vertices) {
        const auto p = transformPoint(draw.model, {vertex.position[0],vertex.position[1],vertex.position[2]});
        casters.include({dot(x,p),dot(result->m_basis[1],p),dot(z,p)});
      }
    }
    if (!casters.valid) return {};
    const float bias = .002f / source.sceneScale;
    result->m_header = sunDepthHeader(result->m_layout, result->m_basis, 4 / source.sceneScale,
        casters.minimum[2] - bias, casters.maximum[2] + bias, bias);
    result->m_extent = result->m_layout.options.tileTexels + 2 * result->m_layout.options.border;
    if (uint64_t(result->m_layout.tiles.size()) * result->m_extent * result->m_extent > UINT32_MAX)
      throw std::length_error("Sun depth capture task count exceeds uint32");
    result->m_table.create(physical, device, tableBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    result->m_table.write(&result->m_header, sizeof(result->m_header));
    result->m_table.write(result->m_layout.directory.data(), result->m_layout.directory.size() * sizeof(uint32_t), sizeof(SunDepthHeader));
    result->m_values.create(physical, device, result->m_layout.depthBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Buffer::Memory::Device);
    std::fprintf(stderr, "Forward sparse sun depth: %zu tiles, %u rays, %llu bytes; diagnostic uncached capture\n",
        result->m_layout.tiles.size(), result->tasks(), static_cast<unsigned long long>(result->bytes()));
    return result;
  }
  std::vector<Transport::DepthRay> SunDepth::batch(uint32_t first, uint32_t count) const {
    if (first > tasks() || count > tasks() - first) throw std::out_of_range("Sun depth capture batch");
    std::vector<Transport::DepthRay> result;
    result.reserve(count);
    const auto &o = m_layout.options;
    for (uint32_t task = first; task < first + count; ++task) {
      const uint32_t slot = task / (m_extent * m_extent), pixel = task % (m_extent * m_extent);
      const uint32_t px = pixel % m_extent, py = pixel / m_extent;
      const auto allocation = m_layout.allocation(slot);
      const float u = (float(m_layout.tiles[slot][0]) + (float(px) - o.border + .5f) / o.tileTexels) * m_header.sampling[0];
      const float v = (float(m_layout.tiles[slot][1]) + (float(py) - o.border + .5f) / o.tileTexels) * m_header.sampling[0];
      Transport::DepthRay ray;
      for (uint32_t axis = 0; axis < 3; ++axis)
        ray.origin[axis] = m_basis[0][axis] * u + m_basis[1][axis] * v + m_basis[2][axis] * m_header.sampling[1];
      ray.direction = m_basis[2];
      ray.destination = (allocation.page * o.pageSize + allocation.y - o.border + py) * o.pageSize + allocation.x - o.border + px;
      result.push_back(ray);
    }
    return result;
  }
}
