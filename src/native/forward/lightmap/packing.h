#pragma once
#include "forward/lightmap/layout.h"

namespace remix::forward::lightmap {
  // Budget probes build only mappings and rectangle allocations. Surface
  // rasterization, seam coverage and sample storage happen once, after selection.
  Layout packCharts(const scene::BakeScene &scene, const Options &options, std::stop_token cancelled);
  Layout sampleCharts(const scene::BakeScene &scene, const Options &options, Layout packed,
                      std::stop_token cancelled);
} // namespace remix::forward::lightmap
