#include "forward/lightmap/seams.h"
#include "forward/lightmap/surface_walk.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace remix::forward::lightmap {
  void extendCurvedSeams(const scene::BakeScene &scene, Layout &layout, uint32_t padding,
                         std::stop_token cancelled) {
    if (layout.luxelMetres <= 0)
      return;
    seam::SurfaceWalk mesh(scene, layout, cancelled);
    struct Donor {
      bake::Point point;
      uint32_t chart;
      double distance;
    };
    std::map<uint32_t, Donor> donors;
    for (auto seed : mesh.joins) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Curved seam padding superseded");
      const auto &triangle = mesh.triangles[seed / 3];
      const auto &allocation = layout.charts[triangle.chart];
      const auto a = triangle.uv[seed % 3], b = triangle.uv[(seed % 3 + 1) % 3];
      const seam::Uv direction{b[0] - a[0], b[1] - a[1]};
      const double length2 = direction[0] * direction[0] + direction[1] * direction[1];
      if (length2 <= 1e-20)
        continue;
      const int minimumX = std::max(-int(padding), int(std::ceil(std::min(a[0], b[0]) - 1))),
                maximumX =
                    std::min(int(allocation.width + padding) - 1, int(std::floor(std::max(a[0], b[0]) + 1))),
                minimumY = std::max(-int(padding), int(std::ceil(std::min(a[1], b[1]) - 1))),
                maximumY =
                    std::min(int(allocation.height + padding) - 1, int(std::floor(std::max(a[1], b[1]) + 1)));
      for (int y = minimumY; y <= maximumY; ++y)
        for (int x = minimumX; x <= maximumX; ++x) {
          const uint32_t atlas =
              (allocation.page * layout.pageSize + allocation.y + y) * layout.pageSize + allocation.x + x;
          const auto owner = layout.owners[atlas];
          if (!layout.footprints[atlas] || owner == unowned)
            continue;
          // Existing samples inside the receiver are authoritative. Only the
          // outside-centre part of a bilinear footprint needs a neighbour.
          if (layout.addresses[owner] == atlas && layout.pointCharts[owner] == triangle.chart &&
              layout.sampleCenters[owner])
            continue;
          const seam::Uv target{double(x), double(y)};
          const double t =
              std::clamp(((x - a[0]) * direction[0] + (y - a[1]) * direction[1]) / length2, 0., 1.);
          const double dx = x - a[0] - t * direction[0], dy = y - a[1] - t * direction[1],
                       distance = dx * dx + dy * dy;
          if (distance > 2.000001)
            continue;
          const auto found = donors.find(atlas);
          if (found != donors.end() && found->second.distance <= distance)
            continue;
          if (auto point = mesh.across(seed, target, cancelled))
            donors.insert_or_assign(atlas, Donor{*point, triangle.chart, distance});
        }
    }
    // Preserve the original receiver samples for compact blocked/clear proofs.
    // Filtering-only donors are baked again with the receiving chart's light
    // identities, so independently ranked mixed-light slots cannot be mixed up.
    for (const auto &[atlas, donor] : donors) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Curved seam publication superseded");
      const auto owner = layout.owners[atlas];
      if (layout.points[owner].position == donor.point.position &&
          layout.points[owner].normal == donor.point.normal)
        continue;
      layout.owners[atlas] = uint32_t(layout.points.size());
      layout.points.push_back(donor.point);
      layout.pointCharts.push_back(donor.chart);
      layout.addresses.push_back(atlas);
      layout.sampleCenters.push_back(1);
      layout.receiverSamples.push_back(0);
      if (!layout.receiverSources.empty()) layout.receiverSources.push_back(unowned);
      ++layout.seamSamples;
      ++layout.curvedSeamSamples;
    }
  }
} // namespace remix::forward::lightmap
