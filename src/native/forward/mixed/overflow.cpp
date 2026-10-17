#include "forward/mixed/overflow.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace remix::forward::mixed {
  bool OverflowAudit::nextRound(Plan &plan, const lightmap::Layout &layout, std::stop_token cancelled) {
    if (cancelled.stop_requested())
      throw std::runtime_error("Overflow visibility superseded");
    if (!plan.lights.source || plan.remaining.size() != plan.charts.size() ||
        plan.next.size() != plan.charts.size() || layout.points.size() != layout.pointCharts.size() ||
        (!layout.receiverSamples.empty() && layout.receiverSamples.size() != layout.points.size()) ||
        m_received != m_tasks.size())
      throw std::invalid_argument("Incomplete or invalid overflow visibility round");
    for (uint32_t chart = 0; chart < m_ranges.size(); ++chart) {
      const auto &range = m_ranges[chart];
      if (range.light == none || !range.sampled)
        continue;
      if (range.maximum == 0 || range.minimum == 1) {
        if (!plan.overflow)
          throw std::logic_error("Overflow visibility count exhausted");
        plan.occlusion.mark(chart, range.light,
                            range.maximum == 0 ? Visibility::Blocked : Visibility::Clear);
        --plan.overflow;
      }
    }
    m_tasks.clear();
    m_seen.clear();
    m_received = 0;
    if (!plan.overflow) {
      m_ranges.clear();
      return false;
    }
    m_ranges.assign(plan.charts.size(), {});
    bool scheduled = false;
    for (uint32_t chart = 0; chart < plan.charts.size(); ++chart) {
      if (plan.next[chart] > plan.remaining[chart].size())
        throw std::invalid_argument("Invalid overflow visibility cursor");
      if (plan.next[chart] == plan.remaining[chart].size())
        continue;
      const auto light = plan.remaining[chart][plan.next[chart]++];
      if (light >= plan.lights.source->lights.size())
        throw std::invalid_argument("Invalid overflow light");
      m_ranges[chart].light = light;
      scheduled = true;
      const auto id = plan.lights.source->lights[light].id;
      const auto [first, last] = plan.lights.identities.equal_range(id);
      if (std::none_of(first, last, [&](const auto &entry) { return entry.second == light; }))
        plan.lights.identities.emplace(id, light);
    }
    if (!scheduled) {
      m_ranges.clear();
      return false;
    }
    for (uint32_t point = 0; point < layout.points.size(); ++point) {
      if ((point & 1023u) == 0 && cancelled.stop_requested())
        throw std::runtime_error("Overflow visibility superseded");
      const auto chart = layout.pointCharts[point];
      if (chart >= m_ranges.size())
        throw std::invalid_argument("Invalid overflow surface chart");
      // Donor samples contribute to filtering, not to chart-wide proofs.
      if (!layout.receiverSamples.empty() && !layout.receiverSamples[point])
        continue;
      if (const auto light = m_ranges[chart].light; light != none)
        m_tasks.push_back({point, light, 0});
    }
    m_seen.assign(m_tasks.size(), 0);
    return !m_tasks.empty();
  }

  void OverflowAudit::observe(uint32_t task, float visibility, const lightmap::Layout &layout) {
    if (task >= m_tasks.size() || m_seen[task] || !std::isfinite(visibility) ||
        visibility < 0 || visibility > 1)
      throw std::invalid_argument("Invalid overflow visibility result");
    const auto chart = layout.pointCharts.at(m_tasks[task].point);
    auto &range = m_ranges.at(chart);
    range.minimum = std::min(range.minimum, visibility);
    range.maximum = std::max(range.maximum, visibility);
    range.sampled = true;
    m_seen[task] = 1;
    ++m_received;
  }
}
