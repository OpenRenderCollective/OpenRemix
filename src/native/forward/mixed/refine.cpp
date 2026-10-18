#include "forward/mixed/data.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace remix::forward::mixed {
  bool refine(Plan &plan, const lightmap::Layout &layout, std::span<const Vec4> visibility,
              std::stop_token cancelled, std::span<const lightmap::Direct> scratch,
              std::span<lightmap::Direct> direct, DirectCapture capture) {
    if (cancelled.stop_requested())
      throw std::runtime_error("Mixed visibility refinement superseded");
    if (!plan.lights.source || visibility.size() != layout.points.size() ||
        layout.pointCharts.size() != visibility.size() ||
        (!layout.receiverSamples.empty() && layout.receiverSamples.size() != visibility.size()) ||
        plan.remaining.size() != plan.charts.size() ||
        plan.next.size() != plan.charts.size() || plan.directCharts.size() != plan.charts.size() ||
        scratch.size() != direct.size() || (!direct.empty() && direct.size() != visibility.size()) ||
        (capture == DirectCapture::Deferred && !direct.empty()))
      throw std::invalid_argument("Invalid mixed visibility round");
    std::vector<Vec4> maximum(plan.charts.size());
    std::vector<Vec4> minimum(plan.charts.size(), {1, 1, 1, 1});
    std::vector<bool> sampled(plan.charts.size());
    for (size_t point = 0; point < visibility.size(); ++point) {
      if ((point & 1023u) == 0 && cancelled.stop_requested())
        throw std::runtime_error("Mixed visibility refinement superseded");
      const auto chart = layout.pointCharts[point];
      if (chart >= plan.charts.size())
        throw std::invalid_argument("Invalid mixed visibility chart");
      // Filtering can borrow a real sample across a coplanar edge. That donor
      // shades the receiving chart but cannot prove visibility over its face.
      const bool receiver = layout.receiverSamples.empty() || layout.receiverSamples[point];
      sampled[chart] = sampled[chart] || receiver;
      for (uint32_t channel = 0; channel < channels; ++channel) {
        const float value = visibility[point][channel];
        if (!std::isfinite(value) || value < 0 || value > 1)
          throw std::invalid_argument("Invalid mixed visibility sample");
        if (!receiver) continue;
        maximum[chart][channel] = std::max(maximum[chart][channel], value);
        minimum[chart][channel] = std::min(minimum[chart][channel], value);
      }
    }
    std::vector<bake::UInt4> changed(plan.charts.size());
    std::vector<bake::UInt4> copies(plan.charts.size(), {none, none, none, none});
    plan.tasks.clear();
    plan.overflow = 0;
    for (uint32_t chart = 0; chart < plan.charts.size(); ++chart) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Mixed visibility refinement superseded");
      for (uint32_t channel = 0; channel < channels; ++channel) {
        auto &light = plan.charts[chart][channel];
        if (!sampled[chart] || light == none)
          continue;
        // Keep the first positive lights in importance order. Their unit direct
        // SH survives even when constant visibility frees this working channel.
        auto &owners = plan.directCharts[chart];
        if ((capture == DirectCapture::Deferred || !direct.empty()) && maximum[chart][channel] > 0 &&
            std::find(owners.begin(), owners.end(), light) == owners.end()) {
          const auto free = std::find(owners.begin(), owners.end(), none);
          if (free != owners.end()) {
            copies[chart][channel] = uint32_t(free - owners.begin());
            *free = light;
          }
        }
        if (maximum[chart][channel] > 0 && minimum[chart][channel] < 1)
          continue;
        plan.occlusion.mark(chart, light,
                            maximum[chart][channel] == 0 ? Visibility::Blocked : Visibility::Clear);
        light = none;
        if (plan.next[chart] < plan.remaining[chart].size()) {
          light = plan.remaining[chart][plan.next[chart]++];
          changed[chart][channel] = 1;
          const auto id = plan.lights.source->lights[light].id;
          const auto [first, last] = plan.lights.identities.equal_range(id);
          if (std::none_of(first, last, [&](const auto &entry) { return entry.second == light; }))
            plan.lights.identities.emplace(id, light);
        }
      }
      plan.overflow += uint32_t(plan.remaining[chart].size() - plan.next[chart]);
    }
    for (uint32_t point = 0; point < layout.points.size(); ++point) {
      if ((point & 1023u) == 0 && cancelled.stop_requested())
        throw std::runtime_error("Mixed visibility refinement superseded");
      const auto chart = layout.pointCharts[point];
      for (uint32_t channel = 0; channel < channels; ++channel) {
        if (const auto destination = copies[chart][channel]; !direct.empty() && destination != none)
          for (uint32_t coefficient = 0; coefficient < lightmap::irradianceLayers; ++coefficient)
            direct[point][coefficient][destination] = scratch[point][coefficient][channel];
        if (changed[chart][channel])
          plan.tasks.push_back({point, plan.charts[chart][channel], channel});
      }
    }
    return !plan.tasks.empty();
  }
} // namespace remix::forward::mixed
