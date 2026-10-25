#include "forward/bake/coarse_transfer.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace remix::forward::bake {
  namespace {
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested()) throw std::runtime_error("Coarse transfer planning superseded");
    }
    uint64_t aligned(uint64_t words) { return (words + 3) & ~uint64_t(3); }
  }
  std::optional<CoarseTransfer> CoarseTransfer::plan(const Grid &grid, Options options,
                                                   std::stop_token cancelled) {
    if (!std::isfinite(options.epsilon) || options.epsilon <= 0)
      throw std::invalid_argument("Invalid coarse transfer endpoint tolerance");
    check(cancelled);
    if (!adaptive(grid.header)) return {};
    const auto origins = directoryOrigins(grid); // Validate ownership and offsets before allocation.
    const uint32_t nodes = gridNodes(grid.header);
    uint64_t wordCount = aligned(uint64_t(nodes) + 4), rayLimit = 0;
    uint32_t patches = 0;
    for (uint32_t cell = 0; cell < nodes; ++cell) {
      if ((cell & 1023u) == 0) check(cancelled);
      const auto reference = grid.lookup[nodes + 4 + cell];
      if (!patchOffset(reference)) continue;
      const uint32_t side = patchFactor(reference, grid.lookup[nodes + 1]) + 1;
      const uint64_t count = uint64_t(side) * side * side;
      wordCount += aligned(12 + count * 3);
      rayLimit += count * 8;
      ++patches;
    }
    const uint64_t estimatedBytes = wordCount * sizeof(uint32_t) + origins.size() * sizeof(Vec3) +
                                    rayLimit * (sizeof(Request) + sizeof(uint8_t));
    if (!patches || wordCount > UINT32_MAX || rayLimit > options.maximumRays ||
        estimatedBytes > options.maximumBytes) return {};
    CoarseTransfer result;
    result.m_epsilon = options.epsilon;
    result.m_words.reserve(size_t(wordCount));
    result.m_words.resize(size_t(aligned(uint64_t(nodes) + 4)));
    result.m_words[0] = magic; result.m_words[1] = nodes;
    result.m_words[2] = patches; result.m_words[3] = uint32_t(wordCount);
    result.m_positions.resize(origins.size());
    for (uint32_t record = 0; record < origins.size(); ++record) {
      if ((record & 1023u) == 0) check(cancelled);
      for (uint32_t axis = 0; axis < 3; ++axis) {
        const float offset = uint32_t(grid.header.settings[2]) & 1u ? grid.probes[record].irradiance[axis][3] : 0;
        const float value = origins[record].position[axis] + offset;
        if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite coarse transfer endpoint");
        result.m_positions[record][axis] = value;
      }
    }
    result.m_requests.reserve(size_t(rayLimit));
    const auto &dimensions = grid.header.dimensions;
    for (uint32_t cell = 0; cell < nodes; ++cell) {
      check(cancelled);
      const auto reference = grid.lookup[nodes + 4 + cell], block = patchOffset(reference);
      if (!block) continue;
      const uint32_t factor = patchFactor(reference, grid.lookup[nodes + 1]), side = factor + 1;
      const uint32_t count = side * side * side, patch = uint32_t(result.m_words.size());
      result.m_words[4 + cell] = patch;
      result.m_words.resize(size_t(aligned(uint64_t(patch) + 12 + count * 3)));
      result.m_words[patch] = factor; result.m_words[patch + 1] = count;
      result.m_words[patch + 2] = patch + 12;
      for (uint32_t corner = 0; corner < 8; ++corner) {
        const uint32_t offset = (((corner >> 2) & 1) * dimensions[1] + ((corner >> 1) & 1)) *
                                dimensions[0] + (corner & 1);
        result.m_words[patch + 4 + corner] = grid.lookup[cell + offset];
      }
      for (uint32_t node = 0; node < count; ++node) {
        if ((node & 255u) == 0) check(cancelled);
        const uint32_t destination = patch + 12 + node * 3, fine = grid.lookup[block + node];
        result.m_words[destination] = result.m_words[destination + 1] = ~0u;
        for (uint32_t corner = 0; corner < 8; ++corner) {
          const uint32_t coarse = result.m_words[patch + 4 + corner];
          if (!coarse || !fine || !(grid.probes[coarse - 1].statistics[3] >= .5f) ||
              !(grid.probes[fine - 1].statistics[3] >= .5f)) continue;
          const auto &a = result.m_positions[coarse - 1], &b = result.m_positions[fine - 1];
          const double length = std::hypot(double(b[0]) - a[0], double(b[1]) - a[1], double(b[2]) - a[2]);
          if (!std::isfinite(length) || length > std::numeric_limits<float>::max() / 2)
            throw std::invalid_argument("Invalid coarse transfer segment length");
          if (length <= 2 * options.epsilon) {
            result.m_words[destination + 2] |= 1u << corner;
            continue; // Shared or coincident endpoints are clear without a ray.
          }
          result.m_requests.push_back({coarse - 1, fine - 1, destination, corner,
                                       float(length) - 2 * options.epsilon});
          result.m_maximumDistance = std::max(result.m_maximumDistance, float(length));
        }
      }
    }
    if (result.m_words.size() != wordCount) throw std::logic_error("Coarse transfer size estimate disagrees");
    result.m_completed.resize(result.m_requests.size());
    return result;
  }
  std::vector<Point> CoarseTransfer::batch(uint32_t first, uint32_t count) const {
    if (first > m_requests.size() || count > m_requests.size() - first)
      throw std::out_of_range("Coarse transfer batch range");
    std::vector<Point> result;
    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      const auto &request = m_requests[first + i];
      const auto &a = m_positions[request.coarse], &b = m_positions[request.fine];
      Vec3 direction;
      const double length = std::hypot(double(b[0]) - a[0], double(b[1]) - a[1], double(b[2]) - a[2]);
      for (uint32_t c = 0; c < 3; ++c) direction[c] = float((double(b[c]) - a[c]) / length);
      Point ray;
      for (uint32_t c = 0; c < 3; ++c) {
        ray.position[c] = a[c] + direction[c] * m_epsilon;
        ray.normal[c] = direction[c];
      }
      result.push_back(ray);
    }
    return result;
  }
  void CoarseTransfer::consume(uint32_t first, std::span<const float> depths) {
    if (first > m_requests.size() || depths.size() > m_requests.size() - first)
      throw std::out_of_range("Coarse transfer readback range");
    for (size_t i = 0; i < depths.size(); ++i) {
      if (m_completed[first + i]) throw std::logic_error("Duplicate coarse transfer readback");
      if (!std::isfinite(depths[i]) || (depths[i] < 0 && depths[i] != -1 && depths[i] != -2))
        throw std::invalid_argument("Invalid coarse transfer depth");
    }
    for (size_t i = 0; i < depths.size(); ++i) {
      const auto &request = m_requests[first + i];
      if (depths[i] != -2) {
        m_words[request.nodeWord + 2] |= 1u << request.corner;
        if (depths[i] >= 0 && depths[i] < request.distance)
          m_words[request.nodeWord + request.corner / 4] &= ~(255u << ((request.corner % 4) * 8));
      }
      m_completed[first + i] = 1;
    }
    m_received += uint32_t(depths.size());
  }
  std::span<const uint32_t> CoarseTransfer::words() const {
    if (!complete()) throw std::logic_error("Coarse transfer field is not complete");
    return m_words;
  }
  uint64_t CoarseTransfer::scratchBytes() const {
    return m_words.capacity() * sizeof(uint32_t) + m_positions.capacity() * sizeof(Vec3) +
           m_requests.capacity() * sizeof(Request) + m_completed.capacity();
  }
}
