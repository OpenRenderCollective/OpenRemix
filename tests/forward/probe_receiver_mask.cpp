#include "forward/probes/receiver_mask.h"
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
  std::puts("Conservative cubemap receiver masks passed");
}
