#include "forward/bake/probe_depth.h"
#include "forward/half.h"
#include "scene/fixtures.h"
#include <cmath>
#include <limits>

using namespace remix::forward;
using remix::test::require;
int main() {
  bake::ProbeDepth depth;
  depth.validate(10);
  require(depth.bytes() == 0 && !depth.visibility(0, {1, 0, 0}, .01f),
          "Absent directional depth is unknown, not invented black visibility");
  depth.maximumDistance = 100;
  depth.records.resize(1);
  const uint32_t packed = uint32_t(half::encode(.125f)) | uint32_t(half::encode(.015625f)) << 16;
  depth.records[0].fill(packed);
  depth.validate(1);
  require(depth.bytes() == 272, "Each directional depth record uses one 256-byte transport result");
  for (uint32_t texel = 0; texel < depth.texels; ++texel)
    for (uint32_t sample = 0; sample < depth.samples; ++sample) {
      const auto direction = depth.direction(texel, sample);
      require(std::abs(dot(direction, direction) - 1) < 1e-5f, "Octahedral geometry directions are unit length");
      const auto moments = depth.moments(0, direction);
      require(moments && std::abs((*moments)[0] - 12.5f) < 1e-5f &&
                  std::abs((*moments)[1] - 156.25f) < 1e-4f,
              "Four-tap octahedral filtering preserves constant moments over the whole sphere");
    }
  for (const auto direction : {Vec3{1, 0, 0}, Vec3{-1, 0, 0}, Vec3{0, 1, 0},
                               Vec3{0, -1, 0}, Vec3{0, 0, 1}, Vec3{0, 0, -1}}) {
    const auto moments = depth.moments(0, direction);
    require(moments && std::abs((*moments)[0] - 12.5f) < 1e-5f,
            "Octahedral axes and folded borders do not lose or duplicate visibility energy");
  }
  require(depth.visibility(0, {0, 0, 5}, .01f) == 1 &&
              *depth.visibility(0, {0, 0, 25}, .01f) < 1e-8f,
          "A flat directional blocker retains near receivers and rejects distant receivers");
  require(!depth.visibility(1, {0, 0, 1}, .01f) && !depth.visibility(0, {0, 0, 101}, .01f) &&
              !depth.moments(0, {0, 0, 0}) &&
              !depth.moments(0, {std::numeric_limits<float>::quiet_NaN(), 0, 1}),
          "Unstored, untraced or malformed directional queries stay unknown");
  for (const auto pair : {0x7c000000u, 0x00007c00u, 0x40000000u, 0x0000bc00u}) {
    depth.records[0][0] = pair;
    bool rejected = false;
    try { depth.validate(1); } catch (const std::invalid_argument &) { rejected = true; }
    require(rejected, "Non-finite or out-of-range half moments cannot enter a published field");
  }
}
