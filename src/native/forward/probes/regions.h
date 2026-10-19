#pragma once
#include "forward/lighting.h"
#include "scene/light_visibility.h"
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

namespace remix::forward::probes {
  // Conservative whole-volume occupancy, independent of PVS visibility. A
  // box may span any number of empty leaves, but must intersect no solid or
  // outside leaf. Invalid/cyclic partitions fail closed.
  bool emptyRegion(const scene::VisibilityPartition &partition, const Bounds &bounds);
  struct RegionTolerance {
    // Maximum thickness of trim in two axes, in scene units. Zero is strict.
    float detailDepth = 0;
    float maxIgnoredFraction = .08f;
  };
  // Worker-local scratch reused for repeated occupancy tests. The immutable
  // partition must outlive this query; separate workers own separate queries.
  class RegionQuery {
  public:
    explicit RegionQuery(const scene::VisibilityPartition &partition, std::stop_token cancelled = {});
    ~RegionQuery();
    bool contains(const Bounds &bounds);
    bool contains(const Bounds &bounds, RegionTolerance tolerance);

  private:
    struct State;
    std::unique_ptr<State> m_state;
  };
  struct RegionSite {
    Vec3 position{};
    float clearance = 0;
    uint32_t enclosedAxes = 0;
  };
  // Assess authored and automatic origins with the same clearance and axial
  // enclosure rules. Solid or malformed origins are refused before GPU testing.
  std::optional<RegionSite> regionSite(const scene::VisibilityPartition &partition, Vec3 position,
                                       float clearance, float enclosureRange,
                                       std::stop_token cancelled = {});
  // Empty-leaf candidates independent of irradiance spacing. Conservative cell
  // bounds are only a search aid: each origin is checked against actual space.
  // Nearby opposite solid boundaries rank sheltered rooms before open areas.
  std::vector<RegionSite> regionSites(const scene::VisibilityPartition &partition, const Bounds &limit,
                                      float clearance, float enclosureRange, std::stop_token cancelled = {});
  // Approximate a large axis-aligned box containing the capture site.
  // Optional detail tolerance permits bounded trim/clutter inside the proxy.
  // Several growth orders reduce the bias of greedy face expansion. This is
  // not a globally maximal room decomposition, especially for rotated rooms.
  // One balanced interior seed also competes when it improves the existing
  // boxes beyond their face-search uncertainty, avoiding diagonal slivers.
  // Retain distinct growth results for the geometry audit. Largest volume comes
  // first, but a narrower doorway-spanning fit must not hide room-shaped alternatives.
  // At most three whole-volume-validated boxes, with the same capture origin.
  std::vector<Bounds> fitRegionCandidates(const scene::VisibilityPartition &partition, const Bounds &limit,
                                          Vec3 site, float margin, std::stop_token cancelled = {},
                                          RegionTolerance tolerance = {});
  std::optional<Bounds> fitRegion(const scene::VisibilityPartition &partition, const Bounds &limit, Vec3 site,
                                  float margin, std::stop_token cancelled = {},
                                  RegionTolerance tolerance = {});
  // Restore contact with nearby axis-aligned room surfaces for receivers.
  // Projection remains inset. Every snap must pass whole-volume occupancy;
  // influence never grows through a wall just because it is within the margin.
  Bounds receiverRegion(const scene::VisibilityPartition &partition,
                        const Bounds &projection, float snapDistance,
                        std::stop_token cancelled = {},
                        RegionTolerance tolerance = {},
                        float overlapDistance = 0);
} // namespace remix::forward::probes
