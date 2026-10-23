#include "forward/probes/regions.h"
#include "forward/probes/receiver_fits.h"
#include "forward/probes/receiver_fit_pairs.h"
#include "partition_fixture.h"
#include "forward/probes/data.h"
#include "forward/probes/selection.h"
#include "forward/probes/placement.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

using namespace remix;
using namespace remix::forward;
namespace p = remix::forward::probes;
namespace {
  void require(bool value, const char *message) {
    if (!value) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
  Bounds box(Vec3 minimum, Vec3 maximum) {
    Bounds result;
    result.include(minimum);
    result.include(maximum);
    return result;
  }
  const Bounds room = box({-4, -3, -2}, {4, 3, 2});
  scene::VisibilityPartition partition(const Bounds *obstacle = nullptr) {
    scene::VisibilityPartition result;
    result.closed = true;
    result.clusterCount = 2;
    result.leafClusters = {-1, 0, 1};
    result.nodes.resize(obstacle ? 12 : 7);
    auto planes = [&](uint32_t first, const Bounds &bounds, int32_t inside, int32_t outside) {
      for (uint32_t i = 0; i < 6; ++i) {
        auto &node = result.nodes[first + i];
        const uint32_t axis = i / 2;
        node.plane[axis] = (i & 1u) ? -1.f : 1.f;
        node.plane[3] = (i & 1u) ? -bounds.maximum[axis] : bounds.minimum[axis];
        node.children[0] = i == 5 ? inside : int32_t(first + i + 1);
        node.children[1] = outside;
      }
    };
    planes(0, room, 6, -1);
    if (obstacle)
      planes(6, *obstacle, -1, -2);
    else
      result.nodes[6] = {{1, 0, 0, 0}, {-2, -3}, {0, 0}};
    return result;
  }
  scene::BakeScene unequalRooms() {
    const Bounds rooms[] = {box({-10, -1, 0}, {-8, 1, .4f}),
                            box({-6, -1, 0}, {-4, 1, .4f}), box({0, -3, 0}, {8, 3, 3})};
    auto space = std::make_shared<scene::VisibilityPartition>();
    space->closed = true;
    space->clusterCount = 3;
    space->leafClusters = {-1, 0, 1, 2};
    space->nodes.resize(18);
    for (uint32_t room = 0; room < 3; ++room)
      for (uint32_t face = 0; face < 6; ++face) {
        auto &node = space->nodes[room * 6 + face];
        const uint32_t axis = face / 2;
        node.plane[axis] = face & 1u ? -1.f : 1.f;
        node.plane[3] = face & 1u ? -rooms[room].maximum[axis] : rooms[room].minimum[axis];
        node.children[0] = face == 5 ? -int32_t(room + 2) : int32_t(room * 6 + face + 1);
        node.children[1] = room == 2 ? -1 : int32_t((room + 1) * 6);
      }
    scene::BakeScene result;
    result.hasBounds = true;
    result.minimum = {-10, -3, 0};
    result.maximum = {8, 3, 3};
    result.partition = space;
    return result;
  }
} // namespace
int main(int argc, char **argv) {
  if (argc == 3 && std::string_view(argv[1]) == "--partition-fixture")
    return remix::forward::test::partitionFixture(argv[2]);
  scene::BakeScene localSource;
  localSource.hasBounds = true;
  localSource.minimum = room.minimum;
  localSource.maximum = room.maximum;
  localSource.sceneScale = 1;
  localSource.partition = std::make_shared<scene::VisibilityPartition>(partition());
  p::ReceiverSample localReceiver{};
  localReceiver.position = {-1, 0, -2};
  localReceiver.normal = {0, 0, 1};
  p::ReceiverSite localSite{};
  localSite.point.position = {-1, 0, -1.25f, 0};
  p::ReceiverSiteAudit admission({localSite});
  bake::Result admitted{};
  admitted.statistics = {0, 0, 64, 0};
  require(admission.observe(0, std::span(&admitted, 1)), "Local site admission recorded");
  p::ReceiverSiteChecks support(admission, std::span(&localReceiver, 1), 1);
  std::vector<bake::Result> clear(support.rays().size());
  for (auto &value : clear) {
    value.statistics = {0, 1, 1, 0};
    value.irradiance[0][3] = 100;
  }
  require(support.observe(0, clear), "Local site support recorded");
  const auto localFits = p::fitReceiverSites(localSource, std::span(&localReceiver, 1), admission, support);
  require(localFits.available && localFits.supported == 1 && localFits.attempts.size() == 1,
          "Supported receiver site reaches bounded room fitting");
  bool coversFloor = false;
  for (const auto &variant : localFits.attempts[0].variants) {
    require(p::emptyRegion(*localSource.partition, variant.projection), "Local proxy respects solid walls");
    coversFloor |= variant.coversReceiver;
  }
  require(coversFloor, "Local influence restores contact with its originating floor receiver");
  p::ReceiverSiteChecks blocked(admission, std::span(&localReceiver, 1), 1);
  clear[0].statistics = {0, 0, 1, 0};
  clear[0].irradiance[0][3] = .1f;
  require(blocked.observe(0, clear), "Blocked receiver support recorded");
  require(p::fitReceiverSites(localSource, std::span(&localReceiver, 1), admission, blocked).attempts.empty(),
          "A free endpoint cannot authorize a fit through a blocked receiver connection");
  auto solidSource = localSource;
  solidSource.minimum = {-10, -10, -10};
  solidSource.maximum = {10, 10, 10};
  const auto obstruction = box({-2, -1, -1.5f}, {0, 1, -.5f});
  solidSource.partition = std::make_shared<scene::VisibilityPartition>(partition(&obstruction));
  const auto refused = p::fitReceiverSites(solidSource, std::span(&localReceiver, 1), admission, support);
  require(refused.partitionRefused == 1 && refused.attempts.empty(),
          "Sampled hardware support cannot override a solid partition origin");
  p::ReceiverFitAudit fitAudit(localFits, admission.sites());
  require(fitAudit.rays().size() == 6 * 8 * 8,
          "Same-origin local projection variants share one geometry ray set");
  bool premature = false;
  try { (void)fitAudit.finish(); } catch (const std::logic_error &) { premature = true; }
  require(premature, "Local projection evidence is unavailable before all readbacks retire");
  std::vector<bake::Result> roomHits(fitAudit.rays().size());
  for (size_t i = 0; i < roomHits.size(); ++i) {
    const auto &ray = fitAudit.rays()[i];
    float distance = std::numeric_limits<float>::max();
    for (uint32_t axis = 0; axis < 3; ++axis)
      if (std::abs(ray.normal[axis]) > 1e-10f) {
        const float boundary = ray.normal[axis] > 0 ? room.maximum[axis] : room.minimum[axis];
        distance = std::min(distance, (boundary - ray.position[axis]) / ray.normal[axis]);
      }
    roomHits[i].statistics = {0, 0, 1, 0};
    roomHits[i].irradiance[0][3] = distance;
  }
  require(fitAudit.observe(0, roomHits), "Analytic room first-hit geometry accepted");
  const auto assessed = fitAudit.finish();
  require(assessed.attempts.size() == localFits.attempts.size(), "Geometry assessment preserves fit owners");
  for (size_t i = 0; i < assessed.attempts[0].variants.size(); ++i) {
    const auto &variant = assessed.attempts[0].variants[i];
    require(variant.geometry && variant.geometry->geometryEvaluated && variant.geometry->quality > .99f &&
            variant.geometry->missFraction == 0 && variant.geometry->backfaceFraction == 0 &&
            variant.influence.minimum == localFits.attempts[0].variants[i].influence.minimum &&
            variant.influence.maximum == localFits.attempts[0].variants[i].influence.maximum,
            "Shared geometry attaches to each original fit without changing its influence");
  }
  p::ReceiverFitAudit nearAudit(localFits, admission.sites());
  for (auto &hit : roomHits) hit.irradiance[0][3] = .01f;
  require(nearAudit.observe(0, roomHits), "Intervening geometry evidence accepted");
  const auto nearFits = nearAudit.finish();
  for (const auto &face : nearFits.attempts[0].variants[0].geometry->faces)
    require(face.nearFraction > .99f, "Near hits remain distinguishable from a matching room shell");
  const auto localPairs = p::receiverFitPairs(std::span(&localReceiver, 1), {}, assessed, .002f);
  require(localPairs.size() == 1 && localPairs[0].receiver == 0 && localPairs[0].capture == 0,
          "Alternate local boxes share one receiver-to-origin coverage segment");
  auto backwards = localReceiver;
  backwards.normal = {0, 0, -1};
  require(p::receiverFitPairs(std::span(&backwards, 1), {}, assessed, .002f).empty(),
          "An influence box cannot authorize a back-facing receiver segment");
  p::Capture baseline;
  baseline.bounds = box({-1, -1, -1}, {1, 1, 1});
  baseline.influence = box({-1, -1, -2}, {1, 1, 2});
  baseline.position = {-1, 0, 0};
  baseline.worldToLocal = {{{0, -1, 0, 0}, {1, 0, 0, 1}, {0, 0, 1, 0}}};
  baseline.resolution = 8;
  require(p::receiverFitPairs(std::span(&localReceiver, 1), std::span(&baseline, 1), assessed, .002f).empty(),
          "Existing rotated influence coverage is excluded even outside its projection inset");
  std::vector<p::ReceiverSample> cohort(25, localReceiver);
  const auto limited = p::receiverFitPairs(cohort, {}, assessed, .002f, 3);
  require(limited.size() == 3 && limited[0].receiver < 8 && limited[1].receiver >= 8 &&
          limited[1].receiver < 17 && limited[2].receiver >= 17,
          "A bounded local coverage cohort spreads across receiver order instead of keeping a prefix");
  bool unassessedRefused = false;
  try { (void)p::receiverFitPairs(std::span(&localReceiver, 1), {}, localFits, .002f); }
  catch (const std::invalid_argument &) { unassessedRefused = true; }
  require(unassessedRefused, "Local coverage never skips completed projection assessment");
  localSource.partition.reset();
  require(!p::fitReceiverSites(localSource, std::span(&localReceiver, 1), admission, support).available,
          "Missing occupancy partition does not permit a projection fallback");
  auto splitRoom = partition();
  const auto interior = box({-3.99f, -2.99f, -1.99f}, {3.99f, 2.99f, 1.99f});
  require(p::emptyRegion(splitRoom, interior), "Artificial empty visleaf splits do not become room walls");
  require(!p::emptyRegion(splitRoom, box({-5, -3, -2}, {4, 3, 2})),
          "Boxes extending outside the map are refused");
  const auto fit = p::fitRegion(splitRoom, room, {-.75f, .5f, 0}, .01f);
  require(fit && p::emptyRegion(splitRoom, *fit), "Fitted region is wholly empty");
  const auto receivers = p::receiverRegion(splitRoom, *fit, .02f);
  require(receivers.minimum == room.minimum && receivers.maximum == room.maximum &&
              p::emptyRegion(splitRoom, receivers),
          "Receiver influence reaches the room's actual surfaces without altering its inset projection");
  require(fit->minimum[2] > room.minimum[2] && receivers.minimum[2] == room.minimum[2],
          "A real floor excluded by the proxy inset is included in receiver influence");
  const auto leftProxy = box({-3.9f, -2.9f, -1.9f}, {-.1f, 2.9f, 1.9f});
  const auto rightProxy = box({.1f, -2.9f, -1.9f}, {3.9f, 2.9f, 1.9f});
  const auto overlapLeft =
      p::receiverRegion(splitRoom, leftProxy, .02f, {}, {}, .5f);
  const auto overlapRight =
      p::receiverRegion(splitRoom, rightProxy, .02f, {}, {}, .5f);
  require(overlapLeft.maximum[0] > overlapRight.minimum[0] &&
              p::emptyRegion(splitRoom, overlapLeft) &&
              p::emptyRegion(splitRoom, overlapRight),
          "Receiver influence overlaps across an artificial empty-space split");
  const auto overlapWall = box({-.05f, -3, -2}, {.05f, 3, 2});
  const auto divided = partition(&overlapWall);
  const auto blockedOverlap =
      p::receiverRegion(divided, leftProxy, .02f, {}, {.5f}, .5f);
  require(
      blockedOverlap.maximum[0] < overlapWall.minimum[0] &&
          p::emptyRegion(divided, blockedOverlap),
      "Overlap does not cross a full wall even inside the detail tolerance");
  for (uint32_t c = 0; c < 3; ++c)
    require(std::abs(fit->minimum[c] - room.minimum[c]) < .03f &&
                std::abs(fit->maximum[c] - room.maximum[c]) < .03f,
            "A rectangular room fits across multiple visleaves");
  const auto pillar = box({-.5f, -.5f, -.5f}, {.5f, .5f, .5f});
  const auto obstructed = partition(&pillar);
  for (uint32_t corner = 0; corner < 8; ++corner) {
    const Vec3 point = {corner & 1u ? 3.9f : -3.9f, corner & 2u ? 2.9f : -2.9f, corner & 4u ? 1.9f : -1.9f};
    require(obstructed.cluster(point.data()) >= 0, "All candidate-box corners lie in empty space");
  }
  require(!p::emptyRegion(obstructed, interior),
          "A solid pillar inside the box is detected despite eight empty corners");
  const auto besidePillar = p::fitRegion(obstructed, room, {-2, 0, 0}, .01f);
  require(besidePillar && besidePillar->maximum[0] < -.49f && p::emptyRegion(obstructed, *besidePillar),
          "Box growth stops at an interior occluder");
  const p::RegionTolerance tolerance{.5f};
  const auto aroundPillar = p::fitRegion(obstructed, room, {-2, 0, 0}, .01f, {}, tolerance);
  require(aroundPillar && aroundPillar->minimum[0] < -3.9f && aroundPillar->maximum[0] > 3.9f,
          "A small interior prop does not divide the reflection room");
  require(!p::fitRegion(obstructed, room, {0, 0, 0}, .01f, {}, tolerance),
          "Detail tolerance never permits a capture origin inside solid geometry");
  auto bevel = partition();
  bevel.nodes[6] = {{1, 0, 1, 5.7f}, {-1, -2}, {0, 0}};
  const auto bevelStrict = p::fitRegion(bevel, room, {0, 0, 0}, .01f);
  const auto bevelTolerant = p::fitRegion(bevel, room, {0, 0, 0}, .01f, {}, tolerance);
  p::RegionQuery bevelQuery(bevel);
  require(!bevelQuery.contains(room) && bevelQuery.contains(room, tolerance) &&
              !bevelQuery.contains(room, {-1}),
          "Reference-bound queries share fitting trim tolerance and reject invalid settings");
  require(bevelStrict && bevelTolerant &&
              (bevelStrict->maximum[0] < 3.9f || bevelStrict->maximum[2] < 1.9f) &&
              bevelTolerant->maximum[0] > 3.9f && bevelTolerant->maximum[2] > 1.9f,
          "A ceiling bevel can remain inside the proxy without shrinking an entire room face");
  auto bevelRing = partition();
  bevelRing.nodes.resize(10);
  const std::array<std::array<float, 4>, 4> bevelPlanes = {
      {{1, 0, 1, 5.7f}, {-1, 0, 1, 5.7f}, {0, 1, 1, 4.7f}, {0, -1, 1, 4.7f}}};
  for (uint32_t i = 0; i < 4; ++i) {
    auto &node = bevelRing.nodes[6 + i];
    std::copy(bevelPlanes[i].begin(), bevelPlanes[i].end(), node.plane);
    node.children[0] = -1;
    node.children[1] = i == 3 ? -2 : int32_t(7 + i);
  }
  const auto ringFit = p::fitRegion(bevelRing, room, {0, 0, 0}, .01f, {}, tolerance);
  require(ringFit && ringFit->minimum[0] < -3.9f && ringFit->maximum[0] > 3.9f &&
              ringFit->minimum[1] < -2.9f && ringFit->maximum[1] > 2.9f && ringFit->maximum[2] > 1.9f,
          "Connected bevels around all four ceiling edges retain the room box");
  const auto tallPillar = box({-.3f, -.3f, -2}, {.3f, .3f, 2});
  const auto columnRoom = partition(&tallPillar);
  const auto columnFit = p::fitRegion(columnRoom, room, {-2, 0, 0}, .01f, {}, tolerance);
  require(columnFit && columnFit->maximum[0] > 3.9f,
          "A narrow floor-to-ceiling column can be treated as interior detail");
  const auto clutter = box({-1.9f, -1.4f, -.9f}, {1.9f, 1.4f, .9f});
  const auto cluttered = partition(&clutter);
  const auto clutterFit = p::fitRegion(cluttered, room, {-3, 0, 0}, .01f, {}, tolerance);
  require(clutterFit && (clutterFit->maximum[0] < 3.9f || clutterFit->maximum[1] < 2.9f ||
                         clutterFit->maximum[2] < 1.9f),
          "Excessive interior solid volume exceeds the ignored-detail budget");
  const auto corner = box({0, 0, -2}, {4, 3, 2});
  const auto elbow = partition(&corner);
  const auto elbowFits = p::fitRegionCandidates(elbow, room, {-1, -1, 0}, .001f);
  require(elbowFits.size() >= 2 && elbowFits.size() <= 3,
          "An L-shaped room retains distinct growth alternatives at one capture origin");
  bool horizontal = false, vertical = false;
  for (const auto &candidate : elbowFits) {
    require(p::emptyRegion(elbow, candidate), "Every alternative retains whole-volume wall exclusion");
    horizontal |= candidate.maximum[0] > 3.9f && candidate.maximum[1] < .01f;
    vertical |= candidate.maximum[1] > 2.9f && candidate.maximum[0] < .01f;
  }
  require(horizontal && vertical, "Geometry assessment can choose either arm rather than only a volume winner");
  require(p::fitRegionCandidates(splitRoom, room, {0, 0, 0}, .001f).size() == 1,
          "Equivalent rectangular-room fits consume only one audit slot");
  scene::BakeScene elbowScene;
  elbowScene.hasBounds = true;
  elbowScene.minimum = room.minimum;
  elbowScene.maximum = room.maximum;
  elbowScene.partition = std::make_shared<scene::VisibilityPartition>(elbow);
  const std::vector<Vec3> elbowSites{{-1, -1, 0}};
  const auto auditCandidates = p::fitCandidates(elbowScene, {}, 16, {}, 0, &elbowSites);
  require(auditCandidates.size() >= 2 && auditCandidates.size() <= 144,
          "Production placement passes shape alternatives to the audit within its existing budget");
  const auto wall = box({-.01f, -3, -2}, {.01f, 3, 2});
  const auto separated = partition(&wall);
  const auto leftInset = p::fitRegion(separated, room, {-2, 0, 0}, .01f);
  require(bool(leftInset), "Thin-wall receiver fixture fits the left room");
  const auto leftInfluence = p::receiverRegion(separated, *leftInset, .04f);
  require(leftInfluence.maximum[0] == wall.minimum[0] && p::emptyRegion(separated, leftInfluence),
          "Receiver snapping reaches a thin wall's near face but refuses its nearby far face");
  const auto left = p::fitRegion(separated, room, {-2, 0, 0}, .001f);
  const auto right = p::fitRegion(separated, room, {2, 0, 0}, .001f);
  require(left && right && left->maximum[0] < 0 && right->minimum[0] > 0,
          "A thin solid wall separates reflection volumes even when both sides use the same cluster ID");
  const auto tolerantLeft = p::fitRegion(separated, room, {-2, 0, 0}, .001f, {}, tolerance);
  require(tolerantLeft && tolerantLeft->maximum[0] < 0,
          "Even a very thin room-spanning sheet remains a wall with tolerance enabled");
  auto dividedWall = separated;
  dividedWall.nodes[11].children[0] = 12;
  dividedWall.nodes.push_back({{0, 1, 0, 0}, {-1, 13}, {0, 0}});
  dividedWall.nodes.push_back({{0, 0, 1, 0}, {-1, -1}, {0, 0}});
  const auto dividedFit = p::fitRegion(dividedWall, room, {-2, 0, 0}, .001f, {}, tolerance);
  require(dividedFit && dividedFit->maximum[0] < 0,
          "A wall subdivided into BSP fragments is merged before detail classification");
  scene::BakeScene authored;
  authored.hasBounds = true;
  authored.minimum = room.minimum;
  authored.maximum = room.maximum;
  authored.partition = std::make_shared<scene::VisibilityPartition>(separated);
  bake::Grid grid;
  for (uint32_t i = 0; i < 4; ++i) {
    grid.points.push_back({{i & 1u ? 2.f : -2.f, i & 2u ? 1.f : -1.f, 0, 0}, {}});
    grid.probes.emplace_back().statistics[3] = 1;
    grid.tasks.push_back(i);
  }
  const auto captures = p::place(authored, grid, 4, 8);
  require(captures.size() == 2, "Placement merges duplicate room fits while retaining both sides of a wall");
  for (const auto &capture : captures)
    require(p::emptyRegion(separated, capture.bounds),
            "Published capture volumes never expand through solid walls");
  auto sparse = grid;
  sparse.tasks.clear(); // a coarsened grid can entirely miss low-ceiling rooms
  const auto automatic = p::place(authored, sparse, 4, 8);
  require(automatic.size() == 2,
          "Empty-leaf candidates preserve both rooms without grid points or authored seeds");
  const auto sites = p::regionSites(separated, room, .05f, 64);
  require(sites.size() == 2 && sites[0].enclosedAxes == 3 && sites[1].enclosedAxes == 3,
          "Axial enclosure crosses empty cluster boundaries and finds actual solid walls");
  for (const auto &site : sites) {
    const Vec3 r{.05f, .05f, .05f};
    require(p::emptyRegion(separated,
                           box({site.position[0] - r[0], site.position[1] - r[1], site.position[2] - r[2]},
                               {site.position[0] + r[0], site.position[1] + r[1], site.position[2] + r[2]})),
            "Automatic origins preserve whole-volume clearance");
  }
  const std::vector<Vec3> onlyValidated{{-2, 0, 0}};
  const auto restricted = p::place(authored, sparse, 4, 8, {}, .5f, &onlyValidated);
  require(restricted.size() == 1 && restricted[0].position[0] == -2,
          "GPU-rejected candidates cannot re-enter through automatic fitting");
  const auto unequal = unequalRooms();
  const std::vector<Vec3> earlyLedges{{-9, 0, .2f}, {-5, 0, .2f}, {4, 0, 1.5f}};
  const auto coverage = p::place(unequal, sparse, 2, 8, {}, 0, &earlyLedges);
  require(coverage.size() == 2 &&
              std::any_of(coverage.begin(), coverage.end(), [](const auto &capture) {
                return capture.bounds.minimum[0] >= 0 && capture.bounds.maximum[0] > 7.9f;
              }),
          "Early thin regions cannot consume the budget before a later substantial room");
  for (const auto &capture : coverage)
    require(p::emptyRegion(*unequal.partition, capture.bounds),
            "Coverage selection changes priority without weakening room boundaries");
  std::vector<p::RegionSite> clustered;
  for (uint32_t i = 0; i < 64; ++i)
    clustered.push_back({{float(i) * .001f, 0, 0}, .1f, 3});
  clustered.push_back({{10, 0, 0}, .1f, 3});
  require(p::spreadSites(clustered, 2, 1) == std::vector<size_t>{0, 64},
          "The fitting work budget reaches a distant region despite many nearby BSP fragments");
  const p::RegionSite nearFloor[] = {{{0, 0, 0}, 1, 3}, {{10, 0, 0}, .05f, 3}, {{9, 0, 1}, 1, 3}};
  require(p::spreadSites(nearFloor, 2, 1) == std::vector<size_t>{0, 2},
          "A slightly farther boundary sliver cannot displace a well-cleared origin in the same room");
  const p::FittedRegion adjoining[] = {
      {box({-2, 0, 0}, {0, 2, 2}), {{-1, 1, 1}, .1f, 3}},
      {box({0, 0, 0}, {2, 2, 2}), {{1, 1, 1}, .1f, 3}},
      {box({-2, 0, 0}, {2, 2, 2}), {{0, 1, 1}, .1f, 2}}};
  require(p::selectRegions(adjoining, 3) == std::vector<size_t>{0, 1},
          "Union coverage refuses a redundant region even when no single prior box contains it");
  const p::FittedRegion enclosing[] = {
      {box({-1, -1, -1}, {1, 1, 1}), {{0, 0, 0}, .1f, 3}},
      {box({-2, -2, -2}, {2, 2, 2}), {{0, 0, 0}, .1f, 2}}};
  require(p::selectRegions(enclosing, 2) == std::vector<size_t>{0, 1},
          "Containing an earlier small box cannot disqualify a region that adds substantial space");
  authored.reflectionHints = {{0, 0, 0}, {-2, 0, 0}, {-2, .2f, 0}, {2, 0, 0}};
  const auto seeded = p::place(authored, sparse, 4, 8);
  require(seeded.size() == 2 && seeded[0].position[0] == -2 && seeded[1].position[0] == 2,
          "Authored seeds reach rooms without grid probes, reject solid origins and merge duplicate fits");
  for (const auto &capture : seeded)
    require(p::emptyRegion(separated, capture.bounds), "Seed hints cannot override a separating wall");
  require(!p::regionSite(separated, {0, 0, 0}, .05f, 64),
          "Authored candidate assessment rejects a solid origin");
  // Stretch the right chamber beyond the enclosure search range. An outdoor
  // authored seed must not consume the only slot before an automatic room site.
  auto openSide = separated;
  openSide.nodes[1].plane[3] = -200;
  authored.maximum[0] = 200;
  authored.partition = std::make_shared<scene::VisibilityPartition>(openSide);
  authored.reflectionHints = {{100, 0, 0}};
  const auto ranked = p::candidateSites(authored, sparse, 1);
  require(!ranked.empty() && ranked.front()[0] < 0,
          "Sheltered automatic rooms rank before open authored cubemap positions");
  const auto indoor = p::place(authored, sparse, 1, 8);
  require(indoor.size() == 1 && indoor[0].bounds.maximum[0] < 0,
          "A limited capture budget still reaches the enclosed room");
  authored.reflectionHints.push_back({-2, .4f, 0});
  const auto preferred = p::candidateSites(authored, sparse, 1);
  require(preferred.front() == Vec3{-2, .4f, 0},
          "Authored origins retain priority among equally enclosed candidates");
  scene::VisibilityPartition angled;
  angled.closed = true;
  angled.clusterCount = 1;
  angled.leafClusters = {-1, 0};
  angled.nodes.push_back({{1, 1, 0, 0}, {-2, -1}, {0, 0}});
  const auto diagonal = p::fitRegion(angled, box({-3, -3, -3}, {3, 3, 3}), {1, 1, 0}, .01f);
  require(diagonal && diagonal->minimum[0] + diagonal->minimum[1] >= 0 && p::emptyRegion(angled, *diagonal),
          "Oblique BSP planes constrain the whole box footprint");
  const auto obliqueSites = p::regionSites(angled, box({-3, -3, -3}, {3, 3, 3}), .05f, 64);
  require(!obliqueSites.empty() && obliqueSites[0].position[0] + obliqueSites[0].position[1] > .1f,
          "Oblique cell bounds cannot approve an origin on the solid boundary");
  scene::VisibilityPartition diamond;
  diamond.closed = true;
  diamond.clusterCount = 1;
  diamond.leafClusters = {-1, 0};
  const std::array<remix::forward::Vec4, 4> walls = {
      {{1, 1, 0, 2}, {1, -1, 0, 2}, {-1, 1, 0, 2}, {-1, -1, 0, 2}}};
  for (uint32_t i = 0; i < walls.size(); ++i) {
    scene::VisibilityPartition::Node node{};
    std::copy(walls[i].begin(), walls[i].end(), node.plane);
    node.children[0] = -1;
    node.children[1] = i + 1 == walls.size() ? -2 : int32_t(i + 1);
    diamond.nodes.push_back(node);
  }
  const auto balanced = p::fitRegion(diamond, box({-3, -3, -1}, {3, 3, 1}), {0, 0, 0}, .01f);
  require(balanced && p::emptyRegion(diamond, *balanced) &&
              balanced->maximum[0] - balanced->minimum[0] > 1.9f &&
              balanced->maximum[1] - balanced->minimum[1] > 1.9f,
          "A broad diagonal room cannot collapse to a margin-wide axis-growth sliver");
  auto cyclic = angled;
  cyclic.nodes[0] = {{1, 0, 0, 0}, {0, -2}, {0, 0}};
  require(!p::emptyRegion(cyclic, box({1, -1, -1}, {2, 1, 1})),
          "Cyclic partition traversal fails closed without hanging");
  require(p::regionSites(cyclic, room, .05f, 64).empty(), "Cyclic candidate search fails closed");
  cyclic.nodes[0].plane[0] = std::numeric_limits<float>::quiet_NaN();
  require(!p::emptyRegion(cyclic, interior), "Non-finite partition planes cannot approve a region");
  require(!p::emptyRegion(splitRoom, box({0, 0, 0}, {0, 1, 1})),
          "Zero-volume regions cannot approve occupancy");
  require(!p::fitRegion(splitRoom, room, {0, 0, 0}, 0), "Zero clearance is refused");
  std::stop_source stop;
  stop.request_stop();
  bool cancelled = false;
  try {
    p::fitRegion(splitRoom, room, {0, 0, 0}, .01f, stop.get_token());
  } catch (const std::runtime_error &) {
    cancelled = true;
  }
  bool sitesCancelled = false;
  try {
    p::regionSites(splitRoom, room, .05f, 64, stop.get_token());
  } catch (const std::runtime_error &) {
    sitesCancelled = true;
  }
  require(sitesCancelled, "Automatic site search supports cancellation before traversal");
  bool receiversCancelled = false;
  try {
    p::receiverRegion(splitRoom, *fit, .02f, stop.get_token());
  } catch (const std::runtime_error &) {
    receiversCancelled = true;
  }
  require(receiversCancelled, "Receiver-boundary snapping supports map-change cancellation");
  bool selectionCancelled = false;
  try {
    p::selectRegions(adjoining, 3, stop.get_token());
  } catch (const std::runtime_error &) {
    selectionCancelled = true;
  }
  require(selectionCancelled, "Map changes cancel marginal coverage selection before allocating its work");
  require(cancelled, "Map changes can cancel region fitting before expensive work");
  std::puts("BSP room fitting: empty leaf merging, interior solids, thin walls, oblique planes and "
            "cancellation passed");
}
