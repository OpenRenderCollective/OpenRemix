#include "forward/depth_batch/batch.h"
#include <algorithm>
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
    std::vector<Position> positions;
    std::vector<uint32_t> indices;
    std::vector<Range> ranges;
    for (const auto &source : sources) {
      if (!source.owner || !source.vertices || !source.indices || source.stride < sizeof(Position) ||
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
          nextVertices * (sizeof(Position) + sizeof(uint32_t)) + nextIndices * sizeof(uint32_t) > m_budget)
        return false;
      const uint32_t firstVertex = uint32_t(positions.size());
      ranges.push_back({firstVertex, uint32_t(vertices), uint32_t(indices.size()), source.indexCount, {}});
      positions.resize(size_t(nextVertices));
      for (uint32_t i = 0; i < vertices; ++i)
        std::memcpy(positions[firstVertex + i].data(),
            static_cast<const std::byte *>(source.vertices->mapped) + (size_t(low) + i) * source.stride,
            sizeof(Position));
      for (uint32_t i = 0; i < source.indexCount; ++i)
        indices.push_back(uint32_t(int64_t(input[i]) + source.vertexOffset - low) + firstVertex);
    }
    if (positions.empty()) return false;
    ensure(m_positions, m_physical, m_device, positions.size() * sizeof(Position), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    ensure(m_tags, m_physical, m_device, positions.size() * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    ensure(m_indices, m_physical, m_device, indices.size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    m_positions->write(positions.data(), positions.size() * sizeof(Position));
    m_allIndices = std::move(indices);
    m_ranges = std::move(ranges);
    m_sources.assign(sources.begin(), sources.end());
    m_indicesCurrent = false;
    return true;
  }
  bool Batch::prepare(std::span<const Source> sources) {
    auto clear = [&] {
      m_indexCount = m_visibleSources = 0;
      m_indicesCurrent = false;
      m_sources.clear(); m_ranges.clear(); m_allIndices.clear();
      m_positions.reset(); m_tags.reset(); m_indices.reset();
    };
    if (sources.empty()) {
      clear();
      m_rejected.clear();
      m_retryAt = {};
      return false;
    }
    if (sources.size() == m_rejected.size() &&
        std::equal(sources.begin(), sources.end(), m_rejected.begin(), sameGeometry)) return false;
    const bool reusable = sources.size() == m_sources.size() &&
        std::equal(sources.begin(), sources.end(), m_sources.begin(), sameGeometry);
    if (!reusable) {
      using Clock = std::chrono::steady_clock;
      if (m_retryAt != Clock::time_point{} && Clock::now() < m_retryAt) return false;
      const auto retryAfterAllocationFailure = [&] {
        clear();
        m_rejected.clear();
        m_retryAt = Clock::now() + std::chrono::seconds(1);
        return false;
      };
      try {
        if (!rebuild(sources)) {
          clear();
          m_rejected.assign(sources.begin(), sources.end());
          return false;
        }
      } catch (VkResult error) {
        if (error != VK_ERROR_OUT_OF_HOST_MEMORY && error != VK_ERROR_OUT_OF_DEVICE_MEMORY) throw;
        return retryAfterAllocationFailure();
      } catch (const std::bad_alloc &) {
        return retryAfterAllocationFailure();
      }
      m_rejected.clear();
      m_retryAt = {};
    }
    // Each frame slot retains its own completed upload. Camera motion and
    // material tags can change independently of immutable source geometry.
    bool visibilityChanged = !m_indicesCurrent;
    bool tagsChanged = false;
    auto *tags = static_cast<uint32_t *>(m_tags->mapped);
    for (size_t i = 0; i < sources.size(); ++i) {
      const auto &source = sources[i];
      const auto &cached = m_sources[i];
      const auto &range = m_ranges[i];
      // Newly built ranges have no tag until they first emit indices.
      if (source.visible && range.uploadedTag != source.vertexTag) {
        std::fill_n(tags + range.vertex, range.vertices, source.vertexTag);
        tagsChanged = true;
      }
      visibilityChanged |= source.visible != cached.visible;
    }
    if (!tagsChanged && !visibilityChanged) return true;
    if (tagsChanged) {
      try {
        m_tags->flush();
      } catch (...) {
        for (auto &range : m_ranges) range.uploadedTag.reset();
        throw;
      }
      for (size_t i = 0; i < sources.size(); ++i)
        if (sources[i].visible) m_ranges[i].uploadedTag = sources[i].vertexTag;
    }
    if (visibilityChanged) {
      m_indicesCurrent = false;
      uint32_t visibleIndices = 0, visibleSources = 0;
      auto *destination = static_cast<uint32_t *>(m_indices->mapped);
      // Adjacent sources occupy consecutive index ranges in the full batch.
      // Copy each visible run directly into the fenced mapping in draw order.
      for (size_t first = 0; first < sources.size();) {
        if (!sources[first].visible) {
          ++first;
          continue;
        }
        size_t end = first + 1;
        while (end < sources.size() && sources[end].visible) ++end;
        const uint32_t beginIndex = m_ranges[first].index;
        const auto &last = m_ranges[end - 1];
        const uint32_t count = last.index + last.indices - beginIndex;
        std::memcpy(destination + visibleIndices, m_allIndices.data() + beginIndex,
                    size_t(count) * sizeof(uint32_t));
        visibleIndices += count;
        visibleSources += uint32_t(end - first);
        first = end;
      }
      if (visibleIndices) m_indices->flush();
      m_indexCount = visibleIndices;
      m_visibleSources = visibleSources;
      for (size_t i = 0; i < sources.size(); ++i) m_sources[i].visible = sources[i].visible;
      m_indicesCurrent = true;
    }
    return true;
  }
  uint64_t Batch::bytes() const {
    return (m_positions ? m_positions->allocationBytes : 0) +
        (m_tags ? m_tags->allocationBytes : 0) + (m_indices ? m_indices->allocationBytes : 0);
  }
} // namespace remix::forward::depth_batch
