#include "forward/probes/receiver_mask.h"
#include "forward/probes/spatial_mask.h"
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace remix::forward;
namespace p = remix::forward::probes;
namespace {
  void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
  }
  bool contains(const std::array<uint32_t, 2> &mask, uint32_t index) {
    return (mask[index / 32] & (1u << (index % 32))) != 0;
  }
}
int main() {
  p::Header header;
  header.parameters[0] = 64;
  for (uint32_t i = 0; i < 64; ++i) {
    auto &record = header.probes[i];
    record.influenceMinimum = {float(i * 4), -1, -1, 0};
    record.influenceMaximum = {float(i * 4 + 2), 1, 1, 0};
  }
  Bounds receivers;
  receivers.include({125, -.5f, -.5f});
  receivers.include({129, .5f, .5f});
  auto mask = p::receiverMask(header, receivers, 0);
  require(mask[0] == (1u << 31) && mask[1] == 1,
          "Receiver overlap preserves both words and rejects unrelated cubemaps");
  receivers.minimum[0] = 254;
  receivers.maximum[0] = 254;
  mask = p::receiverMask(header, receivers, 0);
  require(mask[0] == 0 && mask[1] == (1u << 31), "The last probe and touching bounds are retained");
  receivers.minimum[0] = receivers.maximum[0] = 254.25f;
  require(!contains(p::receiverMask(header, receivers, 0), 63) &&
              contains(p::receiverMask(header, receivers, .3f), 63),
          "Normal-query padding preserves nearby influence even outside the original mesh bounds");
  auto unknown = p::receiverMask(header, {}, 0);
  require(unknown[0] == ~0u && unknown[1] == ~0u, "Unknown mesh bounds retain every probe");
  header.probes[4].worldToLocal[2][0] = std::numeric_limits<float>::quiet_NaN();
  require(contains(p::receiverMask(header, receivers, 0), 4), "An invalid transform cannot prove exclusion");
  header.probes[4].worldToLocal = p::worldSpace;

  // Rotate and translate independent influence boxes. Every sampled receiver
  // accepted by the shader's float dot products must survive the CPU mask.
  for (uint32_t trial = 0; trial < 48; ++trial) {
    const float angle = float(trial) * .173f, c = std::cos(angle), s = std::sin(angle);
    for (uint32_t i = 0; i < 64; ++i) {
      auto &probe = header.probes[i];
      probe.worldToLocal = {{{c, -s, 0, float(i) - 32}, {s, c, 0, 4}, {0, 0, 1, -2}}};
      probe.influenceMinimum = {-2, -3, -4, 0};
      probe.influenceMaximum = {2, 3, 4, 0};
      // Projection proxies intentionally differ from influence bounds.
      probe.minimum = {1000, 1000, 1000, 0};
      probe.maximum = {1001, 1001, 1001, 0};
    }
    receivers = {};
    receivers.include({float(trial) - 24, -8, -3});
    receivers.include({float(trial) - 20, 2, 5});
    mask = p::receiverMask(header, receivers, .1f);
    for (uint32_t i = 0; i < 64; ++i)
      for (uint32_t point = 0; point < 125; ++point) {
        Vec3 position;
        for (uint32_t axis = 0, cell = point; axis < 3; ++axis, cell /= 5)
          position[axis] = receivers.minimum[axis] - .1f + float(cell % 5) / 4 *
              (receivers.maximum[axis] - receivers.minimum[axis] + .2f);
        const auto local = p::projectionPoint(header.probes[i].worldToLocal, position);
        bool inside = true;
        for (uint32_t axis = 0; axis < 3; ++axis)
          inside &= local[axis] >= -float(axis + 2) && local[axis] <= float(axis + 2);
        require(!inside || contains(mask, i), "Rotated padded receivers never lose an influencing probe");
      }
  }
  // Large translations cancel in local coordinates; float rounding may put a
  // boundary receiver inside even when its double-precision centre is outside.
  header.parameters[0] = 1;
  header.probes[0].worldToLocal = p::worldSpace;
  header.probes[0].worldToLocal[0][3] = -1e8f;
  header.probes[0].influenceMinimum = {0, -1, -1, 0};
  header.probes[0].influenceMaximum = {1, 1, 1, 0};
  receivers = {};
  receivers.include({1e8f, 0, 0});
  require(contains(p::receiverMask(header, receivers, 0), 0), "Large-coordinate boundary contact survives");
  header.parameters[0] = 0;
  require(p::receiverMask(header, {}, 0) == std::array<uint32_t, 2>{}, "Empty reflection sets remain empty");
  // Exercise negative cell coordinates, exact cell faces, both mask words,
  // and cancellation at the maximum supported translation.
  header.parameters[0] = 64;
  constexpr size_t budget = 4 * 1024 * 1024 + 24;
  for (uint32_t i = 0; i < 64; ++i) {
    auto &probe = header.probes[i];
    probe.worldToLocal = p::worldSpace;
    probe.worldToLocal[0][3] = 1048576;
    probe.influenceMinimum = {1048576.0f - 256 + float(i * 4), -1, -1, 0};
    probe.influenceMaximum = {probe.influenceMinimum[0] + 4, 1, 1, 0};
  }
  const auto spatial = p::buildSpatialMask(header, budget);
  require(spatial.size() >= 8, "Translated probes publish spatial masks");
  for (int32_t sample = -4100; sample <= 4; ++sample) {
    const Vec3 point{float(sample) / 16, 0, 0};
    std::array<uint32_t, 3> cell{};
    for (uint32_t axis = 0; axis < 3; ++axis) {
      const auto origin = std::bit_cast<int32_t>(spatial[axis]);
      const auto coordinate = int32_t(std::floor(point[axis] / 128)) - origin;
      require(coordinate >= 0 && uint32_t(coordinate) < spatial[axis + 3], "Sample spans the published grid");
      cell[axis] = uint32_t(coordinate);
    }
    const size_t offset = 6 + 2 * (cell[0] + spatial[3] * (cell[1] + spatial[4] * cell[2]));
    const std::array<uint32_t, 2> candidates{spatial[offset], spatial[offset + 1]};
    for (uint32_t i = 0; i < 64; ++i) {
      const auto &probe = header.probes[i];
      const auto local = p::projectionPoint(probe.worldToLocal, point);
      const bool inside = local[0] >= probe.influenceMinimum[0] && local[0] <= probe.influenceMaximum[0];
      require(!inside || contains(candidates, i), "Spatial masks preserve float-accepted cell boundaries");
    }
  }
  require(p::buildSpatialMask(header, spatial.size() * sizeof(uint32_t) - 1).empty(),
          "A storage range below the required size disables the grid");
  header.probes[0].worldToLocal[0][1] = .25f;
  require(p::buildSpatialMask(header, budget).empty(), "Non-translation transforms keep the draw mask");
  header.probes[0].worldToLocal[0][1] = 0;
  header.probes[0].influenceMaximum[0] = std::numeric_limits<float>::infinity();
  require(p::buildSpatialMask(header, budget).empty(), "Invalid influence bounds keep the draw mask");
  std::puts("Conservative cubemap receiver masks passed");
}
