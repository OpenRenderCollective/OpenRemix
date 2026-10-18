#include "forward/bake/styles.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace remix;
namespace b = remix::forward::bake;
namespace {
  void require(bool ok, const char *message) {
    if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
  }
}
int main() {
  scene::BakeScene source;
  for (uint32_t i = 0; i < 3; ++i) {
    scene::SphereLight light;
    light.id = 10 + i;
    light.style = i < 2 ? 32 : 97;
    light.radius = .5f;
    light.radiance[0] = 4;
    light.radiance[1] = 2;
    source.lights.push_back(light);
  }
  auto live = source.lights;
  for (auto &light : live) {
    const float value = light.style == 32 ? .125f : .25f;
    light.style = 0;
    for (auto &c : light.radiance) c *= value;
  }
  const auto baseline = b::styleScales(source, live);
  require(baseline.size() == 2 && baseline[0] == b::StyleScale{32, .125f, b::StyleStatus::Ready} &&
          baseline[1] == b::StyleScale{97, .25f, b::StyleStatus::Ready},
          "Style values account for authored envelope units across every group member");
  std::reverse(live.begin(), live.end());
  require(b::styleScales(source, live) == baseline, "Submission order does not change style ownership");
  std::reverse(live.begin(), live.end());
  auto reordered = source;
  std::reverse(reordered.lights.begin(), reordered.lights.end());
  require(b::styleScales(reordered, live) == baseline, "Captured membership ordering preserves resolved style scales");
  auto edited = live;
  for (uint32_t i = 0; i < 2; ++i) std::fill_n(edited[i].radiance, 3, 0.f);
  require(b::styleScales(source, edited)[0] == b::StyleScale{32, 0, b::StyleStatus::Ready},
          "All-off lights resolve to zero without losing their valid component identity");
  auto check = [&](const auto &lights, b::StyleStatus reason, const char *message) {
    const auto result = b::styleScales(source, lights);
    require(result[0].value == 0 && result[0].status == reason && result[1] == baseline[1], message);
  };
  edited = live;
  edited.erase(edited.begin());
  check(edited, b::StyleStatus::Missing, "A removed emitter cannot leave a stale group glow");
  edited = live;
  edited.push_back(edited.front());
  check(edited, b::StyleStatus::Ambiguous, "Duplicate runtime identity cannot select arbitrary radiance");
  edited = live;
  edited[0].position[0] = 1;
  check(edited, b::StyleStatus::GeometryChanged, "Moved geometry cannot reuse its old style component");
  edited = live;
  edited[0].dynamic = true;
  check(edited, b::StyleStatus::GeometryChanged, "A moving light cannot reuse stationary indirect light");
  edited = live;
  edited[0].radiance[2] = 1;
  check(edited, b::StyleStatus::NonScalar, "A new colour channel cannot be reconstructed from captured RGB");
  edited = live;
  edited[0].radiance[0] *= 2;
  check(edited, b::StyleStatus::NonScalar, "Conflicting per-channel or per-light scale is reported explicitly");
  edited = live;
  edited[0].radiance[0] = std::numeric_limits<float>::infinity();
  check(edited, b::StyleStatus::Invalid, "Non-finite runtime power cannot reach component uniforms");
  edited = live;
  for (uint32_t i = 0; i < 2; ++i) std::fill_n(edited[i].radiance, 3, 0.f);
  edited[0].radiance[0] = 1e-8f;
  edited[0].radiance[1] = 5e-9f;
  check(edited, b::StyleStatus::NonScalar,
        "An absolute tolerance cannot hide conflicting near-zero style values and leave a glow");
  auto mixedEnvelope = source;
  for (auto &c : mixedEnvelope.lights[0].radiance) c *= 2;
  require(b::styleScales(mixedEnvelope, live)[0].status == b::StyleStatus::NonScalar,
          "Differently normalized authored envelopes require normalization before group reconstruction");
  for (auto &light : mixedEnvelope.lights) std::fill_n(light.radiance, 3, 0.f);
  for (auto &light : edited) std::fill_n(light.radiance, 3, 0.f);
  require(b::styleScales(mixedEnvelope, edited)[0] == b::StyleScale{32, 0, b::StyleStatus::Ready},
          "An entirely black component remains finite and off");
  std::puts("Style scalar reconstruction, envelope units, stale-colour rejection and group isolation passed");
}
