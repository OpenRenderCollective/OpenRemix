#pragma once
#include "forward/lightmap/layout.h"
#include <functional>
#include <span>

namespace remix::forward::lightmap {
  // Zero means this build has no OIDN support. Included in the bake cache key.
  uint32_t denoiserVersion();
  struct DenoiseStatistics {
    uint32_t charts = 0;
    uint64_t texels = 0;
    uint32_t executions = 0, preparations = 0, constantFields = 0, monitoredExecutions = 0;
    uint64_t peakHostBytes = 0, imageBufferBytes = 0, transferBytes = 0;
    int deviceType = 0; // OIDNDeviceType; no OIDN types escape this module
    double seconds = 0;
  };
  // Worker-only offline operation, after transport readback and before atlas encoding.
  // Filters indirect RGB SH within each chart. Signed bands are normalized by L0,
  // filtered as directional data, then reconstructed with the filtered L0.
  // Alpha, visibility, direct-light attribution, probe data and moments are untouched.
  // Exceptions abort publication. Cancellation is checked between fields; CPU
  // and large GPU images additionally use OIDN's internal cancellation callback.
  DenoiseStatistics denoise(const Layout &layout, std::span<bake::Result> results,
                            std::stop_token cancelled = {},
                            std::function<void(uint32_t completed, uint32_t total)> progress = {});
} // namespace remix::forward::lightmap
