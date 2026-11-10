#pragma once
#include "forward/lightmap/layout.h"
#include <algorithm>
#include <span>

namespace remix::forward::lightmap::denoising {
  constexpr uint32_t coefficientCount = 9;
  constexpr size_t batchBudgetBytes = 8u << 20;
  constexpr uint32_t queuedChartLimit = 32;
  constexpr size_t queueBudgetBytes = 8u << 20; // includes original/filtered L0 for every pending chart
  // One chart may exceed its worker's budget, but it must still run on its own.
  inline uint32_t cpuChartQueueCapacity(size_t chartBytes, uint32_t queues) {
    return uint32_t(std::min<size_t>(queuedChartLimit / queues,
        std::max<size_t>(1, queueBudgetBytes / queues / chartBytes)));
  }
  // A single field larger than the batch budget still needs one image.
  uint32_t batchFields(size_t fieldBytes);
  uint32_t alignedExtent(uint32_t extent, uint32_t alignment);
  // CPU-only chart encoding. Every plane is tightly packed RGB, with no atlas
  // neighbours. Original L0 stays available until all signed bands are restored.
  class Chart {
  public:
    Chart(const Layout &layout, uint32_t chart, std::span<bake::Result> results,
          std::span<const uint32_t> points, std::stop_token cancelled, uint32_t alignment = 1);
    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }
    uint32_t fieldsPerBatch() const { return m_batch; }
    uint64_t hostBytes() const;
    std::span<float> pack(uint32_t first, uint32_t count);
    void apply(uint32_t first, uint32_t count, std::span<const float> filtered);
    void finish();

  private:
    const Layout &m_layout;
    const Allocation &m_allocation;
    uint32_t m_chart, m_width, m_height, m_batch;
    size_t m_origin, m_plane;
    std::span<bake::Result> m_results;
    std::span<const uint32_t> m_points;
    std::stop_token m_cancelled;
    std::vector<float> m_original, m_l0, m_fields;
    size_t owner(uint32_t x, uint32_t y) const;
    size_t pixel(uint32_t point) const;
  };
} // namespace remix::forward::lightmap::denoising
