#pragma once
#include "forward/bake/probe_visibility.h"
#include "forward/bake/sparse_sh.h"
#include "forward/mixed/directional_links.h"

namespace remix::forward::bake {
  enum class StyledDirectDecline { SurfaceCoverage, ProbeCoverage, Storage };
  struct DeclinedStyle {
    uint32_t style;
    StyledDirectDecline reason;
  };
  struct StyledDirectGroups {
    std::vector<uint32_t> styles, words;
    std::vector<DeclinedStyle> declined;
  };
  // Reuse completed unit-direct captures: no rays, Vulkan objects or dense
  // all-map RGB workspace. Owners must be the final positive chart/light
  // manifest. Probe entries must come from complete planner-selected volumes,
  // never truncated light volumes. Missing coverage declines the entire style.
  // The output is separate from permanent RGB and existing styled indirect SH.
  StyledDirectGroups groupStyledDirect(const scene::BakeScene &source,
      const lightmap::Layout &layout, std::span<const mixed::DirectionalLink> owners,
      std::span<const uint32_t> directionals, const ProbeVisibility &probes,
      uint64_t maximumBytes, std::stop_token cancelled = {});
}
