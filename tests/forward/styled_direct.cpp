#include "forward/bake/styled_direct.h"
#include "forward/mixed/directional_fields.h"
#include "forward/half.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace remix;
using namespace remix::forward;
namespace {
  void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
  }
  template <typename Function> void rejects(Function function) {
    try { function(); } catch (const std::exception &) { return; }
    require(false, "Invalid/cancelled grouped input must reject");
  }
}
int main() {
  scene::BakeScene source;
  source.lights.resize(4);
  source.lights[0].style = source.lights[1].style = 32;
  source.lights[2].style = 127;
  source.lights[0].radiance[0] = 2;
  source.lights[1].radiance[2] = 3;
  source.lights[2].radiance[1] = 4;
  source.lights[3].radiance[0] = 100; // permanent light must not join a controllable group
  lightmap::Layout layout;
  layout.pageSize = 8; layout.pages = 1;
  layout.charts.push_back({0, 0, 0, 8, 8});
  layout.points.resize(1); layout.pointCharts = {0};
  layout.owners.assign(64, 0);
  std::vector<mixed::DirectionalLink> owners{{0,0}, {0,1}, {0,2}, {0,3}};
  auto fields = [&](bool missing) {
    mixed::DirectionalFields table(1, 4);
    for (uint32_t light = 0; light < 4; ++light) {
      if (missing && light == 1) continue;
      std::vector<mixed::directional::Field> coefficients(64);
      for (uint32_t i = 0; i < 64; ++i) {
        coefficients[i][0] = half::encode(float(light + 1));
        coefficients[i][1] = half::encode(-float(i) / 64);
      }
      require(table.add(0, light, layout.charts[0], coefficients), "Fixture fields fit");
    }
    return table.words();
  };
  auto probes = [&](bool missing) {
    std::vector<bake::VisibilityTask> tasks{{0,0,0}, {0,1,0}, {0,2,0}};
    if (missing) tasks.erase(tasks.begin()+1);
    bake::ProbeVisibility result(1, 4, tasks, 4096, true);
    std::vector<bake::VisibilityResult> values(tasks.size());
    for (uint32_t i = 0; i < tasks.size(); ++i) {
      values[i].visibility = 1;
      values[i].irradiance[0] = float(tasks[i].light + 1);
      values[i].irradiance[1] = -.5f;
    }
    result.observe(tasks, values);
    return result;
  };
  const auto complete = fields(false);
  const auto visibility = probes(false);
  const auto grouped = bake::groupStyledDirect(source, layout, owners, complete, visibility, 1u<<20);
  require(grouped.styles == std::vector<uint32_t>{32,127} && grouped.declined.empty(),
          "Two complete independent style groups publish");
  bake::sparse::validate(grouped.words, {8,1,1}, grouped.styles, 1u<<20);
  auto value = bake::sparse::surface(grouped.words, 32, 0, 7, 7);
  require(value[0][0] == 2 && value[0][2] == 6 && value[0][1] == 0 &&
          value[1][0] == -1.96875f && value[1][2] == -2.953125f,
          "Shared-style radiance sums signed SH without permanent-light contamination");
  value = bake::sparse::probe(grouped.words, 127, 0);
  require(value[0][1] == 12 && value[1][1] == -2 && value[0][0] == 0,
          "Probe coefficients use the same independently attributed colour envelope");
  auto cachedLayout = layout;
  cachedLayout.points.clear(); cachedLayout.pointCharts.clear(); cachedLayout.owners.clear();
  const auto restored = bake::groupStyledDirect(source, cachedLayout, owners, complete, visibility, 1u<<20);
  require(restored.words == grouped.words,
          "Cache layouts without transient receiver arrays rebuild the exact grouped publication");
  const auto incomplete = bake::groupStyledDirect(source, layout, owners, fields(true), visibility, 1u<<20);
  require(incomplete.styles == std::vector<uint32_t>{127} && incomplete.declined.size() == 1 &&
          incomplete.declined[0].reason == bake::StyledDirectDecline::SurfaceCoverage,
          "One missing positive owner declines its whole style, preserving other groups");
  const auto unknownProbe = bake::groupStyledDirect(source, layout, owners, complete, probes(true), 1u<<20);
  require(unknownProbe.styles == std::vector<uint32_t>{127} && unknownProbe.declined[0].reason ==
          bake::StyledDirectDecline::ProbeCoverage, "A deferred probe volume cannot become black grouped energy");
  const std::vector<bake::VisibilityTask> partialTasks{{0,0,0}, {0,1,0}, {0,2,0}};
  bake::ProbeVisibility partial(1, 4, partialTasks, 4096, true);
  bake::VisibilityResult received{}; received.visibility = 1; received.irradiance[0] = 3;
  partial.observe(std::span(partialTasks).subspan(2), std::span(&received, 1));
  const auto partialGroups = bake::groupStyledDirect(source, layout, owners, complete, partial, 1u<<20);
  require(partialGroups.styles == std::vector<uint32_t>{127},
          "Unreceived entries decline their own group without discarding an independently complete group");
  const auto tight = bake::groupStyledDirect(source, layout, owners, complete, visibility, 64);
  require(tight.styles.empty() && tight.declined.size() == 2,
          "Storage failure publishes no partial style component");
  bake::sparse::validate(tight.words, {8,1,1}, {}, 64);
  const auto oneFits = bake::groupStyledDirect(source, layout, owners, complete, visibility, 16000);
  require(oneFits.styles == std::vector<uint32_t>{32} && oneFits.declined.size() == 1,
          "A later storage failure preserves the first complete group and no partial second group");
  {
    scene::BakeScene overlap;
    overlap.lights.resize(7);
    mixed::DirectionalFields fields(1, 7);
    std::vector<mixed::DirectionalLink> links;
    std::vector<bake::VisibilityTask> tasks;
    std::vector<bake::VisibilityResult> values;
    for (uint32_t light = 0; light < 7; ++light) {
      overlap.lights[light].style = 32;
      overlap.lights[light].radiance[0] = 1;
      std::vector<mixed::directional::Field> coefficients(64);
      for (auto &field : coefficients) field[0] = half::encode(float(light + 1));
      require(fields.add(0, light, layout.charts[0], coefficients), "Seven-owner fields fit");
      links.push_back({0,light}); tasks.push_back({0,light,0});
      bake::VisibilityResult value{}; value.visibility = 1; value.irradiance[0] = float(light + 1);
      values.push_back(value);
    }
    bake::ProbeVisibility masks(1, 7, tasks, 4096, true); masks.observe(tasks, values);
    const auto seven = bake::groupStyledDirect(overlap, layout, links, fields.words(), masks, 1u<<20);
    require(bake::sparse::surface(seven.words, 32, 0, 0, 0)[0][0] == 28 &&
            bake::sparse::probe(seven.words, 32, 0)[0][0] == 28,
            "All seven overlapping emitters join the group without legacy four-owner truncation");
  }
  {
    scene::BakeScene edgeSource;
    edgeSource.lights.resize(2);
    edgeSource.lights[0].style = 32; edgeSource.lights[0].radiance[0] = 1;
    edgeSource.lights[1].style = 127; edgeSource.lights[1].radiance[1] = 1;
    lightmap::Layout edgeLayout;
    edgeLayout.pageSize = 33; edgeLayout.pages = 2;
    edgeLayout.charts = {{0,15,15,3,3}, {1,32,32,1,1}};
    const std::vector<mixed::DirectionalLink> links{{0,0},{1,1}};
    mixed::DirectionalFields table(2,2);
    for (uint32_t chart = 0; chart < 2; ++chart) {
      std::vector<mixed::directional::Field> values(chart ? 1 : 9);
      for (auto &field : values) {
        field[0] = half::encode(chart ? 3.f : 2.f);
        field[chart ? 2 : 1] = half::encode(chart ? -.75f : -.5f);
      }
      require(table.add(chart,chart,edgeLayout.charts[chart],values), "Edge fields fit");
    }
    const std::vector<bake::VisibilityTask> tasks{{0,0,0},{0,1,0}};
    bake::ProbeVisibility probes(1,2,tasks,4096,true);
    std::vector<bake::VisibilityResult> values(2);
    for (uint32_t light = 0; light < 2; ++light) {
      values[light].visibility = 1;
      values[light].irradiance[0] = light ? 3.f : 2.f;
      values[light].irradiance[light ? 2 : 1] = light ? -.75f : -.5f;
    }
    probes.observe(tasks,values);
    const auto grouped = bake::groupStyledDirect(edgeSource,edgeLayout,links,table.words(),probes,1u<<20);
    bake::sparse::Builder exhaustive({33,2,1},1u<<20);
    for (uint32_t light = 0; light < 2; ++light) {
      auto expected = [&] {
        bake::sparse::Coefficients c{};
        c[0][light] = light ? 3.f : 2.f;
        c[light ? 2 : 1][light] = light ? -.75f : -.5f;
        return c;
      };
      exhaustive.addSampled(light ? 127 : 32, [&](uint32_t page,uint32_t x,uint32_t y) {
        const auto &r = edgeLayout.charts[light];
        return page == r.page && x >= r.x && x < r.x+r.width && y >= r.y && y < r.y+r.height
            ? expected() : bake::sparse::Coefficients{};
      }, [&](uint32_t) { return expected(); });
    }
    require(grouped.words == exhaustive.serialize(),
            "Style occupancy preserves exact signed chart edges, cross-tile donors, pages and independent probes");
  }
  auto stop = std::stop_source{}; stop.request_stop();
  rejects([&] { bake::groupStyledDirect(source, layout, owners, complete, visibility, 1u<<20, stop.get_token()); });
  owners.push_back(owners.back());
  rejects([&] { bake::groupStyledDirect(source, layout, owners, complete, visibility, 1u<<20); });
  std::puts("Grouped styled direct capture reuse passed");
}
