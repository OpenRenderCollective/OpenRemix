#pragma once
#include "forward/lightmap/data.h"
#include "forward/lightmap/receiver_sampler.h"

namespace remix::forward::bake {
  // Adaptive receiver-area integration at detected sunlight visibility edges.
  // Keep only receiver IDs; expand bounded origins for the current GPU batch.
  class SunAntialiasing {
  public:
    SunAntialiasing(const scene::BakeScene &source, const lightmap::Layout &layout,
                    std::span<const lightmap::Sun> sunlight, std::stop_token cancelled = {},
                    uint32_t sitesPerReceiver = 16);
    uint32_t siteCount() const { return m_siteCount; }
    uint32_t count() const { return uint32_t(m_points.size()); }
    uint32_t receiver(uint32_t task) const { return m_points.at(task); }
    std::span<const uint32_t> receivers() const { return m_points; }
    uint32_t fallbackCount() const { return m_sampler.fallbackCount(); }
    std::vector<Point> sites(uint32_t first, uint32_t count);
    uint64_t memoryBytes() const;

  private:
    uint32_t m_siteCount;
    std::vector<uint32_t> m_points;
    lightmap::ReceiverSampler m_sampler;
  };
} // namespace remix::forward::bake
