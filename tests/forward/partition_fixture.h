#pragma once
#include "forward/probes/regions.h"
#include "forward/probes/selection.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace remix::forward::test {
  // Local map regression fixture, not a BSP reader or a runtime interchange ABI.
  // Header: magic[8], node/leaf/cluster/site counts, scale, six bounds floats;
  // then 32-byte partition nodes, int32 clusters and XYZ float sites.
  inline int partitionFixture(const char *path) try {
    static_assert(std::endian::native == std::endian::little);
    static_assert(sizeof(scene::VisibilityPartition::Node) == 32);
    static_assert(sizeof(Vec3) == 12);
    const auto bytes = std::filesystem::file_size(path);
    if (bytes > (64u << 20))
      throw std::runtime_error("Partition fixture too large");
    std::ifstream input(path, std::ios::binary);
    input.exceptions(std::ios::failbit | std::ios::badbit);
    const auto read = [&](void *data, size_t size) { input.read(static_cast<char *>(data), size); };
    char magic[8];
    read(magic, sizeof(magic));
    if (std::string_view(magic, 8) != "RMXPART1")
      throw std::runtime_error("Unknown partition fixture");
    uint32_t counts[4];
    read(counts, sizeof(counts));
    float scale;
    read(&scale, sizeof(scale));
    Bounds limit;
    read(limit.minimum.data(), sizeof(Vec3));
    read(limit.maximum.data(), sizeof(Vec3));
    limit.valid = true;
    if (!counts[0] || !counts[1] || !counts[2] || counts[0] > 1000000 || counts[1] > 1000000 ||
        counts[2] > counts[1] || counts[3] > 128 || !std::isfinite(scale) || scale <= 0 ||
        bytes != 52ull + uint64_t(counts[0]) * 32 + uint64_t(counts[1]) * 4 + uint64_t(counts[3]) * 12)
      throw std::runtime_error("Invalid partition fixture dimensions");
    for (uint32_t c = 0; c < 3; ++c)
      if (!std::isfinite(limit.minimum[c]) || !std::isfinite(limit.maximum[c]) ||
          limit.minimum[c] >= limit.maximum[c])
        throw std::runtime_error("Invalid partition fixture bounds");
    scene::VisibilityPartition partition;
    partition.nodes.resize(counts[0]);
    partition.leafClusters.resize(counts[1]);
    partition.clusterCount = counts[2];
    std::vector<Vec3> sites(counts[3]);
    read(partition.nodes.data(), partition.nodes.size() * sizeof(partition.nodes[0]));
    read(partition.leafClusters.data(), partition.leafClusters.size() * sizeof(int32_t));
    read(sites.data(), sites.size() * sizeof(Vec3));
    for (const auto &site : sites)
      for (float coordinate : site)
        if (!std::isfinite(coordinate))
          throw std::runtime_error("Invalid fixture site");
    for (int32_t cluster : partition.leafClusters)
      if (cluster < -1 || cluster >= int32_t(partition.clusterCount))
        throw std::runtime_error("Invalid fixture cluster");
    for (const auto &node : partition.nodes) {
      double normal = 0;
      for (uint32_t c = 0; c < 4; ++c) {
        if (!std::isfinite(node.plane[c]))
          throw std::runtime_error("Invalid fixture plane");
        if (c < 3)
          normal += double(node.plane[c]) * node.plane[c];
      }
      if (normal == 0)
        throw std::runtime_error("Empty fixture plane");
      for (int32_t child : node.children)
        if ((child >= 0 && uint32_t(child) >= counts[0]) || (child < 0 && -1ll - child >= counts[1]))
          throw std::runtime_error("Invalid fixture child");
    }
    std::vector<uint8_t> seen(counts[0]);
    std::vector<std::pair<uint32_t, bool>> stack{{0, false}};
    while (!stack.empty()) {
      const auto [node, done] = stack.back();
      stack.pop_back();
      if (done) {
        seen[node] = 2;
        continue;
      }
      if (seen[node] == 1)
        throw std::runtime_error("Cyclic fixture partition");
      if (seen[node] == 2)
        continue;
      seen[node] = 1;
      stack.push_back({node, true});
      for (int32_t child : partition.nodes[node].children)
        if (child >= 0)
          stack.push_back({uint32_t(child), false});
    }
    partition.closed = true;
    const auto automatic = probes::regionSites(partition, limit, .05f / scale, 64.f / scale);
    const auto selected = probes::spreadSites(automatic, 128, 4.f / scale);
    uint32_t classes[4]{};
    for (const auto &site : automatic)
      ++classes[std::min(site.enclosedAxes, 3u)];
    probes::RegionQuery query(partition);
    std::printf("{\"nodes\":%u,\"leaves\":%u,\"automaticClasses\":[%u,%u,%u,%u],"
                "\"searchBoundsEmpty\":%s,\"searchBoundsDetailCompatible\":%s,\"sites\":[", counts[0],
                counts[1], classes[0], classes[1], classes[2], classes[3], query.contains(limit) ? "true" : "false",
                query.contains(limit, {.5f / scale}) ? "true" : "false");
    for (uint32_t i = 0; i < sites.size(); ++i) {
      const auto site = probes::regionSite(partition, sites[i], .05f / scale, 64.f / scale);
      std::printf("%s{\"position\":[%.9g,%.9g,%.9g],\"valid\":%s", i ? "," : "", sites[i][0], sites[i][1],
                  sites[i][2], site ? "true" : "false");
      if (site) {
        double nearest = INFINITY;
        for (const auto index : selected) {
          double distance = 0;
          for (uint32_t c = 0; c < 3; ++c) {
            const double delta = double(sites[i][c]) - automatic[index].position[c];
            distance += delta * delta;
          }
          nearest = std::min(nearest, distance);
        }
        if (std::isfinite(nearest))
          std::printf(",\"nearestSpreadSiteDistance\":%.9g", std::sqrt(nearest));
        std::printf(",\"clearance\":%.9g,\"enclosedAxes\":%u,\"bounds\":[", site->clearance,
                    site->enclosedAxes);
        const auto boxes =
            probes::fitRegionCandidates(partition, limit, site->position, .001f / scale, {}, {.5f / scale});
        auto printBox = [](const Bounds &b, uint32_t j) {
          std::printf("%s[[%.9g,%.9g,%.9g],[%.9g,%.9g,%.9g]]", j ? "," : "", b.minimum[0], b.minimum[1],
                      b.minimum[2], b.maximum[0], b.maximum[1], b.maximum[2]);
        };
        for (uint32_t j = 0; j < boxes.size(); ++j) printBox(boxes[j], j);
        std::printf("],\"influence\":[");
        for (uint32_t j = 0; j < boxes.size(); ++j)
          printBox(probes::receiverRegion(partition, boxes[j], .002f / scale, {}, {.5f / scale}, .5f / scale), j);
        std::printf("]");
      }
      std::printf("}");
    }
    std::printf("]}\n");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "Partition fixture: %s\n", error.what());
    return 1;
  }
} // namespace remix::forward::test
