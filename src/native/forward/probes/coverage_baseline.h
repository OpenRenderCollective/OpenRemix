#pragma once
#include "forward/probes/composition.h"

namespace remix::forward::probes {
  struct CoverageVolume {
    Record record;
    float blendDistance = 0; // Exact pre-publication capture setting; composed records carry inverse widths.
    Bounds influenceBounds() const {
      Bounds bounds;
      for (uint32_t axis = 0; axis < 3; ++axis) {
        bounds.minimum[axis] = record.influenceMinimum[axis];
        bounds.maximum[axis] = record.influenceMaximum[axis];
      }
      bounds.valid = true;
      return bounds;
    }
    Vec3 position() const { return {record.position[0], record.position[1], record.position[2]}; }
    LayerCopy source;
    bool protectedCapture = false;
  };
  struct CoverageBaselineSet {
    std::vector<CoverageVolume> volumes;
    std::array<uint32_t, 4> parameters{};
    bool composed = false, disabled = false, automaticAllowed = true;
    uint64_t authoredSceneId = 0, authoredRevision = 0;
  };
  // Metadata only: no radiance copies. Source indices identify image layers,
  // not canonical cache entries. Composed output order follows its copy plan.
  CoverageBaselineSet automaticCoverageBaseline(std::span<const Capture> captures);
  CoverageBaselineSet automaticCoverageBaseline(const Header &published);
  CoverageBaselineSet composedCoverageBaseline(const Composition &composition, uint64_t sceneId, uint64_t revision,
                                              bool automaticAllowed = true);
}
