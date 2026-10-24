#pragma once
#include "forward/probes/coverage_selection.h"
#include "forward/probes/receiver_fits.h"

namespace remix::forward::probes {
  struct ReceiverCoveragePlan {
    float maximumError = .2f;
    uint32_t maximumChanges = 4;
    uint32_t surfaceSamples = 0, guardSamples = 0;
    std::vector<CoverageReplacement> replacements;
    std::vector<std::array<uint32_t, 2>> owners; // Local attempt and variant.
    std::vector<float> errors;
  };
  // Diagnostic automatic baseline, before authored composition. Do not apply
  // this plan to a published authored-plus-automatic set without replanning.
  ReceiverCoveragePlan planReceiverCoverage(const scene::BakeScene &source,
      std::span<const Capture> baseline, const ReceiverFits &fits, const ReceiverAudit &coverage,
      std::stop_token cancelled = {});
}
