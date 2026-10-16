#include "scene/fixtures.h"
#include <limits>
#include <remix/remix_bake_ext.h>

using namespace remix;
using namespace remix::test;
int main() {
  NativeResources resources;
  auto mat = material(resources, 10, 1, 1, 1);
  auto mesh = quad(resources, 20, mat);
  uint64_t instances[] = {resources.createRetained(instance(mesh)),
                          resources.createRetained(instance(mesh, 4))};
  float uv[] = {0, 0, 4, 4, 4, 0, 0, 0, 0, 4, 4, 4};
  remixapi_BakeSurfaceChartEXT charts[] = {{instances[0], 0, 0, 0, 6, 5, 5, uv},
                                           {instances[1], 0, 0, 0, 6, 5, 5, uv}};
  remixapi_BakeSceneSurfacesInfoEXT info{};
  auto &base = info.bounds.scene;
  base = {sizeof(info),
          REMIXAPI_BAKE_SCENE_SURFACES_VERSION_EXT,
          7,
          1,
          1,
          REMIXAPI_BAKE_SCENE_READY_EXT,
          2,
          0,
          instances,
          nullptr};
  for (uint32_t c = 0; c < 3; ++c) {
    info.bounds.minimum[c] = -16;
    info.bounds.maximum[c] = 16;
  }
  info.chartRevision = 1;
  info.chartCount = 2;
  info.charts = charts;
  resources.setBakeScene(&base);
  const auto original = resources.bakeScene();
  require(original->charts.size() == 2 && original->charts[1].draw == 1 &&
              original->charts[0].texcoords[1][0] == 4,
          "Chart ownership resolves to the correct retained draw and copies corner UVs");
  resources.setBakeScene(&base);
  require(resources.bakeScene() == original, "Unchanged chart revisions reuse the immutable snapshot");
  ++info.chartRevision;
  resources.setBakeScene(&base);
  require(resources.bakeScene()->contentHash == original->contentHash,
          "Chart scheduling revisions do not change content identity");
  std::swap(instances[0], instances[1]);
  std::swap(charts[0], charts[1]);
  ++info.chartRevision;
  resources.setBakeScene(&base);
  require(resources.bakeScene()->contentHash == original->contentHash,
          "Membership and chart order do not change chart-to-geometry content identity");
  uv[3] = 3;
  ++info.chartRevision;
  resources.setBakeScene(&base);
  auto changed = resources.bakeScene();
  require(changed->contentHash != original->contentHash && original->charts[0].texcoords[1][0] == 4,
          "UV edits invalidate content without changing an older worker's charts");
  // A fold smaller than a texel is still invalid even if no sample centre
  // lies inside its overlap. Restore the shared edge after the check.
  uv[3] = 4;
  uv[2] = 3.99f;
  ++info.chartRevision;
  rejected([&] { resources.setBakeScene(&base); });
  uv[2] = std::numeric_limits<float>::quiet_NaN();
  ++info.chartRevision;
  rejected([&] { resources.setBakeScene(&base); });
  require(resources.bakeScene() == changed, "Nonfinite chart input is rejected atomically");
  uv[2] = 5;
  rejected([&] { resources.setBakeScene(&base); });
  uv[2] = 4;
  charts[0].instance = 0;
  rejected([&] { resources.setBakeScene(&base); });
  charts[0] = charts[1];
  rejected([&] { resources.setBakeScene(&base); }); // overlapping range ownership
  info.chartCount = 1;
  charts[0].instanceIndex = 1;
  rejected([&] { resources.setBakeScene(&base); });
  charts[0].instanceIndex = 0;
  charts[0].surfaceIndex = 1;
  rejected([&] { resources.setBakeScene(&base); });
  charts[0].surfaceIndex = 0;
  charts[0].firstIndex = 3;
  rejected([&] { resources.setBakeScene(&base); });
  charts[0].firstIndex = 0;
  charts[0].width = 1;
  rejected([&] { resources.setBakeScene(&base); });
  charts[0].width = 5;
  base.state = REMIXAPI_BAKE_SCENE_LOADING_EXT;
  rejected([&] { resources.setBakeScene(&base); });
  base.state = REMIXAPI_BAKE_SCENE_READY_EXT;
  resources.setBakeScene(&base);
  const auto saved = resources.bakeScene();
  base.version = REMIXAPI_BAKE_SCENE_BOUNDS_VERSION_EXT;
  base.structSize = sizeof(info.bounds);
  resources.setBakeScene(&base);
  require(resources.bakeScene()->charts.empty() && !saved->charts.empty(),
          "Legacy bounded scenes remain supported and clear future chart membership");
  resources.reset();
  require(saved->charts[0].texcoords.size() == 6, "Chart ownership survives source resource destruction");
  std::puts("Surface chart ABI capture, ownership, validation, fingerprints and lifetime passed");
}
