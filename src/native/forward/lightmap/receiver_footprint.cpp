#include "forward/lightmap/receiver_footprint.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace remix::forward::lightmap {
  namespace {
    double triangleArea(ReceiverFootprint::Uv a, ReceiverFootprint::Uv b, ReceiverFootprint::Uv c) {
      return std::abs((double(b[0]) - a[0]) * (double(c[1]) - a[1]) -
                      (double(b[1]) - a[1]) * (double(c[0]) - a[0]));
    }
  } // namespace
  ReceiverFootprint::ReceiverFootprint(const std::array<Uv, 3> &triangle, Uv centre) {
    for (const auto &vertex : triangle)
      for (float value : vertex)
        if (!std::isfinite(value))
          throw std::invalid_argument("Non-finite receiver footprint");
    for (float value : centre)
      if (!std::isfinite(value))
        throw std::invalid_argument("Non-finite receiver footprint centre");
    std::copy(triangle.begin(), triangle.end(), m_vertices.begin());
    m_count = 3;
    for (uint32_t axis = 0; axis < 2; ++axis)
      for (float side : {-1.f, 1.f}) {
        if (!m_count)
          return;
        const float boundary = centre[axis] + side * .5f;
        std::array<Uv, 7> clipped{};
        uint32_t count = 0;
        auto previous = m_vertices[m_count - 1];
        float previousDistance = side * (previous[axis] - boundary);
        const auto append = [&](Uv point) {
          // Exact boundary vertices may otherwise be emitted twice.
          if (count && clipped[count - 1] == point)
            return;
          if (count == clipped.size())
            throw std::logic_error("Receiver footprint exceeds convex bound");
          clipped[count++] = point;
        };
        for (uint32_t i = 0; i < m_count; ++i) {
          const auto current = m_vertices[i];
          const float distance = side * (current[axis] - boundary);
          if ((distance <= 0) != (previousDistance <= 0)) {
            const float t = previousDistance / (previousDistance - distance);
            Uv intersection{previous[0] + t * (current[0] - previous[0]),
                            previous[1] + t * (current[1] - previous[1])};
            intersection[axis] = boundary;
            append(intersection);
          }
          if (distance <= 0)
            append(current);
          previous = current;
          previousDistance = distance;
        }
        if (count > 1 && clipped[0] == clipped[count - 1])
          --count;
        m_vertices = clipped;
        m_count = count;
      }
    for (uint32_t i = 1; i + 1 < m_count; ++i)
      m_twiceArea += triangleArea(m_vertices[0], m_vertices[i], m_vertices[i + 1]);
  }
  ReceiverFootprint::Uv ReceiverFootprint::centroid() const {
    if (empty())
      throw std::logic_error("Empty receiver footprint has no centroid");
    std::array<double, 2> sum{};
    for (uint32_t i = 1; i + 1 < m_count; ++i) {
      const double weight = triangleArea(m_vertices[0], m_vertices[i], m_vertices[i + 1]);
      for (uint32_t axis = 0; axis < 2; ++axis)
        sum[axis] +=
            weight * (double(m_vertices[0][axis]) + m_vertices[i][axis] + m_vertices[i + 1][axis]) / 3;
    }
    return {float(sum[0] / m_twiceArea), float(sum[1] / m_twiceArea)};
  }
  ReceiverFootprint::Uv ReceiverFootprint::sample(double u, double v) const {
    if (empty())
      throw std::logic_error("Cannot sample an empty receiver footprint");
    if (!(u >= 0 && u < 1 && v >= 0 && v < 1))
      throw std::invalid_argument("Receiver footprint variates must be in [0,1)");
    double target = u * m_twiceArea;
    for (uint32_t i = 1; i + 1 < m_count; ++i) {
      const double weight = triangleArea(m_vertices[0], m_vertices[i], m_vertices[i + 1]);
      if (weight <= 0)
        continue;
      if (target < weight || i + 2 == m_count) {
        const double root = std::sqrt(std::clamp(target / weight, 0., 1.));
        Uv result;
        for (uint32_t axis = 0; axis < 2; ++axis)
          result[axis] = float((1 - root) * m_vertices[0][axis] + root * (1 - v) * m_vertices[i][axis] +
                               root * v * m_vertices[i + 1][axis]);
        return result;
      }
      target -= weight;
    }
    throw std::logic_error("Receiver footprint contains no area");
  }
} // namespace remix::forward::lightmap
