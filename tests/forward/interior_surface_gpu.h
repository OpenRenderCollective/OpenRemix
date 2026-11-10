#pragma once
#include "forward/bake/transport.h"
#include "forward/lightmap/interior_donors.h"
#include "gpu_fixture.h"
#include "runtime/pipelines.h"
#include <array>
#include <chrono>
#include <cmath>
#include <iterator>
#include <thread>

namespace remix::forward::test {
  template <class World>
  void interiorSurfaceDonors(Fixture &fixture, pipeline::Service &pipelines, World &world) {
    namespace b = remix::forward::bake;
    namespace lm = remix::forward::lightmap;
    const auto dark = world.material({});
    const auto bottom = world.wall(world.material({.emission = 2}), -1, .5f);
    const auto top = world.quad(dark, {{{-.5f, -.5f, 1}, {.5f, -.5f, 1},
                                       {.5f, .5f, 1}, {-.5f, .5f, 1}}}, nullptr, 0xffffffff, {0, 0, 1});
    const auto left = world.quad(dark, {{{-.5f, -.5f, -1}, {-.5f, .5f, -1},
                                        {-.5f, .5f, 1}, {-.5f, -.5f, 1}}}, nullptr, 0xffffffff, {-1, 0, 0});
    const auto right = world.quad(dark, {{{.5f, -.5f, -1}, {.5f, .5f, -1},
                                         {.5f, .5f, 1}, {.5f, -.5f, 1}}}, nullptr, 0xffffffff, {1, 0, 0});
    const auto front = world.quad(dark, {{{-.5f, -.5f, -1}, {.5f, -.5f, -1},
                                         {.5f, -.5f, 1}, {-.5f, -.5f, 1}}}, nullptr, 0xffffffff, {0, -1, 0});
    const auto rear = world.quad(dark, {{{-.5f, .5f, -1}, {.5f, .5f, -1},
                                        {.5f, .5f, 1}, {-.5f, .5f, 1}}}, nullptr, 0xffffffff, {0, 1, 0});
    const auto source = world.scene({bottom, top, left, right, front, rear});
    const b::Point points[] = {{{0, 0, 0, 0}, {0, 0, -1, 0}},
                               {{.75f, 0, 0, 0}, {0, 0, -1, 0}}};
    for (const bool parallel : {false, true}) {
      b::Transport transport;
      b::Options options;
      options.parallel = parallel;
      options.batchPoints = 2;
      auto job = pipelines.prepare(parallel ? "interior-surface-parallel" : "interior-surface-serial", [&] {
        transport.initialize(fixture.physical, fixture.device, source, options);
      });
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
      while (!job->ready() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      require(job->ready(), "Interior surface preparation timed out");
      job->check();
      fixture.begin();
      transport.recordPrepare(fixture.command, world.resources);
      transport.recordBatch(fixture.command, 0, points, 512, 1);
      fixture.finish();
      std::array<b::Result, 2> values;
      transport.read(0, values);
      require(values[0].statistics[0] > .95f && values[0].statistics[1] == 0 &&
                  values[0].irradiance[0][0] > .1f && values[1].statistics[0] < .05f &&
                  std::abs(values[1].irradiance[0][0]) < 1e-5f,
              "Closed fixture contains bright interior lighting but blocks the exterior receiver");
      lm::Layout layout;
      layout.pageSize = 2;
      layout.pages = 1;
      layout.charts = {{0, 0, 0, 2, 1}};
      layout.points.assign(std::begin(points), std::end(points));
      layout.pointCharts = {0, 0};
      layout.owners = {0, 1, lm::unowned, lm::unowned};
      const auto repaired = lm::repairInteriorDonors(layout, values);
      require(repaired.hiddenSamples == 1 && repaired.repairedTexels == 1 &&
                  layout.owners[0] == 1 && layout.owners[1] == 1,
              "Hidden fixture sample cannot leak its emission into exterior lightmap filtering");
    }
  }
} // namespace remix::forward::test
