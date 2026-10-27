#include "forward/probes/receiver_fits.h"
#include "forward/probes/selection.h"
#include "forward/probes/refinement.h"
#include <limits>
#include <set>
#include <algorithm>

namespace remix::forward::probes {
  ReceiverFitAudit::ReceiverFitAudit(ReceiverFits fits, std::span<const ReceiverSite> sites,
                                   std::stop_token cancelled) : m_fits(std::move(fits)) {
    if (m_fits.attempts.size() > 32 || sites.size() > 512)
      throw std::invalid_argument("Local projection assessment budget exceeded");
    std::vector<FittedRegion> regions;
    for (const auto &fit : m_fits.attempts) {
      if (fit.site >= sites.size() || fit.variants.size() > ReceiverSiteFit::maximumVariants)
        throw std::invalid_argument("Invalid local projection assessment owner");
      const auto &point = sites[fit.site].point.position;
      for (const auto &variant : fit.variants) {
        FittedRegion region;
        region.bounds = variant.projection;
        region.site.position = {point[0], point[1], point[2]};
        regions.push_back(region);
      }
    }
    m_audit = std::make_unique<ProjectionAudit>(std::move(regions), cancelled, true);
  }
  ReceiverFits ReceiverFitAudit::finish() const {
    auto result = m_fits;
    const auto geometry = m_audit->finish();
    size_t index = 0;
    for (auto &fit : result.attempts)
      for (auto &variant : fit.variants) variant.geometry = geometry.at(index++);
    return result;
  }
  ReceiverFits ReceiverFitAudit::finish(const scene::BakeScene &source, std::stop_token cancelled) const {
    if (cancelled.stop_requested()) throw std::runtime_error("Receiver refinement superseded");
    auto result = finish();
    if (!source.partition || !source.partition->closed) return result;
    if (!std::isfinite(source.sceneScale) || source.sceneScale <= 0)
      throw std::invalid_argument("Invalid refined projection scene scale");
    RegionQuery occupancy(*source.partition, cancelled);
    RefinementPolicy policy;
    policy.maximumAbsoluteUncertainty = result.detailToleranceMetres / source.sceneScale;
    auto residual = [](const FittedRegion &region) {
      double total = 0, weight = 0;
      for (const auto &face : region.faces) {
        const double w = double(face.solidAngleFraction) * (1 - face.missFraction);
        total += w * face.meanAbsoluteRelativeResidual;
        weight += w;
      }
      return weight > 0 ? total / weight : std::numeric_limits<double>::infinity();
    };
    constexpr double minimumRelativeImprovement = .001;
    for (auto &fit : result.attempts) {
      if (cancelled.stop_requested()) throw std::runtime_error("Receiver refinement superseded");
      if (fit.variants.size() == ReceiverSiteFit::maximumVariants) continue;
      std::vector<ReceiverProjection> improvements;
      for (const auto &original : fit.variants) {
        const auto proposal = refineProjection(*original.geometry, .001f / source.sceneScale, policy);
        if (!proposal) continue;
        const RegionTolerance tolerance{original.detailTolerant ? policy.maximumAbsoluteUncertainty : 0};
        if (!occupancy.contains(proposal->bounds, tolerance)) continue;
        auto duplicate = [&](const ReceiverProjection &other) {
          return other.projection.minimum == proposal->bounds.minimum &&
                 other.projection.maximum == proposal->bounds.maximum &&
                 other.influence.minimum == original.influence.minimum &&
                 other.influence.maximum == original.influence.maximum;
        };
        if (std::any_of(fit.variants.begin(), fit.variants.end(), duplicate) ||
            std::any_of(improvements.begin(), improvements.end(), duplicate)) continue;
        auto geometry = *original.geometry;
        geometry.bounds = proposal->bounds;
        auto assessed = m_audit->reassess({geometry});
        if (!(residual(assessed.front()) < residual(*original.geometry) * (1 - minimumRelativeImprovement))) continue;
        auto improved = original; // Influence and receiver ownership stay unchanged.
        improved.projection = proposal->bounds;
        improved.geometry = std::move(assessed.front());
        improvements.push_back(std::move(improved));
      }
      std::stable_sort(improvements.begin(), improvements.end(), [&](const auto &a, const auto &b) {
        return residual(*a.geometry) < residual(*b.geometry);
      });
      for (auto &improved : improvements) {
        if (fit.variants.size() == ReceiverSiteFit::maximumVariants) break;
        fit.variants.push_back(std::move(improved));
      }
    }
    return result;
  }
  ReceiverFits fitReceiverSites(const scene::BakeScene &source,
      std::span<const ReceiverSample> receivers, const ReceiverSiteAudit &admission,
      const ReceiverSiteChecks &checks, uint32_t budget, std::stop_token cancelled, float detailToleranceMetres) {
    auto check = [&] {
      if (cancelled.stop_requested()) throw std::runtime_error("Receiver projection fitting superseded");
    };
    check();
    if (!admission.complete() || !checks.complete() || !budget || budget > 32 || receivers.size() > 8192 ||
        !std::isfinite(source.sceneScale) || source.sceneScale <= 0 ||
        !std::isfinite(detailToleranceMetres) || detailToleranceMetres < 0)
      throw std::invalid_argument("Invalid receiver projection inputs or budget");
    ReceiverFits result;
    result.detailToleranceMetres = detailToleranceMetres;
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
      const uint32_t policies = detailToleranceMetres > 0 ? 2 : 1;
      for (uint32_t policy = 0; policy < policies; ++policy) {
        const RegionTolerance tolerance{policy ? detailToleranceMetres / source.sceneScale : 0};
        // Keep the fit tied to the receiver that requested it. A tall empty
        // box above a courtyard is not a useful replacement for its ground.
        const auto &normal = receivers[admission.sites()[owner].receiver].normal;
        const double length = std::hypot(normal[0], normal[1], normal[2]);
        std::optional<Vec3> anchor;
        if (std::isfinite(length) && length > 0) {
          anchor = position;
          for (uint32_t axis = 0; axis < 3; ++axis)
            (*anchor)[axis] += float(normal[axis] / length * (.002 / source.sceneScale));
        }
        auto influenceFor = [&](const Bounds &box) {
          return receiverRegion(*source.partition, box, .002f / source.sceneScale,
              cancelled, tolerance, .5f / source.sceneScale);
        };
        auto coversReceiver = [&](const Bounds &influence) {
          for (uint32_t axis = 0; axis < 3; ++axis)
            if (position[axis] < influence.minimum[axis] || position[axis] > influence.maximum[axis]) return false;
          return true;
        };
        auto boxes = fitRegionCandidates(*source.partition, limit, sites[index].position,
            .001f / source.sceneScale, cancelled, tolerance);
        const bool covered = std::any_of(boxes.begin(), boxes.end(),
            [&](const Bounds &box) { return coversReceiver(influenceFor(box)); });
        if (!covered && anchor) {
          auto anchored = fitRegionCandidates(*source.partition, limit, sites[index].position,
              .001f / source.sceneScale, cancelled, tolerance, anchor);
          // An occupancy disagreement cannot authorize growth through a wall.
          // Keep the old family unless the new one really reaches its receiver.
          if (std::any_of(anchored.begin(), anchored.end(),
                [&](const Bounds &box) { return coversReceiver(influenceFor(box)); }))
            boxes = std::move(anchored);
        }
        for (const auto &box : boxes) {
          const auto influence = influenceFor(box);
          const bool duplicate = std::any_of(fit.variants.begin(), fit.variants.end(), [&](const auto &v) {
            return v.projection.minimum == box.minimum && v.projection.maximum == box.maximum &&
                   v.influence.minimum == influence.minimum && v.influence.maximum == influence.maximum;
          });
          if (duplicate) continue;
          ReceiverProjection variant{box, influence, coversReceiver(influence)};
          variant.detailTolerant = policy != 0;
          fit.variants.push_back(std::move(variant));
        }
      }
      result.attempts.push_back(std::move(fit));
    }
    return result;
  }
} // namespace remix::forward::probes
