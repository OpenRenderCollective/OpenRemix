#include "forward/probes/evaluation.h"
#include "forward/probes/refinement.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

using namespace remix::forward;
namespace p = remix::forward::probes;
namespace {
  void require(bool value, const char *message) {
    if (!value) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
  Bounds box(Vec3 lo, Vec3 hi) {
    Bounds b;
    b.include(lo);
    b.include(hi);
    return b;
  }
  // Independent analytic empty-room union. Merge ray/box intervals to find
  // the first solid wall, including passage through the low doorway.
  double wallDistance(const bake::Point &ray, std::span<const Bounds> rooms) {
    std::vector<std::pair<double, double>> intervals;
    for (const auto &room : rooms) {
      double first = -1e30, last = 1e30;
      for (uint32_t axis = 0; axis < 3; ++axis) {
        const double d = ray.normal[axis], o = ray.position[axis];
        if (std::abs(d) < 1e-10) {
          if (o < room.minimum[axis] || o > room.maximum[axis])
            last = -1e30;
        } else {
          const double a = (room.minimum[axis] - o) / d, b = (room.maximum[axis] - o) / d;
          first = std::max(first, std::min(a, b));
          last = std::min(last, std::max(a, b));
        }
      }
      if (first <= last && last >= 0)
        intervals.emplace_back(first, last);
    }
    std::sort(intervals.begin(), intervals.end());
    double exit = 0;
    for (const auto &[first, last] : intervals) {
      if (first > exit + 1e-7)
        break;
      exit = std::max(exit, last);
    }
    require(exit > 0, "Analytic geometry starts in an empty room");
    return exit;
  }
  std::vector<bake::Result> geometry(const p::ProjectionAudit &audit, std::span<const Bounds> rooms) {
    std::vector<bake::Result> values(audit.rays().size());
    for (size_t i = 0; i < values.size(); ++i) {
      values[i].irradiance[0][3] = float(wallDistance(audit.rays()[i], rooms));
      values[i].statistics[2] = 1;
    }
    return values;
  }
} // namespace
int main() {
  const Bounds rooms[] = {box({-10, -2, 0}, {-.1f, 2, 2.5f}), box({.1f, -2, 0}, {10, 2, 2.5f}),
                          box({-.1f, -1.75f, 0}, {.1f, 1.75f, 1.5f})};
  const std::vector<p::FittedRegion> candidates = {
      {box({-9.999f, -1.749f, .001f}, {9.999f, 1.749f, 1.499f}), {{0, 0, .75f}, .1f, 3}},
      {box({-9.999f, -1.999f, .001f}, {-.101f, 1.999f, 2.499f}), {{-5, 0, 1.75f}, .1f, 3}},
      {box({.101f, -1.999f, .001f}, {9.999f, 1.999f, 2.499f}), {{5, 0, 1.75f}, .1f, 3}}};
  require(p::selectRegions(candidates, 1) == std::vector<size_t>{0},
          "The larger empty doorway proxy reproduces the volume-only "
          "allocation problem");
  p::ProjectionAudit audit(candidates);
  require(audit.rays().size() == 3 * 6 * 8 * 8,
          "Candidate geometry work has a fixed bounded directional set");
  auto values = geometry(audit, rooms);
  bool incomplete = false;
  try {
    audit.finish();
  } catch (const std::logic_error &) {
    incomplete = true;
  }
  require(incomplete, "Incomplete geometry cannot choose capture slots");
  auto invalid = values;
  invalid.back().statistics[1] = std::numeric_limits<float>::quiet_NaN();
  require(!audit.observe(0, invalid), "Invalid flags refuse the entire geometry batch");
  require(!audit.observe(uint32_t(values.size()), std::span(values).first(1)),
          "Out-of-range readback cannot address unrelated candidate memory");
  require(audit.observe(0, values), "A valid batch remains usable after an invalid batch was refused");
  require(!audit.observe(0, values), "Duplicate readback cannot bias geometry preference");
  const auto assessed = audit.finish();
  require(assessed[1].faces[5].hitBounds.valid &&
              std::abs(assessed[1].faces[5].hitBounds.minimum[2]) < 1e-5f,
          "First-hit envelopes retain the actual floor beyond the inset projection proxy");
  auto displacedRooms = std::array{rooms[0], rooms[1], rooms[2]};
  for (auto &room : displacedRooms) room.minimum[2] = -1;
  p::ProjectionAudit displaced(candidates);
  const auto displacedValues = geometry(displaced, displacedRooms);
  require(displaced.observe(0, displacedValues), "Displaced floor readbacks remain valid geometry");
  require(std::abs(displaced.finish()[1].faces[5].hitBounds.minimum[2] + 1) < 1e-5f,
          "Geometry envelopes expose floors below BSP-derived candidate bounds without extending those bounds");
  const auto selected = p::selectRegions(assessed, 2);
  require(selected == std::vector<size_t>({1, 2}) && assessed[1].quality > assessed[0].quality + .2f,
          "Measured geometry selects both full-height rooms instead of the "
          "larger low doorway proxy");
  p::ProjectionAudit streamed(candidates);
  const size_t half = values.size() / 2;
  require(streamed.observe(uint32_t(half), std::span(values).subspan(half)) &&
              streamed.observe(0, std::span(values).first(half)),
          "Completed frame slots may deliver geometry out of order");
  const auto reordered = streamed.finish();
  for (size_t i = 0; i < assessed.size(); ++i)
    require(std::abs(reordered[i].quality - assessed[i].quality) < 1e-6f,
            "Geometry preference is independent of batch boundaries and "
            "completion order");
  const std::vector<p::FittedRegion> repeatedOrigins = {candidates[1], candidates[0], candidates[1]};
  p::ProjectionAudit repeated(repeatedOrigins);
  require(repeated.rays().size() == 2 * 6 * 8 * 8,
          "Interleaved duplicate origins preserve one query set per distinct position");
  const auto repeatedValues = geometry(repeated, rooms);
  const uint32_t split = 417;
  require(repeated.observe(split, std::span(repeatedValues).subspan(split)) &&
              repeated.observe(0, std::span(repeatedValues).first(split)),
          "Shared queries accept out-of-order batches crossing an origin boundary");
  const auto repeatedResult = repeated.finish();
  require(repeatedResult[0].quality == assessed[1].quality &&
              repeatedResult[1].quality == assessed[0].quality &&
              repeatedResult[2].quality == assessed[1].quality,
          "Shared readbacks reach all original indices without changing the scores");
  p::ProjectionAudit misses(candidates);
  auto missValues = values;
  for (auto &value : missValues)
    value.statistics[1] = 1;
  require(misses.observe(0, missValues), "Distance-cap misses retain their explicit flags");
  for (const auto &region : misses.finish()) {
    require(region.quality == .05f && std::abs(region.missFraction - 1) < 1e-6f,
            "A miss cannot masquerade as a supporting wall even at an "
            "identical reported distance");
    for (const auto &face : region.faces)
      require(!face.hitBounds.valid && (face.solidAngleFraction == 0 || (std::abs(face.missFraction - 1) < 1e-6f &&
                  face.meanRelativeResidual == 0 && face.nearFraction == 0 && face.farFraction == 0)),
              "An all-miss face does not invent finite-hit geometry residuals or bounds");
  }
  auto scaled = candidates;
  for (auto &region : scaled) {
    for (auto *v : {&region.bounds.minimum, &region.bounds.maximum, &region.site.position})
      for (auto &axis : *v)
        axis *= 40;
  }
  p::ProjectionAudit units(scaled);
  auto scaledValues = values;
  for (auto &value : scaledValues)
    value.irradiance[0][3] *= 40;
  require(units.observe(0, scaledValues), "Scaled geometry queries are valid");
  const auto unitRegions = units.finish();
  for (size_t i = 0; i < assessed.size(); ++i) {
    require(std::abs(unitRegions[i].quality - assessed[i].quality) < 1e-6f,
            "Candidate geometry agreement is independent of world units");
    for (size_t face = 0; face < 6; ++face)
      require(std::abs(unitRegions[i].faces[face].meanRelativeResidual -
                  assessed[i].faces[face].meanRelativeResidual) < 1e-6f,
              "Face residuals remain comparable between Source units and metres");
  }
  // One capture sees two equally large empty boxes in an L-shaped room.
  // Keep both until independent first-hit geometry can resolve the ambiguity.
  const auto outer = box({-4, -3, -2}, {4, 3, 2});
  const auto solid = box({0, 0, -2}, {4, 3, 2});
  remix::scene::VisibilityPartition elbow;
  elbow.closed = true;
  elbow.clusterCount = 1;
  elbow.leafClusters = {-1, 0};
  elbow.nodes.resize(12);
  for (uint32_t part = 0; part < 2; ++part)
    for (uint32_t face = 0; face < 6; ++face) {
      auto &node = elbow.nodes[part * 6 + face];
      const auto &bounds = part ? solid : outer;
      const uint32_t axis = face / 2;
      node.plane[axis] = face & 1u ? -1.f : 1.f;
      node.plane[3] = face & 1u ? -bounds.maximum[axis] : bounds.minimum[axis];
      node.children[0] = face < 5 ? int32_t(part * 6 + face + 1) : part ? -1 : 6;
      node.children[1] = part ? -2 : -1;
    }
  const Vec3 capture{-1, -2.5f, 0};
  const auto shapes = p::fitRegionCandidates(elbow, outer, capture, .001f);
  std::vector<p::FittedRegion> shapeCandidates;
  for (const auto &shape : shapes) shapeCandidates.push_back({shape, {capture, 1, 3}});
  p::ProjectionAudit shapeAudit(shapeCandidates);
  const Bounds elbowRooms[]{box({-4, -3, -2}, {0, 3, 2}), box({0, -3, -2}, {4, 0, 2})};
  require(shapeAudit.observe(0, geometry(shapeAudit, elbowRooms)), "Alternative shapes accept analytic room distances");
  const auto shapeScores = shapeAudit.finish();
  const auto choice = p::selectRegions(shapeScores, 1);
  require(shapeScores.size() >= 2 && choice.size() == 1, "Shape alternatives reach the geometry selector");
  std::printf("Elbow shape audit: primary %.4f, alternative %.4f, selected %zu\n",
              shapeScores[0].quality, shapeScores[1].quality, choice.front());
  require(choice.front() != 0 && shapeScores[choice.front()].quality > shapeScores[0].quality,
          "Geometry can recover the room-shaped alternative discarded by largest-volume-only fitting");
  require(shapeAudit.rays().size() == 6 * 8 * 8,
          "Shape alternatives at the same capture origin trace geometry only once");
  for (size_t i = 0; i < shapeCandidates.size(); ++i) {
    p::ProjectionAudit separate({shapeCandidates[i]});
    require(separate.observe(0, geometry(separate, elbowRooms)), "Independent alternative accepts geometry");
    const auto single = separate.finish().front();
    require(single.quality == shapeScores[i].quality && single.missFraction == shapeScores[i].missFraction,
            "Sharing first-hit rays preserves the independent candidate score exactly");
    double total = 0;
    for (size_t face = 0; face < 6; ++face) {
      const auto &a = single.faces[face], &b = shapeScores[i].faces[face];
      require(a.solidAngleFraction == b.solidAngleFraction && a.nearFraction == b.nearFraction &&
                  a.farFraction == b.farFraction && a.meanRelativeResidual == b.meanRelativeResidual,
              "Shared origins retain each projection box's independent per-face residuals");
      total += a.solidAngleFraction;
    }
    require(std::abs(total - 1) < 1e-6, "Projection-face weights cover the full solid angle");
  }
  const std::vector<p::FittedRegion> rectangular = {{box({-1,-1,-1},{1,1,1}), {{0,0,0},1,3}}};
  const Bounds rectangleRooms[] = {rectangular.front().bounds};
  p::ProjectionAudit rectangle(rectangular);
  auto rectangleValues = geometry(rectangle, rectangleRooms);
  require(rectangle.observe(0, rectangleValues), "Exact box geometry accepts all face queries");
  const auto rectangleResult = rectangle.finish();
  for (const auto &face : rectangleResult.front().faces)
    require(std::abs(face.solidAngleFraction - 1.f/6) < 1e-6f && face.supportFraction == 1 &&
                face.nearFraction == 0 && face.farFraction == 0 && std::abs(face.meanRelativeResidual) < 1e-6f,
            "A matching rectangular room reports zero face residuals");
  p::ProjectionAudit clutter(rectangular);
  for (auto &value : rectangleValues) value.irradiance[0][3] *= .5f;
  require(clutter.observe(0, rectangleValues), "Near geometry remains valid inside the proxy");
  const auto clutterResult = clutter.finish();
  for (const auto &face : clutterResult.front().faces)
    require(face.supportFraction == 1 && face.nearFraction == 1 && face.farFraction == 0 &&
                std::abs(face.meanRelativeResidual + .5f) < 1e-6f &&
                std::abs(face.meanAbsoluteRelativeResidual - .5f) < 1e-6f,
            "Nearer props have negative residuals without changing existing agreement policy");
  p::ProjectionAudit shortBox(rectangular);
  for (auto &value : rectangleValues) value.irradiance[0][3] *= 4;
  require(shortBox.observe(0, rectangleValues), "Beyond-proxy wall queries remain valid");
  const auto shortBoxResult = shortBox.finish();
  for (const auto &face : shortBoxResult.front().faces)
    require(face.supportFraction == 0 && face.nearFraction == 0 && face.farFraction == 1 &&
                std::abs(face.meanRelativeResidual - 1) < 1e-6f &&
                std::abs(face.meanAbsoluteRelativeResidual - 1) < 1e-6f,
            "A box stopping short of a wall has positive residuals, distinct from clutter");
  for (const auto &face : rectangleResult.front().faces)
    require(!face.plane, "Normal candidate assessment allocates no plane diagnostics");
  p::ProjectionAudit planes(rectangular, {}, true);
  auto planeValues = geometry(planes, rectangleRooms);
  for (size_t face = 0; face < 6; ++face)
    for (size_t i = 0; i < 4; ++i) planeValues[face * 64 + i].irradiance[0][3] *= .1f;
  require(planes.observe(0, planeValues), "Sparse foreground clutter is retained as plane evidence");
  const auto planeResult = planes.finish();
  for (size_t face = 0; face < 6; ++face) {
    const auto &fit = planeResult[0].faces[face].plane;
    const float coordinate = face & 1u ? -1.f : 1.f;
    require(fit && std::abs(fit->median - coordinate) < 1e-6f &&
            std::abs(fit->lower - coordinate) < 1e-6f && std::abs(fit->upper - coordinate) < 1e-6f &&
            fit->hits == 64 && std::abs(fit->sampleFraction - 1) < 1e-6f,
            "Weighted plane percentiles retain a coherent wall despite sparse foreground clutter");
  }
  p::ProjectionAudit ordinaryReassessment(planeResult);
  require(ordinaryReassessment.observe(0, geometry(ordinaryReassessment, rectangleRooms)),
          "Previously assessed geometry can be reassessed without plane storage");
  const auto ordinaryResult = ordinaryReassessment.finish();
  for (const auto &face : ordinaryResult[0].faces)
    require(!face.plane, "Disabled plane diagnostics cannot retain evidence from an earlier assessment");
  p::ProjectionAudit scatteredPlanes(rectangular, {}, true);
  auto scatteredValues = geometry(scatteredPlanes, rectangleRooms);
  for (size_t i = 0; i < scatteredValues.size(); i += 2) scatteredValues[i].irradiance[0][3] *= .5f;
  require(scatteredPlanes.observe(0, scatteredValues), "Distributed foreground geometry remains measurable");
  const auto scatteredResult = scatteredPlanes.finish();
  for (const auto &face : scatteredResult[0].faces)
    require(face.plane && face.plane->upper - face.plane->lower > .49f,
            "Broad hit distributions cannot masquerade as a coherent wall plane");
  p::ProjectionAudit unavailablePlanes(rectangular, {}, true);
  planeValues = geometry(unavailablePlanes, rectangleRooms);
  for (size_t i = 0; i < 64; ++i) planeValues[i].statistics[0] = 1;
  for (size_t i = 64; i < 128; ++i) planeValues[i].statistics[1] = 1;
  require(unavailablePlanes.observe(0, planeValues), "Backfaces and misses retain their raw classifications");
  const auto unavailableResult = unavailablePlanes.finish();
  require(!unavailableResult[0].faces[0].plane && !unavailableResult[0].faces[1].plane &&
          unavailableResult[0].faces[2].plane,
          "Missing or back-facing geometry cannot supply a solid front-surface plane");
  const std::vector<p::FittedRegion> oversized = {{box({-2,-2,-2},{2,2,2}), {{0,0,0},1,3}}};
  p::ProjectionAudit refinementAudit(oversized, {}, true);
  require(refinementAudit.observe(0, geometry(refinementAudit, rectangleRooms)),
          "Captured inner room supports an oversized projection comparison");
  const auto refinementInput = refinementAudit.finish()[0];
  const auto proposal = p::refineProjection(refinementInput, .001f);
  require(proposal && proposal->faces == 63, "Consistent interior planes propose six inward face shifts");
  for (uint32_t axis = 0; axis < 3; ++axis)
    require(proposal->bounds.minimum[axis] > oversized[0].bounds.minimum[axis] &&
            proposal->bounds.maximum[axis] < oversized[0].bounds.maximum[axis] &&
            proposal->bounds.minimum[axis] < 0 && proposal->bounds.maximum[axis] > 0,
            "Projection refinement contracts the box while preserving an interior capture");
  auto revised = oversized[0];
  revised.bounds = proposal->bounds;
  p::ProjectionAudit reassessment({revised});
  require(reassessment.observe(0, geometry(reassessment, rectangleRooms)), "Refined proposal remains independently assessable");
  const auto revisedResult = reassessment.finish();
  for (const auto &face : revisedResult[0].faces)
    require(face.nearFraction == 0 && face.farFraction == 0,
            "Coherent plane refinement removes the analytic room's original projection mismatch");
  const auto reusedResult = refinementAudit.reassess({revised});
  for (uint32_t face = 0; face < 6; ++face) {
    const auto &fresh = revisedResult[0].faces[face];
    const auto &reused = reusedResult[0].faces[face];
    require(fresh.solidAngleFraction == reused.solidAngleFraction && fresh.nearFraction == reused.nearFraction &&
            fresh.farFraction == reused.farFraction && fresh.missFraction == reused.missFraction &&
            fresh.meanRelativeResidual == reused.meanRelativeResidual &&
            fresh.meanAbsoluteRelativeResidual == reused.meanAbsoluteRelativeResidual,
            "Reused first hits exactly match an independent analytic reassessment of the new box");
  }
  bool reuseRefused = false;
  try { (void)rectangle.reassess({revised}); } catch (const std::logic_error &) { reuseRefused = true; }
  require(reuseRefused, "Ordinary assessments do not retain reusable per-ray geometry");
  p::ProjectionAudit pendingReuse(oversized, {}, true);
  reuseRefused = false;
  try { (void)pendingReuse.reassess({revised}); } catch (const std::logic_error &) { reuseRefused = true; }
  require(reuseRefused, "Pending readbacks cannot support reassessment");
  auto movedOrigin = revised;
  movedOrigin.site.position[0] = .1f;
  reuseRefused = false;
  try { (void)refinementAudit.reassess({movedOrigin}); } catch (const std::invalid_argument &) { reuseRefused = true; }
  require(reuseRefused, "New origins need fresh geometry rather than borrowing nearby evidence");
  auto signedOrigin = revised;
  signedOrigin.site.position[0] = -0.f;
  reuseRefused = false;
  try { (void)refinementAudit.reassess({signedOrigin}); } catch (const std::invalid_argument &) { reuseRefused = true; }
  require(reuseRefused, "Reassessment preserves bit-identical origins used by stochastic alpha coverage");
  auto shifted = oversized[0];
  shifted.site.position[0] = .25f;
  p::ProjectionAudit twoOrigins({oversized[0], shifted}, {}, true);
  require(twoOrigins.observe(0, geometry(twoOrigins, rectangleRooms)), "Two independent geometry origins complete");
  const auto reorderedOrigins = twoOrigins.reassess({shifted, oversized[0]});
  for (const auto &region : reorderedOrigins)
    require(region.faces[0].plane && region.faces[1].plane &&
            std::abs(region.faces[0].plane->median - 1) < 1e-6f &&
            std::abs(region.faces[1].plane->median + 1) < 1e-6f,
            "Reordered origins retain their own first-hit distances rather than borrowing batch neighbours");
  auto uncertain = refinementInput;
  for (auto &face : uncertain.faces) {
    face.plane->lower -= .5f;
    face.plane->upper += .5f;
  }
  require(!p::refineProjection(uncertain, .001f), "Broad plane uncertainty cannot authorize contraction");
  p::RefinementPolicy detailBudget;
  detailBudget.maximumAbsoluteUncertainty = .6f;
  require(p::refineProjection(uncertain, .001f, detailBudget).has_value(),
          "Explicit scene-unit detail budget can propose a reassessable contraction near an origin");
  detailBudget.maximumAbsoluteUncertainty = .49f;
  require(!p::refineProjection(uncertain, .001f, detailBudget),
          "Absolute uncertainty remains bounded by the supplied detail budget");
  detailBudget.maximumAbsoluteUncertainty = .6f;
  auto outward = refinementInput;
  for (uint32_t face = 0; face < 6; ++face) {
    const float coordinate = face & 1u ? -3.f : 3.f;
    outward.faces[face].plane = p::ProjectionPlaneAudit{coordinate, coordinate, coordinate, 1, 64};
  }
  require(!p::refineProjection(outward, .001f, detailBudget), "Measured planes beyond the proxy cannot grow it through walls");
  auto unsupported = refinementInput;
  for (auto &face : unsupported.faces) face.plane->sampleFraction = .5f;
  require(!p::refineProjection(unsupported, .001f, detailBudget), "A minority of usable hits cannot move a projection face");
  auto crossedOrigin = refinementInput;
  for (auto &face : crossedOrigin.faces) face.plane = p::ProjectionPlaneAudit{0, 0, 0, 1, 64};
  require(!p::refineProjection(crossedOrigin, .001f, detailBudget), "No proposal may place the capture on its surface");
  p::ProjectionAudit balancedError(rectangular);
  auto balancedValues = geometry(balancedError, rectangleRooms);
  for (size_t i = 0; i < balancedValues.size(); ++i)
    balancedValues[i].irradiance[0][3] *= i & 1u ? .5f : 1.5f;
  require(balancedError.observe(0, balancedValues), "Near and far projection errors remain independently measurable");
  const auto balancedResult = balancedError.finish();
  for (const auto &face : balancedResult[0].faces)
    require(std::abs(face.meanRelativeResidual) < 1e-6f &&
            std::abs(face.meanAbsoluteRelativeResidual - .5f) < 1e-6f,
            "Opposite projection errors cannot cancel in the absolute residual metric");
  p::ProjectionAudit empty({});
  require(empty.rays().empty() && empty.finish().empty(), "Empty candidate work needs no GPU submission");
  bool bounded = false;
  try {
    p::ProjectionAudit excessive(std::vector<p::FittedRegion>(p::ProjectionAudit::maximumCandidates + 1,
                                                            candidates.front()));
  } catch (const std::invalid_argument &) {
    bounded = true;
  }
  require(bounded, "Candidate limits are checked before ray allocation");
  p::ProjectionAudit aliases(std::vector<p::FittedRegion>(p::ProjectionAudit::maximumCandidates,
                                                        candidates.front()));
  require(aliases.rays().size() == 6u * 8u * 8u,
          "Maximum alternative records still reuse one origin's geometry rays");
  auto origins = std::vector<p::FittedRegion>(p::ProjectionAudit::maximumOrigins + 1, candidates.front());
  for (size_t i = 0; i < origins.size(); ++i) origins[i].site.position[0] += float(i) * .001f;
  bounded = false;
  try { p::ProjectionAudit excessive(origins); }
  catch (const std::invalid_argument &) { bounded = true; }
  require(bounded, "Alternative records must not increase unique-origin ray budget");
  std::stop_source stop;
  stop.request_stop();
  bool cancelled = false;
  try {
    p::ProjectionAudit stopped(candidates, stop.get_token());
  } catch (const std::runtime_error &) {
    cancelled = true;
  }
  require(cancelled, "Map changes cancel geometry preparation");
  std::printf("Candidate geometry: doorway %.4f, rooms %.4f/%.4f; bounded "
              "streaming and explicit misses passed\n",
              assessed[0].quality, assessed[1].quality, assessed[2].quality);
}
