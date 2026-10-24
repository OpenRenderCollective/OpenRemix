#include "forward/probes/coverage_baseline.h"
#include <cmath>
#include <set>
#include <stdexcept>

namespace remix::forward::probes {
  CoverageBaselineSet automaticCoverageBaseline(std::span<const Capture> captures) {
    if (captures.size() > maxProbes) throw std::invalid_argument("Automatic coverage baseline exceeds capacity");
    CoverageBaselineSet result;
    for (uint32_t i = 0; i < captures.size(); ++i) {
      const auto &capture = captures[i];
      validateCapture(capture);
      result.volumes.push_back({capture.influence.valid ? capture.influence : capture.bounds,
                                capture.position, capture.worldToLocal, {true, i}, false});
    }
    return result;
  }
  CoverageBaselineSet composedCoverageBaseline(const Composition &composition) {
    const auto count = composition.header.parameters[0];
    if (count > maxProbes || composition.layers.size() != count)
      throw std::invalid_argument("Composed coverage baseline has inconsistent layers");
    CoverageBaselineSet result;
    result.composed = true;
    result.disabled = count == 0;
    std::set<std::pair<bool, uint32_t>> sources;
    for (uint32_t i = 0; i < count; ++i) {
      const auto &record = composition.header.probes[i];
      const auto &source = composition.layers[i];
      if (source.layer >= maxProbes || !sources.insert({source.automatic, source.layer}).second ||
          !std::isfinite(record.minimum[3]))
        throw std::invalid_argument("Invalid composed coverage source layer");
      // Public Capture priority validation excludes the internal -32769
      // fallback sentinel. Coverage ignores priority, so validate geometry with
      // a neutral priority instead of narrowing or clamping published metadata.
      Capture metadata;
      metadata.resolution = composition.header.parameters[1];
      metadata.worldToLocal = record.worldToLocal;
      for (uint32_t axis = 0; axis < 3; ++axis) {
        metadata.bounds.minimum[axis] = record.minimum[axis];
        metadata.bounds.maximum[axis] = record.maximum[axis];
        metadata.influence.minimum[axis] = record.influenceMinimum[axis];
        metadata.influence.maximum[axis] = record.influenceMaximum[axis];
        metadata.position[axis] = record.position[axis];
      }
      metadata.bounds.valid = metadata.influence.valid = true;
      validateCapture(metadata);
      result.volumes.push_back({metadata.influence, metadata.position, metadata.worldToLocal,
                                source, !source.automatic});
    }
    return result;
  }
}
