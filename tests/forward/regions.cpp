#include "forward/probes/regions.h"
#include "forward/probes/data.h"
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
} // namespace
int main() {
  auto splitRoom = partition();
  const auto interior = box({-3.99f, -2.99f, -1.99f}, {3.99f, 2.99f, 1.99f});
  require(p::emptyRegion(splitRoom, interior), "Artificial empty visleaf splits do not become room walls");
  require(!p::emptyRegion(splitRoom, box({-5, -3, -2}, {4, 3, 2})),
          "Boxes extending outside the map are refused");
  const auto fit = p::fitRegion(splitRoom, room, {-.75f, .5f, 0}, .01f);
  require(fit && p::emptyRegion(splitRoom, *fit), "Fitted region is wholly empty");
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
  const auto wall = box({-.01f, -3, -2}, {.01f, 3, 2});
  const auto separated = partition(&wall);
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
  authored.reflectionHints = {{0, 0, 0}, {-2, 0, 0}, {-2, .2f, 0}, {2, 0, 0}};
  const auto seeded = p::place(authored, sparse, 4, 8);
  require(seeded.size() == 2 && seeded[0].position[0] == -2 && seeded[1].position[0] == 2,
          "Authored seeds reach rooms without grid probes, reject solid origins and merge duplicate fits");
  for (const auto &capture : seeded)
    require(p::emptyRegion(separated, capture.bounds), "Seed hints cannot override a separating wall");
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
  require(cancelled, "Map changes can cancel region fitting before expensive work");
  std::puts("BSP room fitting: empty leaf merging, interior solids, thin walls, oblique planes and "
            "cancellation passed");
}
