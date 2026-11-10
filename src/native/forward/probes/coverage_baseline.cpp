#include "forward/probes/coverage_baseline.h"
#include <cmath>
#include <bit>
#include <set>
#include <stdexcept>

namespace remix::forward::probes {
  CoverageBaselineSet automaticCoverageBaseline(std::span<const Capture> captures) {
    if (captures.size() > maxProbes) throw std::invalid_argument("Automatic coverage baseline exceeds capacity");
    CoverageBaselineSet result;
    for (uint32_t i = 0; i < captures.size(); ++i) {
      const auto &capture = captures[i];
      validateCapture(capture);
      if (i && capture.resolution != captures[0].resolution)
        throw std::invalid_argument("Automatic coverage baseline has mixed image dimensions");
      CoverageVolume volume;
      const auto &influence = capture.influence.valid ? capture.influence : capture.bounds;
      for (uint32_t axis = 0; axis < 3; ++axis) {
        volume.record.minimum[axis] = capture.bounds.minimum[axis];
        volume.record.maximum[axis] = capture.bounds.maximum[axis];
        volume.record.influenceMinimum[axis] = influence.minimum[axis];
        volume.record.influenceMaximum[axis] = influence.maximum[axis];
        volume.record.position[axis] = capture.position[axis];
      }
      volume.record.minimum[3] = float(capture.priority);
      volume.record.worldToLocal = capture.worldToLocal;
      volume.blendDistance = capture.blendDistance;
      volume.source = {true, i};
      result.volumes.push_back(volume);
    }
    if (!captures.empty()) {
      const auto resolution = captures[0].resolution;
      const uint32_t levels = uint32_t(std::bit_width(resolution));
      result.parameters = {uint32_t(captures.size()), resolution, levels, levelOffset(resolution, levels)};
    }
    return result;
  }
  namespace {
    CoverageBaselineSet copyBaseline(const Composition &composition) {
      const auto count = composition.header.parameters[0];
      if (count > maxProbes || composition.layers.size() != count)
        throw std::invalid_argument("Composed coverage baseline has inconsistent layers");
      CoverageBaselineSet result;
      result.parameters = composition.header.parameters;
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
        CoverageVolume volume;
        volume.record = record;
        volume.source = source;
        volume.protectedCapture = !source.automatic;
        result.volumes.push_back(volume);
      }
      return result;
    }
  }
  CoverageBaselineSet automaticCoverageBaseline(const Header &published) {
    if (published.parameters[0] > maxProbes) throw std::invalid_argument("Published coverage count exceeds capacity");
    Composition copy;
    copy.header = published;
    for (uint32_t i = 0; i < published.parameters[0]; ++i) copy.layers.push_back({true, i});
    return copyBaseline(copy);
  }
  CoverageBaselineSet composedCoverageBaseline(const Composition &composition, uint64_t sceneId, uint64_t revision,
                                              bool automaticAllowed) {
    if (!sceneId || !revision) throw std::invalid_argument("Missing authored coverage identity");
    auto result = copyBaseline(composition);
    result.composed = true;
    result.authoredSceneId = sceneId;
    result.authoredRevision = revision;
    result.disabled = result.volumes.empty();
    result.automaticAllowed = automaticAllowed;
    return result;
  }
}
