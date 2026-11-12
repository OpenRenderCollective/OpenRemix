#pragma once
#include "forward/probes/data.h"
#include "forward/probes/selection.h"
#include "forward/probes/placement_seeds.h"
#include "forward/probes/receivers.h"

namespace remix::forward::probes {
  // Allocate fitting work to sites with nearby front-facing receiver support.
  // BSP-clear segments assign support; remaining budget uses the existing spread.
  std::vector<size_t> receiverSupportedSites(const scene::BakeScene &scene,
      std::span<const RegionSite> sites, std::span<const ReceiverSample> receivers,
      uint32_t budget, std::stop_token cancelled = {});
  // Surface samples inside the published influence with front-facing, clear
  // BSP segments. Used for allocation; static prop occlusion is not certified.
  std::vector<std::vector<uint32_t>> receiverFootprints(const scene::BakeScene &scene,
      std::span<const Capture> captures, std::span<const ReceiverSample> receivers,
      std::stop_token cancelled = {});
  PlacementSeeds placementSeeds(const scene::BakeScene &scene, const bake::Grid &grid,
                                 std::stop_token cancelled = {});
  std::vector<Vec3> candidateSeedSites(const scene::BakeScene &scene, const PlacementSeeds &seeds,
                                      uint32_t count, std::stop_token cancelled = {});
  std::vector<FittedRegion> fitSeedCandidates(const scene::BakeScene &scene, const PlacementSeeds &seeds,
      uint32_t count, std::stop_token cancelled = {}, float detailTolerance = .5f,
      const std::vector<Vec3> *validatedSites = nullptr);

// Receiver overlap/feather metadata is derived independently of baked radiance.
// Cache restores can upgrade it without retracing or filtering diffuse
// lighting.
void configureInfluence(const scene::BakeScene &scene,
                        std::span<Capture> captures, float detailTolerance,
                        std::stop_token cancelled = {});
// Geometry fitting and capture allocation are separate so hardware queries
// can assess the bounded candidate list before lighting is traced.
std::vector<FittedRegion>
fitCandidates(const scene::BakeScene &scene, const bake::Grid &grid,
              uint32_t count, std::stop_token cancelled = {},
              float detailTolerance = .5f,
              const std::vector<Vec3> *validatedSites = nullptr);
std::vector<Capture> captureCandidates(const scene::BakeScene &scene,
                                       std::span<const FittedRegion> regions,
                                       uint32_t count, uint32_t resolution,
                                       std::stop_token cancelled = {},
                                       float detailTolerance = .5f);
} // namespace remix::forward::probes
