#include "forward/probes/placement.h"
#include "forward/probes/regions.h"
#include "forward/probes/selection.h"
#include "forward/probes/receiver_visibility.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>
#include <cstdlib>
#include <fstream>
#include <filesystem>

namespace remix::forward::probes {
  namespace {
    // Every cell keeps at least one valid capture origin. Subdividing empty
    // cells and dropping them later leaves holes when the irradiance grid is
    // sparse relative to the requested reflection budget.
    std::vector<Bounds> fallbackBoxes(const Bounds &bounds, const PlacementSeeds &seeds, uint32_t count,
                                      std::stop_token cancelled) {
      struct Cell { Bounds bounds; std::vector<Vec3> sites; };
      if (cancelled.stop_requested()) throw std::runtime_error("Reflection placement superseded");
      std::vector<Cell> cells{{bounds, {}}};
      for (const auto &seed : seeds.points) {
        if ((seed.originalIndex & 4095u) == 0 && cancelled.stop_requested())
          throw std::runtime_error("Reflection placement superseded");
        const auto &point = seed.position;
        bool inside = true;
        for (uint32_t c = 0; c < 3; ++c)
          inside &= std::isfinite(point[c]) && point[c] >= bounds.minimum[c] && point[c] <= bounds.maximum[c];
        if (inside) cells.front().sites.push_back({point[0], point[1], point[2]});
      }
      if (cells.front().sites.empty()) return {};
      while (cells.size() < count) {
        if (cancelled.stop_requested()) throw std::runtime_error("Reflection placement superseded");
        size_t selected = 0;
        uint32_t axis = 0;
        float longest = 0, middle = 0;
        for (size_t i = 0; i < cells.size(); ++i) {
          Vec3 low = cells[i].sites.front(), high = low;
          for (const auto &site : cells[i].sites)
            for (uint32_t c = 0; c < 3; ++c) {
              low[c] = std::min(low[c], site[c]);
              high[c] = std::max(high[c], site[c]);
            }
          for (uint32_t c = 0; c < 3; ++c) {
            const float cut = std::midpoint(low[c], high[c]);
            const float length = cells[i].bounds.maximum[c] - cells[i].bounds.minimum[c];
            if (low[c] < cut && cut < high[c] && length > longest) {
              selected = i; axis = c; longest = length; middle = cut;
            }
          }
        }
        if (longest == 0) break;
        Cell second{cells[selected].bounds, {}};
        second.bounds.minimum[axis] = middle;
        cells[selected].bounds.maximum[axis] = middle;
        auto &sites = cells[selected].sites;
        const auto split = std::stable_partition(sites.begin(), sites.end(),
            [&](const Vec3 &site) { return site[axis] < middle; });
        second.sites.assign(split, sites.end());
        sites.erase(split, sites.end());
        cells.push_back(std::move(second));
      }
      std::vector<Bounds> result;
      for (const auto &cell : cells) result.push_back(cell.bounds);
      return result;
    }
  } // namespace
  std::vector<size_t> receiverSupportedSites(const scene::BakeScene &scene,
      std::span<const RegionSite> sites, std::span<const ReceiverSample> receivers,
      uint32_t budget, std::stop_token cancelled) {
    if (budget > 128 || receivers.size() > 8192 || !std::isfinite(scene.sceneScale) || scene.sceneScale <= 0)
      throw std::invalid_argument("Invalid receiver-supported fitting budget");
    auto check = [&] {
      if (cancelled.stop_requested()) throw std::runtime_error("Reflection site allocation superseded");
    };
    check();
    for (const auto &receiver : receivers)
      if (!std::isfinite(receiver.areaMetresSquared) || receiver.areaMetresSquared < 0)
        throw std::invalid_argument("Invalid reflection receiver area");
    for (const auto &site : sites)
      for (uint32_t axis = 0; axis < 3; ++axis)
        if (!std::isfinite(site.position[axis])) throw std::invalid_argument("Nonfinite reflection origin");
    std::vector<std::vector<uint32_t>> footprints(sites.size());
    if (scene.partition && scene.partition->closed && !receivers.empty()) {
      struct NearSite { double distance = std::numeric_limits<double>::infinity(); size_t index = 0; };
      constexpr size_t nearestCount = 4;
      for (size_t receiverIndex = 0; receiverIndex < receivers.size(); ++receiverIndex) {
        check();
        const auto &receiver = receivers[receiverIndex];
        double normalLength = std::hypot(receiver.normal[0], receiver.normal[1], receiver.normal[2]);
        if (!std::isfinite(normalLength) || normalLength <= 0 || receiver.areaMetresSquared == 0) continue;
        Vec3 point, normal;
        for (uint32_t axis = 0; axis < 3; ++axis) {
          if (!std::isfinite(receiver.position[axis])) throw std::invalid_argument("Nonfinite reflection receiver");
          normal[axis] = float(receiver.normal[axis] / normalLength);
          point[axis] = float(receiver.position[axis] + normal[axis] * (.002 / scene.sceneScale));
        }
        std::array<NearSite, nearestCount> nearest{};
        size_t nearestSize = 0;
        for (size_t siteIndex = 0; siteIndex < sites.size(); ++siteIndex) {
          if ((siteIndex & 255u) == 0) check();
          double distance = 0, facing = 0;
          for (uint32_t axis = 0; axis < 3; ++axis) {
            const double delta = sites[siteIndex].position[axis] - point[axis];
            distance += delta * delta;
            facing += delta * normal[axis];
          }
          if (facing <= 0 || distance <= double(.002 / scene.sceneScale) * (.002 / scene.sceneScale)) continue;
          size_t insert = 0;
          while (insert < nearestSize && nearest[insert].distance <= distance) ++insert;
          if (insert >= nearestCount) continue;
          const size_t end = std::min(nearestSize, nearestCount - 1);
          for (size_t i = end; i > insert; --i) nearest[i] = nearest[i - 1];
          nearest[insert] = {distance, siteIndex};
          nearestSize = std::min(nearestSize + 1, nearestCount);
        }
        const float bias = .002f / scene.sceneScale;
        for (size_t i = 0; i < nearestSize; ++i) {
          check();
          const size_t siteIndex = nearest[i].index;
          const auto ray = receiverRay(receiver, sites[siteIndex].position, bias);
          if (!ray) continue;
          if (scene.partition->firstSolid(ray->point.position.data(), sites[siteIndex].position.data()).state ==
              scene::VisibilityPartition::SegmentState::Clear)
            footprints[siteIndex].push_back(uint32_t(receiverIndex));
        }
      }
    }
    std::vector<bool> covered(receivers.size()), selected(sites.size());
    std::vector<size_t> result;
    while (result.size() < std::min(size_t(budget), sites.size())) {
      check();
      size_t best = sites.size();
      double bestGain = 0;
      for (size_t i = 0; i < sites.size(); ++i) {
        if (selected[i]) continue;
        double gain = 0;
        for (const auto receiver : footprints[i])
          if (!covered[receiver]) gain += receivers[receiver].areaMetresSquared;
        if (!(gain > 0)) continue;
        if (best == sites.size() || sites[i].enclosedAxes > sites[best].enclosedAxes ||
            (sites[i].enclosedAxes == sites[best].enclosedAxes && gain > bestGain * 1.00001)) {
          best = i;
          bestGain = gain;
        }
      }
      if (best == sites.size()) break;
      result.push_back(best);
      selected[best] = true;
      for (const auto receiver : footprints[best]) covered[receiver] = true;
    }
    return spreadSites(sites, budget, 4.f / scene.sceneScale, cancelled, result);
  }
  void configureInfluence(const scene::BakeScene &scene,
                          std::span<Capture> captures, float detailTolerance,
                          std::stop_token cancelled) {
    if (!std::isfinite(scene.sceneScale) || scene.sceneScale <= 0 ||
        !std::isfinite(detailTolerance) || detailTolerance < 0)
      throw std::invalid_argument(
          "Invalid reflection influence scale or tolerance");
    const float distance = .5f / scene.sceneScale;
    for (auto &capture : captures) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Reflection influence preparation superseded");
      capture.blendDistance = distance;
      if (scene.partition && scene.partition->closed &&
          capture.worldToLocal == worldSpace)
        capture.influence = receiverRegion(
            *scene.partition, capture.bounds, .002f / scene.sceneScale,
            cancelled, {detailTolerance / scene.sceneScale}, distance);
    }
  }
  std::vector<Vec3> candidateSeedSites(const scene::BakeScene &scene, const PlacementSeeds &seeds, uint32_t count,
                                   std::stop_token cancelled) {
    if (!count || !scene.partition || !scene.partition->closed)
      return {};
    const auto bounds = seeds.bounds;
    const float clearance = .05f / scene.sceneScale, range = 64.f / scene.sceneScale;
    std::vector<RegionSite> ranked;
    for (const auto &hint : scene.reflectionHints) {
      const auto site = regionSite(*scene.partition, hint, clearance, range, cancelled);
      if (site)
        ranked.push_back(*site);
    }
    const auto automatic =
        regionSites(*scene.partition, bounds, clearance, range, cancelled);
    ranked.insert(ranked.end(), automatic.begin(), automatic.end());
    // Sheltered rooms need local parallax more than open areas. Preserve authored
    // preference within each enclosure class, rather than consuming every slot
    // with the first outdoor env_cubemap entities in BSP order.
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
      return a.enclosedAxes > b.enclosedAxes;
    });
    std::vector<Vec3> result;
    result.reserve(ranked.size());
    for (const auto &site : ranked)
      result.push_back(site.position);
    return result;
  }
  std::vector<FittedRegion> fitSeedCandidates(const scene::BakeScene &scene, const PlacementSeeds &seeds,
                                          uint32_t count, std::stop_token cancelled, float detailTolerance,
                                          const std::vector<Vec3> *validatedSites) {
    if (count > maxProbes || !std::isfinite(detailTolerance) || detailTolerance < 0)
      throw std::invalid_argument("Too many reflection probes");
    if (!count)
      return {};
    const auto bounds = seeds.bounds;
    const bool fitted = scene.partition && scene.partition->closed;
    std::vector<Bounds> boxes{bounds};
    if (!fitted) boxes = fallbackBoxes(bounds, seeds, count, cancelled);
    while (fitted && boxes.size() < count) {
      size_t selected = 0;
      uint32_t axis = 0;
      float longest = 0;
      for (size_t i = 0; i < boxes.size(); ++i)
        for (uint32_t c = 0; c < 3; ++c) {
          const float length = boxes[i].maximum[c] - boxes[i].minimum[c];
          if (length > longest) {
            selected = i;
            axis = c;
            longest = length;
          }
        }
      auto second = boxes[selected];
      const float middle = std::midpoint(second.minimum[axis], second.maximum[axis]);
      boxes[selected].maximum[axis] = middle;
      second.minimum[axis] = middle;
      boxes.push_back(second);
    }
    std::vector<FittedRegion> regions, alternatives;
    auto covered = [&](Vec3 position) {
      return std::any_of(regions.begin(), regions.end(), [&](const auto &capture) {
        for (uint32_t c = 0; c < 3; ++c)
          if (position[c] < capture.bounds.minimum[c] || position[c] > capture.bounds.maximum[c])
            return false;
        return true;
      });
    };
    // Production passes GPU-validated sites. Direct CPU callers can exercise
    // geometry fitting independently, with the same bounded candidate policy.
    const auto generated =
        validatedSites ? std::vector<Vec3>{} : candidateSeedSites(scene, seeds, count, cancelled);
    const auto &sites = validatedSites ? *validatedSites : generated;
    // Keep expensive BSP fitting bounded even when a map uses more captures.
    const uint32_t budget = std::clamp(count * 8, 32u, 128u);
    // Larger resident sets do not expand the total geometry-audit cap. Up to
    // 128 fitted origins leave the same bounded room for grid fallbacks.
    const uint32_t auditLimit = budget + std::min(count, 32u);
    if (fitted) {
      std::vector<RegionSite> assessed;
      std::set<Vec3> seen;
      for (const auto &position : sites) {
        if (!seen.insert(position).second)
          continue;
        if (const auto site = regionSite(*scene.partition, position, .05f / scene.sceneScale,
                                          64.f / scene.sceneScale, cancelled))
          assessed.push_back(*site);
      }
      const auto receivers = sampleReceivers(scene, 8192, cancelled);
      const auto fitting = receiverSupportedSites(scene, assessed, receivers, budget, cancelled);
      // Temporary bounded origin-pool diagnostic for the initial allocator.
      if (const char *prefix = std::getenv("REMIX_DEBUG_PROBE_CANDIDATES"); prefix && *prefix) {
        const auto path = std::string(prefix) + ".origins.json";
        if (!std::filesystem::exists(path)) {
          std::ofstream output(path);
          output.precision(9);
          output << "[";
          for (size_t i = 0; i < assessed.size(); ++i) {
            const auto &site = assessed[i];
            output << (i ? "," : "") << "{\"position\":[" << site.position[0] << ',' << site.position[1]
                   << ',' << site.position[2] << "],\"cluster\":" << scene.partition->cluster(site.position.data())
                   << ",\"enclosedAxes\":" << site.enclosedAxes << ",\"clearance\":" << site.clearance
                   << ",\"fitted\":" << (std::find(fitting.begin(), fitting.end(), i) != fitting.end() ? "true" : "false") << '}';
          }
          output << "]";
        }
      }
      for (auto index : fitting) {
        const auto &site = assessed[index];
        const auto fits = fitRegionCandidates(*scene.partition, bounds, site.position, .001f / scene.sceneScale,
                                               cancelled, {detailTolerance / scene.sceneScale});
        if (!fits.empty()) regions.push_back({fits.front(), site});
        for (size_t fit = 1; fit < fits.size(); ++fit)
          alternatives.push_back({fits[fit], site});
      }
    }
    for (auto box : boxes) {
      if (regions.size() == auditLimit)
        break;
      Vec3 center;
      for (uint32_t c = 0; c < 3; ++c)
        center[c] = std::midpoint(box.minimum[c], box.maximum[c]);
      std::vector<std::pair<float, uint32_t>> candidates;
      for (uint32_t index = 0; index < seeds.points.size(); ++index) {
        const auto &p = seeds.points[index].position;
        bool inside = true;
        float distance = 0;
        for (uint32_t c = 0; c < 3; ++c) {
          inside &= p[c] >= box.minimum[c] && p[c] <= box.maximum[c];
          const float delta = p[c] - center[c];
          distance += delta * delta;
        }
        if (inside)
          candidates.emplace_back(distance, index);
      }
      std::sort(candidates.begin(), candidates.end(), [&](const auto &a, const auto &b) {
        if (a.first != b.first) return a.first < b.first;
        return seeds.points[a.second].originalIndex < seeds.points[b.second].originalIndex;
      });
      uint32_t selected = ~0u;
      for (size_t candidate = 0; candidate < std::min(size_t(16), candidates.size()); ++candidate) {
        if (cancelled.stop_requested())
          throw std::runtime_error("Reflection placement superseded");
        const auto index = candidates[candidate].second;
        const auto &p = seeds.points[index].position;
        if (fitted) {
          if (covered({p[0], p[1], p[2]}))
            continue;
          const auto region =
              fitRegion(*scene.partition, bounds, {p[0], p[1], p[2]}, .001f / scene.sceneScale, cancelled,
                        {detailTolerance / scene.sceneScale});
          if (!region)
            continue;
          box = *region;
        }
        selected = index;
        break;
      }
      if (selected == ~0u)
        continue;
      // Without occupancy data retain the coarse, overlapping fallback. Fitted
      // regions retain their wall constraints, including with detail tolerance.
      for (uint32_t c = 0; !fitted && c < 3; ++c) {
        const float padding = (box.maximum[c] - box.minimum[c]) * .1f;
        box.minimum[c] = std::max(bounds.minimum[c], box.minimum[c] - padding);
        box.maximum[c] = std::min(bounds.maximum[c], box.maximum[c] + padding);
      }
      const auto &p = seeds.points[selected].position;
      regions.push_back({box, {{p[0], p[1], p[2]}, 0, 0}});
    }
    // Shape alternatives reuse the first-hit queries at an existing origin.
    // Keep the origin/ray budget fixed, while retaining each bounded fit for
    // assessment instead of letting primary boxes consume all audit records.
    regions.insert(regions.end(), alternatives.begin(), alternatives.end());
    return regions;
  }
  std::vector<std::vector<uint32_t>> receiverFootprints(const scene::BakeScene &scene,
      std::span<const Capture> captures, std::span<const ReceiverSample> receivers,
      std::stop_token cancelled) {
    if (!scene.partition || !scene.partition->closed || receivers.size() > 8192 ||
        !std::isfinite(scene.sceneScale) || scene.sceneScale <= 0)
      throw std::invalid_argument("Invalid reflection receiver footprint inputs");
    std::vector<std::vector<uint32_t>> footprints(captures.size());
    for (size_t i = 0; i < captures.size(); ++i) {
      if (cancelled.stop_requested()) throw std::runtime_error("Reflection receiver coverage superseded");
      const auto &capture = captures[i];
      for (uint32_t r = 0; r < receivers.size(); ++r) {
        const auto &receiver = receivers[r];
        const auto point = projectionPoint(capture.worldToLocal, receiver.position);
        const auto &influence = capture.influence.valid ? capture.influence : capture.bounds;
        bool inside = true;
        for (uint32_t axis = 0; axis < 3; ++axis)
          inside &= point[axis] >= influence.minimum[axis] && point[axis] <= influence.maximum[axis];
        if (!inside) continue;
        const auto ray = receiverRay(receiver, capture.position, .002f / scene.sceneScale);
        if (!ray) continue;
        // A tolerant box can cross thin partitions. It must not claim the
        // surfaces behind them just because their coordinates lie inside.
        if (scene.partition->firstSolid(ray->point.position.data(), capture.position.data()).state ==
            scene::VisibilityPartition::SegmentState::Clear)
          footprints[i].push_back(r);
      }
    }
    return footprints;
  }
  std::vector<Capture> captureCandidates(const scene::BakeScene &scene, std::span<const FittedRegion> regions,
                                         uint32_t count, uint32_t resolution, std::stop_token cancelled,
                                         float detailTolerance) {
    if (count > maxProbes || !std::isfinite(detailTolerance) || detailTolerance < 0)
      throw std::invalid_argument("Invalid reflection capture budget");
    std::vector<Capture> candidates;
    candidates.reserve(regions.size());
    for (const auto &region : regions)
      candidates.push_back({region.bounds, region.site.position, resolution, {}, region.bounds});
    configureInfluence(scene, candidates, detailTolerance, cancelled);
    std::vector<size_t> selected;
    if (scene.partition && scene.partition->closed && !scene.charts.empty()) {
      const auto receivers = sampleReceivers(scene, 8192, cancelled);
      std::vector<double> areas;
      for (const auto &receiver : receivers) areas.push_back(receiver.areaMetresSquared);
      const auto footprints = receiverFootprints(scene, candidates, receivers, cancelled);
      selected = selectSupportedRegions(regions, footprints, areas, count, cancelled);
      // Temporary allocation evidence, bounded by the same sampled footprints.
      if (const char *prefix = std::getenv("REMIX_DEBUG_PROBE_CANDIDATES"); prefix && *prefix) {
        std::ofstream output(std::string(prefix) + ".footprints.json");
        output << "{\"footprints\":[";
        for (size_t i = 0; i < footprints.size(); ++i) {
          output << (i ? ",[" : "[");
          for (size_t r = 0; r < footprints[i].size(); ++r)
            output << (r ? "," : "") << footprints[i][r];
          output << ']';
        }
        output << "],\"selected\":[";
        for (size_t i = 0; i < selected.size(); ++i) output << (i ? "," : "") << selected[i];
        output << "]}";
      }
    } else selected = selectRegions(regions, count, cancelled);
    std::vector<Capture> captures;
    for (const auto index : selected) {
      auto capture = std::move(candidates[index]);
      capture.radiance.resize(rays(capture).size());
      captures.push_back(std::move(capture));
    }
    return captures;
  }
} // namespace remix::forward::probes
