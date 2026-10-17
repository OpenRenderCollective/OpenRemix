#include "forward/lightmap/curved.h"
#include "forward/lighting.h"
#include "forward/lightmap/surface_metric.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <stdexcept>

namespace remix::forward::lightmap {
  namespace curved {
    double length(Point value) { return std::hypot(value[0], value[1], value[2]); }
    namespace {
      void check(std::stop_token cancelled) {
        if (cancelled.stop_requested())
          throw std::runtime_error("Curved surface mapping superseded");
      }
      struct Metric {
        double u, uv, v, area;
      };
      std::optional<Metric> metric(const Surface &surface, std::span<const Uv> uv, size_t first) {
        const auto a = surface.corners[first], b = surface.corners[first + 1], c = surface.corners[first + 2];
        const auto e = difference(surface.positions[b], surface.positions[a]),
                   f = difference(surface.positions[c], surface.positions[a]);
        const double area = length(cross(e, f));
        if (area <= 1e-20)
          return std::nullopt;
        const double longest = std::max({length(e), length(f), length(difference(e, f))});
        const auto originalS = surface.authored[b], originalT = surface.authored[c],
                   originalA = surface.authored[a];
        const double originalArea = (originalS[0] - originalA[0]) * (originalT[1] - originalA[1]) -
                                    (originalS[1] - originalA[1]) * (originalT[0] - originalA[0]);
        // BSP fan vertices can be collinear in authored UVs yet acquire a
        // sub-millimetre sliver in world space through float rounding. They
        // already own no atlas area. Match the planar joining tolerance.
        if (std::abs(originalArea) < 1e-12 && area / longest <= .0001)
          return std::nullopt;
        const Uv s{uv[b][0] - uv[a][0], uv[b][1] - uv[a][1]}, t{uv[c][0] - uv[a][0], uv[c][1] - uv[a][1]};
        const double determinant = s[0] * t[1] - s[1] * t[0];
        if (std::abs(determinant) <= 1e-20) {
          throw std::invalid_argument("Curved chart triangle " + std::to_string(first / 3) + " altitude " +
                                      std::to_string(area / longest) + " m has collapsed UVs");
        }
        Point du, dv;
        for (uint32_t axis = 0; axis < 3; ++axis) {
          du[axis] = (e[axis] * t[1] - f[axis] * s[1]) / determinant;
          dv[axis] = (f[axis] * s[0] - e[axis] * t[0]) / determinant;
        }
        const Metric result{dot(du, du), dot(du, dv), dot(dv, dv), area};
        if (!std::isfinite(result.u) || !std::isfinite(result.uv) || !std::isfinite(result.v))
          throw std::invalid_argument("Nonfinite curved surface metric");
        return result;
      }
    } // namespace
    std::vector<Uv> correctMetric(const Surface &surface, std::span<const Uv> coordinates,
                                  std::stop_token cancelled) {
      Metric average{};
      for (size_t first = 0; first < surface.corners.size(); first += 3) {
        if ((first & 1023u) == 0)
          check(cancelled);
        if (const auto m = metric(surface, coordinates, first)) {
          average.u += m->u * m->area;
          average.uv += m->uv * m->area;
          average.v += m->v * m->area;
          average.area += m->area;
        }
      }
      if (average.area <= 0)
        throw std::invalid_argument("Curved chart has no surface area");
      average.u /= average.area;
      average.uv /= average.area;
      average.v /= average.area;
      // The positive square root of the average metric removes authored UV
      // scale/shear without changing topology. Ill-conditioned metrics use
      // diagonal scaling; the per-triangle bound below still applies.
      double a = std::sqrt(average.u), b = 0, c = std::sqrt(average.v);
      const double determinant = average.u * average.v - average.uv * average.uv;
      if (determinant > 1e-12 * average.u * average.v) {
        const double root = std::sqrt(determinant), divisor = std::sqrt(average.u + average.v + 2 * root);
        a = (average.u + root) / divisor;
        b = average.uv / divisor;
        c = (average.v + root) / divisor;
      }
      if (!(a > 0 && c > 0))
        throw std::invalid_argument("Degenerate curved chart metric");
      std::vector<Uv> result;
      result.reserve(coordinates.size());
      for (const auto &uv : coordinates)
        result.push_back({a * uv[0] + b * uv[1], b * uv[0] + c * uv[1]});
      double stretch = 0;
      for (size_t first = 0; first < surface.corners.size(); first += 3) {
        if ((first & 1023u) == 0)
          check(cancelled);
        if (const auto m = metric(surface, result, first)) {
          const double eigenvalue = .5 * (m->u + m->v + std::hypot(m->u - m->v, 2 * m->uv));
          stretch = std::max(stretch, std::sqrt(eigenvalue));
        }
      }
      if (!(stretch > 0) || !std::isfinite(stretch))
        throw std::invalid_argument("Invalid curved chart stretch");
      Uv minimum{INFINITY, INFINITY};
      for (const auto &uv : result)
        for (uint32_t axis = 0; axis < 2; ++axis)
          minimum[axis] = std::min(minimum[axis], uv[axis]);
      for (auto &uv : result)
        for (uint32_t axis = 0; axis < 2; ++axis)
          // Final atlas coordinates are floats. Reserve a small density margin
          // for cancellation on long, thin fan triangles after that conversion.
          uv[axis] = (uv[axis] - minimum[axis]) * stretch * (1 + 1e-4);
      return result;
    }
  } // namespace curved
  CurvedCoordinates curvedCoordinates(const scene::BakeScene &scene, const scene::BakeSurfaceChart &chart,
                                      std::stop_token cancelled) {
    curved::Surface surface;
    const auto &draw = scene.draws[chart.draw];
    std::map<std::array<uint64_t, 3>, uint32_t> vertices;
    for (size_t corner = 0; corner < chart.texcoords.size(); ++corner) {
      if ((corner & 1023u) == 0 && cancelled.stop_requested())
        throw std::runtime_error("Curved surface mapping superseded");
      const auto index =
          uint64_t(int64_t(draw.surface->indices[chart.firstIndex + corner]) + draw.vertexOffset);
      const auto uv = chart.texcoords[corner];
      // Keep authored seams even if the host reuses a geometric vertex there.
      const std::array<uint64_t, 3> key{index, std::bit_cast<uint32_t>(uv[0]),
                                        std::bit_cast<uint32_t>(uv[1])};
      const auto [found, added] = vertices.emplace(key, uint32_t(surface.positions.size()));
      if (added) {
        const auto &p = draw.surface->vertices[size_t(index)].position;
        const auto world = transformPoint(draw.model, {p[0], p[1], p[2]});
        curved::Point metres;
        for (uint32_t c = 0; c < 3; ++c)
          metres[c] = double(world[c]) * scene.sceneScale;
        for (double value : metres)
          if (!std::isfinite(value))
            throw std::invalid_argument("Nonfinite curved surface");
        surface.positions.push_back(metres);
        surface.authored.push_back({uv[0], uv[1]});
      }
      surface.corners.push_back(found->second);
    }
    // A topologically valid harmonic disk can still flatten a warped boundary
    // triangle. Validate its metric before selecting it; an open fan has an
    // exact intrinsic alternative. Do not discard geometry or relax density
    // checks to make a malformed parameterization pass.
    std::optional<std::vector<curved::Uv>> corrected;
    const auto accept = [&](const auto &candidate) {
      if (!candidate)
        return false;
      try {
        corrected = curved::correctMetric(surface, *candidate, cancelled);
        return true;
      } catch (const std::invalid_argument &) {
        return false;
      }
    };
    const bool intrinsic = accept(curved::harmonic(surface, cancelled)) ||
                           accept(curved::unfoldFan(surface, cancelled));
    if (!corrected)
      corrected = curved::correctMetric(surface, surface.authored, cancelled);
    CurvedCoordinates result;
    result.intrinsic = intrinsic;
    result.metres.reserve(surface.corners.size());
    for (auto corner : surface.corners)
      result.metres.push_back((*corrected)[corner]);
    return result;
  }
} // namespace remix::forward::lightmap
