#include "forward/bake/sun_antialiasing.h"
#include <cmath>
#include <stdexcept>

namespace remix::forward::bake {
  SunAntialiasing::SunAntialiasing(const scene::BakeScene &source, const lightmap::Layout &layout,
                                   std::span<const lightmap::Sun> sunlight, std::stop_token cancelled,
                                   uint32_t sitesPerReceiver)
      : m_siteCount(sitesPerReceiver), m_sampler(source, layout) {
    if (m_siteCount != 4 && m_siteCount != 16)
      throw std::invalid_argument("Sunlight quadrature requires four or sixteen sites");
    if (sunlight.size() != layout.points.size() ||
        layout.owners.size() != uint64_t(layout.pageSize) * layout.pageSize * layout.pages)
      throw std::invalid_argument("Incomplete sunlight edge sampling layout");
    for (uint32_t point = 0; point < sunlight.size(); ++point) {
      if ((point & 1023u) == 0 && cancelled.stop_requested())
        throw std::runtime_error("Sunlight edge sampling superseded");
      const float visibility = sunlight[point].visibility;
      if (!std::isfinite(visibility) || visibility < 0 || visibility > 1)
        throw std::invalid_argument("Invalid sunlight edge visibility");
      const bool sharedFootprint = !layout.receiverSources.empty() &&
          layout.receiverSources[point] != lightmap::unowned;
      if (!layout.receiverSamples.empty() && !layout.receiverSamples[point] && !sharedFootprint)
        continue;
      const auto address = layout.addresses[point];
      if (address >= layout.owners.size())
        throw std::invalid_argument("Invalid sunlight edge address");
      if (layout.owners[address] != point)
        continue;
      const auto chart = layout.pointCharts[point];
      const int64_t x = address % layout.pageSize, y = (address / layout.pageSize) % layout.pageSize;
      bool edge = false;
      for (const auto &delta : {std::array<int, 2>{-1, 0}, {1, 0}, {0, -1}, {0, 1}}) {
        if (x + delta[0] < 0 || x + delta[0] >= layout.pageSize || y + delta[1] < 0 ||
            y + delta[1] >= layout.pageSize)
          continue;
        const auto neighbour =
            layout.owners[size_t(int64_t(address) + delta[0] + int64_t(delta[1]) * layout.pageSize)];
        if (neighbour == lightmap::unowned)
          continue;
        if (neighbour >= sunlight.size())
          throw std::invalid_argument("Invalid sunlight edge donor");
        if (layout.pointCharts[neighbour] != chart)
          continue;
        edge |= std::abs(visibility - sunlight[neighbour].visibility) >= .0625f;
      }
      if (edge)
        m_points.push_back(point);
    }
  }
  std::vector<Point> SunAntialiasing::sites(uint32_t first, uint32_t count) {
    if (first > m_points.size() || count > m_points.size() - first)
      throw std::out_of_range("Sunlight edge batch leaves its task range");
    std::vector<Point> result;
    result.resize(size_t(count) * m_siteCount);
    for (uint32_t i = first; i < first + count; ++i) {
      m_sampler.sample(m_points[i], std::span(result).subspan(size_t(i - first) * m_siteCount, m_siteCount));
    }
    return result;
  }
  uint64_t SunAntialiasing::memoryBytes() const {
    return m_points.capacity() * sizeof(uint32_t) + m_sampler.memoryBytes();
  }
} // namespace remix::forward::bake
