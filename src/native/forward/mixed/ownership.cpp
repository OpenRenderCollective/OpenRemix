#include "forward/mixed/ownership.h"
#include "forward/mixed/directional_links.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace remix::forward::mixed {
  OwnershipSummary summarizeOwnership(const Plan &plan, const lightmap::Layout &layout,
                                        std::stop_token cancelled) {
    if (cancelled.stop_requested()) throw std::runtime_error("Ownership report cancelled");
    if (!plan.lights.source || plan.charts.size() != layout.charts.size() ||
        plan.directCharts.size() != layout.charts.size() || plan.occlusion.chartCount() != layout.charts.size())
      throw std::invalid_argument("Ownership report requires a complete matching layout");
    const auto lights = plan.lights.source->lights.size();
    OwnershipSummary result;
    // Nine RGBA16F layers allocate four unit-direct responses at every texel.
    constexpr uint64_t pairBytes = lightmap::irradianceLayers * sizeof(uint16_t);
    result.allocatedDirectionalBytes =
        uint64_t(layout.pageSize) * layout.pageSize * layout.pages * channels * pairBytes;
    std::vector<std::vector<uint32_t>> positive(layout.charts.size());
    for (const auto &link : positiveDirectionalLinks(plan, cancelled))
      positive[link.chart].push_back(link.light);
    for (size_t chart = 0; chart < layout.charts.size(); ++chart) {
      if (cancelled.stop_requested()) throw std::runtime_error("Ownership report cancelled");
      const auto &allocation = layout.charts[chart];
      if (!allocation.width || !allocation.height || allocation.page >= layout.pages ||
          allocation.width > layout.pageSize || allocation.height > layout.pageSize ||
          allocation.x > layout.pageSize - allocation.width || allocation.y > layout.pageSize - allocation.height)
        throw std::invalid_argument("Invalid ownership chart rectangle");
      uint32_t retained = 0;
      for (const auto *owners : {&plan.charts[chart], &plan.directCharts[chart]})
        for (const auto light : *owners) {
          if (light == none) continue;
          if (light >= lights) throw std::invalid_argument("Invalid direct-light owner");
          retained += owners == &plan.directCharts[chart];
        }
      const auto &known = positive[chart];
      const uint32_t clear = uint32_t(std::count_if(known.begin(), known.end(), [&](uint32_t light) {
        return plan.occlusion.state(uint32_t(chart), light) == Visibility::Clear;
      }));
      const uint64_t area = uint64_t(allocation.width) * allocation.height;
      result.clearPairs += clear;
      result.clearRectangleBytes += area * clear * pairBytes;
      result.retainedPairs += retained;
      result.knownPositivePairs += known.size();
      result.retainedRectangleBytes += area * retained * pairBytes;
      result.knownPositiveRectangleBytes += area * known.size() * pairBytes;
      ++result.chartsByRetainedCount[retained];
      result.charts.push_back({allocation.width, allocation.height, retained, uint32_t(known.size()), clear});
    }
    return result;
  }
  void dumpOwnership(const Plan &plan, const lightmap::Layout &layout) noexcept {
    const char *path = std::getenv("REMIX_DEBUG_DIRECT_OWNERSHIP");
    if (!path || !*path) return;
    try {
      if (std::filesystem::exists(path))
        throw std::runtime_error("Ownership report exists; choose a fresh path");
      const auto summary = summarizeOwnership(plan, layout);
      std::ofstream output(path);
      output.exceptions(std::ios::failbit | std::ios::badbit);
      output << "{\"version\":1,\"sceneHash\":\"" << plan.lights.source->contentHash
             << "\",\"allocatedDirectionalBytes\":" << summary.allocatedDirectionalBytes
             << ",\"retainedRectangleBytes\":" << summary.retainedRectangleBytes
             << ",\"knownPositiveRectangleBytes\":" << summary.knownPositiveRectangleBytes
             << ",\"clearRectangleBytes\":" << summary.clearRectangleBytes
             << ",\"clearPairs\":" << summary.clearPairs
             << ",\"retainedPairs\":" << summary.retainedPairs
             << ",\"knownPositivePairs\":" << summary.knownPositivePairs
             << ",\"chartFields\":[\"width\",\"height\",\"retained\",\"knownPositive\",\"clear\"],\"charts\":[";
      for (size_t i = 0; i < summary.charts.size(); ++i) {
        const auto &chart = summary.charts[i];
        output << (i ? "," : "") << '[' << chart.width << ',' << chart.height << ',' << chart.retained
               << ',' << chart.knownPositive << ',' << chart.clear << ']';
      }
      output << "],\"limitations\":\"Rectangle sums include chart padding and exclude array packing padding, "
                "lookup tables and GPU allocation alignment. Known positive links exclude unknown or untraced coverage; "
                "this is not a complete unlimited-light requirement.\"}\n";
    } catch (const std::exception &error) {
      std::fprintf(stderr, "Direct ownership report unavailable: %s\n", error.what());
    }
  }
}
