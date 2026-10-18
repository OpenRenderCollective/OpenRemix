#pragma once
#include "forward/bake/transport.h"
#include "forward/bake/styles.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include "gpu_fixture.h"
#include "runtime/pipelines.h"
#include <chrono>
#include <thread>

namespace remix::forward::test {
  // A single immutable geometry preparation serves every component. Geometry,
  // throughput, random paths and PDFs are shared; only attributed energy changes.
  template <class World>
  void lightingComponents(Fixture &fixture, pipeline::Service &pipelines, World &world) {
    namespace b = remix::forward::bake;
    const auto wall = world.wall(world.material({.diffuse = .65f, .emission = .15f}), 2, 3);
    const uint8_t mask[] = {255, 255, 255, 0, 255, 255, 255, 255};
    world.texture(0x9340, mask);
    const auto cutout = world.wall(world.material({.diffuse = .4f, .albedo = L"0x9340", .cutout = true}), .8f, .6f);
    auto source = world.scene({wall, cutout});
    source->dome.enabled = true;
    source->dome.radiance = {.03f, .04f, .05f};
    source->sun.isDistant = true;
    source->sun.direction[0] = source->sun.direction[1] = 0;
    source->sun.direction[2] = 1;
    source->sun.radiance[0] = .2f;
    for (uint32_t i = 0; i < 3; ++i) {
      scene::SphereLight light;
      light.id = 0x9341 + i;
      light.position[0] = (float(i) - 1) * .7f;
      light.position[1] = .2f;
      light.position[2] = .3f;
      light.radius = .2f;
      light.radiance[i == 0 ? 2 : i - 1] = 8;
      light.style = i == 0 ? 0 : i == 1 ? 32 : 97;
      source->lights.push_back(light);
    }
    for (bool wrongTag : {false, true}) {
      auto packed = b::pack(*source);
      if (wrongTag) packed.lights[1].shaping[3] = 0;
      else packed.lights.pop_back();
      b::Transport invalid;
      b::Options componentOptions;
      componentOptions.lightingComponents = true;
      bool refused = false;
      try { invalid.initialize(fixture.physical, fixture.device, source, std::move(packed), componentOptions); }
      catch (const std::invalid_argument &) { refused = true; }
      require(refused, "Caller-packed component data must retain captured light count and style ownership");
    }
    const b::Point points[] = {{{-.4f, 0, 0, 0}, {0, 0, 1, 0}},
                              {{0, 0, 0, 0}, {0, 0, 1, 0}},
                              {{.4f, 0, 0, 0}, {0, 0, 1, 0}},
                              {{10, 0, 0, 0}, {0, 0, 1, 0}}};
    using Results = std::array<b::Result, 4>;
    uint32_t generation = 0;
    auto prepare = [&](b::Transport &transport, const auto &scene, const b::Options &options) {
      auto job = pipelines.prepare("lighting-components-" + std::to_string(++generation), [&] {
        transport.initialize(fixture.physical, fixture.device, scene, options);
      });
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
      while (!job->ready() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      require(job->ready(), "Component transport preparation completes within its deadline");
      job->check();
      fixture.begin();
      transport.recordPrepare(fixture.command, world.resources);
      fixture.finish();
      transport.releaseBuildInputs();
    };
    float maximumSumError = 0, maximumMixError = 0;
    for (bool parallel : {false, true}) for (bool importance : {false, true}) {
      b::Options options;
      options.textureResolution = 16;
      options.batchPoints = options.initialBatchPoints = 4;
      options.parallel = parallel;
      options.lightImportanceSampling = importance;
      options.lightingComponents = true;
      // Cover aggregate diffuse as well as ordinary indirect transport.
      if (parallel) {
        const uint32_t unstyled[] = {0};
        options.directLighting = b::directPolicy(*source, unstyled);
      }
      b::Transport transport;
      prepare(transport, source, options);
      const auto memory = transport.memoryBytes(), acceleration = transport.accelerationBytes();
      auto trace = [&](b::Transport &target, uint32_t component, bool radiance) {
        Results result;
        fixture.begin();
        if (radiance) target.recordRadianceComponent(fixture.command, 0, points, component, 3, 256, 13);
        else target.recordComponent(fixture.command, 0, points, component, 256, 3, 13);
        fixture.finish();
        target.read(0, result);
        return result;
      };
      for (bool radiance : {false, true}) {
        const auto base = trace(transport, 0, radiance);
        const auto red = trace(transport, 32, radiance);
        const auto green = trace(transport, 97, radiance);
        const auto combined = trace(transport, b::Transport::combinedLighting, radiance);
        double redEnergy = 0, greenEnergy = 0;
        const uint32_t coefficients = radiance ? 1 : 9;
        for (uint32_t point = 0; point < 4; ++point) {
          require(base[point].statistics == red[point].statistics &&
                  base[point].statistics == green[point].statistics &&
                  base[point].statistics == combined[point].statistics,
                  "Components retain identical hit/backface/miss/count metadata");
          require(base[point].moments == red[point].moments && base[point].moments == green[point].moments,
                  "Component isolation does not change geometry paths or distance moments");
          for (uint32_t coefficient = 0; coefficient < coefficients; ++coefficient) {
            const auto &a = base[point].irradiance[coefficient];
            const auto &r = red[point].irradiance[coefficient];
            const auto &g = green[point].irradiance[coefficient];
            require(r[1] == 0 && r[2] == 0 && g[0] == 0 && g[2] == 0,
                    "Two nonconsecutive style IDs retain independent coloured energy");
            redEnergy += std::abs(r[0]);
            greenEnergy += std::abs(g[1]);
            for (uint32_t channel = 0; channel < 3; ++channel) {
              const float error = std::abs(a[channel] + r[channel] + g[channel] -
                                           combined[point].irradiance[coefficient][channel]);
              maximumSumError = std::max(maximumSumError, error);
              require(error < 2e-5f, "Base plus styled components reconstruct the combined authored lighting");
            }
          }
        }
        require(redEnergy > .001 && greenEnergy > .001, "Both style groups contribute measurable energy");
        require(transport.memoryBytes() == memory && transport.accelerationBytes() == acceleration,
                "All component passes reuse the prepared geometry and fixed batch storage");
        if (!importance) {
          auto weightedSource = std::make_shared<scene::BakeScene>(*source);
          for (auto &light : weightedSource->lights)
            if (light.style) for (auto &value : light.radiance) value *= light.style == 32 ? .25f : .75f;
          const auto scales = b::styleScales(*source, weightedSource->lights);
          require(scales.size() == 2 && scales[0].status == b::StyleStatus::Ready &&
                  scales[1].status == b::StyleStatus::Ready,
                  "Current source radiance resolves valid scalar style weights");
          b::Transport weighted;
          prepare(weighted, weightedSource, options);
          const auto reference = trace(weighted, b::Transport::combinedLighting, radiance);
          for (uint32_t point = 0; point < 4; ++point)
            for (uint32_t coefficient = 0; coefficient < coefficients; ++coefficient)
              for (uint32_t channel = 0; channel < 3; ++channel) {
                const float mixed = base[point].irradiance[coefficient][channel] +
                                     red[point].irradiance[coefficient][channel] * scales[0].value +
                                     green[point].irradiance[coefficient][channel] * scales[1].value;
                const float error = std::abs(mixed - reference[point].irradiance[coefficient][channel]);
                maximumMixError = std::max(maximumMixError, error);
                require(error < 2e-5f, "Component style scalars match a physically reweighted light-source reference");
              }
        }
      }
      const auto fullBatch = trace(transport, 32, false);
      fixture.begin();
      transport.recordComponent(fixture.command, 0, std::span(points).last(2), 32, 256, 3, 13);
      fixture.finish();
      std::array<b::Result, 2> tail;
      transport.read(0, tail);
      for (uint32_t point = 0; point < 2; ++point)
        require(tail[point].irradiance == fullBatch[point + 2].irradiance &&
                tail[point].moments == fullBatch[point + 2].moments,
                "Component identity is independent of CPU point-batch boundaries");
      for (uint32_t invalid : {7u, 129u, UINT32_MAX}) {
        bool refused = false;
        try { transport.recordComponent(VK_NULL_HANDLE, 0, points, invalid, 256); }
        catch (const std::invalid_argument &) { refused = true; }
        require(refused, "Invalid or absent style groups are refused before recording commands");
      }
      (void)trace(transport, 32, true);
      fixture.begin();
      transport.recordRadiance(fixture.command, 0, std::span(points).last(1), 3, 256, 13);
      fixture.finish();
      b::Result legacy;
      transport.read(0, {&legacy, 1});
      for (uint32_t c = 0; c < 3; ++c)
        require(std::abs(legacy.irradiance[0][c] - source->dome.radiance[c]) < 1e-6f,
                "Ordinary radiance resets component selection and preserves the base environment");
    }
    b::Transport ordinary;
    b::Options defaults;
    defaults.textureResolution = 16;
    defaults.batchPoints = defaults.initialBatchPoints = 4;
    prepare(ordinary, source, defaults);
    bool refused = false;
    try { ordinary.recordComponent(VK_NULL_HANDLE, 0, points, 32, 256); }
    catch (const std::invalid_argument &) { refused = true; }
    require(refused, "Component passes require explicit preparation with styled sampling power");
    std::printf("Lighting components: additive SH/radiance max error %.9g; physical scalar mix %.9g; "
                "serial/parallel, uniform/spatial, alpha coverage and shared acceleration passed\n",
                maximumSumError, maximumMixError);
  }
} // namespace remix::forward::test
