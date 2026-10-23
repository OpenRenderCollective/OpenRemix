#include "forward/bake/receiver_tiles.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace remix::forward::bake {
  namespace {
    bool overlaps(const std::array<Vec3, 3> &triangle, double x, double y, double extent) {
      for (uint32_t edge = 0; edge < 3; ++edge) {
        const auto &a = triangle[edge], &b = triangle[(edge + 1) % 3];
        const double nx = double(a[1]) - b[1], ny = double(b[0]) - a[0];
        double low = std::numeric_limits<double>::infinity(), high = -low;
        for (const auto &p : triangle) {
          const double value = nx * p[0] + ny * p[1];
          low = std::min(low, value); high = std::max(high, value);
        }
        const double center = nx * x + ny * y, radius = (std::abs(nx) + std::abs(ny)) * extent;
        if (center + radius < low || center - radius > high) return false;
      }
      return true;
    }
  }
  ReceiverTiles::ReceiverTiles(ReceiverTileOptions options) : m_options(options) {
    if (!std::isfinite(options.tileMetres) || options.tileMetres <= 0 ||
        !std::isfinite(options.haloMetres) || options.haloMetres < 0 ||
        !options.maximumTiles || options.maximumTiles > (1u << 20) ||
        !options.maximumCandidates || options.maximumCandidates > (16u << 20))
      throw std::invalid_argument("Invalid receiver tile budget");
  }
  void ReceiverTiles::include(const std::array<Vec3, 3> &triangle, std::stop_token cancelled) {
    std::array<int32_t, 2> low, high;
    const double size = m_options.tileMetres, halo = m_options.haloMetres;
    for (uint32_t axis = 0; axis < 2; ++axis) {
      double minimum = std::numeric_limits<double>::infinity(), maximum = -minimum;
      for (const auto &p : triangle) {
        if (!std::isfinite(p[axis])) throw std::invalid_argument("Nonfinite receiver tile vertex");
        minimum = std::min(minimum, double(p[axis])); maximum = std::max(maximum, double(p[axis]));
      }
      // Include both tiles when a closed triangle touches their shared boundary.
      const double first = std::ceil((minimum - halo) / size) - 1;
      const double last = std::floor((maximum + halo) / size);
      if (first < INT32_MIN || last > INT32_MAX)
        throw std::length_error("Receiver tile coordinates exceed int32");
      low[axis] = int32_t(first); high[axis] = int32_t(last);
    }
    const uint64_t width = uint64_t(int64_t(high[0]) - low[0] + 1);
    const uint64_t height = uint64_t(int64_t(high[1]) - low[1] + 1);
    if (width > (m_options.maximumCandidates - m_candidates) / height)
      throw std::length_error("Receiver tile candidate budget exceeded");
    m_candidates += width * height;
    for (int64_t y = low[1]; y <= high[1]; ++y) {
      if (cancelled.stop_requested()) throw std::runtime_error("Receiver tiling superseded");
      for (int64_t x = low[0]; x <= high[0]; ++x)
        if (overlaps(triangle, (double(x) + .5) * size, (double(y) + .5) * size, size * .5 + halo)) {
          m_tiles.insert({int32_t(x), int32_t(y)});
          if (m_tiles.size() > m_options.maximumTiles) throw std::length_error("Receiver tile storage budget exceeded");
        }
    }
  }
  ReceiverTileCoverage ReceiverTiles::coverage() const {
    ReceiverTileCoverage result{m_options.tileMetres, m_options.haloMetres, m_tiles.size(), 0};
    if (m_tiles.empty()) return result;
    auto low = *m_tiles.begin(), high = low;
    for (const auto &tile : m_tiles) for (uint32_t axis = 0; axis < 2; ++axis) {
      low[axis] = std::min(low[axis], tile[axis]); high[axis] = std::max(high[axis], tile[axis]);
    }
    const uint64_t width = uint64_t(int64_t(high[0]) - low[0] + 1), height = uint64_t(int64_t(high[1]) - low[1] + 1);
    if (width > UINT64_MAX / height) throw std::length_error("Receiver rectangle exceeds uint64");
    result.rectangle = width * height;
    return result;
  }
}
