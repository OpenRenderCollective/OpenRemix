#include "forward/probes/receiver_fits.h"
#include "forward/probes/selection.h"
#include <set>

namespace remix::forward::probes {
  ReceiverFits fitReceiverSites(const scene::BakeScene &source,
      std::span<const ReceiverSample> receivers, const ReceiverSiteAudit &admission,
      const ReceiverSiteChecks &checks, uint32_t budget, std::stop_token cancelled) {
    auto check = [&] {
      if (cancelled.stop_requested()) throw std::runtime_error("Receiver projection fitting superseded");
    };
    check();
    if (!admission.complete() || !checks.complete() || !budget || budget > 32 || receivers.size() > 8192 ||
        !std::isfinite(source.sceneScale) || source.sceneScale <= 0)
      throw std::invalid_argument("Invalid receiver projection inputs or budget");
    ReceiverFits result;
    result.available = source.hasBounds && source.partition && source.partition->closed;
    Bounds limit;
    if (result.available) {
      for (uint32_t c = 0; c < 3; ++c)
        if (!std::isfinite(source.minimum[c]) || !std::isfinite(source.maximum[c]) || source.minimum[c] >= source.maximum[c])
          throw std::invalid_argument("Invalid receiver projection fitting bounds");
      limit.include(source.minimum); limit.include(source.maximum);
    }
    std::vector<RegionSite> sites;
    std::vector<uint32_t> owners;
    std::set<Vec3> seen;
    for (uint32_t i = 0; i < admission.sites().size(); ++i) {
      check();
      if (!checks.supported(i)) continue;
      ++result.supported;
      const auto &site = admission.sites()[i];
      if (site.receiver >= receivers.size()) throw std::invalid_argument("Invalid projection receiver owner");
      for (float v : receivers[site.receiver].position)
        if (!std::isfinite(v)) throw std::invalid_argument("Nonfinite projection receiver");
      if (!result.available) continue;
      const Vec3 position{site.point.position[0], site.point.position[1], site.point.position[2]};
      if (source.partition->cluster(position.data()) < 0) { ++result.partitionRefused; continue; }
      if (!seen.insert(position).second) { ++result.duplicates; continue; }
      sites.push_back({position, .1f / source.sceneScale, 0});
      owners.push_back(i);
    }
    if (!result.available) return result;
    for (const auto index : spreadSites(sites, budget, 4.f / source.sceneScale, cancelled)) {
      const uint32_t owner = owners[index];
      const auto &position = receivers[admission.sites()[owner].receiver].position;
      ReceiverSiteFit fit;
      fit.site = owner;
      for (const auto &box : fitRegionCandidates(*source.partition, limit, sites[index].position,
                                                .001f / source.sceneScale, cancelled)) {
        const auto influence = receiverRegion(*source.partition, box, .002f / source.sceneScale,
                                              cancelled, {}, .5f / source.sceneScale);
        bool covered = true;
        for (uint32_t c = 0; c < 3; ++c)
          covered &= position[c] >= influence.minimum[c] && position[c] <= influence.maximum[c];
        fit.variants.push_back({box, influence, covered});
      }
      result.attempts.push_back(std::move(fit));
    }
    return result;
  }
} // namespace remix::forward::probes
