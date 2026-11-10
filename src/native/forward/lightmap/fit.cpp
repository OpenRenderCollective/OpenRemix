#include "forward/lightmap/packing.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace remix::forward::lightmap {
  namespace {
    Layout fill(std::span<const ChartShape> shapes, Options &options, std::stop_token cancelled) {
      float failed = 0, selected = 1;
      Layout best;
      const auto trySpacing = [&](float spacing) {
        options.luxelMetres = spacing;
        try {
          best = packCharts(shapes, options, cancelled);
          selected = spacing;
          return true;
        } catch (const std::length_error &) {
          failed = spacing;
          return false;
        }
      };
      // Bracket capacity in both directions: small maps get finer detail,
      // large maps coarsen. Geometry analysis and sample generation are not
      // repeated by these inexpensive packing probes.
      float spacing = 1;
      for (uint32_t attempt = 0; attempt < 64; ++attempt) {
        if (trySpacing(spacing)) {
          if (failed > 0)
            break;
          spacing *= .5f;
        } else {
          if (best.pages)
            break;
          spacing *= 2;
        }
      }
      if (!best.pages || failed <= 0)
        throw std::length_error("Cannot bracket surface atlas density within coordinate limits");
      // Shelf ordering is not monotonic at every rounding boundary. Always
      // retain an actual successful packing, rather than estimating occupancy
      // from surface area or promising a globally optimal rectangle layout.
      for (uint32_t attempt = 0; attempt < 16 && selected > failed * 1.0001f; ++attempt)
        trySpacing(std::sqrt(failed * selected));
      options.luxelMetres = selected;
      return best;
    }
  }
  Layout fit(const scene::BakeScene &scene, Options options, std::stop_token cancelled) {
    if (options.fillBudget) {
      options.luxelMetres = 1;
      options.texelStep = 1;
    }
    const auto shapes = prepareCharts(scene, options, cancelled);
    if (options.fillBudget && !shapes.empty()) {
      auto best = fill(shapes, options, cancelled);
      return sampleCharts(scene, options, std::move(best), cancelled);
    }
    constexpr uint32_t maximumStep = 4096;
    uint32_t failed = 0, selected = options.texelStep;
    Layout best;
    // First bracket a successful layout. Invalid inputs and cancellation must
    // propagate; only an exhausted packing/coordinate budget permits coarsening.
    for (;;) {
      options.texelStep = selected;
      try {
        best = packCharts(shapes, options, cancelled);
        break;
      } catch (const std::length_error &) {
        if (selected == maximumStep)
          throw;
        failed = selected;
        selected = std::min(maximumStep, selected * 2);
      }
    }
    // Keep a successful candidate at every iteration. Rectangle ordering can
    // affect shelf utilization, so this is a bounded best-fit refinement rather
    // than a claim that every finer density has been exhaustively ruled out.
    while (failed && selected - failed > 1) {
      const uint32_t candidate = failed + (selected - failed) / 2;
      options.texelStep = candidate;
      try {
        auto packed = packCharts(shapes, options, cancelled);
        best = std::move(packed);
        selected = candidate;
      } catch (const std::length_error &) {
        failed = candidate;
      }
    }
    options.texelStep = selected;
    // A page size is a ceiling, not a reason to reserve mostly empty layers.
    // Try smaller pages at exactly the selected world density; retain a change
    // only when total allocated texels falls, including padding and layer limits.
    uint32_t minimumPage = 8;
    for (const auto &mapping : best.mappings)
      minimumPage = std::max(minimumPage,
                             std::bit_ceil(std::max(mapping.width, mapping.height) + 2 * options.padding));
    for (uint32_t size = best.pageSize / 2; size >= minimumPage; size /= 2) {
      auto compact = options;
      compact.pageSize = size;
      try {
        auto packed = packCharts(shapes, compact, cancelled);
        if (uint64_t(packed.pages) * size * size <
            uint64_t(best.pages) * best.pageSize * best.pageSize)
          best = std::move(packed);
      } catch (const std::length_error &) {
        // Smaller pages can exceed the array-layer limit or a chart extent.
      }
    }
    options.pageSize = best.pageSize;
    return sampleCharts(scene, options, std::move(best), cancelled);
  }
} // namespace remix::forward::lightmap
