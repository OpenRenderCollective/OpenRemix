#include "forward/lightmap/packing.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace remix::forward::lightmap {
  Layout fit(const scene::BakeScene &scene, Options options, std::stop_token cancelled) {
    const auto shapes = prepareCharts(scene, options, cancelled);
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
    return sampleCharts(scene, options, std::move(best), cancelled);
  }
} // namespace remix::forward::lightmap
