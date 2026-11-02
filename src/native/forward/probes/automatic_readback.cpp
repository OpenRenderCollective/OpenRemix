#include "forward/probes/automatic_capture.h"
#include <algorithm>
#include <cmath>

namespace remix::forward::probes {
  bool AutomaticCapture::observe(Step step, uint32_t first, std::span<const bake::Result> results) {
    if (step == Step::CheckGeometry) {
      if (!projectionAudit || !projectionAudit->observe(first, results)) return false;
      geometryReceived += uint32_t(results.size());
      return true;
    }
    const bool sites = step == Step::ValidateSites;
    if (!sites && step != Step::Capture) return false;
    const size_t size = sites ? reflectionSites.size() : work.rays().size();
    const uint32_t submitted = sites ? siteNext : captureNext;
    const uint32_t received = sites ? siteReceived : captureReceived;
    if (first > size || results.size() > size - first ||
        first > submitted || results.size() > submitted - first ||
        received > submitted || results.size() > submitted - received) return false;
    // Validate before publishing any part of a batch. Match transport's finite
    // irradiance contract, including coefficients unused by radiance captures.
    for (const auto &value : results)
      for (const auto &coefficient : value.irradiance)
        for (float channel : coefficient)
          if (!std::isfinite(channel)) return false;
    for (uint32_t i = 0; i < results.size(); ++i) {
      const auto &value = results[i];
      if (sites) {
        usableReflectionSites[first + i] = value.statistics[2] == siteSamples &&
            std::isfinite(value.statistics[0]) && value.statistics[0] >= 0 && value.statistics[0] < .5f;
      } else {
        const auto [capture, texel] = work.destination(first + i);
        auto color = value.irradiance[0];
        for (uint32_t c = 0; c < 3; ++c) color[c] = std::clamp(color[c], 0.f, 65000.f);
        captures[capture].radiance[texel] = color;
      }
    }
    (sites ? siteReceived : captureReceived) += uint32_t(results.size());
    return true;
  }
}
