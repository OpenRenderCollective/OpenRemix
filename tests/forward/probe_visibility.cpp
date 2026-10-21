#include "forward/bake/probe_visibility.h"
#include "forward/bake/probe_visibility_plan.h"
#include "forward/bake/probe_visibility_budget.h"
#include "forward/bake/spherical_harmonics.h"
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
  void adaptiveSupport(uint32_t factor) {
    const uint32_t side = factor + 1, recordCount = side * side * side;
    Grid grid;
    grid.header.originSpacing[3] = 8;
    grid.header.dimensions = {2, 2, 2, recordCount};
    grid.header.settings[2] = 5;
    grid.probes.resize(recordCount);
    grid.points.resize(recordCount);
    grid.lookup.resize(20 + ((recordCount + 3) & ~3u));
    for (uint32_t corner = 0; corner < 8; ++corner) grid.lookup[corner] = corner + 1;
    grid.lookup[8] = adaptiveDirectoryMagic;
    grid.lookup[9] = factor;
    grid.lookup[10] = uint32_t(grid.lookup.size());
    grid.lookup[11] = 8;
    grid.lookup[12] = patchReference(20, factor);
    uint32_t next = 8;
    for (uint32_t z = 0; z < side; ++z) for (uint32_t y = 0; y < side; ++y) for (uint32_t x = 0; x < side; ++x) {
      const bool coarse = (x % factor == 0) && (y % factor == 0) && (z % factor == 0);
      const uint32_t record = coarse ? (z / factor * 2 + y / factor) * 2 + x / factor : next++;
      grid.lookup[20 + (z * side + y) * side + x] = record + 1;
    }
    const auto origins = directoryOrigins(grid);
    uint32_t centre = UINT32_MAX;
    for (uint32_t i = 0; i < origins.size(); ++i) {
      const auto &p = origins[i].position;
      grid.points[i].position = {p[0], p[1], p[2], 0};
      if (p[0] == 4 && p[1] == 4 && p[2] == 4) centre = i;
    }
    require(centre != UINT32_MAX && origins[1].spacing == 8.f / factor && origins[1].relocationSpacing == 8,
            "Adaptive fixture shares coarse corners with the fine stencil");
    grid.probes[1].statistics[3] = grid.probes[centre].statistics[3] = 1;
    remix::scene::SphereLight light;
    light.position[0] = 20; light.position[1] = light.position[2] = 4;
    light.radius = .001f;
    light.radiance[0] = .01f;
    const auto outsideFine = planProbeVisibility(grid, std::span(&light, 1), .001f, 1);
    require(outsideFine && outsideFine->tasks.size() == 1 && outsideFine->tasks[0].point == 1,
            "Fine-only records avoid coarse-radius work while shared coarse corners retain full support");
    std::fill_n(light.position, 3, 0);
    grid.points[centre].position = {5.8f, 5.8f, 5.8f, 0};
    const auto relocated = planProbeVisibility(grid, std::span(&light, 1), .001f, 2);
    require(relocated && relocated->tasks.size() == 2,
            "Fine probe support still includes diagonal interpolation and maximal local relocation");
    // Check the planner against the reconstruction stencil, not another copy
    // of its radius formula. Include biased receivers around fine-cell edges
    // and trace displacements at the allowed extent in every axis.
    grid.header.settings[0] = .8f;
    for (uint32_t i = 0; i < origins.size(); ++i) {
      grid.probes[i].statistics[3] = 1;
      for (uint32_t axis = 0; axis < 3; ++axis) {
        const float offset = .45f * origins[i].relocationSpacing * ((i + axis) % 2 ? 1 : -1);
        grid.points[i].position[axis] = origins[i].position[axis] + offset;
        grid.probes[i].irradiance[axis][3] = offset;
      }
      for (auto &moment : grid.probes[i].moments) moment = {1000, 1000000, 0, 1};
    }
    const std::array<std::array<float, 3>, 6> normals{{{1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}}};
    const std::array<float, 6> coordinates{.001f, 1.9f, 3.999f, 4.001f, 6.1f, 7.999f};
    uint32_t coveredCorners = 0;
    for (float z : coordinates) for (float y : coordinates) for (float x : coordinates) {
      light.position[0] = x; light.position[1] = y; light.position[2] = z;
      const auto plan = planProbeVisibility(grid, std::span(&light, 1), .001f, recordCount);
      require(plan.has_value(), "Conservative adaptive probe volume fits the full fixture budget");
      for (const auto &normal : normals) {
        const auto stencil = probeStencil(grid, {x,y,z}, normal);
        for (uint32_t corner = 0; corner < stencil.count; ++corner) {
          if (stencil.weights[corner] <= 0) continue;
          require(std::any_of(plan->tasks.begin(), plan->tasks.end(), [&](const auto &task) {
                    return task.point == stencil.indices[corner];
                  }), "Every contributing coarse/fine receiver corner keeps its light visibility task");
          ++coveredCorners;
        }
      }
    }
    require(coveredCorners > 1000, "Adaptive support parity exercised nonempty reconstruction stencils");
  }
} // namespace
int main() {
  {
    const auto scalar = probeVisibilityBudget(32768, 512, 128ull << 20, false);
    const auto direct = probeVisibilityBudget(32768, 512, 128ull << 20, true);
    require(scalar.bytes == ProbeVisibility::maximumBytes && scalar.pairs == 131072,
            "Scalar-only bakes retain their existing bounded work policy");
    const uint64_t offsets = 32769ull * sizeof(uint32_t);
    require(direct.bytes == ProbeVisibility::maximumBytes && direct.pairs > scalar.pairs &&
                offsets + uint64_t(direct.pairs) * (sizeof(ProbeVisibility::Entry) + sizeof(ProbeVisibility::Direct))
                    <= direct.bytes &&
                offsets + uint64_t(direct.pairs + 1) * (sizeof(ProbeVisibility::Entry) + sizeof(ProbeVisibility::Direct))
                    > direct.bytes,
            "Directional capacity fills the actual storage allowance without truncating its stride");
    require(probeVisibilityBudget(32768, 512, 16ull << 20, true).pairs == 0 &&
                probeVisibilityBudget(8, 0, UINT64_MAX, true).pairs == 0 &&
                probeVisibilityBudget(UINT32_MAX - 1, 1, UINT64_MAX, true).pairs == 0,
            "Grid, lookup overhead and absent lights cannot underflow into an enormous pair budget");
    rejects([&] { probeVisibilityBudget(UINT32_MAX, 1, UINT64_MAX, true); });
    rejects([&] { probeVisibilityBudget(8, 1u << 24, UINT64_MAX, true); });
    Grid grid;
    grid.header.originSpacing = {0, 0, 0, .1f};
    grid.probes.resize(32768); grid.points.resize(32768);
    for (uint32_t i = 0; i < grid.probes.size(); ++i) {
      grid.probes[i].statistics[3] = 1;
      grid.points[i].position = {float(i % 32) * .1f, float(i / 32 % 32) * .1f, float(i / 1024) * .1f, 0};
    }
    std::vector<remix::scene::SphereLight> lights(5);
    for (auto &light : lights) {
      light.position[0] = light.position[1] = light.position[2] = -1;
      light.radius = 1;
      light.radiance[0] = light.radiance[1] = light.radiance[2] = 100;
    }
    const auto oldPlan = planProbeVisibility(grid, lights, .001f, scalar.pairs);
    const auto complete = planProbeVisibility(grid, lights, .001f, direct.pairs);
    require(oldPlan && oldPlan->selectedLights == 4 && oldPlan->deferredLights == 1 &&
                complete && complete->selectedLights == 5 && complete->deferredLights == 0 &&
                complete->tasks.size() == 5 * grid.probes.size(),
            "Storage-backed directional planning retains whole volumes beyond the old 131072-pair cap");
  }
  {
    const UnitDirect golden{1, -1, .5f, -.5f, 0, -0.f, .25f, -.25f, 1};
    const auto packed = packUnitDirect(golden);
    require(packed[0] == 0xbc003c00u && packed[1] == 0xb8003800u &&
                packed[2] == 0x80000000u && packed[3] == 0xb4003400u &&
                packed[4] == 0x00003c00u && packed[5] == 0x3f800000u &&
                unpackUnitDirect(packed) == golden,
            "Unit-direct GPU words use signed half pairs, a zero padding half and full-range amplitude");
    UnitDirect zero{};
    zero[4] = -0.f;
    require(std::signbit(unpackUnitDirect(packUnitDirect(zero))[4]),
            "Zero-amplitude unit-direct fields preserve signed zero without division by zero");
    uint32_t random = 19;
    const auto next = [&] {
      random = random * 1664525u + 1013904223u;
      return float(random >> 8) / float(1u << 24) * 2 - 1;
    };
    for (float scale : {1e-40f, 1e-20f, 1e-4f, 1.f, 1e4f, 1e20f, 1e38f}) {
      for (uint32_t record = 0; record < 100; ++record) {
        UnitDirect coefficients;
        for (float &coefficient : coefficients) coefficient = next() * scale;
        const auto wire = packUnitDirect(coefficients);
        const auto decoded = unpackUnitDirect(wire);
        double maximumError = 0;
        const double amplitude = std::bit_cast<float>(wire[5]);
        for (uint32_t i = 0; i < 9; ++i) {
          require(std::isfinite(decoded[i]), "Finite HDR input cannot become infinite after GPU packing");
          maximumError = std::max(maximumError, std::abs(double(decoded[i]) - coefficients[i]));
        }
        const double tolerance = amplitude / 2048 + 2 * double(std::numeric_limits<float>::denorm_min());
        require(maximumError <= tolerance,
                "Unit-direct packing bounds coefficient error across tiny, signed and HDR fields");
        for (const remix::forward::Vec3 normal : {remix::forward::Vec3{1, 0, 0}, {0, 1, 0}, {0, 0, 1},
                                                 {0, 0, -1}, {.577350269f, -.577350269f, .577350269f}}) {
          const auto basis = irradianceBasis(normal);
          double original = 0, restored = 0, bound = 0;
          for (uint32_t i = 0; i < 9; ++i) {
            original += double(coefficients[i]) * basis[i];
            restored += double(decoded[i]) * basis[i];
            bound += std::abs(double(basis[i])) * tolerance;
          }
          require(std::abs(restored - original) <= bound,
                  "Directional reconstruction error is bounded before final irradiance clamping");
        }
      }
    }
    auto invalid = golden;
    invalid[8] = std::numeric_limits<float>::quiet_NaN();
    rejects([&] { packUnitDirect(invalid); });
    invalid[8] = std::numeric_limits<float>::infinity();
    rejects([&] { packUnitDirect(invalid); });
    auto broken = packed;
    broken[4] |= 1u << 16;
    rejects([&] { unpackUnitDirect(broken); });
    broken = packed;
    broken[5] = std::bit_cast<uint32_t>(-1.f);
    rejects([&] { unpackUnitDirect(broken); });
    broken = packed;
    broken[0] = 0x00003c01u;
    rejects([&] { unpackUnitDirect(broken); });
  }
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
  const auto packed = table.words();
  require(packed[1] == 3 && packed[3] == 10 && packed[4 + 9] != UINT32_MAX && packed[4] == UINT32_MAX &&
              packed.size() * sizeof(uint32_t) == table.gpuBytes(),
          "Shader availability includes blocked lights, rejects uncovered lights and obeys exact wire size");
  {
    std::vector<VisibilityTask> dense;
    for (uint32_t light = 0; light < 66; ++light)
      dense.push_back({0, light, 0});
    ProbeVisibility wide(350000, 66, dense, 2u << 20);
    std::vector<float> masks(dense.size(), .5f);
    wide.observe(dense, masks);
    const auto bounded = wide.words();
    require(bounded[1] == 2 && bounded[3] == 3 && bounded[4] == UINT32_MAX && bounded[6] == 3 &&
                bounded.size() * sizeof(uint32_t) == wide.gpuBytes() && wide.gpuBytes() < (2u << 20),
            "A sparse large probe grid falls back before its ranked directory exceeds the eight-MiB bound");
    ProbeVisibility wideDirectional(350000, 66, dense, 2u << 20, true);
    std::vector<VisibilityResult> directionalResults(dense.size());
    for (uint32_t i = 0; i < directionalResults.size(); ++i) {
      directionalResults[i].visibility = masks[i];
      directionalResults[i].irradiance[0] = float(i + 1);
      directionalResults[i].irradiance[2] = -float(i + 1);
    }
    wideDirectional.observe(dense, directionalResults);
    const auto directionalWire = wideDirectional.words();
    require(directionalWire[1] == 7 && directionalWire[9] == 2 &&
                directionalWire.size() * 4 == wideDirectional.gpuBytes(),
            "Compact directional records also wrap the bounded sorted-entry fallback");
    const auto directIndices = 8 + directionalWire[3];
    const auto directOffset = directIndices + directionalWire[4];
    for (uint32_t i = 0; i < dense.size(); ++i) {
      PackedUnitDirect record;
      const auto field = directionalWire[directIndices + i];
      require(field != 0, "Positive directional fields cannot alias the shared black record");
      std::copy_n(directionalWire.begin() + directOffset + (field - 1) * record.size(), record.size(), record.begin());
      require(unpackUnitDirect(record) == directionalResults[i].irradiance,
              "Rank-directory fallback preserves directional entry order for every overlapping light");
    }
  }
  {
    const uint64_t directionalBytes = bytes + pairs.size() * sizeof(ProbeVisibility::Direct);
    ProbeVisibility directional(3, 10, pairs, directionalBytes, true);
    require(directional.directional() && directional.bytes() == directionalBytes &&
                !directional.sampleDirect(0, 1), "Untraced directional pairs remain unknown");
    std::array<VisibilityResult, pairs.size()> results{};
    for (size_t i = 0; i < results.size(); ++i) {
      results[i].visibility = values[i];
      for (uint32_t coefficient = 0; coefficient < 9; ++coefficient)
        results[i].irradiance[coefficient] = float(int(i * 9 + coefficient) - 20) * .01f;
    }
    auto invalid = results;
    invalid.back().irradiance[8] = std::numeric_limits<float>::infinity();
    rejects([&] { directional.observe(pairs, invalid); });
    rejects([&] { directional.observe(pairs, values); });
    require(!directional.sample(2, 9) && !directional.sampleDirect(0, 1),
            "Failed directional observation publishes neither masks nor partial coefficients");
    directional.observe(pairs, results);
    require(directional.sampleDirect(0, 1) == results[2].irradiance &&
                directional.sampleDirect(2, 9) == results[0].irradiance,
            "Directional coefficients follow sparse captured-light ownership, not submission order");
    const auto wire = directional.words();
    require(wire[1] == 7 && wire[9] == 3 && wire[4] == pairs.size() && wire[6] == 6 && wire[7] == 0 &&
                wire.size() * 4 == directional.gpuBytes(),
            "Directional wrapper preserves ranked scalar masks and exact padded allocation");
    const auto indices = 8 + wire[3];
    const auto coefficientOffset = indices + wire[4];
    for (size_t i = 0; i < directional.direct().size(); ++i) {
      PackedUnitDirect record;
      const auto field = wire[indices + i];
      require(field != 0, "Signed nonzero coefficients must retain a directional field");
      std::copy_n(wire.begin() + coefficientOffset + (field - 1) * record.size(), record.size(), record.begin());
      const auto decoded = unpackUnitDirect(record);
      for (uint32_t coefficient = 0; coefficient < 9; ++coefficient)
        require(std::abs(decoded[coefficient] - directional.direct()[i][coefficient]) < .0005f,
                "Packed GPU coefficients preserve signed sparse ownership within normalized half precision");
    }
    const auto oldGpuBytes = (16 + table.gpuBytes() + pairs.size() * sizeof(ProbeVisibility::Direct) + 15) & ~uint64_t(15);
    require(directional.gpuBytes() < oldGpuBytes,
            "Directional GPU packing saves storage without dropping probe/light pairs");
    const auto restored = ProbeVisibility::restore(3, 10, directional.offsets(), directional.entries(),
                                                   directionalBytes, directional.direct());
    require(restored.words() == wire && restored.sampleDirect(0, 1) == results[2].irradiance,
            "Directional cache payload retains signed SH and shader words exactly");
    rejects([&] { ProbeVisibility(3, 10, pairs, directionalBytes - 1, true); });
    rejects([&] { ProbeVisibility::restore(3, 10, directional.offsets(), directional.entries(),
                                           directionalBytes - 1, directional.direct()); });
    rejects([&] { directional.observe(pairs, results); });
  }
  {
    ProbeVisibility black(3, 10, pairs, bytes + pairs.size() * sizeof(ProbeVisibility::Direct), true);
    std::array<VisibilityResult, pairs.size()> results{};
    // A clear mask with a black directional field is still known coverage;
    // conversely no field may be discarded just because its scalar mask is zero.
    for (auto &value : results) value.visibility = 1;
    results[0].visibility = 0;
    results[0].irradiance[8] = -.75f;
    black.observe(pairs, results);
    const auto words = black.words();
    require(words[5] == 1 && words.size() * 4 == black.gpuBytes(),
            "Exactly black directional fields share one reference without reserving coefficient records");
    const auto indices = 8 + words[3];
    for (uint32_t i = 0; i < black.entries().size(); ++i)
      require((words[indices + i] != 0) == (black.entries()[i].light == 9),
              "Directional blackness is independent of clear/blocked scalar coverage and captured light identity");
    require(black.sample(0, 1) == 1 && black.sampleDirect(0, 1) == UnitDirect{} && !black.sampleDirect(1, 1),
            "A shared black field remains distinct from unallocated probe/light coverage");
  }
  const auto imported = ProbeVisibility::restore(3, 10, table.offsets(), table.entries(), bytes);
  require(imported.sample(0, 1) == .25f && !imported.sample(1, 1),
          "Completed restoration preserves sparse ownership and uncovered probes");
  rejects([&] { ProbeVisibility::restore(3, 10, table.offsets(), table.entries(), bytes - 1); });
  auto badOffsets = std::vector<uint32_t>(table.offsets().begin(), table.offsets().end());
  badOffsets[1] = 7;
  rejects([&] { ProbeVisibility::restore(3, 10, badOffsets, table.entries(), bytes); });
  badOffsets.assign(table.offsets().begin(), table.offsets().end());
  badOffsets[0] = 1;
  rejects([&] { ProbeVisibility::restore(3, 10, badOffsets, table.entries(), bytes); });
  auto badEntries = std::vector<ProbeVisibility::Entry>(table.entries().begin(), table.entries().end());
  badEntries[1].light = badEntries[0].light;
  rejects([&] { ProbeVisibility::restore(3, 10, table.offsets(), badEntries, bytes); });
  badEntries.assign(table.entries().begin(), table.entries().end());
  badEntries[0].visibility = -1;
  rejects([&] { ProbeVisibility::restore(3, 10, table.offsets(), badEntries, bytes); });
  badEntries[0].visibility = std::numeric_limits<float>::quiet_NaN();
  rejects([&] { ProbeVisibility::restore(3, 10, table.offsets(), badEntries, bytes); });
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
    DirectPolicy policy;
    policy.lights.assign(lights.size(), DirectMode::Baked);
    policy.lights[2] = DirectMode::Mixed;
    policy.lights[4] = DirectMode::BakedMixed;
    const auto mixedOnly = planProbeVisibility(grid, lights, .001f, 7, {}, policy);
    require(mixedOnly && mixedOnly->selectedLights == 2 && mixedOnly->tasks.size() == 2 &&
                std::all_of(mixedOnly->tasks.begin(), mixedOnly->tasks.end(),
                            [](const auto &task) { return task.light == 2 || task.light == 4; }),
            "Fully baked lights schedule no probe visibility while mixed and styled indices remain stable");
    policy.lights.pop_back();
    rejects([&] { planProbeVisibility(grid, lights, .001f, 7, {}, policy); });
    require(planned && planned->tasks.size() == 7 && planned->candidates < 100,
            "Spatial lookup prunes distant probes rather than enumerating the full light/probe product");
    require(std::any_of(planned->tasks.begin(), planned->tasks.end(),
                        [](const auto &task) { return task.light == 2 && task.point == 300; }),
            "Stationary toggle/style lights retain visibility candidates");
    const auto bounded = planProbeVisibility(grid, lights, .001f, 6);
    require(bounded && bounded->tasks.size() == 6 && bounded->selectedLights == 6 &&
                bounded->eligibleLights == 7 && bounded->deferredLights == 1,
            "Budget overflow retains complete light volumes rather than discarding the whole map");
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
    grid.header.originSpacing[3] = .1f;
    grid.points.resize(6);
    grid.probes.resize(6);
    for (uint32_t i = 0; i < 6; ++i) {
      grid.points[i].position = {float(i) * 100, 0, 0, 0};
      grid.probes[i].statistics[3] = 1;
    }
    std::array<remix::scene::SphereLight, 6> lights{};
    lights[0].radius = 1;
    lights[0].radiance[0] = 1000000; // first light covers the whole grid, exceeding the pair budget
    for (uint32_t i = 1; i < lights.size(); ++i) {
      lights[i].position[0] = float(i) * 100;
      lights[i].radius = .1f;
      lights[i].radiance[0] = .001f;
    }
    const auto full = planProbeVisibility(grid, lights, .001f, 100);
    const auto selected = planProbeVisibility(grid, lights, .001f, 5);
    require(full && selected && selected->tasks.size() == 5 && selected->selectedLights == 5 &&
                selected->eligibleLights == 6 && selected->deferredLights == 1,
            "An oversized first light cannot starve five smaller complete light volumes");
    for (uint32_t light = 1; light < lights.size(); ++light) {
      auto countLight = [&](const auto &plan) {
        return std::count_if(plan.tasks.begin(), plan.tasks.end(),
                             [&](const auto &task) { return task.light == light; });
      };
      require(countLight(*selected) == countLight(*full),
              "Every selected light retains its complete unbudgeted probe coverage");
    }
    ProbeVisibility table(6, 6, selected->tasks, 1024);
    std::vector<float> masks(selected->tasks.size(), 1);
    table.observe(selected->tasks, masks);
    for (uint32_t probe = 0; probe < 6; ++probe)
      require(!table.sample(probe, 0),
              "Deferred large light remains unknown rather than clear or partially covered");
    require(!planProbeVisibility(grid, lights, .001f, 0),
            "A zero pair budget schedules no trace work or false coverage");
    const auto tied = planProbeVisibility(grid, lights, .001f, 3);
    require(tied && tied->tasks.size() == 3 && tied->tasks[0].light == 1 && tied->tasks[2].light == 3,
            "Equal-cost volumes use stable captured indices, independent of camera order");
  }
  {
    Grid grid;
    grid.header.originSpacing[3] = .1f;
    grid.points.resize(4);
    grid.probes.resize(4);
    for (uint32_t i = 0; i < 4; ++i) {
      grid.points[i].position = {float(i) * 100, 0, 0, 0};
      grid.probes[i].statistics[3] = 1;
    }
    std::array<remix::scene::SphereLight, 5> lights{};
    lights[0].radius = 1;
    lights[0].radiance[0] = 1000000; // Four probes, while its two group partners cost one each.
    for (uint32_t i = 0; i < 3; ++i) lights[i].style = 32;
    for (uint32_t i = 1; i < 3; ++i) {
      lights[i].position[0] = i == 1 ? 0.f : 300.f;
      lights[i].radius = .1f;
      lights[i].radiance[0] = .001f;
    }
    for (uint32_t i = 3; i < 5; ++i) {
      lights[i].position[0] = 150;
      lights[i].radius = 1;
      lights[i].radiance[0] = 2; // Two probes per independent volume.
    }
    lights[3].position[0] = 100;
    lights[3].radius = .1f;
    lights[3].radiance[0] = .001f; // This cheap volume fits first but strands the three-light component.
    const auto legacy = planProbeVisibility(grid, lights, .001f, 6);
    const auto grouped = planProbeVisibility(grid, lights, .001f, 6, {}, {}, true);
    require(legacy && grouped && legacy->selectedLights == 4 && grouped->selectedLights == 3 &&
                grouped->tasks.size() == 6 && grouped->eligibleLights == 5 && grouped->deferredLights == 2 &&
                std::all_of(grouped->tasks.begin(), grouped->tasks.end(), [](const auto &task) { return task.light < 3; }),
            "Repacking one cheaper volume completes a three-light component without raising the pair budget");
    lights[3].style = lights[4].style = 33;
    const auto bounded = planProbeVisibility(grid, lights, .001f, 5, {}, {}, true);
    require(bounded && bounded->tasks.size() == 3 && bounded->selectedLights == 2 &&
                std::all_of(bounded->tasks.begin(), bounded->tasks.end(), [](const auto &task) { return task.light >= 3; }),
            "An oversized component stays entirely unknown while a later fitting component still bakes");
    const auto full = planProbeVisibility(grid, lights, .001f, 100);
    const auto complete = planProbeVisibility(grid, lights, .001f, 100, {}, {}, true);
    require(full && complete && full->tasks.size() == complete->tasks.size() &&
                std::equal(full->tasks.begin(), full->tasks.end(), complete->tasks.begin(),
                           [](const auto &a, const auto &b) { return a.point == b.point && a.light == b.light; }),
            "When all volumes fit, group selection preserves exact transport order and coverage");
    for (uint32_t budget = 0; budget <= 10; ++budget) {
      const auto plan = planProbeVisibility(grid, lights, .001f, budget, {}, {}, true);
      if (!plan) continue;
      require(plan->tasks.size() <= budget, "Group repacking never exceeds its fixed pair budget");
      std::array<bool, 5> selected{};
      for (const auto &task : plan->tasks) selected[task.light] = true;
      require(selected[0] == selected[1] && selected[1] == selected[2] && selected[3] == selected[4],
              "Every controllable component remains entirely selected or entirely unknown at budget boundaries");
      for (uint32_t light = 0; light < lights.size(); ++light)
        if (selected[light])
          require(std::count_if(plan->tasks.begin(), plan->tasks.end(), [&](const auto &task) { return task.light == light; }) ==
                      std::count_if(full->tasks.begin(), full->tasks.end(), [&](const auto &task) { return task.light == light; }),
                  "Selected components retain complete light volumes after eviction and refill");
    }
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
  for (uint32_t factor : {2u, 4u, 8u}) adaptiveSupport(factor);
  std::puts("Sparse probe visibility ownership, overlap, budgets and atomic "
            "readback passed");
}
