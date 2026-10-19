#include "forward/mixed/directional_links.h"
#include "forward/mixed/data.h"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace remix::forward::mixed {
  std::vector<DirectionalLink> positiveDirectionalLinks(const Plan &plan, std::stop_token cancelled,
                                                       uint64_t byteBudget) {
    const auto check = [&] {
      if (cancelled.stop_requested()) throw std::runtime_error("Directional ownership cancelled");
    };
    check();
    if (!plan.lights.source || plan.charts.size() != plan.directCharts.size() ||
        plan.charts.size() != plan.occlusion.chartCount())
      throw std::invalid_argument("Directional ownership requires matching chart tables");
    const auto lights = plan.lights.source->lights.size();
    std::vector<DirectionalLink> links;
    const auto append = [&](uint32_t chart, uint32_t light) {
      if (chart >= plan.charts.size() || light >= lights)
        throw std::invalid_argument("Invalid directional owner");
      const uint64_t limit = byteBudget / sizeof(DirectionalLink);
      if (links.size() == limit)
        throw std::length_error("Directional ownership exceeds manifest budget");
      if (links.size() == links.capacity())
        links.reserve(size_t(std::min<uint64_t>(limit, std::max<uint64_t>(16, 2 * links.capacity()))));
      links.push_back({chart, light});
    };
    for (const auto &entry : plan.masks.entries()) {
      check();
      append(entry.chart, entry.light);
    }
    const auto words = plan.occlusion.words();
    const auto stride = plan.occlusion.wordsPerChart();
    for (uint32_t chart = 0; chart < plan.charts.size(); ++chart) {
      check();
      for (const auto *owners : {&plan.charts[chart], &plan.directCharts[chart]})
        for (const auto light : *owners)
          if (light != none) append(chart, light);
      for (uint32_t word = 0; word < stride; ++word) {
        if ((word & 63u) == 0) check();
        const auto bits = words[size_t(chart) * stride + word];
        // Clear is 10, blocked is 01, unknown is 00. Visit only clear pairs.
        auto clear = (bits >> 1) & ~bits & 0x55555555u;
        while (clear) {
          append(chart, word * 16 + std::countr_zero(clear) / 2);
          clear &= clear - 1;
        }
      }
    }
    std::sort(links.begin(), links.end(), [](const auto &a, const auto &b) {
      return a.chart < b.chart || (a.chart == b.chart && a.light < b.light);
    });
    links.erase(std::unique(links.begin(), links.end()), links.end());
    check();
    return links;
  }
}
