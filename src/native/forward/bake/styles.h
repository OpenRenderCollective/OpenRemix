#pragma once
#include "scene/bake_scene.h"
#include <span>

namespace remix::forward::bake {
  enum class StyleStatus { Ready, Missing, Ambiguous, GeometryChanged, NonScalar, Invalid };
  struct StyleScale {
    uint32_t style = 0;
    float value = 0; // current radiance / captured authored-envelope radiance
    StyleStatus status = StyleStatus::Missing;
    bool operator==(const StyleScale &) const = default;
  };
  std::vector<uint32_t> styleIds(const scene::BakeScene &captured);
  // An attributed RGB component can follow one scalar only if every member of
  // its style group agrees. Missing/changed/non-scalar groups return zero with
  // an explicit reason, preventing stale baked colour or glow. Other groups
  // remain usable. Supply the complete live light set, before view/range culling.
  // Results are ordered by style ID; inputs are read-only.
  std::vector<StyleScale> styleScales(const scene::BakeScene &captured,
                                    std::span<const scene::SphereLight> current);
} // namespace remix::forward::bake
