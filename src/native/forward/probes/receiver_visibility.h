#pragma once
#include "forward/probes/receivers.h"
#include "forward/bake/data.h"
#include <cmath>
#include <optional>
#include <stdexcept>

namespace remix::forward::probes {
  enum class ReceiverVisibility { Unknown, Visible, Blocked };
  struct ReceiverRay {
    bake::Point point;
    float distance = 0;
  };
  // Host scene units throughout. recordGeometry applies no normal bias: apply
  // the geometric receiver offset here, then aim at the independent capture.
  // Invalid/back-facing/too-close pairs remain unknown, never implicitly clear.
  inline std::optional<ReceiverRay> receiverRay(const ReceiverSample &receiver, Vec3 capture, float bias) {
    if (!std::isfinite(bias) || bias <= 0) throw std::invalid_argument("Invalid receiver ray bias");
    double normalLength = 0;
    for (uint32_t c = 0; c < 3; ++c) {
      if (!std::isfinite(receiver.position[c]) || !std::isfinite(receiver.normal[c]) || !std::isfinite(capture[c]))
        throw std::invalid_argument("Nonfinite receiver ray input");
      normalLength += double(receiver.normal[c]) * receiver.normal[c];
    }
    if (!normalLength) return std::nullopt;
    normalLength = std::sqrt(normalLength);
    ReceiverRay result{};
    Vec3 delta;
    double distanceSquared = 0, facing = 0;
    for (uint32_t c = 0; c < 3; ++c) {
      const double normal = receiver.normal[c] / normalLength;
      result.point.position[c] = float(receiver.position[c] + normal * bias);
      delta[c] = capture[c] - result.point.position[c];
      distanceSquared += double(delta[c]) * delta[c];
      facing += delta[c] * normal;
    }
    result.distance = float(std::sqrt(distanceSquared));
    if (!std::isfinite(result.distance) || result.distance <= bias || facing <= 0) return std::nullopt;
    for (uint32_t c = 0; c < 3; ++c) result.point.normal[c] = delta[c] / result.distance;
    return result;
  }
  // A range-capped miss proves visibility only if traversal reached beyond the
  // capture. Near-endpoint hits stay unknown; neither backfaces nor short misses
  // may silently authorize growth through a wall.
  inline ReceiverVisibility receiverVisibility(const ReceiverRay &ray, const bake::Result &hit, float tolerance) {
    const float distance = hit.irradiance[0][3], back = hit.statistics[0], miss = hit.statistics[1];
    if (!std::isfinite(tolerance) || tolerance < 0 || !std::isfinite(ray.distance) || ray.distance <= 0)
      throw std::invalid_argument("Invalid receiver visibility interval");
    if (!std::isfinite(distance) || distance <= 0 || hit.statistics[2] != 1 ||
        (back != 0 && back != 1) || (miss != 0 && miss != 1) || (back && miss)) return ReceiverVisibility::Unknown;
    if (!miss && distance < ray.distance - tolerance) return ReceiverVisibility::Blocked;
    if (distance > ray.distance + tolerance) return ReceiverVisibility::Visible;
    return ReceiverVisibility::Unknown;
  }
} // namespace remix::forward::probes
