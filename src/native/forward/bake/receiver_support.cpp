#include "forward/bake/receiver_support.h"
#include "forward/lightmap/layout.h"
#include <algorithm>
#include <cmath>

namespace remix::forward::bake {
  std::vector<uint8_t> receiverSupport(const GridHeader &header, const lightmap::Layout &layout,
                                      float halo, std::stop_token cancelled) {
    const auto &d = header.dimensions;
    const uint32_t count = gridNodes(header);
    const float spacing = header.originSpacing[3];
    if (!std::isfinite(halo) || halo < 0 || !std::isfinite(spacing) || spacing <= 0 ||
        (!layout.receiverSamples.empty() && layout.receiverSamples.size() != layout.points.size()))
      throw std::invalid_argument("Invalid receiver support inputs");
    for (uint32_t axis = 0; axis < 3; ++axis)
      if (!std::isfinite(header.originSpacing[axis]))
        throw std::invalid_argument("Invalid receiver support origin");
    auto check = [&] {
      if (cancelled.stop_requested()) throw std::runtime_error("Receiver support superseded");
    };
    check();
    std::vector<uint8_t> cells(count), nodes(count);
    for (size_t i = 0; i < layout.points.size(); ++i) {
      if ((i & 1023u) == 0) check();
      if (!layout.receiverSamples.empty() && !layout.receiverSamples[i]) continue;
      std::array<uint32_t, 3> cell;
      bool inside = true;
      for (uint32_t axis = 0; axis < 3; ++axis) {
        const float q = (layout.points[i].position[axis] - header.originSpacing[axis]) / spacing;
        if (!std::isfinite(q)) throw std::invalid_argument("Non-finite receiver support position");
        const double margin = double(halo) / spacing;
        if (q < -margin || q > double(d[axis] - 1) + margin) { inside = false; break; }
        cell[axis] = uint32_t(std::clamp(q, 0.f, float(d[axis] - 2)));
      }
      if (inside) cells[(cell[2] * d[1] + cell[1]) * d[0] + cell[0]] = 1;
    }
    // Mark complete trilinear support once per occupied cell.
    for (uint32_t node = 0; node < count; ++node) {
      if ((node & 1023u) == 0) check();
      if (!cells[node]) continue;
      for (uint32_t z = 0; z < 2; ++z)
        for (uint32_t y = 0; y < 2; ++y)
          for (uint32_t x = 0; x < 2; ++x)
            nodes[node + (z * d[1] + y) * d[0] + x] = 1;
    }
    // Separable dilation costs O(nodes), even for an extremely large halo.
    // Visiting every receiver's expanded box would scale with halo volume.
    const uint32_t radius = uint32_t(std::min(double(count), std::ceil(double(halo) / spacing)));
    if (!radius) return nodes;
    uint32_t stride = 1;
    std::vector<uint8_t> expanded(count);
    for (uint32_t axis = 0; axis < 3; ++axis) {
      const uint32_t extent = d[axis], groups = count / (extent * stride);
      std::vector<uint32_t> prefix(extent + 1);
      for (uint32_t group = 0; group < groups; ++group)
        for (uint32_t line = 0; line < stride; ++line) {
          check();
          const uint32_t first = group * extent * stride + line;
          prefix[0] = 0;
          for (uint32_t i = 0; i < extent; ++i) prefix[i + 1] = prefix[i] + nodes[first + i * stride];
          for (uint32_t i = 0; i < extent; ++i) {
            const uint32_t low = i > radius ? i - radius : 0;
            const uint32_t high = uint32_t(std::min(uint64_t(extent), uint64_t(i) + radius + 1));
            expanded[first + i * stride] = prefix[high] != prefix[low];
          }
        }
      nodes.swap(expanded);
      stride *= extent;
    }
    return nodes;
  }
  std::vector<uint32_t> nearbyReceivers(const GridHeader &header, const lightmap::Layout &layout,
                                       std::span<const std::vector<uint32_t>> bins, uint32_t cell,
                                       float halo, std::stop_token cancelled) {
    const auto &d = header.dimensions;
    const uint32_t nodes = gridNodes(header);
    const float spacing = header.originSpacing[3];
    if (bins.size() != nodes || cell >= nodes || !std::isfinite(spacing) || spacing <= 0 ||
        !std::isfinite(halo) || halo < 0)
      throw std::invalid_argument("Invalid receiver neighbourhood inputs");
    const std::array<uint32_t, 3> parent{cell % d[0], (cell / d[0]) % d[1], cell / (d[0] * d[1])};
    std::array<uint32_t, 3> low, high;
    Vec3 minimum, maximum;
    const uint32_t radius = uint32_t(std::min(double(nodes), std::max(1., std::ceil(double(halo) / spacing))));
    for (uint32_t axis = 0; axis < 3; ++axis) {
      if (parent[axis] + 1 >= d[axis]) throw std::invalid_argument("Invalid receiver cell");
      low[axis] = parent[axis] > radius ? parent[axis] - radius : 0;
      high[axis] = uint32_t(std::min(uint64_t(d[axis] - 2), uint64_t(parent[axis]) + radius));
      minimum[axis] = header.originSpacing[axis] + parent[axis] * spacing - halo;
      maximum[axis] = header.originSpacing[axis] + (parent[axis] + 1) * spacing + halo;
    }
    std::vector<uint32_t> result;
    for (uint32_t z = low[2]; z <= high[2]; ++z)
      for (uint32_t y = low[1]; y <= high[1]; ++y)
        for (uint32_t x = low[0]; x <= high[0]; ++x) {
          if (cancelled.stop_requested()) throw std::runtime_error("Receiver neighbourhood superseded");
          const auto &bin = bins[(z * d[1] + y) * d[0] + x];
          for (uint32_t index : bin) {
            const auto &point = layout.points.at(index).position;
            bool inside = true;
            for (uint32_t axis = 0; axis < 3; ++axis)
              inside &= point[axis] >= minimum[axis] && point[axis] <= maximum[axis];
            if (inside) result.push_back(index);
          }
        }
    std::sort(result.begin(), result.end());
    return result;
  }
}
