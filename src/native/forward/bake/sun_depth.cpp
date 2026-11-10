#include "forward/bake/sun_depth.h"
#include "forward/bake/cache.h"
#include "forward/bake/shaders/embedded.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
namespace remix::forward::bake {
  std::shared_ptr<SunDepth> SunDepth::prepare(VkPhysicalDevice physical, VkDevice device,
      const scene::BakeScene &source, const lightmap::Layout &receivers, std::stop_token cancelled,
      bool diskCache, uint32_t textureResolution) {
    if (!source.sun.isDistant) return {};
    auto result = std::shared_ptr<SunDepth>(new SunDepth);
    const auto z = normalized({source.sun.direction[0], source.sun.direction[1], source.sun.direction[2]});
    const auto x = normalized(cross(std::abs(z[2]) < .99f ? Vec3{0,0,1} : Vec3{0,1,0}, z));
    result->m_basis = {x, cross(z,x), z};
    const std::array<ReceiverTileOptions, 1> trials{{{4, 1}}};
    ReceiverProjection projected;
    try {
      projected = projectReceivers(source, receivers, result->m_basis, cancelled, trials);
    } catch (const std::length_error &error) {
      // Optional coverage must not invalidate an otherwise usable light bake.
      // Cancellation and malformed input still propagate to the caller.
      std::fprintf(stderr, "Forward sparse sun depth declined: %s\n", error.what());
      return {};
    }
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
    for (size_t i = 0; i < source.draws.size(); ++i) {
      if (source.receiverOnly(i)) continue;
      const auto &draw = source.draws[i];
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
    if (diskCache) {
      const uint32_t policy[]{1, textureResolution};
      result->m_cacheKey = scene::fastHash(policy, sizeof(policy), source.contentHash);
      result->m_cacheKey = scene::fastHash(depthShader, sizeof(depthShader), result->m_cacheKey);
      result->m_cacheKey = scene::fastHash(&result->m_header, sizeof(result->m_header), result->m_cacheKey);
      result->m_cacheKey = scene::fastHash(result->m_layout.directory.data(), result->m_layout.directory.size()*sizeof(uint32_t), result->m_cacheKey);
      try { result->m_cacheFile = cache::path(result->m_cacheKey); result->m_cacheFile.replace_extension(".sun-depth"); }
      catch (const std::exception &) { result->m_cacheFile.clear(); }
    }
    auto restored = result->loadCache(cancelled);
    result->m_table.create(physical, device, tableBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    result->m_table.write(&result->m_header, sizeof(result->m_header));
    result->m_table.write(result->m_layout.directory.data(), result->m_layout.directory.size() * sizeof(uint32_t), sizeof(SunDepthHeader));
    result->m_values.create(physical, device, result->m_layout.depthBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buffer::Memory::Device);
    if (!restored.empty()) {
      result->m_transfer = std::make_unique<Buffer>();
      result->m_transfer->create(physical, device, result->m_layout.depthBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
      result->m_transfer->write(restored.data(), restored.size()*sizeof(float));
      result->m_transferBytes = result->m_transfer->allocationBytes;
      result->m_restored = true;
    }
    std::fprintf(stderr, "Forward sparse sun depth: %zu tiles, %u rays, %llu bytes; restored %u\n",
        result->m_layout.tiles.size(), result->tasks(), static_cast<unsigned long long>(result->bytes()), uint32_t(result->m_restored));
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
