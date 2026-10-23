#include "forward/mixed/directional_capture.h"
#include "forward/half.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <stdexcept>

namespace remix::forward::mixed {
  namespace {
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested()) throw std::runtime_error("Directional capture superseded");
    }
    void require(bool condition) {
      if (!condition) throw std::invalid_argument("Invalid directional capture");
    }
    bool validResult(const bake::VisibilityResult &value) {
      return std::isfinite(value.visibility) && value.visibility >= 0 && value.visibility <= 1 &&
          std::all_of(value.irradiance.begin(), value.irradiance.end(), [](float x) { return std::isfinite(x); });
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
        m_window.capacity() * sizeof(Field) + m_samples.capacity() * sizeof(directional::Field) + m_seen.capacity();
  }
  uint32_t DirectionalCapture::begin(std::stop_token cancelled) {
    return beginWindow(UINT32_MAX, 1, cancelled);
  }
  uint32_t DirectionalCapture::beginWindow(uint32_t pointTarget, uint32_t fieldLimit,
      std::stop_token cancelled) {
    check(cancelled);
    require(!m_active && !m_failed && !done() && pointTarget && fieldLimit && fieldLimit <= maximumWindowFields);
    std::array<Field, maximumWindowFields> planned{};
    uint32_t fields = 0;
    const auto resident = stagingBytes();
    uint64_t count = 0, rectangleBytes = 0;
    for (size_t next = m_next; next < m_links.size() && fields < fieldLimit; ++next) {
      check(cancelled);
      const auto &link = m_links[next];
      const uint32_t samples = m_offsets[link.chart + 1] - m_offsets[link.chart];
      const uint64_t wanted = count + samples;
      if (wanted > UINT32_MAX || (count && wanted > pointTarget)) break;
      const auto &r = m_layout.charts[link.chart];
      const auto rectangle = std::max(rectangleBytes, uint64_t(r.width) * r.height * sizeof(directional::Field));
      // Indexed receipts use a temporary sorted index copy for atomic duplicate
      // validation. Budget it for the largest possible receipt before tracing.
      const auto bytes = resident + uint64_t(fields + 1) * sizeof(Field) +
          wanted * (sizeof(directional::Field) + 1 + sizeof(uint32_t)) + rectangle;
      if (bytes > m_budget) {
        if (!fields) throw std::length_error("Directional field exceeds staging budget");
        break;
      }
      planned[fields++] = {link, uint32_t(count), samples};
      count = wanted; rectangleBytes = rectangle;
    }
    require(fields > 0);
    m_window.assign(planned.begin(), planned.begin() + fields);
    m_samples.resize(size_t(count)); m_seen.assign(size_t(count), 0);
    m_received = 0; m_active = true;
    return uint32_t(count);
  }
  bake::VisibilityTask DirectionalCapture::task(uint32_t sample) const {
    require(m_active && !m_failed && sample < m_samples.size());
    const auto end = std::upper_bound(m_window.begin(), m_window.end(), sample,
        [](uint32_t point, const Field &field) { return point < field.first; });
    require(end != m_window.begin());
    const auto &field = *std::prev(end);
    require(sample - field.first < field.count);
    return {m_points[m_offsets[field.owner.chart] + sample - field.first], field.owner.light,
            uint32_t(m_next + size_t(std::prev(end) - m_window.begin()))};
  }
  std::vector<bake::VisibilityTask> DirectionalCapture::batch(uint32_t first, uint32_t count) const {
    require(m_active && !m_failed && first <= m_samples.size() && count <= m_samples.size() - first);
    std::vector<bake::VisibilityTask> tasks;
    tasks.reserve(count);
    for (const auto &field : m_window) {
      const auto start = std::max(first, field.first), end = std::min(first + count, field.first + field.count);
      for (uint32_t i = start; i < end; ++i)
        tasks.push_back({m_points[m_offsets[field.owner.chart] + i - field.first], field.owner.light,
                        uint32_t(m_next + size_t(&field - m_window.data()))});
    }
    return tasks;
  }
  std::vector<bake::VisibilityTask> DirectionalCapture::batch(std::span<const uint32_t> indices) const {
    require(m_active && !m_failed);
    std::vector<bake::VisibilityTask> tasks;
    tasks.reserve(indices.size());
    for (const auto index : indices) tasks.push_back(task(index));
    return tasks;
  }
  void DirectionalCapture::observe(uint32_t first, std::span<const bake::VisibilityResult> values,
      std::stop_token cancelled) {
    check(cancelled);
    require(m_active && !m_failed && first <= m_samples.size() && values.size() <= m_samples.size() - first);
    // Validate the complete receipt before changing any sample or coverage bit.
    for (size_t i = 0; i < values.size(); ++i) {
      if ((i & 1023u) == 0) check(cancelled);
      require(!m_seen[first + i] && validResult(values[i]));
    }
    for (size_t i = 0; i < values.size(); ++i) {
      for (uint32_t c = 0; c < 9; ++c) m_samples[first + i][c] = half::encode(values[i].irradiance[c]);
      m_seen[first + i] = 1;
    }
    m_received += uint32_t(values.size());
  }
  void DirectionalCapture::observe(std::span<const uint32_t> indices,
      std::span<const bake::VisibilityResult> values, std::stop_token cancelled) {
    check(cancelled);
    require(m_active && !m_failed && indices.size() == values.size() && indices.size() <= m_samples.size());
    std::vector<uint32_t> sorted(indices.begin(), indices.end());
    std::sort(sorted.begin(), sorted.end());
    require(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
    for (size_t i = 0; i < values.size(); ++i) {
      if ((i & 1023u) == 0) check(cancelled);
      require(indices[i] < m_samples.size() && !m_seen[indices[i]] && validResult(values[i]));
    }
    for (size_t i = 0; i < values.size(); ++i) {
      for (uint32_t c = 0; c < 9; ++c) m_samples[indices[i]][c] = half::encode(values[i].irradiance[c]);
      m_seen[indices[i]] = 1;
    }
    m_received += uint32_t(values.size());
  }
  bool DirectionalCapture::finish(std::stop_token cancelled) {
    check(cancelled);
    require(m_active && !m_failed && m_received == m_samples.size());
    for (const auto &field : m_window) {
      const auto &r = m_layout.charts[field.owner.chart];
      const auto points = std::span(m_points).subspan(m_offsets[field.owner.chart], field.count);
      std::vector<directional::Field> rectangle(size_t(r.width) * r.height);
      for (uint32_t y = 0; y < r.height; ++y) {
        check(cancelled);
        for (uint32_t x = 0; x < r.width; ++x) {
          const auto owner = m_layout.owners[(size_t(r.page) * m_layout.pageSize + r.y + y) * m_layout.pageSize + r.x + x];
          if (owner == lightmap::unowned) continue;
          const auto found = std::lower_bound(points.begin(), points.end(), owner);
          require(found != points.end() && *found == owner);
          rectangle[size_t(y) * r.width + x] = m_samples[field.first + size_t(found - points.begin())];
        }
      }
      if (!m_fields.add(field.owner.chart, field.owner.light, r, rectangle, cancelled)) {
        const auto usage = m_fields.storageUsage();
        std::fprintf(stderr, "Forward directional storage exhausted: accepted_fields=%zu total_fields=%zu "
                             "encoded_bytes=%llu raw_payload_bytes=%llu raw_fields=%u tiled_fields=%u "
                             "next_chart=%u next_light=%u next_size=%ux%u\n",
            m_fields.size(), m_links.size(), static_cast<unsigned long long>(usage.encodedBytes),
            static_cast<unsigned long long>(usage.rawBytes), usage.rawFields, usage.tiledFields,
            field.owner.chart, field.owner.light, r.width, r.height);
        m_failed = true;
        return false;
      }
    }
    m_next += m_window.size(); m_active = false;
    std::vector<Field>().swap(m_window);
    std::vector<directional::Field>().swap(m_samples);
    std::vector<uint8_t>().swap(m_seen);
    return true;
  }
  std::vector<uint32_t> DirectionalCapture::words(std::stop_token cancelled) const {
    require(done() && !m_active && !m_failed);
    return m_fields.words(cancelled);
  }
}
