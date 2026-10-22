#include "forward/probes/composition.h"
#include <limits>
#include <stdexcept>

namespace p = remix::forward::probes;
namespace {
  void require(bool value) {
    if (!value) throw std::runtime_error("Reflection fallback composition invariant failed");
  }
  p::Record box(float radius) {
    p::Record record;
    record.influenceMinimum = {-radius, -radius, -radius, 0};
    record.influenceMaximum = {radius, radius, radius, 0};
    return record;
  }
}
void testProbeComposition() {
  p::Header authored, automatic;
  authored.parameters = {1, 128, 8, 131070};
  automatic.parameters = {3, 128, 8, 131070};
  authored.probes[0] = box(4);
  automatic.probes[0] = box(2);
  automatic.probes[1] = box(5);
  automatic.probes[2] = box(6);
  auto result = p::compose(authored, automatic);
  require(result.covered == 1 && result.automatic == 2 && result.omitted == 0);
  require(result.header.parameters[0] == 3 && !result.layers[0].automatic);
  require(result.layers[1].automatic && result.layers[1].layer == 1 && result.layers[2].layer == 2);
  require(result.header.probes[1].minimum[3] == -1);
  result = p::compose(authored, automatic, 2);
  require(result.covered == 1 && result.automatic == 1 && result.omitted == 1);
  authored.probes[0].minimum[3] = -32768;
  require(p::compose(authored, automatic).header.probes[1].minimum[3] == -32769);
  // Different translated/rotated frames still prove full containment.
  automatic.parameters[0] = 1;
  automatic.probes[0].worldToLocal = {{{0, 1, 0, 1}, {-1, 0, 0, 0}, {0, 0, 1, 0}}};
  require(p::compose(authored, automatic).covered == 1);
  automatic.probes[0].worldToLocal[0][3] = 4;
  require(p::compose(authored, automatic).automatic == 1);
  // Unknown or malformed bounds must never erase fallback coverage.
  automatic.probes[0] = box(2);
  automatic.probes[0].influenceMinimum[0] = std::numeric_limits<float>::quiet_NaN();
  require(p::compose(authored, automatic).automatic == 1);
  authored.parameters[0] = 0;
  require(p::compose(authored, automatic).layers.empty());
  authored.parameters[0] = 64;
  for (auto &record : authored.probes) record = box(1);
  result = p::compose(authored, automatic);
  require(result.layers.size() == 64 && result.omitted == 1 && !result.automatic);
  automatic.parameters[1] = 64;
  bool rejected = false;
  try { p::compose(authored, automatic); } catch (const std::invalid_argument &) { rejected = true; }
  require(rejected);
}
