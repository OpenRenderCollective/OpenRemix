#include "forward/lightmap/interior_donors.h"
#include <cmath>
#include <stdexcept>

namespace remix::forward::lightmap {
  namespace {
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested()) throw std::runtime_error("Interior donor repair superseded");
    }
    bool interior(const bake::Result &value) {
      const auto &s = value.statistics;
      // Require strong evidence of enclosure. Sparse, open or ambiguous samples
      // remain usable; volume-probe validity has its own, less strict policy.
      return std::isfinite(s[0]) && s[0] >= .95f && s[0] <= 1 &&
             std::isfinite(s[1]) && s[1] >= 0 && s[1] <= .01f &&
             std::isfinite(s[2]) && s[2] >= 64;
    }
  }
  InteriorDonors repairInteriorDonors(Layout &layout, std::span<const bake::Result> results,
                                      std::stop_token cancelled) {
    check(cancelled);
    if (results.size() != layout.points.size() || layout.pointCharts.size() != results.size() ||
        layout.owners.size() != uint64_t(layout.pageSize) * layout.pageSize * layout.pages)
      throw std::invalid_argument("Interior donor repair requires a complete surface layout");
    InteriorDonors summary;
    std::vector<uint8_t> hidden(results.size());
    for (size_t i = 0; i < results.size(); ++i) {
      if ((i & 1023u) == 0) check(cancelled);
      hidden[i] = interior(results[i]);
      summary.hiddenSamples += hidden[i];
    }
    if (!summary.hiddenSamples) return summary;
    std::vector<uint32_t> donors, queue;
    for (uint32_t chart = 0; chart < layout.charts.size(); ++chart) {
      check(cancelled);
      const auto &r = layout.charts[chart];
      if (!r.width || !r.height || r.page >= layout.pages ||
          uint64_t(r.x) + r.width > layout.pageSize || uint64_t(r.y) + r.height > layout.pageSize ||
          uint64_t(r.width) * r.height > UINT32_MAX)
        throw std::invalid_argument("Invalid interior donor chart extent");
      const auto address = [&](uint32_t pixel) {
        return (size_t(r.page) * layout.pageSize + r.y + pixel / r.width) * layout.pageSize +
               r.x + pixel % r.width;
      };
      const uint32_t count = r.width * r.height;
      uint32_t covered = 0;
      for (uint32_t pixel = 0; pixel < count; ++pixel) {
        if ((pixel & 1023u) == 0) check(cancelled);
        const auto owner = layout.owners[address(pixel)];
        if (owner == unowned) continue;
        if (owner >= hidden.size() || layout.pointCharts[owner] != chart)
          throw std::invalid_argument("Interior donor belongs to another chart");
        covered += hidden[owner];
      }
      if (!covered) continue;
      donors.assign(count, unowned);
      queue.clear();
      for (uint32_t pixel = 0; pixel < count; ++pixel) {
        const auto owner = layout.owners[address(pixel)];
        if (owner != unowned && !hidden[owner]) {
          donors[pixel] = owner;
          queue.push_back(pixel);
        }
      }
      if (queue.empty()) {
        ++summary.unresolvedCharts;
        continue;
      }
      // Deterministic nearest donor in four-neighbour grid distance. Flood-fill
      // scratch storage covers one affected chart, with no extra ray queries.
      for (size_t head = 0; head < queue.size(); ++head) {
        if ((head & 1023u) == 0) check(cancelled);
        const auto pixel = queue[head];
        const auto visit = [&](uint32_t neighbour) {
          if (donors[neighbour] != unowned) return;
          donors[neighbour] = donors[pixel];
          queue.push_back(neighbour);
        };
        if (pixel % r.width) visit(pixel - 1);
        if (pixel % r.width + 1 < r.width) visit(pixel + 1);
        if (pixel >= r.width) visit(pixel - r.width);
        if (pixel < count - r.width) visit(pixel + r.width);
      }
      for (uint32_t pixel = 0; pixel < count; ++pixel) {
        if ((pixel & 1023u) == 0) check(cancelled);
        auto &owner = layout.owners[address(pixel)];
        if (owner != unowned && hidden[owner]) {
          owner = donors[pixel];
          ++summary.repairedTexels;
        }
      }
    }
    return summary;
  }
} // namespace remix::forward::lightmap
