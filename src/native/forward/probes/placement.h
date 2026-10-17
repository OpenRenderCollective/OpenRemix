#pragma once
#include "forward/probes/data.h"
#include "forward/probes/selection.h"

namespace remix::forward::probes {
  // Geometry fitting and capture allocation are separate so hardware queries
  // can assess the bounded candidate list before lighting is traced.
  std::vector<FittedRegion> fitCandidates(const scene::BakeScene &scene, const bake::Grid &grid,
                                          uint32_t count, std::stop_token cancelled = {},
                                          float detailTolerance = .5f,
                                          const std::vector<Vec3> *validatedSites = nullptr);
  std::vector<Capture> captureCandidates(const scene::BakeScene &scene, std::span<const FittedRegion> regions,
                                         uint32_t count, uint32_t resolution, std::stop_token cancelled = {},
                                         float detailTolerance = .5f);
} // namespace remix::forward::probes
