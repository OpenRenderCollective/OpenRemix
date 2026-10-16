#include "scene/fixtures.h"
#include <limits>

using namespace remix;
using namespace remix::test;

int main() {
  NativeResources resources;
  auto red = material(resources, 10, 1, 0, 0);
  auto mesh = quad(resources, 20, red);
  auto first = instance(mesh);
  auto second = instance(mesh);
  second.transform.matrix[0][3] = 1000; // Never submitted to a camera frame.
  uint64_t authored[] = {resources.createRetained(first), resources.createRetained(second)};
  const auto unrelated = resources.createRetained(instance(mesh));
  remixapi_BakeSceneInfoEXT info{};
  info.structSize = sizeof(info);
  info.version = REMIXAPI_BAKE_SCENE_VERSION_EXT;
  info.sceneId = 0x425350;
  info.revision = 1;
  info.sceneScale = .0254f;
  info.state = REMIXAPI_BAKE_SCENE_READY_EXT;
  info.instanceCount = 2;
  info.instances = authored;
  resources.setBakeScene(&info);
  const auto original = resources.bakeScene();
  resources.setBakeScene(&info);
  require(resources.bakeScene() == original, "Unchanged requests reuse their captured inputs");
  require(original && original->ready && original->contentHash, "Ready snapshot has a content fingerprint");
  require(original->draws.size() == 2 && original->draws[1].model[12] == 1000,
          "Only explicit authored membership, including offscreen geometry, enters the bake");
  require(original->draws[0].surface == original->draws[1].surface,
          "Shared geometry is frozen once per snapshot");
  require(original->draws[0].surface != resources.retained(authored[0])->records[0].surface &&
              original->draws[0].material != resources.retained(authored[0])->records[0].material,
          "Geometry and material state belong to the snapshot");
  std::swap(authored[0], authored[1]);
  resources.setBakeScene(&info);
  require(resources.bakeScene()->contentHash == original->contentHash,
          "Authored membership order does not change transport identity");
  auto stable = resources.bakeScene();
  authored[1] = authored[0];
  rejected([&] { resources.setBakeScene(&info); });
  require(resources.bakeScene() == stable, "Invalid commits leave the previous scene intact");
  authored[1] = unrelated;
  resources.setBakeScene(&info);
  require(resources.bakeScene()->contentHash == original->contentHash,
          "Equivalent content under different runtime handles shares a fingerprint");
  const uint64_t coincident[] = {unrelated, resources.createRetained(instance(mesh))};
  info.instances = coincident;
  resources.setBakeScene(&info);
  require(resources.bakeScene()->contentHash != original->contentHash,
          "Changing placement multiplicity changes transport identity");
  info.instanceCount = 1;
  info.instances = &unrelated;
  resources.setBakeScene(&info);
  const auto beforeMaterial = resources.bakeScene();
  ++info.revision;
  resources.setBakeScene(&info);
  require(resources.bakeScene()->contentHash == beforeMaterial->contentHash,
          "A host scheduling revision alone does not defeat content-addressed cache reuse");
  resources.destroyMaterial(red);
  red = material(resources, 10, 0, 1, 0); // Same handle/hash, different shading.
  rejected([&] { resources.setBakeScene(&info); });
  resources.drawRetained(nullptr);
  resources.finishFrame();
  resources.setBakeScene(&info);
  require(resources.bakeScene()->contentHash != beforeMaterial->contentHash &&
              beforeMaterial->draws[0].material->tint[0] == 1 &&
              resources.bakeScene()->draws[0].material->tint[1] == 1,
          "Material replacement changes the fingerprint while old bake inputs stay frozen");
  const auto beforeMod = resources.bakeScene();
  resources.setMaterialSourceHash(red, 10);
  const auto blue = material(resources, 11, 0, 0, 1);
  resources.addModMaterial(10, blue);
  rejected([&] { resources.setBakeScene(&info); });
  resources.drawRetained(nullptr);
  resources.finishFrame();
  resources.setBakeScene(&info);
  require(resources.bakeScene()->contentHash != beforeMod->contentHash &&
              resources.bakeScene()->draws[0].material->tint[2] == 1,
          "Replayed mod replacements invalidate unchanged host membership and revisions");
  const auto beforeMove = resources.bakeScene();
  auto moved = instance(mesh);
  moved.transform.matrix[0][3] = 5;
  resources.updateRetained(unrelated, moved);
  rejected([&] { resources.setBakeScene(&info); }); // Moving entry is dynamic.
  require(resources.bakeScene() == beforeMove, "Dynamic placement cannot silently enter a static bake");
  info.sceneScale = std::numeric_limits<float>::quiet_NaN();
  rejected([&] { resources.setBakeScene(&info); });
  info.sceneScale = .0254f;
  NativeResources published;
  resources.publishFrame(published);
  const auto oldFrame = published.bakeScene();
  info.state = REMIXAPI_BAKE_SCENE_LOADING_EXT;
  info.instanceCount = 0;
  info.instances = nullptr;
  ++info.revision;
  resources.setBakeScene(&info);
  require(resources.bakeScene() && !resources.bakeScene()->ready && resources.bakeScene()->draws.empty(),
          "Map loading invalidates ready inputs immediately");
  require(oldFrame == beforeMove && oldFrame->ready,
          "Published frames retain their exact bake generation during asynchronous map changes");
  resources.setBakeScene(nullptr);
  require(!resources.bakeScene() && published.bakeScene() == oldFrame,
          "Unload releases future inputs without touching an in-flight snapshot");
  scene::BakeScene dynamicTexture;
  dynamicTexture.draws = beforeMove->draws;
  auto animated = std::make_shared<scene::Material>();
  animated->texture = std::make_shared<scene::Texture>();
  animated->texture->dynamic = true;
  dynamicTexture.draws[0].mesh = resources.retained(unrelated)->mesh;
  dynamicTexture.draws[0].material = animated;
  rejected([&] { scene::freezeBakeScene(dynamicTexture); });
  scene::BakeScene boundarySource;
  boundarySource.draws = dynamicTexture.draws;
  boundarySource.draws.resize(1);
  auto water = std::make_shared<scene::Material>();
  water->transmission = water->translucent = water->bakeSmoothBoundary = true;
  water->refractiveIndex = 1.333f;
  water->transmittanceDistance = 10;
  water->normalEncoding = 5;
  water->normalTexture = std::make_shared<scene::Texture>();
  water->normalTexture->dynamic = true;
  boundarySource.draws[0].material = water;
  auto boundary = boundarySource;
  scene::freezeBakeScene(boundary);
  const auto boundaryHash = boundary.contentHash;
  require(boundary.draws[0].material->bakeSmoothBoundary && !boundary.draws[0].material->normalTexture &&
              boundary.draws[0].material->normalEncoding == 0 && water->normalTexture &&
              water->normalEncoding == 5,
          "Explicit optical capture excludes ripple normals without modifying the live material");
  water->scroll[0] = 4;
  water->normalTexture->pixels.width = 100;
  boundary = boundarySource;
  scene::freezeBakeScene(boundary);
  require(boundary.contentHash == boundaryHash,
          "Live ripple edits do not invalidate a mean optical boundary");
  water->transmittanceDistance = 20;
  boundary = boundarySource;
  scene::freezeBakeScene(boundary);
  require(boundary.contentHash != boundaryHash, "Optical edits invalidate the frozen boundary");
  water->bakeSmoothBoundary = false;
  rejected([&] { scene::freezeBakeScene(boundarySource); });
  water->bakeSmoothBoundary = true;
  water->texture = water->normalTexture;
  rejected([&] { scene::freezeBakeScene(boundarySource); });
  resources.reset();
  require(!resources.bakeScene() && oldFrame->draws[0].surface->indices.size() == 6,
          "Shutdown cannot invalidate a worker's owned input geometry");
  // Light definition revisions are independent of the host's map revision.
  // Recreating dynamic engine lights must not recapture an unchanged map.
  remixapi_LightInfoSphereEXT sphere{};
  sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
  sphere.radius = .5f;
  remixapi_LightInfo light{};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  light.pNext = &sphere;
  light.hash = 50;
  light.radiance = {1, 0, 0};
  remixapi_LightHandle lamp;
  resources.createLight(light, lamp);
  info.state = REMIXAPI_BAKE_SCENE_READY_EXT;
  info.lightCount = 1;
  info.lights = &lamp;
  resources.setBakeScene(&info);
  const auto lit = resources.bakeScene();
  require(lit->lights.size() == 1 && lit->lights[0].radiance[0] == 1,
          "Explicit static lights enter the snapshot without a camera draw");
  light.hash = 51;
  remixapi_LightHandle dynamicLamp;
  resources.createLight(light, dynamicLamp);
  resources.setBakeScene(&info);
  require(resources.bakeScene() == lit, "Unrelated lights do not invalidate static bake inputs");
  light.radiance = {0, 1, 0};
  resources.updateLight(lamp, light);
  resources.setBakeScene(&info);
  require(resources.bakeScene()->contentHash != lit->contentHash && lit->lights[0].radiance[0] == 1,
          "Changed light definitions recapture while prior lighting remains immutable");
  light.isDynamic = 1;
  resources.updateLight(lamp, light);
  rejected([&] { resources.setBakeScene(&info); });
  info.lightCount = 0;
  info.lights = nullptr;
  remixapi_BakeSceneBoundsInfoEXT bounded{};
  bounded.scene = info;
  bounded.scene.structSize = sizeof(bounded);
  bounded.scene.version = REMIXAPI_BAKE_SCENE_BOUNDS_VERSION_EXT;
  for (uint32_t c = 0; c < 3; ++c) {
    bounded.minimum[c] = -64;
    bounded.maximum[c] = 64;
  }
  resources.setBakeScene(&bounded.scene);
  const auto volume = resources.bakeScene();
  require(volume->hasBounds && volume->minimum[0] == -64 && volume->maximum[2] == 64,
          "Version 2 captures caller-owned playable bounds");
  bounded.maximum[0] = 128;
  resources.setBakeScene(&bounded.scene);
  const auto enlarged = resources.bakeScene();
  require(enlarged->contentHash != volume->contentHash && volume->maximum[0] == 64,
          "Changed probe bounds invalidate the bake without mutating old frames");
  bounded.maximum[1] = bounded.minimum[1];
  rejected([&] { resources.setBakeScene(&bounded.scene); });
  require(resources.bakeScene() == enlarged, "Invalid bounds are rejected atomically");
  resources.setBakeScene(&info);
  require(!resources.bakeScene()->hasBounds, "Version 1 remains supported after a bounded request");
  remixapi_BakeSceneReflectionsInfoEXT reflectionRequest{};
  auto &reflectionScene = reflectionRequest.surfaces.bounds.scene;
  reflectionRequest.surfaces.bounds = bounded;
  reflectionRequest.surfaces.bounds.maximum[1] = 64;
  reflectionScene.structSize = sizeof(reflectionRequest);
  reflectionScene.version = REMIXAPI_BAKE_SCENE_REFLECTIONS_VERSION_EXT;
  float sites[] = {0, 0, 0, 10, 20, 30};
  reflectionRequest.positions = sites;
  reflectionRequest.positionCount = 2;
  reflectionRequest.reflectionRevision = 1;
  resources.setBakeScene(&reflectionScene);
  const auto hints = resources.bakeScene();
  require(hints->reflectionHints.size() == 2 && hints->reflectionHints[1][1] == 20,
          "Version 4 owns authored reflection sites independently of irradiance probes");
  sites[0] = 5;
  ++reflectionRequest.reflectionRevision;
  resources.setBakeScene(&reflectionScene);
  const auto editedHints = resources.bakeScene();
  require(hints->reflectionHints[0][0] == 0 && editedHints->contentHash != hints->contentHash,
          "Site edits invalidate content without mutating an earlier snapshot");
  ++reflectionRequest.reflectionRevision;
  resources.setBakeScene(&reflectionScene);
  require(resources.bakeScene()->contentHash == editedHints->contentHash,
          "Reflection scheduling revisions are excluded from content identity");
  sites[0] = std::numeric_limits<float>::quiet_NaN();
  ++reflectionRequest.reflectionRevision;
  rejected([&] { resources.setBakeScene(&reflectionScene); });
  sites[0] = 1000;
  rejected([&] { resources.setBakeScene(&reflectionScene); });
  sites[0] = 5;
  reflectionRequest.positionCount = 4097;
  rejected([&] { resources.setBakeScene(&reflectionScene); });
  reflectionRequest.positionCount = 2;
  reflectionRequest.reserved = 1;
  rejected([&] { resources.setBakeScene(&reflectionScene); });
  reflectionRequest.reserved = 0;
  reflectionScene.state = REMIXAPI_BAKE_SCENE_LOADING_EXT;
  rejected([&] { resources.setBakeScene(&reflectionScene); });
  resources.setBakeScene(&info);
  require(resources.bakeScene()->reflectionHints.empty() && !resources.bakeScene()->reflectionRevision,
          "Legacy clients do not retain reflection sites from a preceding version 4 request");
  std::puts("Bake scene authored membership, fingerprints, atomic commits and publication lifetimes passed");
}
