#pragma once
#include "scene/bake_scene.h"

namespace remix::forward::bake {
  // Candidate charts for the existing world-metric atlas fitter. Caller charts
  // remain untouched; generated ranges retain the original draw/index bindings.
  // The caller must append these to a derived snapshot and use fillBudget.
  // Exhausting the chart cap rejects the whole candidate, never partial coverage.
  std::vector<scene::BakeSurfaceChart> modelCharts(const scene::BakeScene &source,
      uint32_t maximumCharts, std::stop_token cancelled = {});
}
