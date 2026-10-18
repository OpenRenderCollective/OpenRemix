#include "forward/mixed/directional_capture.h"
#include "forward/half.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace remix::forward::mixed {
  namespace {
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested()) throw std::runtime_error("Directional capture superseded");
    }
    void require(bool condition) {
      if (!condition) throw std::invalid_argument("Invalid directional capture");
    }
  }
  DirectionalCapture::DirectionalCapture(const lightmap::Layout &layout, uint32_t lights,
      std::span<const Link> links, uint64_t stagingBudget, uint64_t storageBudget, std::stop_token cancelled)
      : m_layout(layout), m_fields(uint32_t(layout.charts.size()), lights, storageBudget), m_budget(stagingBudget) {
    check(cancelled);
    require(layout.charts.size() < UINT32_MAX && layout.points.size() <= UINT32_MAX &&
        layout.points.size() == layout.pointCharts.size() && layout.pageSize > 0 && layout.pageSize <= 4096 &&
        layout.pages <= 256 && links.size() < UINT32_MAX &&
        layout.owners.size() == uint64_t(layout.pageSize) * layout.pageSize * layout.pages);
    if (links.empty()) return;
    const uint64_t indexBytes = uint64_t(layout.charts.size() + 1 + layout.points.size()) * 4 + links.size() * sizeof(Link);
    if (indexBytes > m_budget) throw std::length_error("Directional capture index exceeds staging budget");
    {
      std::vector<uint64_t> identities;
      identities.reserve(links.size());
      for (const auto &link : links) {
        check(cancelled);
        require(link.chart < layout.charts.size() && link.light < lights);
        identities.push_back(uint64_t(link.chart) << 32 | link.light);
        const auto &r = layout.charts[link.chart];
        require(r.page < layout.pages && r.width > 0 && r.height > 0 && r.width <= layout.pageSize &&
            r.height <= layout.pageSize && r.x <= layout.pageSize - r.width && r.y <= layout.pageSize - r.height);
      }
      std::sort(identities.begin(), identities.end());
      require(std::adjacent_find(identities.begin(), identities.end()) == identities.end());
      uint32_t previous = lightmap::unowned;
      for (const auto identity : identities) {
        const auto chart = uint32_t(identity >> 32);
        if (chart == previous) continue;
        previous = chart;
        const auto &r = layout.charts[chart];
        for (uint32_t y = 0; y < r.height; ++y) {
          check(cancelled);
          for (uint32_t x = 0; x < r.width; ++x) {
            const auto owner = layout.owners[(size_t(r.page) * layout.pageSize + r.y + y) * layout.pageSize + r.x + x];
            require(owner == lightmap::unowned ||
                (owner < layout.pointCharts.size() && layout.pointCharts[owner] == chart));
          }
        }
      }
    }
    m_links.assign(links.begin(), links.end());
    m_offsets.resize(layout.charts.size() + 1);
    for (size_t point = 0; point < layout.points.size(); ++point) {
      if ((point & 1023u) == 0) check(cancelled);
      const auto chart = layout.pointCharts[point];
      require(chart < layout.charts.size());
      ++m_offsets[chart + 1];
    }
    for (size_t i = 1; i < m_offsets.size(); ++i) m_offsets[i] += m_offsets[i - 1];
    m_points.resize(layout.points.size());
    for (uint32_t point = 0; point < layout.points.size(); ++point) {
      if ((point & 1023u) == 0) check(cancelled);
      m_points[m_offsets[layout.pointCharts[point]]++] = point;
    }
    for (size_t chart = m_offsets.size() - 1; chart > 0; --chart) m_offsets[chart] = m_offsets[chart - 1];
    m_offsets[0] = 0;
  }
  uint64_t DirectionalCapture::stagingBytes() const {
    return m_links.capacity() * sizeof(Link) + (m_offsets.capacity() + m_points.capacity()) * 4 +
        m_samples.capacity() * sizeof(directional::Field) + m_seen.capacity();
  }
  uint32_t DirectionalCapture::begin(std::stop_token cancelled) {
    check(cancelled);
    require(!m_active && !m_failed && !done());
    const auto &link = m_links[m_next];
    const uint32_t count = m_offsets[link.chart + 1] - m_offsets[link.chart];
    const auto &r = m_layout.charts[link.chart];
    const uint64_t bytes = stagingBytes() + uint64_t(count) * (sizeof(directional::Field) + 1) +
        uint64_t(r.width) * r.height * sizeof(directional::Field);
    if (bytes > m_budget) throw std::length_error("Directional field exceeds staging budget");
    m_samples.resize(count); m_seen.assign(count, 0);
    m_received = 0;
    m_active = true;
    return count;
  }
  std::vector<bake::VisibilityTask> DirectionalCapture::batch(uint32_t first, uint32_t count) const {
    require(m_active && !m_failed && first <= m_samples.size() && count <= m_samples.size() - first);
    const auto &link = m_links[m_next];
    std::vector<bake::VisibilityTask> tasks;
    tasks.reserve(count);
    for (uint32_t i = first; i < first + count; ++i)
      tasks.push_back({m_points[m_offsets[link.chart] + i], link.light, uint32_t(m_next)});
    return tasks;
  }
  void DirectionalCapture::observe(uint32_t first, std::span<const bake::VisibilityResult> values,
      std::stop_token cancelled) {
    check(cancelled);
    require(m_active && !m_failed && first <= m_samples.size() && values.size() <= m_samples.size() - first);
    // Validate the complete receipt before changing any sample or coverage bit.
    for (size_t i = 0; i < values.size(); ++i) {
      if ((i & 1023u) == 0) check(cancelled);
      const auto &value = values[i];
      require(!m_seen[first + i] && std::isfinite(value.visibility) && value.visibility >= 0 && value.visibility <= 1 &&
          std::all_of(value.irradiance.begin(), value.irradiance.end(), [](float x) { return std::isfinite(x); }));
    }
    for (size_t i = 0; i < values.size(); ++i) {
      for (uint32_t c = 0; c < 9; ++c) m_samples[first + i][c] = half::encode(values[i].irradiance[c]);
      m_seen[first + i] = 1;
    }
    m_received += uint32_t(values.size());
  }
  bool DirectionalCapture::finish(std::stop_token cancelled) {
    check(cancelled);
    require(m_active && !m_failed && m_received == m_samples.size());
    const auto &link = m_links[m_next];
    const auto &r = m_layout.charts[link.chart];
    const auto points = std::span(m_points).subspan(m_offsets[link.chart], m_samples.size());
    std::vector<directional::Field> rectangle(size_t(r.width) * r.height);
    for (uint32_t y = 0; y < r.height; ++y) {
      check(cancelled);
      for (uint32_t x = 0; x < r.width; ++x) {
        const auto owner = m_layout.owners[(size_t(r.page) * m_layout.pageSize + r.y + y) * m_layout.pageSize + r.x + x];
        if (owner == lightmap::unowned) continue;
        const auto found = std::lower_bound(points.begin(), points.end(), owner);
        require(found != points.end() && *found == owner);
        rectangle[size_t(y) * r.width + x] = m_samples[size_t(found - points.begin())];
      }
    }
    if (!m_fields.add(link.chart, link.light, r, rectangle, cancelled)) {
      m_failed = true;
      return false;
    }
    ++m_next; m_active = false;
    // Release a large field before sizing the next; never accumulate all-map SH scratch.
    std::vector<directional::Field>().swap(m_samples);
    std::vector<uint8_t>().swap(m_seen);
    return true;
  }
  std::vector<uint32_t> DirectionalCapture::words(std::stop_token cancelled) const {
    require(done() && !m_active && !m_failed);
    return m_fields.words(cancelled);
  }
}
