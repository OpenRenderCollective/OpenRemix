#include "forward/bake/probe_visibility.h"
#include "forward/bake/probe_visibility_plan.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

using namespace remix::forward::bake;
namespace {
  void require(bool value, const char *message) {
    if (!value) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
  template <typename Function> void rejects(Function function) {
    bool rejected = false;
    try {
      function();
    } catch (const std::exception &) {
      rejected = true;
    }
    require(rejected, "Invalid input must be rejected");
  }
} // namespace
int main() {
  const std::array<VisibilityTask, 6> pairs{
      {{2, 9, 0}, {0, 5, 0}, {0, 1, 0}, {0, 8, 0}, {0, 3, 0}, {0, 7, 0}}};
  constexpr uint64_t bytes = 4 * sizeof(uint32_t) + pairs.size() * sizeof(ProbeVisibility::Entry);
  ProbeVisibility table(3, 10, pairs, bytes);
  require(table.bytes() == bytes && table.offsets()[1] == 5 && table.offsets()[2] == 5,
          "Five overlapping lights and empty probes use only sparse allocated "
          "entries");
  require(table.entries()[0].light == 1 && table.entries()[4].light == 8,
          "Captured indices are sorted without renumbering");
  require(!table.sample(0, 1) && !table.sample(1, 1) && !table.sample(UINT32_MAX, 1),
          "Untraced, unallocated and invalid coverage remains unknown");
  const std::array<float, 6> values{0, 1, .25f, .5f, .75f, 0};
  table.observe(pairs, values);
  require(table.sample(2, 9) == 0 && table.sample(0, 5) == 1 && table.sample(0, 1) == .25f,
          "Readback preserves blocked, clear and partial visibility under "
          "original light ownership");
  rejects([&] { table.observe(pairs, values); });
  rejects([&] { ProbeVisibility undersized(3, 10, pairs, bytes - 1); });
  rejects([&] { ProbeVisibility invalid(2, 10, pairs, bytes); });
  rejects([&] { ProbeVisibility invalid(3, 9, pairs, bytes); });
  const std::array<VisibilityTask, 2> duplicate{{pairs[0], pairs[0]}};
  rejects([&] { ProbeVisibility invalid(3, 10, duplicate, bytes); });
  ProbeVisibility pending(3, 10, pairs, bytes);
  auto bad = values;
  bad.back() = std::numeric_limits<float>::quiet_NaN();
  rejects([&] { pending.observe(pairs, bad); });
  require(!pending.sample(2, 9) && !pending.sample(0, 5), "Failed readback publishes no partial coverage");
  bad.back() = 1.01f;
  rejects([&] { pending.observe(pairs, bad); });
  bad.back() = -1;
  rejects([&] { pending.observe(pairs, bad); });
  const std::array<VisibilityTask, 1> unallocated{{{1, 1, 0}}};
  rejects([&] { pending.observe(unallocated, std::span(values).first(1)); });
  rejects([&] { pending.observe(duplicate, std::span(values).first(2)); });
  require(!pending.sample(2, 9), "Duplicate readback remains unknown");
  rejects([&] { pending.observe(pairs, std::span(values).first(2)); });
  pending.observe(pairs, values);
  require(pending.sample(0, 3) == .75f, "Valid retry can publish after rejected readback");
  ProbeVisibility empty(0, 0, {}, sizeof(uint32_t));
  require(empty.offsets().size() == 1 && empty.entries().empty(),
          "Empty table retains a valid offset sentinel");
  {
    Grid grid;
    grid.header.originSpacing[3] = 1;
    grid.points.resize(1000);
    grid.probes.resize(1000);
    for (uint32_t i = 0; i < 1000; ++i) {
      grid.points[i].position = {float(i) * 10, 0, 0, 0};
      grid.probes[i].statistics[3] = 1;
    }
    std::array<remix::scene::SphereLight, 8> lights{};
    for (uint32_t i = 0; i < lights.size(); ++i) {
      lights[i].position[0] = float(i + 1) * 1000;
      lights[i].radius = .1f;
      lights[i].radiance[0] = .01f;
    }
    lights[1].dynamic = true;
    lights[2].style = 32;
    const auto planned = planProbeVisibility(grid, lights, .001f, 7);
    require(planned && planned->tasks.size() == 7 && planned->candidates < 100,
            "Spatial lookup prunes distant probes rather than enumerating the full light/probe product");
    require(std::any_of(planned->tasks.begin(), planned->tasks.end(),
                        [](const auto &task) { return task.light == 2 && task.point == 300; }),
            "Stationary toggle/style lights retain visibility candidates");
    require(!planProbeVisibility(grid, lights, .001f, 6),
            "Overflow refuses the entire plan instead of returning truncated coverage");
    auto vertical = grid;
    auto verticalLights = lights;
    for (auto &point : vertical.points)
      std::swap(point.position[0], point.position[2]);
    for (auto &light : verticalLights)
      std::swap(light.position[0], light.position[2]);
    const auto verticalPlan = planProbeVisibility(vertical, verticalLights, .001f, 7);
    require(verticalPlan && verticalPlan->tasks.size() == 7 && verticalPlan->candidates < 100,
            "The broad phase chooses the widest spatial axis for vertical maps too");
    const auto repeated = planProbeVisibility(grid, lights, .001f, 7);
    require(
        repeated && repeated->tasks.size() == planned->tasks.size() &&
            std::equal(planned->tasks.begin(), planned->tasks.end(), repeated->tasks.begin(),
                       [](const auto &a, const auto &b) { return a.point == b.point && a.light == b.light; }),
        "Planning is deterministic under stable captured indices");
    grid.probes[100].statistics[3] = 0;
    require(planProbeVisibility(grid, lights, .001f, 7)->tasks.size() == 6,
            "Unusable solid/outside probes schedule no visibility rays");
    std::stop_source stop;
    stop.request_stop();
    rejects([&] { planProbeVisibility(grid, lights, .001f, 7, stop.get_token()); });
    rejects([&] { planProbeVisibility(grid, lights, 0, 7); });
    grid.points[300].position[0] = std::numeric_limits<float>::quiet_NaN();
    rejects([&] { planProbeVisibility(grid, lights, .001f, 7); });
  }
  {
    Grid grid;
    grid.header.originSpacing[3] = 1;
    grid.points.resize(1);
    grid.probes.resize(1);
    grid.probes[0].statistics[3] = 1;
    // Receiver at the origin can use the diagonally opposite cell corner,
    // relocated another .45 cells per axis. It lies outside light influence,
    // but excluding it would create missing interpolation coverage.
    grid.points[0].position = {1.45f, 1.45f, 1.45f, 0};
    remix::scene::SphereLight light;
    light.radius = .001f;
    light.radiance[0] = .01f;
    const auto halo = planProbeVisibility(grid, std::span(&light, 1), .001f, 1);
    require(halo && halo->tasks.size() == 1,
            "Cell interpolation and relocated corners are retained beyond the light influence sphere");
    grid.points[0].position = {0, 0, 0, 0};
    const auto moved = planProbeVisibility(grid, std::span(&light, 1), .001f, 1);
    require(moved && moved->tasks[0].point == 0,
            "Actual relocated positions, not original grid-coordinate approximations, determine candidates");
  }
  std::puts("Sparse probe visibility ownership, overlap, budgets and atomic "
            "readback passed");
}
