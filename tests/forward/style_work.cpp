#include "forward/bake/style_work.h"
#include "scene/fixtures.h"
#include <limits>

using namespace remix;
using namespace remix::test;
using forward::Vec4;
namespace b = forward::bake;
namespace lm = forward::lightmap;
int main() {
  scene::BakeScene source;
  scene::SphereLight light;
  light.style = 97;
  source.lights.push_back(light);
  light.style = 32;
  source.lights.push_back(light);
  source.lights.push_back(light);
  require(b::styleIds(source) == std::vector<uint32_t>{32, 97}, "Component groups are sorted and deduplicated");
  lm::Layout layout;
  layout.pageSize = 8;
  layout.pages = 1;
  layout.points.resize(2);
  layout.owners.resize(64, lm::unowned);
  layout.owners[0] = 0;
  layout.owners[1] = 1;
  b::Grid base;
  base.probes.resize(3);
  base.probes[0].statistics[3] = base.probes[2].statistics[3] = 1;
  b::StyleWork work(source, layout, base, 1u << 20);
  require(work.probeTasks() == std::vector<uint32_t>{0, 2} && work.style() == 32 && work.count() == 2,
          "Style probes share only valid base geometry and actual group identities");
  std::vector<b::Result> surfaces(2);
  const auto *allocation = surfaces.data();
  surfaces[0].irradiance[0][0] = 99;
  work.takeSurfaces(surfaces, 2);
  require(surfaces.empty() && work.surfaceCount() == 2, "The completed base workspace is transferred without a second dense map");
  (void)allocation;
  auto refused = [&](auto operation) {
    bool rejected = false;
    try { operation(); } catch (const std::exception &) { rejected = true; }
    require(rejected, "Invalid or cancelled attributed work is refused");
  };
  refused([&] { work.encode(layout, false, {}); });
  refused([&] { work.finish({}); });
  b::Result first, last;
  first.irradiance[0] = {1, 2, 3, 123};
  last.irradiance[1] = {-1, -2, -3, 456};
  work.consume(32, true, 1, {&last, 1});
  refused([&] { work.consume(32, true, 1, {&last, 1}); });
  refused([&] { work.consume(97, true, 0, {&first, 1}); });
  refused([&] { work.consume(32, false, 2, {&first, 1}); });
  auto bad = first;
  bad.irradiance[8][2] = std::numeric_limits<float>::infinity();
  refused([&] { work.consume(32, true, 0, {&bad, 1}); });
  require(work.surfaceReceived == 1, "Invalid readback leaves the accepted batch count unchanged");
  work.consume(32, true, 0, {&first, 1});
  work.consume(32, false, 1, {&last, 1});
  work.consume(32, false, 0, {&first, 1});
  require(work.received(), "Fence completion order need not match submission order");
  std::stop_source stop;
  stop.request_stop();
  refused([&] { work.encode(layout, false, stop.get_token()); });
  require(work.style() == 32 && work.received(), "Cancelled encoding does not advance or partially publish the group");
  work.encode(layout, false, {});
  require(work.style() == 97 && work.ordinal() == 2 && !work.received(), "Next style reuses workspace with independent readback ownership");
  const b::Result black[2]{};
  work.consume(97, true, 0, black);
  work.consume(97, false, 0, black);
  work.encode(layout, false, {});
  require(work.complete(), "Every captured group must finish before serialization");
  const auto words = work.finish({});
  const uint32_t styles[] = {32, 97};
  b::sparse::validate(words, {8, 1, 3}, styles, 1u << 20);
  require(b::sparse::surface(words, 32, 0, 0, 0)[0] == Vec4{1, 2, 3, 0} &&
          b::sparse::surface(words, 32, 0, 1, 0)[1] == Vec4{-1, -2, -3, 0} &&
          b::sparse::probe(words, 32, 1) == b::sparse::Coefficients{} &&
          b::sparse::probe(words, 32, 2)[1] == Vec4{-1, -2, -3, 0},
          "Encoded styles preserve atlas owners and valid-probe remapping, never alpha geometry");
  require(b::sparse::surface(words, 97, 0, 0, 0) == b::sparse::Coefficients{} &&
          b::sparse::probe(words, 97, 2) == b::sparse::Coefficients{},
          "A black next group cannot inherit the previous group's surface or probe energy");
  std::puts("Attributed workspace: reuse, remapping, out-of-order fences, ownership, finite RGB and cancellation passed");
}
