#include "scene/light_gather.h"
#include <cstdio>
#include <cstdlib>
#include <random>

namespace {
  void require(bool value, const char *message) {
    if (!value) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
} // namespace

// The gathered input must be exactly what gathering everything gives (spheres,
// then the runs' lights with their dynamic flag, none without size), and the
// changes it names must cover every light that differs from last frame's.
int main() {
  using namespace remix::scene;
  std::mt19937 random(4242);
  auto list = [&](uint64_t id, size_t count) {
    auto lights = std::make_shared<std::vector<SphereLight>>(count);
    for (size_t i = 0; i < count; ++i) {
      auto &light = (*lights)[i];
      light.id = id * 1000 + i;
      light.triangle = 1;
      light.group = id;
      light.rank = uint32_t(i);
      light.radius = i % 9 == 4 ? 0.f : .5f; // some without size
      light.position[0] = float(random() % 1000);
      light.radiance[0] = float(1 + random() % 20);
    }
    return lights;
  };
  struct Source {
    std::shared_ptr<std::vector<SphereLight>> lights;
    std::shared_ptr<std::vector<SurfaceEmissionCache::Triangle>> reduction;
    bool animated = false, dynamic = false;
  };
  std::vector<Source> sources;
  for (uint64_t id = 1; id <= 12; ++id) {
    Source source{list(id, 4 + random() % 30)};
    source.reduction = std::make_shared<std::vector<SurfaceEmissionCache::Triangle>>(source.lights->size());
    for (size_t i = 0; i < source.lights->size(); ++i)
      (*source.reduction)[i].layout = id * 7919 + i;
    source.animated = id % 5 == 0;
    source.dynamic = id % 4 == 0;
    sources.push_back(source);
  }
  std::vector<SphereLight> spheres(6);
  for (size_t i = 0; i < spheres.size(); ++i) {
    spheres[i].id = 1 + i;
    spheres[i].radius = .1f;
  }
  LightGather gather;
  LightBvh::InputChanges changes;
  std::vector<SphereLight> previous, expected;
  EmissionHistory history, expectedHistory;
  uint64_t hinted = 0;
  for (uint32_t frame = 0; frame < 600; ++frame) {
    // A sphere moves; now and then a run's list is replaced (an instance that
    // moved), by an equal one or a changed one; runs come and go.
    spheres[frame % spheres.size()].position[1] = float(frame % 13);
    if (frame % 7 == 0) {
      auto &source = sources[random() % sources.size()];
      auto copy = std::make_shared<std::vector<SphereLight>>(*source.lights);
      if (frame % 14 == 0)
        (*copy)[random() % copy->size()].position[2] += 1;
      source.lights = copy;
    }
    if (frame % 53 == 0)
      std::swap(sources[random() % sources.size()], sources[random() % sources.size()]);
    if (frame == 300)
      sources.pop_back();
    if (frame % 61 == 0) {
      auto added = spheres.back();
      added.id += 100;
      spheres.push_back(added);
    }
    TraceFrame trace;
    for (const auto &source : sources)
      trace.emitters.push_back({source.lights, source.reduction, source.animated, source.dynamic});
    // The animated runs' emission goes through the history, smoothing it.
    history.begin(1, frame * .016, 1);
    expectedHistory.begin(1, frame * .016, 1);
    const bool emissive = frame < 500 || frame > 520;
    const auto &lights = gather.gather(spheres, trace, nullptr, history, {emissive, true}, changes);
    history.end();
    expected.assign(spheres.begin(), spheres.end());
    if (emissive)
      for (const auto &source : sources)
        for (size_t i = 0; i < source.lights->size(); ++i) {
          auto light = (*source.lights)[i];
          light.dynamic = source.dynamic;
          if (source.animated)
            expectedHistory.apply(light, (*source.reduction)[i].layout);
          if (light.radius > 1e-8f)
            expected.push_back(light);
        }
    expectedHistory.end();
    require(lights == expected, "The gathered lights are not those of gathering everything");
    require(gather.sphereCount() == spheres.size(), "The gather lost the sphere count");
    if (changes.previous) {
      ++hinted;
      require(changes.previous + 1 == changes.serial && lights.size() == previous.size(),
              "A hint for another input or size");
      size_t r = 0;
      for (uint32_t i = 0; i < lights.size(); ++i) {
        while (r < changes.ranges.size() && changes.ranges[r].second <= i)
          ++r;
        const bool named = r < changes.ranges.size() && changes.ranges[r].first <= i;
        require(named || lights[i] == previous[i], "A changed light outside the named changes");
      }
    }
    previous = lights;
  }
  require(hinted > 400, "The gather rarely kept its layout");
  std::printf("Light gather: %llu of 600 frames kept their layout and named their changes\n",
              (unsigned long long)hinted);
}
