#pragma once
#include <cstdint>
#include <span>
#include <stop_token>
#include <vector>

namespace remix::forward::probes {
  inline constexpr uint32_t maximumCoverageSamples = 16384;
  struct CoverageBaseline {
    std::vector<uint32_t> receivers;
    bool protectedCapture = false;
  };
  struct CoverageCandidate {
    std::vector<uint32_t> receivers; // Only visibility-supported samples belong here.
    float error = 0;
  };
  struct CoverageReplacement {
    uint32_t candidate = 0, baseline = UINT32_MAX; // UINT32_MAX uses a free layer.
    double gainedArea = 0;
  };
  // Greedy sampled-coverage plan, not whole-map preservation or visual acceptance.
  // Weights are finite and nonnegative: zero-weight spatial guards constrain
  // preservation without rewarding empty volume as newly lit surface area.
  // Existing covered samples cannot be lost; protected baselines cannot be removed.
  // Equal gains prefer lower projection error, then original input order.
  std::vector<CoverageReplacement> planCoverageReplacements(std::span<const float> sampleWeights,
      std::span<const CoverageBaseline> baseline, std::span<const CoverageCandidate> candidates,
      float maximumError, uint32_t capacity = 64, uint32_t maximumChanges = 4,
      std::stop_token cancelled = {});
}
