#include "forward/probes/regions.h"
#include <bit>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>

using namespace remix;
using namespace remix::forward;
namespace probes = remix::forward::probes;
namespace {
  template <class T> T read(std::istream &input) {
    T value{};
    input.read(reinterpret_cast<char *>(&value), sizeof(value));
    if (!input) throw std::runtime_error("Truncated fit-study input");
    return value;
  }
  void writeBounds(std::ostream &output, const Bounds &bounds) {
    output << "[";
    for (uint32_t axis = 0; axis < 3; ++axis)
      output << (axis ? "," : "") << bounds.minimum[axis];
    for (uint32_t axis = 0; axis < 3; ++axis) output << "," << bounds.maximum[axis];
    output << "]";
  }
}

// Private offline study: a bounded partition and receiver/capture pairs, without
// textures, lighting transport, Vulkan or modifications to the source map.
// Little-endian input: RMXFIT01; four u32 counts (nodes, leaves, clusters,
// queries); six bounds floats, metres/unit and detail tolerance in metres;
// 32-byte partition nodes; i32 leaf clusters; six floats per capture/anchor pair.
// Output is JSONL with projection/influence bounds for both fitting policies.
int main(int argc, char **argv) try {
  static_assert(std::endian::native == std::endian::little);
  if (argc != 3) throw std::runtime_error("Usage: probe-fit-study input.bin output.jsonl");
  std::ifstream input(argv[1], std::ios::binary);
  if (read<uint64_t>(input) != 0x3130544946584d52ull)
    throw std::runtime_error("Unrecognized fit-study format"); // RMXFIT01
  const auto nodeCount = read<uint32_t>(input), leafCount = read<uint32_t>(input);
  const auto clusterCount = read<uint32_t>(input), queryCount = read<uint32_t>(input);
  if (!nodeCount || nodeCount > (1u << 20) || !leafCount || leafCount > (1u << 20) ||
      !clusterCount || clusterCount > leafCount || queryCount > 512)
    throw std::runtime_error("Fit-study budget exceeded");
  const auto minimum = read<Vec3>(input), maximum = read<Vec3>(input);
  const float scale = read<float>(input), detail = read<float>(input);
  if (!std::isfinite(scale) || scale <= 0 || !std::isfinite(detail) || detail < 0)
    throw std::runtime_error("Invalid fit-study scale or tolerance");
  Bounds bounds;
  for (uint32_t axis = 0; axis < 3; ++axis)
    if (!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) || minimum[axis] >= maximum[axis])
      throw std::runtime_error("Invalid fit-study bounds");
  bounds.include(minimum); bounds.include(maximum);
  scene::VisibilityPartition partition;
  partition.clusterCount = clusterCount;
  for (uint32_t i = 0; i < nodeCount; ++i) {
    const auto node = read<scene::VisibilityPartition::Node>(input);
    double length = 0;
    for (uint32_t axis = 0; axis < 4; ++axis) {
      if (!std::isfinite(node.plane[axis])) throw std::runtime_error("Nonfinite partition plane");
      if (axis < 3) length += double(node.plane[axis]) * node.plane[axis];
    }
    if (length == 0) throw std::runtime_error("Degenerate partition plane");
    for (const auto child : node.children)
      if ((child >= 0 && uint32_t(child) >= nodeCount) ||
          (child < 0 && uint64_t(-1 - int64_t(child)) >= leafCount))
        throw std::runtime_error("Invalid partition child");
    partition.nodes.push_back(node);
  }
  for (uint32_t i = 0; i < leafCount; ++i) {
    const auto cluster = read<int32_t>(input);
    if (cluster < -1 || (cluster >= 0 && uint32_t(cluster) >= clusterCount))
      throw std::runtime_error("Invalid leaf cluster");
    partition.leafClusters.push_back(cluster);
  }
  partition.closed = true;
  if (std::filesystem::exists(argv[2])) throw std::runtime_error("Choose a fresh fit-study output");
  std::ofstream output(argv[2]);
  output.exceptions(std::ios::failbit | std::ios::badbit);
  output << std::setprecision(9);
  for (uint32_t i = 0; i < queryCount; ++i) {
    const auto capture = read<Vec3>(input), receiver = read<Vec3>(input);
    for (const auto &point : {capture, receiver})
      for (const auto value : point)
        if (!std::isfinite(value)) throw std::runtime_error("Nonfinite fit-study point");
    for (bool anchored : {false, true}) {
      const auto fits = probes::fitRegionCandidates(partition, bounds, capture, .001f / scale,
          {}, {detail / scale}, anchored ? std::optional(receiver) : std::nullopt);
      output << "{\"query\":" << i << ",\"anchored\":" << (anchored ? "true" : "false") << ",\"fits\":[";
      for (size_t fit = 0; fit < fits.size(); ++fit) {
        if (fit) output << ",";
        const auto influence = probes::receiverRegion(partition, fits[fit], .002f / scale, {},
            {detail / scale}, .5f / scale);
        output << "{\"projection\":";
        writeBounds(output, fits[fit]);
        output << ",\"influence\":";
        writeBounds(output, influence);
        output << "}";
      }
      output << "]}\n";
      output.flush();
    }
    std::cout << "Fitted pair " << i + 1 << "/" << queryCount << std::endl;
  }
  if (input.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Trailing fit-study data");
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}
