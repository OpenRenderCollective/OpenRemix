#include "forward/bake/refinement_builder.h"
#include "forward/bake/receiver_support.h"
#include "forward/bake/relocation_cache.h"
#include "forward/lightmap/layout.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace remix::forward::bake {
  namespace {
    using Coordinate = std::array<uint32_t, 3>;
    using Key = std::pair<Coordinate, Vec3>;
    Coordinate coordinate(uint32_t node, const UInt4 &dimensions) {
      Coordinate result;
      for (uint32_t axis = 0; axis < 3; ++axis) {
        result[axis] = node % dimensions[axis];
        node /= dimensions[axis];
      }
      return result;
    }
    struct Patch {
      uint32_t factor = 0;
      std::vector<uint32_t> records;
    };
  }
  uint32_t buildRefinement(Grid &grid, const scene::BakeScene &scene, const lightmap::Layout &layout,
                           const Refinement &policy, std::span<const std::vector<uint32_t>> receivers,
                           std::span<const uint32_t> priority, std::stop_token cancelled) {
    auto check = [&] {
      if (cancelled.stop_requested()) throw std::runtime_error("Probe refinement superseded");
    };
    const auto header = grid.header;
    const bool timing = std::getenv("REMIX_DEBUG_BAKE_TIMING") != nullptr;
    double neighbourhoodMs = 0, supportMs = 0, relocationMs = 0;
    uint64_t receiverVisits = 0;
    uint32_t attempts = 0;
    auto measured = [&](double &milliseconds, auto &&work) {
      const auto began = timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      auto result = work();
      if (timing) milliseconds += std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - began).count();
      return result;
    };
    const uint32_t nodes = gridNodes(header), coarseRecords = uint32_t(grid.probes.size());
    std::map<Key, uint32_t> records;
    for (uint32_t node = 0; node < nodes; ++node) {
      if ((node & 1023u) == 0) check();
      const uint32_t record = grid.lookup[node];
      if (!record) continue;
      auto key = coordinate(node, header.dimensions);
      for (auto &axis : key) axis *= policy.factor;
      const auto &p = grid.points[record - 1].position;
      records.emplace(Key{key, {p[0], p[1], p[2]}}, record);
    }
    std::map<uint32_t, Patch> patches;
    std::vector<uint32_t> uses(coarseRecords);
    uint32_t active = coarseRecords;
    bool relocated = header.settings[2] == 3;
    std::optional<RelocationCache> relocationCache;
    if (policy.relocate) relocationCache.emplace(scene, layout);
    for (uint32_t factor = 2; factor <= policy.factor; factor *= 2) {
      const uint32_t side = factor + 1, fineNodes = side * side * side;
      const float spacing = header.originSpacing[3] / factor;
      // Bound directory/work growth consistently across all refinement levels.
      for (uint32_t ordinal = 0; ordinal < std::min<size_t>(priority.size(), maximumRefinedCells); ++ordinal) {
        check();
        const uint32_t cell = priority[ordinal];
        const auto parent = coordinate(cell, header.dimensions);
        Grid fine;
        fine.header.originSpacing[3] = spacing;
        fine.header.dimensions = {side, side, side, fineNodes};
        fine.header.settings = {header.settings[0] / factor, header.settings[1] / (factor * factor), 0, 0};
        for (uint32_t axis = 0; axis < 3; ++axis)
          fine.header.originSpacing[axis] = header.originSpacing[axis] + parent[axis] * header.originSpacing[3];
        lightmap::Layout local;
        // At 2x, any receiver in the cell already supports the whole 3^3 patch.
        // Only relocation needs neighbours, within its .45-cell reach. Avoid
        // scanning a full root-cell halo for this first coverage pass.
        const float searchHalo = (factor == 2 ? spacing * .45f : spacing * 2) + .25f / scene.sceneScale;
        ++attempts;
        const auto neighbourhood = measured(neighbourhoodMs, [&] {
          return nearbyReceivers(header, layout, receivers, cell, searchHalo, cancelled);
        });
        receiverVisits += neighbourhood.size();
        local.points.reserve(neighbourhood.size());
        for (uint32_t receiver : neighbourhood) local.points.push_back(layout.points[receiver]);
        const auto support = measured(supportMs, [&] {
          return receiverSupport(fine.header, local, spacing * 2, cancelled);
        });
        fine.points.resize(fineNodes);
        fine.probes.resize(fineNodes);
        std::vector<Coordinate> positions(fineNodes);
        for (uint32_t node = 0; node < fineNodes; ++node) {
          const auto localPosition = coordinate(node, fine.header.dimensions);
          for (uint32_t axis = 0; axis < 3; ++axis) {
            positions[node][axis] = parent[axis] * policy.factor + localPosition[axis] * (policy.factor / factor);
            fine.points[node].position[axis] = header.originSpacing[axis] +
                positions[node][axis] * (header.originSpacing[3] / policy.factor);
          }
          if (support[node] && scene.partition->cluster(fine.points[node].position.data()) >= 0)
            fine.tasks.push_back(node);
        }
        bool moved = false;
        if (policy.relocate) {
          moved = measured(relocationMs, [&] {
            return relocate(fine, scene, local, cancelled, &*relocationCache, neighbourhood);
          }) != 0;
          std::erase_if(fine.tasks, [&](uint32_t node) { return !support[node]; });
        }
        Patch next{factor, std::vector<uint32_t>(fineNodes)};
        struct Pending { uint32_t record; Point point; };
        std::map<Key, Pending> pending;
        for (uint32_t node : fine.tasks) {
          const auto &point = fine.points[node];
          const Key key{positions[node], {point.position[0], point.position[1], point.position[2]}};
          if (const auto found = records.find(key); found != records.end()) next.records[node] = found->second;
          else {
            const uint32_t record = uint32_t(grid.probes.size() + pending.size()) + 1;
            next.records[node] = pending.try_emplace(key, Pending{record, point}).first->second.record;
          }
        }
        const auto previous = patches.find(cell);
        std::map<uint32_t, int32_t> changes;
        if (previous != patches.end()) {
          const auto &old = previous->second;
          const uint32_t oldSide = old.factor + 1, ratio = factor / old.factor;
          for (uint32_t node = 0; node < old.records.size(); ++node) {
            const uint32_t record = old.records[node];
            if (!record) continue;
            --changes[record];
            const auto position = coordinate(node, {oldSide, oldSide, oldSide, 0});
            const uint32_t target = ((position[2] * ratio * side + position[1] * ratio) * side + position[0] * ratio);
            // Retain the previous level's outer support when a denser level's
            // physical halo is smaller. Otherwise an upgrade could create holes.
            if (!next.records[target]) next.records[target] = record;
          }
        }
        for (uint32_t record : next.records) if (record) ++changes[record];
        if (std::none_of(next.records.begin(), next.records.end(),
                         [&](uint32_t record) { return record > coarseRecords; })) continue;
        int64_t proposed = active;
        for (const auto &[record, change] : changes) {
          if (record <= coarseRecords) continue;
          const uint32_t before = record <= uses.size() ? uses[record - 1] : 0;
          const int64_t after = int64_t(before) + change;
          if (after < 0) throw std::logic_error("Invalid refinement record ownership");
          proposed += int64_t(after > 0) - int64_t(before > 0);
        }
        if (proposed > policy.recordBudget || proposed == coarseRecords) continue;
        grid.points.resize(grid.points.size() + pending.size());
        grid.probes.resize(grid.probes.size() + pending.size());
        uses.resize(grid.probes.size());
        for (const auto &[key, value] : pending) {
          records.emplace(key, value.record);
          grid.points[value.record - 1] = value.point;
        }
        for (const auto &[record, change] : changes)
          uses[record - 1] = uint32_t(int64_t(uses[record - 1]) + change);
        active = uint32_t(proposed);
        relocated |= moved;
        patches.insert_or_assign(cell, std::move(next));
      }
    }
    if (timing)
      std::fprintf(stderr, "Forward refinement planning: %u patch attempts, %llu receiver visits, "
                           "neighbourhood_ms=%.3f support_ms=%.3f relocation_ms=%.3f\n",
                   attempts, static_cast<unsigned long long>(receiverVisits),
                   neighbourhoodMs, supportMs, relocationMs);
    if (timing && relocationCache)
      std::fprintf(stderr, "Forward refinement clearance: %llu queries, %llu reused, %zu scratch bytes\n",
                   static_cast<unsigned long long>(relocationCache->queries()),
                   static_cast<unsigned long long>(relocationCache->hits()),
                   layout.points.size() * (sizeof(Vec4) + sizeof(uint8_t)));
    if (patches.empty()) return 0;
    check();
    Grid result;
    result.header = header;
    result.header.dimensions[3] = active;
    result.header.settings[2] = relocated ? 5 : 4;
    result.tasks = grid.tasks; // Completed coarse indices remain unchanged.
    std::vector<uint32_t> remap(grid.probes.size() + 1);
    for (uint32_t record = 1; record <= grid.probes.size(); ++record) {
      if (record > coarseRecords && !uses[record - 1]) continue;
      remap[record] = uint32_t(result.probes.size()) + 1;
      if (record > coarseRecords) result.tasks.push_back(uint32_t(result.probes.size()));
      result.points.push_back(grid.points[record - 1]);
      result.probes.push_back(grid.probes[record - 1]);
    }
    if (result.probes.size() != active) throw std::logic_error("Invalid refinement record budget");
    result.lookup.resize((nodes * 2 + 7) & ~3u);
    std::copy_n(grid.lookup.begin(), nodes, result.lookup.begin());
    result.lookup[nodes] = adaptiveDirectoryMagic;
    result.lookup[nodes + 1] = policy.factor;
    result.lookup[nodes + 3] = coarseRecords;
    for (const auto &[cell, patch] : patches) {
      const uint32_t block = uint32_t(result.lookup.size());
      result.lookup[nodes + 4 + cell] = patchReference(block, patch.factor);
      result.lookup.resize(block + ((patch.records.size() + 3) & ~size_t(3)));
      for (uint32_t node = 0; node < patch.records.size(); ++node)
        result.lookup[block + node] = remap[patch.records[node]];
    }
    result.lookup[nodes + 2] = uint32_t(result.lookup.size());
    const auto shared = sharedPatchFaces(result);
    for (uint32_t cell = 0; cell < nodes; ++cell)
      result.lookup[nodes + 4 + cell] |= uint32_t(shared[cell]) << 26;
    result.origins = directoryOrigins(result);
    grid = std::move(result);
    return uint32_t(patches.size());
  }
}
