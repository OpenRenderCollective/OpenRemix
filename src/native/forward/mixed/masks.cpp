#include "forward/mixed/masks.h"
#include "forward/mixed/mask_codec.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numeric>
#include <set>
#include <stdexcept>

namespace remix::forward::mixed {
  bool Masks::add(uint32_t chart, uint32_t light, const lightmap::Layout &layout,
                   std::span<const float> values, uint64_t wordBudget, std::stop_token cancelled) {
    if (chart >= layout.charts.size() || values.size() != layout.points.size())
      throw std::invalid_argument("Invalid overflow visibility chart");
    const auto &a = layout.charts[chart];
    const uint64_t count = uint64_t(a.width) * a.height;
    const uint64_t padded = (count + 3) & ~3ull;
    const uint64_t tableWords = layout.charts.size() * 2 + (m_entries.size() + 1) * 4;
    const uint64_t minimumBytes = std::min(padded, ((uint64_t(a.width) + 7) / 8) *
                                                  ((uint64_t(a.height) + 7) / 8) * 4);
    const uint64_t minimumWords = tableWords + (m_bytes.size() + minimumBytes) / 4;
    const bool audit = std::getenv("REMIX_DEBUG_MASK_COSTS") != nullptr;
    if (!count || (!audit && (minimumWords * sizeof(uint32_t) > maximumBytes || minimumWords > wordBudget)))
      return false;
    if (m_identities.contains({chart, light}))
      throw std::invalid_argument("Duplicate overflow visibility mask");
    const auto offset = uint32_t(m_bytes.size());
    std::vector<uint8_t> texels(size_t(padded), 255);
    for (uint32_t y = 0; y < a.height; ++y) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Overflow mask preparation superseded");
      for (uint32_t x = 0; x < a.width; ++x) {
        const size_t address = (size_t(a.page) * layout.pageSize + a.y + y) * layout.pageSize + a.x + x;
        const auto owner = layout.owners.at(address);
        if (owner == lightmap::unowned)
          continue; // unused chart-box texels cannot be sampled by a receiver
        if (owner >= values.size())
          throw std::invalid_argument("Invalid overflow visibility donor");
        const float value = values[owner];
        if (layout.pointCharts.at(owner) != chart || !std::isfinite(value) || value < 0 || value > 1)
          throw std::invalid_argument("Incomplete overflow visibility donor");
        texels[size_t(y) * a.width + x] = uint8_t(std::lround(value * 255));
      }
    }
    const auto input = std::span(texels).first(size_t(count));
    const auto encoded = mask::encodeCompact(input, a.width, a.height, cancelled);
    if (audit)
      m_candidateCosts.observe(input, a.width, a.height,
                                mask::encode(input, a.width, a.height, cancelled).bytes.size(), cancelled);
    const uint64_t words = tableWords + (m_bytes.size() + encoded.bytes.size()) / 4;
    if (words * sizeof(uint32_t) > maximumBytes || words > wordBudget)
      return false;
    m_entries.push_back({chart, light, offset, a.width, a.height, encoded.encoding});
    m_bytes.insert(m_bytes.end(), encoded.bytes.begin(), encoded.bytes.end());
    m_identities.emplace(chart, light);
    return true;
  }
  Masks Masks::restore(std::span<const Entry> entries, std::span<const uint8_t> bytes,
                         const lightmap::Layout &layout, uint32_t lights) {
    const uint64_t encoded = entries.empty() ? bytes.size()
        : layout.charts.size() * 8 + entries.size() * 16 + bytes.size();
    if (encoded > maximumBytes || entries.size() > uint64_t(layout.charts.size()) * lights)
      throw std::invalid_argument("Overflow visibility cache exceeds budget");
    size_t offset = 0;
    std::set<std::pair<uint32_t, uint32_t>> identities;
    for (const auto &entry : entries) {
      if (entry.chart >= layout.charts.size() || entry.light >= lights || entry.offset != offset ||
          !identities.emplace(entry.chart, entry.light).second)
        throw std::invalid_argument("Invalid overflow visibility cache identity");
      const auto &a = layout.charts[entry.chart];
      if (!entry.width || !entry.height || entry.width != a.width || entry.height != a.height)
        throw std::invalid_argument("Invalid overflow visibility cache dimensions");
      if (offset > bytes.size())
        throw std::invalid_argument("Truncated overflow visibility cache");
      offset += mask::validate(bytes.subspan(offset), entry.width, entry.height, entry.encoding);
    }
    if (offset != bytes.size())
      throw std::invalid_argument("Trailing overflow visibility cache bytes");
    Masks result;
    result.m_entries.assign(entries.begin(), entries.end());
    result.m_bytes.assign(bytes.begin(), bytes.end());
    result.m_identities = std::move(identities);
    return result;
  }
  std::vector<uint32_t> Masks::words(uint32_t charts) const {
    if (m_entries.empty())
      return {};
    std::vector<size_t> order(m_entries.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
      return std::pair{m_entries[a].chart, m_entries[a].light} <
             std::pair{m_entries[b].chart, m_entries[b].light};
    });
    const size_t data = size_t(charts) * 2 + m_entries.size() * 4;
    std::vector<uint32_t> result(data + m_bytes.size() / 4);
    for (size_t i = 0; i < order.size(); ++i) {
      const auto &e = m_entries[order[i]];
      if (e.chart >= charts)
        throw std::invalid_argument("Invalid overflow visibility table chart");
      auto &count = result[e.chart * 2 + 1];
      const auto record = uint32_t(charts * 2 + i * 4);
      if (!count++)
        result[e.chart * 2] = record;
      result[record] = e.light;
      result[record + 1] = uint32_t(data * 4 + e.offset);
      result[record + 2] = e.width | (e.encoding == mask::tiled ? 0x80000000u : 0u);
      result[record + 3] = e.height;
    }
    for (size_t i = 0; i < m_bytes.size(); ++i)
      result[data + i / 4] |= uint32_t(m_bytes[i]) << ((i % 4) * 8);
    return result;
  }
} // namespace remix::forward::mixed
