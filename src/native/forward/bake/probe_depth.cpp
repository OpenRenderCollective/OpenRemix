#include "forward/bake/probe_depth.h"
#include "forward/half.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <bit>
#include <stdexcept>

namespace remix::forward::bake {
  namespace {
    float sign(float value) { return value >= 0 ? 1.f : -1.f; }
    Vec3 decode(float u, float v) {
      Vec3 n{u * 2 - 1, v * 2 - 1, 1 - std::abs(u * 2 - 1) - std::abs(v * 2 - 1)};
      if (n[2] < 0) {
        const float x = (1 - std::abs(n[1])) * sign(n[0]);
        n[1] = (1 - std::abs(n[0])) * sign(n[1]);
        n[0] = x;
      }
      return normalized(n);
    }
    std::array<int, 2> folded(int x, int y) {
      constexpr int side = ProbeDepth::side;
      if (x < 0 || x >= side) {
        x = x < 0 ? -1 - x : 2 * side - 1 - x;
        y = side - 1 - y;
      }
      if (y < 0 || y >= side) {
        y = y < 0 ? -1 - y : 2 * side - 1 - y;
        x = side - 1 - x;
      }
      return {x, y};
    }
  }
  Vec3 ProbeDepth::direction(uint32_t texel, uint32_t sample) {
    if (texel >= texels || sample >= samples) throw std::out_of_range("Probe depth direction");
    return decode((texel % side + .25f + .5f * (sample & 1u)) / side,
                  (texel / side + .25f + .5f * (sample >> 1u)) / side);
  }
  void ProbeDepth::validate(uint32_t probeCount) const {
    if (empty()) {
      if (maximumDistance != 0) throw std::invalid_argument("Empty probe depth has a distance");
      return;
    }
    if (records.size() != probeCount || !std::isfinite(maximumDistance) || maximumDistance <= 0 ||
        !std::isfinite(maximumDistance * maximumDistance))
      throw std::invalid_argument("Invalid probe depth shape");
    for (const auto &record : records)
      for (uint32_t packed : record) {
        const float mean = half::decode(uint16_t(packed)), second = half::decode(uint16_t(packed >> 16));
        if (!(mean >= 0 && mean <= 1 && second >= 0 && second <= 1))
          throw std::invalid_argument("Invalid normalized probe depth moments");
      }
  }
  std::optional<std::array<float, 2>> ProbeDepth::moments(uint32_t probe, Vec3 direction) const {
    if (probe >= records.size() || !(maximumDistance > 0) || !std::isfinite(maximumDistance)) return {};
    float sum = 0;
    for (float value : direction) {
      if (!std::isfinite(value)) return {};
      sum += std::abs(value);
    }
    if (!(sum > 0) || !std::isfinite(sum)) return {};
    for (float &value : direction) value /= sum;
    if (direction[2] < 0) {
      const float x = (1 - std::abs(direction[1])) * sign(direction[0]);
      direction[1] = (1 - std::abs(direction[0])) * sign(direction[1]);
      direction[0] = x;
    }
    std::array<int, 2> base;
    std::array<float, 2> fraction;
    for (uint32_t axis = 0; axis < 2; ++axis) {
      const float q = (direction[axis] * .5f + .5f) * side - .5f;
      base[axis] = int(std::floor(q));
      fraction[axis] = q - base[axis];
    }
    std::array<float, 2> value{};
    for (uint32_t tap = 0; tap < 4; ++tap) {
      const uint32_t x = tap & 1u, y = tap >> 1u;
      const auto coordinate = folded(base[0] + int(x), base[1] + int(y));
      const uint32_t packed = records[probe][coordinate[1] * side + coordinate[0]];
      const float weight = (x ? fraction[0] : 1 - fraction[0]) * (y ? fraction[1] : 1 - fraction[1]);
      value[0] += weight * half::decode(uint16_t(packed));
      value[1] += weight * half::decode(uint16_t(packed >> 16));
    }
    value[0] *= maximumDistance;
    value[1] *= maximumDistance * maximumDistance;
    return value;
  }
  void ProbeDepth::write(std::span<std::byte> output) const {
    if (output.size() != bytes()) throw std::invalid_argument("Probe depth publication byte count");
    if (empty()) return;
    const std::array<uint32_t, 4> header{side, std::bit_cast<uint32_t>(maximumDistance), uint32_t(records.size()), 0};
    std::memcpy(output.data(), header.data(), sizeof(header));
    std::memcpy(output.data() + sizeof(header), records.data(), records.size() * sizeof(Record));
  }
  std::optional<float> ProbeDepth::visibility(uint32_t probe, Vec3 delta, float minimumVariance) const {
    const float distance = std::sqrt(dot(delta, delta));
    if (!std::isfinite(maximumDistance) || maximumDistance <= 0 ||
        !std::isfinite(distance) || distance > maximumDistance || !std::isfinite(minimumVariance) ||
        minimumVariance <= 0 || probe >= records.size()) return {};
    if (distance <= 1e-6f) return 1.f;
    const auto value = moments(probe, delta);
    if (!value) return {};
    if (distance <= (*value)[0]) return 1.f;
    const float variance = std::max(minimumVariance, (*value)[1] - (*value)[0] * (*value)[0]);
    const float difference = distance - (*value)[0];
    return std::pow(variance / (variance + difference * difference), 3.f);
  }
}
