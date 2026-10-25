#include "../scene/fixtures.h"
#include "forward/bake/data.h"
#include <cstddef>

using namespace remix;
using namespace remix::test;

int main() {
  NativeResources resources;
  const auto mesh = quad(resources, 2, material(resources, 1, 1, 0, 0));
  const uint64_t instances[]{resources.createRetained(instance(mesh)),
                              resources.createRetained(instance(mesh, 2))};
  remixapi_BakeSceneReceiversInfoEXT request{};
  auto &surfaces = request.styled.reflections.surfaces;
  auto &bounds = surfaces.bounds;
  auto &info = bounds.scene;
  info.structSize = sizeof(request);
  info.version = REMIXAPI_BAKE_SCENE_RECEIVERS_VERSION_EXT;
  info.sceneId = info.revision = 1;
  info.sceneScale = 1;
  info.state = REMIXAPI_BAKE_SCENE_READY_EXT;
  info.instanceCount = 2;
  info.instances = instances;
  for (uint32_t axis = 0; axis < 3; ++axis) {
    bounds.minimum[axis] = -4;
    bounds.maximum[axis] = 4;
  }
  const float uv[]{0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1};
  remixapi_BakeSurfaceChartEXT chart{instances[1], 0, 0, 0, 6, 2, 2, uv};
  surfaces.chartRevision = 1;
  surfaces.chartCount = 1;
  surfaces.charts = &chart;
  request.receiverCount = 1;
  request.receivers = instances + 1;
  resources.setBakeScene(&info);
  const auto captured = resources.bakeScene();
  require(!captured->receiverOnly(0) && captured->receiverOnly(1) && captured->charts.size() == 1,
          "Explicit receiver roles retain chart ownership independently of transport membership");
  const auto transport = forward::bake::pack(*captured);
  require(transport.triangles.size() == 2 && transport.vertices.size() == 4 && transport.materials.size() == 2,
          "Receiver-only geometry contributes no ray triangles or vertices and preserves material indexing");
  require(transport.triangles[0][3] == 0 && transport.triangles[1][3] == 0,
          "Transport triangles still address their original draw material");
  auto reversedRoles = *captured;
  reversedRoles.receiverDraws = {1, 0};
  const auto laterTransport = forward::bake::pack(reversedRoles);
  require(laterTransport.triangles.size() == 2 && laterTransport.triangles[0][3] == 1,
          "A receiver preceding transport geometry does not shift its material address");
  resources.setBakeScene(&info);
  require(resources.bakeScene() == captured, "Unchanged receiver publication reuses its immutable snapshot");
  resources.drawRetained(nullptr);
  const auto found = std::find_if(resources.draws().begin(), resources.draws().end(), [&](const auto &draw) {
    return draw.instanceId == captured->draws[1].instanceId;
  });
  require(found != resources.draws().end(), "Retained receiver appears in the live frame");
  auto live = *found;
  require(scene::matchesBakeDraw(*captured, 1, live), "Unchanged receiver binds to its chart");
  live.material = std::make_shared<scene::Material>();
  live.material->fps = 30;
  require(scene::matchesBakeDraw(*captured, 1, live), "Receiver material edits retain geometry-only coverage");
  live.model[12] += 1;
  require(!scene::matchesBakeDraw(*captured, 1, live), "Moved receivers cannot reuse lighting from their old placement");

  // Recapture a receiver with a different animated/emissive source material.
  // It never contributes that material to transport or the bake content key.
  scene::BakeScene edited;
  edited.ready = true;
  edited.draws = resources.draws();
  edited.receiverDraws = {0, 1};
  const auto original = edited;
  scene::freezeBakeScene(edited);
  auto changed = original;
  changed.draws[1].material = std::make_shared<scene::Material>();
  changed.draws[1].material->fps = 30;
  changed.draws[1].material->emission[0] = 100;
  scene::freezeBakeScene(changed);
  require(edited.contentHash == changed.contentHash,
          "Editable receiver material state does not alter static transport identity");
  auto ordinary = original;
  ordinary.receiverDraws.clear();
  scene::freezeBakeScene(ordinary);
  require(edited.contentHash != ordinary.contentHash, "Receiver and transport roles have distinct content identity");
  auto malformed = original;
  malformed.receiverDraws = {1};
  rejected([&] { scene::freezeBakeScene(malformed); });
  malformed = original;
  malformed.receiverDraws = {0, 2};
  rejected([&] { scene::freezeBakeScene(malformed); });

  request.reserved = 1;
  rejected([&] { resources.setBakeScene(&info); });
  request.reserved = 0;
  const uint64_t unknown = 0;
  request.receivers = &unknown;
  rejected([&] { resources.setBakeScene(&info); });
  const uint64_t duplicate[]{instances[1], instances[1]};
  request.receiverCount = 2;
  request.receivers = duplicate;
  rejected([&] { resources.setBakeScene(&info); });
  require(resources.bakeScene() == captured, "Invalid receiver lists leave the previous snapshot intact");
  request.receiverCount = 0;
  request.receivers = nullptr;
  resources.setBakeScene(&info);
  require(resources.bakeScene()->receiverDraws.empty() && !resources.bakeScene()->receiverOnly(1),
          "Clearing receiver membership restores ordinary transport semantics");
  const auto ordinaryHash = resources.bakeScene()->contentHash;
  auto legacy = request.styled;
  auto &legacyInfo = legacy.reflections.surfaces.bounds.scene;
  legacyInfo.structSize = sizeof(legacy);
  legacyInfo.version = REMIXAPI_BAKE_SCENE_STYLED_LIGHTS_VERSION_EXT;
  resources.setBakeScene(&legacyInfo);
  require(resources.bakeScene()->contentHash == ordinaryHash,
          "An empty receiver descriptor preserves the version-five content identity");
  static_assert(offsetof(remixapi_BakeSceneReceiversInfoEXT, styled) == 0);
  std::puts("Receiver-only bake roles, transport exclusion, material edits and binding guards passed");
}
