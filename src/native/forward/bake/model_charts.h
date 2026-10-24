#pragma once
#include "scene/bake_scene.h"
#include "forward/lightmap/layout.h"

namespace remix::forward::bake {
  // Minimum chart footprints bound generation before fitting; packing may
  // still reject the result. A zero return leaves no model chart capacity.
  uint32_t modelChartCapacity(const lightmap::Options &options, size_t authoredCharts);

  // Candidate charts for the existing world-metric atlas fitter. Caller charts
  // remain untouched; generated ranges retain the original draw/index bindings.
  // The caller must append these to a derived snapshot and use fillBudget.
  // Explicit budget is 1..1048576; callers also enforce atlas capacity.
  // Exhausting it rejects the whole candidate, never partial coverage.
  std::vector<scene::BakeSurfaceChart> modelCharts(const scene::BakeScene &source,
      uint32_t maximumCharts, std::stop_token cancelled = {});
}
