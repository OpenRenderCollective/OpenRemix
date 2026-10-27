#include "forward/depth_batch/batch.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace remix::forward::depth_batch {
  namespace {
    bool sameGeometry(const Source &a, const Source &b) {
      return a.vertices == b.vertices && a.indices == b.indices && a.stride == b.stride &&
          a.firstIndex == b.firstIndex && a.indexCount == b.indexCount &&
          a.vertexOffset == b.vertexOffset && a.owner == b.owner;
    }
    void ensure(std::unique_ptr<Buffer> &buffer, VkPhysicalDevice physical, VkDevice device, size_t bytes,
                VkBufferUsageFlags usage) {
      if (!buffer || buffer->capacity != bytes) {
        auto next = std::make_unique<Buffer>();
        next->create(physical, device, bytes, usage);
        buffer = std::move(next);
      }
    }
  }
  Batch::Batch(VkPhysicalDevice physical, VkDevice device, uint64_t budget)
      : m_physical(physical), m_device(device), m_budget(std::min(budget, maximumBytes)) {}
  bool Batch::rebuild(std::span<const Source> sources) {
    std::vector<std::array<float, 4>> positions;
    std::vector<uint32_t> indices;
    std::vector<Range> ranges;
    for (const auto &source : sources) {
      if (!source.owner || !source.vertices || !source.indices || source.stride < 12 ||
          !source.vertices->mapped || !source.indices->mapped || !source.indexCount || source.indexCount % 3 != 0 ||
          source.vertices->device != m_device || source.indices->device != m_device ||
          uint64_t(source.firstIndex) + source.indexCount > source.indices->capacity / 4) return false;
      const auto *input = static_cast<const uint32_t *>(source.indices->mapped) + source.firstIndex;
      int64_t low = std::numeric_limits<int64_t>::max(), high = -1;
      for (uint32_t i = 0; i < source.indexCount; ++i) {
        const int64_t vertex = int64_t(input[i]) + source.vertexOffset;
        low = std::min(low, vertex);
        high = std::max(high, vertex);
      }
      if (low < 0 || high < low || uint64_t(high) >= source.vertices->capacity / source.stride) return false;
      const uint64_t vertices = uint64_t(high - low + 1);
      const uint64_t nextVertices = positions.size() + vertices, nextIndices = indices.size() + source.indexCount;
      if (nextVertices > UINT32_MAX || nextIndices > UINT32_MAX ||
          nextVertices * 20 + nextIndices * 4 > m_budget) return false;
      const uint32_t firstVertex = uint32_t(positions.size());
      ranges.push_back({firstVertex, uint32_t(vertices), uint32_t(indices.size()), source.indexCount});
      positions.resize(size_t(nextVertices));
      for (uint32_t i = 0; i < vertices; ++i)
        std::memcpy(positions[firstVertex + i].data(),
            static_cast<const std::byte *>(source.vertices->mapped) + (size_t(low) + i) * source.stride, 12);
      for (uint32_t i = 0; i < source.indexCount; ++i)
        indices.push_back(uint32_t(int64_t(input[i]) + source.vertexOffset - low) + firstVertex);
    }
    if (positions.empty()) return false;
    ensure(m_positions, m_physical, m_device, positions.size() * 16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    ensure(m_materials, m_physical, m_device, positions.size() * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    ensure(m_indices, m_physical, m_device, indices.size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    m_positions->write(positions.data(), positions.size() * 16);
    m_allIndices = std::move(indices);
    m_ranges = std::move(ranges);
    m_sources.assign(sources.begin(), sources.end());
    return true;
  }
  bool Batch::prepare(std::span<const Source> sources) {
    auto clear = [&] {
      m_indexCount = m_visibleSources = 0;
      m_sources.clear(); m_ranges.clear(); m_allIndices.clear(); m_visibleIndices.clear();
      m_positions.reset(); m_materials.reset(); m_indices.reset();
    };
    if (sources.empty()) { clear(); m_rejected.clear(); return false; }
    if (sources.size() == m_rejected.size() &&
        std::equal(sources.begin(), sources.end(), m_rejected.begin(), sameGeometry)) return false;
    const bool reusable = sources.size() == m_sources.size() &&
        std::equal(sources.begin(), sources.end(), m_sources.begin(), sameGeometry);
    if (!reusable) {
      bool ready = false;
      try { ready = rebuild(sources); }
      catch (VkResult error) {
        if (error != VK_ERROR_OUT_OF_HOST_MEMORY && error != VK_ERROR_OUT_OF_DEVICE_MEMORY) throw;
      } catch (const std::bad_alloc &) {}
      if (!ready) {
        clear();
        m_rejected.assign(sources.begin(), sources.end());
        return false;
      }
      m_rejected.clear();
    }
    // Each frame slot retains its own completed upload. Camera motion and
    // material-table reordering are the only changes to these two streams;
    // transforms remain in the ordinary current-frame material buffer.
    const bool unchanged = reusable && std::equal(sources.begin(), sources.end(), m_sources.begin(),
        [](const Source &a, const Source &b) {
          return a.visible == b.visible && (!a.visible || a.materialIndex == b.materialIndex);
        });
    if (unchanged) return true;
    m_indexCount = m_visibleSources = 0;
    m_visibleIndices.clear();
    auto *materials = static_cast<uint32_t *>(m_materials->mapped);
    for (size_t i = 0; i < sources.size(); ++i) {
      if (!sources[i].visible) continue;
      const auto &range = m_ranges[i];
      std::fill_n(materials + range.vertex, range.vertices, sources[i].materialIndex);
      m_visibleIndices.insert(m_visibleIndices.end(), m_allIndices.begin() + range.index,
                              m_allIndices.begin() + range.index + range.indices);
      ++m_visibleSources;
    }
    if (!m_visibleIndices.empty()) {
      m_materials->flush();
      m_indices->write(m_visibleIndices.data(), m_visibleIndices.size() * 4);
    }
    m_indexCount = uint32_t(m_visibleIndices.size());
    for (size_t i = 0; i < sources.size(); ++i) {
      m_sources[i].visible = sources[i].visible;
      m_sources[i].materialIndex = sources[i].materialIndex;
    }
    return true;
  }
  uint64_t Batch::bytes() const {
    return (m_positions ? m_positions->allocationBytes : 0) +
        (m_materials ? m_materials->allocationBytes : 0) + (m_indices ? m_indices->allocationBytes : 0);
  }
} // namespace remix::forward::depth_batch
