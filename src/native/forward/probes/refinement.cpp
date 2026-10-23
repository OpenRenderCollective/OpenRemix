#include "forward/probes/refinement.h"
#include "forward/probes/evaluation.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace remix::forward::probes {
  std::optional<ProjectionRefinement> refineProjection(const FittedRegion &region, float margin,
                                                       RefinementPolicy policy) {
    constexpr uint32_t maximumSamples = 6 * ProjectionAudit::resolution * ProjectionAudit::resolution;
    if (!std::isfinite(margin) || margin <= 0 || !policy.minimumSamples || policy.minimumSamples > maximumSamples ||
        !std::isfinite(policy.minimumSampleFraction) || policy.minimumSampleFraction <= 0 ||
        policy.minimumSampleFraction > 1 || !std::isfinite(policy.maximumRelativeUncertainty) ||
        policy.maximumRelativeUncertainty <= 0 || policy.maximumRelativeUncertainty >= 1)
      throw std::invalid_argument("Invalid projection refinement policy");
    if (!region.bounds.valid) throw std::invalid_argument("Invalid refinement projection");
    for (uint32_t axis = 0; axis < 3; ++axis)
      if (!std::isfinite(region.bounds.minimum[axis]) || !std::isfinite(region.bounds.maximum[axis]) ||
          !std::isfinite(region.site.position[axis]) || region.site.position[axis] <= region.bounds.minimum[axis] ||
          region.site.position[axis] >= region.bounds.maximum[axis])
        throw std::invalid_argument("Projection refinement requires an interior origin");
    if (!region.geometryEvaluated) return {};
    ProjectionRefinement result{region.bounds};
    for (uint32_t face = 0; face < 6; ++face) {
      const auto &plane = region.faces[face].plane;
      if (!plane || plane->hits < policy.minimumSamples || plane->hits > maximumSamples || !std::isfinite(plane->sampleFraction) ||
          plane->sampleFraction < policy.minimumSampleFraction || plane->sampleFraction > 1 ||
          !std::isfinite(plane->lower) || !std::isfinite(plane->median) || !std::isfinite(plane->upper) ||
          plane->lower > plane->median || plane->median > plane->upper) continue;
      const uint32_t axis = face / 2;
      const double sign = face & 1u ? -1 : 1;
      const double distance = sign * (double(plane->median) - region.site.position[axis]);
      const double uncertainty = std::max(double(plane->median) - plane->lower,
                                          double(plane->upper) - plane->median);
      if (distance <= 2 * double(margin) || uncertainty > policy.maximumRelativeUncertainty * distance) continue;
      const float target = float(double(plane->median) - sign * margin);
      auto &coordinate = face & 1u ? result.bounds.minimum[axis] : result.bounds.maximum[axis];
      // A shift must exceed its observed uncertainty, and never grow through
      // walls or move the capture onto/outside the new projection surface.
      if (sign * (double(coordinate) - target) <= uncertainty + margin ||
          sign * (double(target) - region.site.position[axis]) <= margin) continue;
      coordinate = target;
      result.faces |= 1u << face;
    }
    return result.faces ? std::optional(result) : std::nullopt;
  }
}
