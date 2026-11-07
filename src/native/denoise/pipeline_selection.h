#pragma once
#include <string_view>

namespace remix::denoise {
  // Pinned NRD 4.18 dispatch contract. Runtime never uses split screen or
  // disables temporal stabilization. See the CPU dispatch test for all reachable
  // branches.
  inline bool pipelineRequired(std::string_view shader, bool sigma, bool local, bool pbr, bool validation,
                               bool localReconstruction) {
    if (shader.find("SplitScreen") != shader.npos || shader.find("TEMPORAL_STABILIZATION=0") != shader.npos)
      return false;
    if (shader.find("Validation") != shader.npos)
      return validation && !sigma && !local;
    if (shader.find("HitDistReconstruction") != shader.npos)
      return pbr || (local && localReconstruction);
    if (shader.find("PrePass") != shader.npos && local && !localReconstruction)
      return false;
    return true;
  }
} // namespace remix::denoise
