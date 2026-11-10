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
    for (const uint32_t chart : m_activeCharts) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Overflow visibility superseded");
      const auto &range = m_ranges[chart];
      if (range.light == none || !range.sampled)
        continue;
      if (range.maximum == 0 || range.minimum == 1) {
        if (!plan.overflow)
          throw std::logic_error("Overflow visibility count exhausted");
        plan.occlusion.mark(chart, range.light,
                            range.maximum == 0 ? Visibility::Blocked : Visibility::Clear);
        --plan.overflow;
      } else if (plan.masks.add(chart, range.light, layout, m_values, m_wordBudget, cancelled)) {
        if (!plan.overflow)
          throw std::logic_error("Overflow visibility count exhausted");
        --plan.overflow;
      }
    }
    m_tasks.clear();
    m_seen.clear();
    m_received = 0;
    if (!plan.overflow) {
      m_ranges.clear();
      m_activeCharts.clear();
      return false;
    }
    if (!m_initialized) {
      m_ranges.resize(plan.charts.size());
      m_chartPoints.resize(plan.charts.size());
      m_registeredLights.resize(plan.lights.source->lights.size());
      for (uint32_t chart = 0; chart < plan.charts.size(); ++chart)
        if (plan.next[chart] > plan.remaining[chart].size())
          throw std::invalid_argument("Invalid overflow visibility cursor");
      std::vector<uint8_t> receivers(plan.charts.size());
      std::vector<uint8_t> needed(layout.points.size());
      for (size_t address = 0; address < layout.owners.size(); ++address) {
        if ((address & 1023u) == 0 && cancelled.stop_requested())
          throw std::runtime_error("Overflow visibility superseded");
        if (const auto owner = layout.owners[address]; owner != lightmap::unowned)
          needed.at(owner) = 1;
      }
      for (size_t point = 0; point < layout.points.size(); ++point) {
        if ((point & 1023u) == 0 && cancelled.stop_requested())
          throw std::runtime_error("Overflow visibility superseded");
        const uint32_t chart = layout.pointCharts[point];
        if (chart >= m_chartPoints.size())
          throw std::invalid_argument("Invalid overflow surface chart");
        if (plan.next[chart] == plan.remaining[chart].size())
          continue;
        if (layout.receiverSamples.empty() || layout.receiverSamples[point]) {
          receivers[chart] = 1;
          needed[point] = 1;
        }
        // A filtering sample superseded by another atlas donor cannot affect
        // either receiver classification or the uploaded visibility mask.
        if (needed[point])
          m_chartPoints[chart].push_back(uint32_t(point));
      }
      for (uint32_t chart = 0; chart < plan.charts.size(); ++chart) {
        if (receivers[chart]) {
          m_activeCharts.push_back(chart);
          m_totalWork += uint64_t(m_chartPoints[chart].size()) *
                         (plan.remaining[chart].size() - plan.next[chart]);
        }
      }
      m_initialized = true;
    }
    std::erase_if(m_activeCharts, [&](uint32_t chart) {
      if (plan.next[chart] > plan.remaining[chart].size())
        throw std::invalid_argument("Invalid overflow visibility cursor");
      return plan.next[chart] == plan.remaining[chart].size();
    });
    if (m_activeCharts.empty()) return false;
    m_values.resize(layout.points.size(), -1.f);
    for (const uint32_t chart : m_activeCharts) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Overflow visibility superseded");
      const auto light = plan.remaining[chart][plan.next[chart]++];
      if (light >= plan.lights.source->lights.size())
        throw std::invalid_argument("Invalid overflow light");
      m_ranges[chart] = Range{light};
      if (!m_registeredLights[light]) {
        const auto id = plan.lights.source->lights[light].id;
        const auto [first, last] = plan.lights.identities.equal_range(id);
        if (std::none_of(first, last, [&](const auto &entry) { return entry.second == light; }))
          plan.lights.identities.emplace(id, light);
        m_registeredLights[light] = 1;
      }
      for (const auto point : m_chartPoints[chart]) {
        if ((m_tasks.size() & 1023u) == 0 && cancelled.stop_requested())
          throw std::runtime_error("Overflow visibility superseded");
        m_tasks.push_back({point, light, 0});
      }
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
    m_values[m_tasks[task].point] = visibility;
    // Donors filter the mask, but cannot prove a whole receiver clear/blocked.
    if (layout.receiverSamples.empty() || layout.receiverSamples[m_tasks[task].point]) {
      range.minimum = std::min(range.minimum, visibility);
      range.maximum = std::max(range.maximum, visibility);
      range.sampled = true;
    }
    m_seen[task] = 1;
    ++m_received;
    ++m_completedWork;
  }
}
