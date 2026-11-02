#include "forward/probes/automatic_capture.h"
#include "forward/probes/placement.h"
#include <algorithm>
#include <stdexcept>

namespace remix::forward::probes {
  void AutomaticCapture::planSites(const scene::BakeScene &source, PlacementSeeds input,
                                    uint32_t count, std::stop_token cancelled) {
    requirePreparing();
    seeds = std::move(input);
    sitesPlanned = true;
    const auto sites = candidateSeedSites(source, seeds, count, cancelled);
    reflectionSites.reserve(sites.size());
    for (const auto &position : sites)
      reflectionSites.push_back({{position[0], position[1], position[2], 0}, {}});
    usableReflectionSites.resize(sites.size());
  }
  void AutomaticCapture::fit(const scene::BakeScene &source, uint32_t count,
                              float detailTolerance, std::stop_token cancelled) {
    requirePreparing();
    candidatesPlanned = true;
    if (validateSeeds) {
      std::erase_if(seeds.points, [&](const PlacementSeeds::Point &seed) {
        return !usableReflectionSites[seedSiteOffset + seed.originalIndex];
      });
    }
    std::vector<Vec3> validated;
    for (size_t i = 0; i < reflectionSites.size(); ++i)
      if (usableReflectionSites[i]) {
        const auto &position = reflectionSites[i].position;
        validated.push_back({position[0], position[1], position[2]});
      }
    auto regions = fitSeedCandidates(source, seeds, count, cancelled, detailTolerance, &validated);
    if (source.partition && source.partition->closed && !regions.empty())
      projectionAudit = std::make_unique<ProjectionAudit>(std::move(regions), cancelled);
    else
      candidateRegions = std::move(regions);
  }
  void AutomaticCapture::planSites(const scene::BakeScene &source, uint32_t count,
                                    std::stop_token cancelled) {
    requirePreparing();
    if (count > maxProbes) throw std::invalid_argument("Too many standalone reflection probes");
    auto input = count ? geometricSeedCandidates(source, std::clamp(count * 16, 64u, 4096u), cancelled)
                       : PlacementSeeds{};
    planSites(source, std::move(input), count, cancelled);
    validateSeeds = true;
    seedSiteOffset = uint32_t(reflectionSites.size());
    for (const auto &seed : seeds.points)
      reflectionSites.push_back({{seed.position[0], seed.position[1], seed.position[2], 0}, {}});
    usableReflectionSites.resize(reflectionSites.size());
  }
}
