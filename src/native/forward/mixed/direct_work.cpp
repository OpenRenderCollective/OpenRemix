#include "forward/mixed/direct_work.h"
#include <algorithm>
#include <stdexcept>

namespace remix::forward::mixed {
  DirectWork::DirectWork(const Plan &plan, const lightmap::Layout &layout, std::stop_token cancelled) {
    if (cancelled.stop_requested()) throw std::runtime_error("Direct lighting superseded");
    if (!plan.lights.source || plan.directCharts.size() != plan.charts.size() ||
        layout.points.size() != layout.pointCharts.size() || layout.points.size() > UINT32_MAX)
      throw std::invalid_argument("Invalid retained direct-light plan");
    if (plan.lights.source->lights.size() > UINT32_MAX || plan.directCharts.size() >= UINT32_MAX)
      throw std::length_error("Too many direct-light owners");
    uint64_t ownerCount = 0;
    for (const auto &chart : plan.directCharts) {
      if (cancelled.stop_requested()) throw std::runtime_error("Direct lighting superseded");
      ownerCount += std::count_if(chart.begin(), chart.end(), [](uint32_t light) { return light != none; });
    }
    if (ownerCount > UINT32_MAX) throw std::length_error("Too many direct-light owners");
    m_owners.reserve(size_t(ownerCount));
    m_chartOffsets.reserve(plan.directCharts.size() + 1);
    for (const auto &chart : plan.directCharts) {
      if (cancelled.stop_requested()) throw std::runtime_error("Direct lighting superseded");
      m_chartOffsets.push_back(uint32_t(m_owners.size()));
      for (uint32_t channel = 0; channel < channels; ++channel) {
        if (chart[channel] == none) continue;
        if (m_owners.size() == UINT32_MAX) throw std::length_error("Too many direct-light owners");
        m_owners.push_back({chart[channel], channel});
      }
    }
    m_chartOffsets.push_back(uint32_t(m_owners.size()));
    index(uint32_t(plan.lights.source->lights.size()), layout, cancelled);
  }

  DirectWork::DirectWork(uint32_t lightCount, std::span<const uint32_t> chartOffsets,
                         std::span<const Owner> owners, const lightmap::Layout &layout,
                         std::stop_token cancelled) {
    if (cancelled.stop_requested()) throw std::runtime_error("Direct lighting superseded");
    if (owners.size() > UINT32_MAX || chartOffsets.size() > UINT32_MAX)
      throw std::length_error("Too many direct-light owners");
    m_chartOffsets.assign(chartOffsets.begin(), chartOffsets.end());
    m_owners.assign(owners.begin(), owners.end());
    index(lightCount, layout, cancelled);
  }

  void DirectWork::index(uint32_t lightCount, const lightmap::Layout &layout, std::stop_token cancelled) {
    if (m_chartOffsets.empty() || m_chartOffsets.front() != 0 || m_chartOffsets.back() != m_owners.size() ||
        layout.points.size() != layout.pointCharts.size() || layout.points.size() > UINT32_MAX)
      throw std::invalid_argument("Invalid sparse direct-light index");
    std::vector<uint32_t> lights, outputs;
    for (size_t chart = 0; chart + 1 < m_chartOffsets.size(); ++chart) {
      if (cancelled.stop_requested()) throw std::runtime_error("Direct lighting superseded");
      const auto first = m_chartOffsets[chart], end = m_chartOffsets[chart + 1];
      if (first > end || end > m_owners.size()) throw std::invalid_argument("Invalid direct-light chart range");
      lights.clear(); outputs.clear();
      for (uint32_t i = first; i < end; ++i) {
        if ((i & 1023u) == 0 && cancelled.stop_requested())
          throw std::runtime_error("Direct lighting superseded");
        const auto &owner = m_owners[i];
        if (owner.light >= lightCount || owner.output == none)
          throw std::invalid_argument("Invalid direct-light owner");
        lights.push_back(owner.light); outputs.push_back(owner.output);
      }
      for (auto *values : {&lights, &outputs}) {
        std::sort(values->begin(), values->end());
        if (std::adjacent_find(values->begin(), values->end()) != values->end())
          throw std::invalid_argument("Duplicate direct-light owner or output");
      }
    }
    m_receivers.reserve(layout.points.size() + 1);
    uint64_t total = 0;
    for (size_t point = 0; point < layout.points.size(); ++point) {
      if ((point & 1023u) == 0 && cancelled.stop_requested())
        throw std::runtime_error("Direct lighting superseded");
      const auto chart = layout.pointCharts[point];
      if (chart >= m_chartOffsets.size() - 1) throw std::invalid_argument("Invalid direct-light receiver chart");
      m_receivers.push_back({uint32_t(total), chart});
      total += m_chartOffsets[chart + 1] - m_chartOffsets[chart];
      if (total > UINT32_MAX) throw std::length_error("Too many retained direct-light receivers");
    }
    m_count = uint32_t(total);
    m_receivers.push_back({m_count, none});
  }

  std::vector<Task> DirectWork::batch(uint32_t first, uint32_t count) const {
    if (first > m_count || count > m_count - first)
      throw std::out_of_range("Direct-light batch exceeds retained work");
    std::vector<Task> tasks;
    if (!count) return tasks;
    tasks.reserve(count);
    const auto start = std::upper_bound(m_receivers.begin(), m_receivers.end(), first,
        [](uint32_t task, const Receiver &receiver) { return task < receiver.first; });
    auto point = size_t(start - m_receivers.begin() - 1);
    uint32_t skip = first - m_receivers[point].first;
    for (; point + 1 < m_receivers.size(); ++point) {
      const auto chart = m_receivers[point].chart;
      for (uint32_t i = m_chartOffsets[chart]; i < m_chartOffsets[chart + 1]; ++i) {
        const auto &owner = m_owners[i];
        if (skip) { --skip; continue; }
        tasks.push_back({uint32_t(point), owner.light, owner.output});
        if (tasks.size() == count) return tasks;
      }
    }
    throw std::logic_error("Incomplete direct-light prefix index");
  }
} // namespace remix::forward::mixed
