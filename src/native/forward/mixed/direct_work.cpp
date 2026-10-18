#include "forward/mixed/direct_work.h"
#include <algorithm>
#include <stdexcept>

namespace remix::forward::mixed {
  DirectWork::DirectWork(const Plan &plan, const lightmap::Layout &layout, std::stop_token cancelled) {
    if (cancelled.stop_requested()) throw std::runtime_error("Direct lighting superseded");
    if (!plan.lights.source || plan.directCharts.size() != plan.charts.size() ||
        layout.points.size() != layout.pointCharts.size() || layout.points.size() > UINT32_MAX)
      throw std::invalid_argument("Invalid retained direct-light plan");
    m_owners = plan.directCharts;
    std::vector<uint32_t> counts(m_owners.size());
    for (size_t chart = 0; chart < m_owners.size(); ++chart)
      for (auto light : m_owners[chart]) {
        if (light == none) continue;
        if (light >= plan.lights.source->lights.size())
          throw std::invalid_argument("Invalid retained direct-light owner");
        ++counts[chart];
      }
    m_receivers.reserve(layout.points.size() + 1);
    uint64_t total = 0;
    for (size_t point = 0; point < layout.points.size(); ++point) {
      if ((point & 1023u) == 0 && cancelled.stop_requested())
        throw std::runtime_error("Direct lighting superseded");
      const auto chart = layout.pointCharts[point];
      if (chart >= counts.size()) throw std::invalid_argument("Invalid direct-light receiver chart");
      m_receivers.push_back({uint32_t(total), chart});
      total += counts[chart];
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
      for (uint32_t channel = 0; channel < channels; ++channel) {
        const auto light = m_owners[m_receivers[point].chart][channel];
        if (light == none) continue;
        if (skip) { --skip; continue; }
        tasks.push_back({uint32_t(point), light, channel});
        if (tasks.size() == count) return tasks;
      }
    }
    throw std::logic_error("Incomplete direct-light prefix index");
  }
} // namespace remix::forward::mixed
