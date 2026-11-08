#include "pathtrace/tlas_policy.h"
#include <algorithm>
#include <cmath>

namespace remix::trace {
  bool TlasPolicy::changed(const std::vector<InstanceInput> &inputs) const {
    return !initialized || inputs.size() != previous.size() ||
           !std::equal(inputs.begin(), inputs.end(), previous.begin());
  }
  uint32_t TlasPolicy::reason(const std::vector<InstanceInput> &inputs, bool dynamic, bool force) const {
    if (force)
      return REMIXAPI_REBUILD_FORCED_EXT;
    if (!initialized || inputs.size() != previous.size())
      return REMIXAPI_REBUILD_MEMBERSHIP_EXT;
    for (size_t i = 0; i < inputs.size(); ++i) {
      if (inputs[i].id != previous[i].id || inputs[i].sub != previous[i].sub)
        return REMIXAPI_REBUILD_MEMBERSHIP_EXT;
      // A facing change can change the instance flags: never refit those.
      if (inputs[i].geometry != previous[i].geometry || inputs[i].oneSided != previous[i].oneSided)
        return REMIXAPI_REBUILD_GEOMETRY_EXT;
    }
    if (!changed(inputs))
      return REMIXAPI_REBUILD_NONE_EXT;
    if (!dynamic)
      return REMIXAPI_REBUILD_STATIC_TRANSFORM_EXT;
    if (refits >= 300)
      return REMIXAPI_REBUILD_REFIT_LIMIT_EXT;
    for (size_t i = 0; i < inputs.size(); ++i) {
      float distance = std::hypot(inputs[i].model[12] - built[i].model[12], inputs[i].model[13] - built[i].model[13],
                                  inputs[i].model[14] - built[i].model[14]);
      if (distance > std::max(16.f, 4 * inputs[i].diagonal))
        return REMIXAPI_REBUILD_DISPLACEMENT_EXT;
    }
    return REMIXAPI_REBUILD_NONE_EXT;
  }
  void TlasPolicy::commit(const std::vector<InstanceInput> &inputs, uint32_t reason) {
    previous.assign(inputs.begin(), inputs.end());
    if (reason || !initialized) {
      built = previous;
      refits = 0;
    } else
      ++refits;
    initialized = true;
  }
  CombinedTlasPolicy::Decision CombinedTlasPolicy::decide(const std::vector<InstanceInput> &staticInputs,
                                                          uint64_t generation,
                                                          const std::vector<InstanceInput> &dynamicInputs,
                                                          bool force) const {
    Decision decision;
    const bool known = staticPart.initialized && generation && generation == staticGeneration;
    if (!known) {
      // Any static difference rebuilds, as the static TLAS does.
      decision.reason = staticPart.reason(staticInputs, false, false);
      decision.staticChanged = decision.reason != REMIXAPI_REBUILD_NONE_EXT;
    }
    const uint32_t dynamicReason = dynamicPart.reason(dynamicInputs, true, force);
    if (force || !decision.staticChanged)
      decision.reason = dynamicReason;
    if (decision.reason != REMIXAPI_REBUILD_NONE_EXT)
      decision.action = Build;
    else if (dynamicPart.changed(dynamicInputs))
      decision.action = Update;
    return decision;
  }
  void CombinedTlasPolicy::commit(const std::vector<InstanceInput> &staticInputs, uint64_t generation,
                                  const std::vector<InstanceInput> &dynamicInputs, const Decision &decision) {
    if (decision.staticChanged)
      staticPart.commit(staticInputs, decision.reason);
    staticGeneration = staticPart.initialized ? generation : 0;
    if (decision.action == Build)
      dynamicPart.commit(dynamicInputs, decision.reason);
    else if (decision.action == Update)
      dynamicPart.commit(dynamicInputs, REMIXAPI_REBUILD_NONE_EXT);
  }
} // namespace remix::trace
