#include "scene/fixtures.h"
#include <limits>
#include <remix/remix_native_ext.h>

using namespace remix;
using namespace remix::test;

int main() {
  {
    NativeResources fixture;
    const auto surface = quad(fixture, 2, material(fixture, 1, 1, 0, 0));
    auto instanceInfo = instance(surface);
    auto snapshot = [&](uint32_t category, bool chart) {
      fixture.finishFrame();
      instanceInfo.categoryFlags = category;
      fixture.drawInstance(instanceInfo);
      auto result = std::make_shared<scene::BakeScene>();
      result->ready = true;
      result->draws.push_back(fixture.draws().back());
      result->draws.back().classification = REMIXAPI_INSTANCE_STATIC_EXT;
      if (chart) result->charts.push_back({0, 0, 2, 2, {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}}});
      scene::freezeBakeScene(*result);
      return result;
    };
    const auto ordinary = snapshot(0, false);
    const auto reflectionOnly = snapshot(REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_BODY, false);
    require(ordinary->contentHash != reflectionOnly->contentHash &&
                ordinary->contentHash == snapshot(0, false)->contentHash,
            "Ray-purpose changes invalidate affected bakes without changing ordinary scene identity");
    const auto receiver = snapshot(0, true);
    const auto invisibleReceiver = snapshot(REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_MODEL, true);
    require(receiver->charts.size() == 1 && invisibleReceiver->charts.empty() && invisibleReceiver->draws.size() == 1,
            "Reflection-only geometry remains available for bounces without wasting surface lightmap charts");
  }
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
  {
    remixapi_ReflectionVolumeEXT volume{};
    volume.id = 7;
    volume.priority = -2;
    for (uint32_t c = 0; c < 3; ++c) {
      volume.minimum[c] = -1;
      volume.maximum[c] = 1;
      volume.capture[c] = float(c + 1) * 10;
      volume.worldToLocal[c][c] = 1;
      volume.worldToLocal[c][3] = -volume.capture[c];
    }
    remixapi_ReflectionVolumesInfoEXT request{sizeof(request), REMIXAPI_REFLECTION_VOLUMES_VERSION_EXT,
        info.sceneId, 1, 1, 0, &volume};
    resources.setReflectionVolumes(&request);
    const auto authored = resources.reflectionVolumes();
    require(authored && authored->volumes.size() == 1 && authored->volumes[0].capture[0] == 10 &&
                authored->volumes[0].priority == -2 && resources.bakeScene() == original &&
                resources.bakeScene()->contentHash == original->contentHash,
            "Authored reflection metadata does not invalidate frozen diffuse lighting");
    ++request.revision;
    resources.setReflectionVolumes(&request);
    require(resources.reflectionVolumes() == authored, "Unchanged editor saves preserve reflection job ownership");
    NativeResources frame;
    resources.publishFrame(frame);
    volume.capture[0] = 10.5f;
    resources.setReflectionVolumes(&request);
    require(frame.reflectionVolumes() == authored && authored->volumes[0].capture[0] == 10 &&
                resources.reflectionVolumes()->volumes[0].capture[0] == 10.5f,
            "Published reflection requests copy caller data and survive later editor changes");
    const auto current = resources.reflectionVolumes();
    volume.worldToLocal[0][0] = 2;
    rejected([&] { resources.setReflectionVolumes(&request); });
    require(resources.reflectionVolumes() == current, "Invalid transforms preserve the last valid authoring request");
    volume.worldToLocal[0][0] = 1;
    static_assert(sizeof(remixapi_ReflectionVolumeEXT) == 96);
    static_assert(offsetof(remixapi_ReflectionVolumesSamplingInfoEXT, base) == 0);
    remixapi_ReflectionSamplingEXT sampling{};
    for (uint32_t c = 0; c < 3; ++c) { sampling.minimum[c] = -2; sampling.maximum[c] = 2; }
    sampling.blendDistance = .25f;
    remixapi_ReflectionVolumesSamplingInfoEXT extended{request, &sampling};
    extended.base.structSize = sizeof(extended);
    extended.base.version = REMIXAPI_REFLECTION_VOLUMES_SAMPLING_VERSION_EXT;
    resources.setReflectionVolumes(&extended.base);
    const auto separate = resources.reflectionVolumes();
    require(separate && separate->volumes[0].minimum[0] == -1 &&
                separate->volumes[0].influenceMinimum[0] == -2 && separate->volumes[0].blendDistance == .25f &&
                resources.bakeScene() == original,
            "Extended reflection authoring separates influence and feathering without changing diffuse input");
    resources.publishFrame(frame);
    sampling.minimum[0] = 2; sampling.maximum[0] = 3;
    resources.setReflectionVolumes(&extended.base);
    require(frame.reflectionVolumes() == separate && separate->volumes[0].influenceMinimum[0] == -2 &&
                resources.reflectionVolumes()->volumes[0].influenceMinimum[0] == 2,
            "Capture may lie outside influence; published sampling metadata owns its copied values");
    const auto validSeparate = resources.reflectionVolumes();
    sampling.blendDistance = -1;
    rejected([&] { resources.setReflectionVolumes(&extended.base); });
    sampling.blendDistance = std::numeric_limits<float>::quiet_NaN();
    rejected([&] { resources.setReflectionVolumes(&extended.base); });
    sampling.blendDistance = 0;
    sampling.maximum[0] = sampling.minimum[0];
    rejected([&] { resources.setReflectionVolumes(&extended.base); });
    sampling.maximum[0] = std::numeric_limits<float>::infinity();
    rejected([&] { resources.setReflectionVolumes(&extended.base); });
    extended.sampling = nullptr;
    rejected([&] { resources.setReflectionVolumes(&extended.base); });
    require(resources.reflectionVolumes() == validSeparate, "Invalid sampling edits preserve the last complete request");
    resources.setReflectionVolumes(&request);
    require(resources.reflectionVolumes()->volumes[0].influenceMinimum ==
                resources.reflectionVolumes()->volumes[0].minimum &&
                resources.reflectionVolumes()->volumes[0].blendDistance == -1,
            "Version-one requests retain their combined bounds and legacy feathering");
    std::vector<remixapi_ReflectionVolumeEXT> full(REMIXAPI_REFLECTION_VOLUMES_MAX_EXT, volume);
    for (uint32_t i = 0; i < full.size(); ++i) full[i].id = i + 1;
    request.volumeCount = uint32_t(full.size()); request.volumes = full.data();
    resources.setReflectionVolumes(&request);
    require(resources.reflectionVolumes()->volumes.size() == full.size() && resources.bakeScene() == original,
            "The complete authored quota preserves all volume identities without invalidating diffuse lighting");
    remixapi_ReflectionVolumeEXT duplicates[]{volume, volume};
    request.volumeCount = 2; request.volumes = duplicates;
    rejected([&] { resources.setReflectionVolumes(&request); });
    request.volumeCount = REMIXAPI_REFLECTION_VOLUMES_MAX_EXT + 1;
    rejected([&] { resources.setReflectionVolumes(&request); });
    request.volumeCount = 1; request.volumes = &volume;
    ++request.sceneId;
    resources.setReflectionVolumes(&request);
    require(!resources.reflectionVolumes(), "Another scene's volumes remain dormant instead of leaking across maps");
    request.sceneId = info.sceneId;
    request.volumeCount = 0; request.volumes = nullptr;
    resources.setReflectionVolumes(&request);
    require(resources.reflectionVolumes() && resources.reflectionVolumes()->volumes.empty(),
            "An explicit empty authoring document differs from restoring automatic cubemaps");
    resources.setReflectionVolumes(nullptr);
    require(!resources.reflectionVolumes() && resources.bakeScene() == original,
            "Restoring automatic reflections keeps diffuse bake ownership intact");
  }
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
  const auto weapon = material(resources, 90, .5f, .5f, .5f);
  resources.setMaterialSourceHash(weapon, 900);
  resources.addModMaterial(900, material(resources, 91, .25f, .25f, .25f));
  resources.drawRetained(nullptr);
  resources.finishFrame();
  resources.setBakeScene(&info);
  require(resources.bakeScene() == beforeMod,
          "Loading an unrelated weapon replacement preserves the world bake snapshot");
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
  auto decalIdentity = boundarySource;
  decalIdentity.draws[0].decal = true;
  scene::freezeBakeScene(decalIdentity);
  require(decalIdentity.contentHash != boundaryHash,
          "Decal surface identity participates in immutable bake content and cache invalidation");
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
  const auto measuredBodyHash = boundary.contentHash;
  water->thinWallThickness = .25f;
  boundary = boundarySource;
  scene::freezeBakeScene(boundary);
  const auto sheetHash = boundary.contentHash;
  require(sheetHash != measuredBodyHash && boundary.draws[0].material->thinWallThickness == .25f,
          "Frozen sheet thickness has its own immutable optical identity");
  water->thinWallThickness = .5f;
  require(boundary.draws[0].material->thinWallThickness == .25f,
          "A thickness edit cannot mutate the published sheet snapshot");
  boundary = boundarySource;
  scene::freezeBakeScene(boundary);
  require(boundary.contentHash != sheetHash, "Thickness edits invalidate the frozen sheet");
  water->thinWallThickness = 0;
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
  require(resources.bakeLightsCurrent(), "Captured light validity is independent of camera submission");
  light.hash = 51;
  remixapi_LightHandle dynamicLamp;
  resources.createLight(light, dynamicLamp);
  resources.setBakeScene(&info);
  require(resources.bakeScene() == lit, "Unrelated lights do not invalidate static bake inputs");
  require(resources.bakeLightsCurrent(), "Unrelated definition revisions preserve captured lighting validity");
  light.radiance = {0, 1, 0};
  resources.updateLight(lamp, light);
  require(!resources.bakeLightsCurrent(), "A changed definition invalidates aggregate lighting before recapture");
  resources.setBakeScene(&info);
  require(resources.bakeLightsCurrent(), "Recapturing changed definitions restores lighting validity");
  require(resources.bakeScene()->contentHash != lit->contentHash && lit->lights[0].radiance[0] == 1,
          "Changed light definitions recapture while prior lighting remains immutable");
  const auto beforeHint = resources.bakeScene();
  remixapi_LightInfoBakingEXT baking{REMIXAPI_STRUCT_TYPE_LIGHT_BAKING_EXT, nullptr,
                                   REMIXAPI_LIGHT_BAKING_PREFER_BAKED_EXT};
  sphere.pNext = &baking;
  resources.updateLight(lamp, light);
  require(!resources.bakeLightsCurrent(), "Changing representation hints invalidates captured light definitions");
  resources.setBakeScene(&info);
  const auto bakedHint = resources.bakeScene();
  require(bakedHint->lights[0].bakeHint == REMIXAPI_LIGHT_BAKING_PREFER_BAKED_EXT &&
              bakedHint->contentHash != beforeHint->contentHash && beforeHint->lights[0].bakeHint == 0,
          "Captured hint is immutable and participates in bake content identity");
  baking.hint = REMIXAPI_LIGHT_BAKING_REQUIRE_MIXED_EXT;
  resources.updateLight(lamp, light);
  resources.setBakeScene(&info);
  require(resources.bakeScene()->contentHash != bakedHint->contentHash &&
              bakedHint->lights[0].bakeHint == REMIXAPI_LIGHT_BAKING_PREFER_BAKED_EXT,
          "Different valid policy hints cannot reuse one baked content identity");
  sphere.pNext = nullptr;
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
  {
    remixapi_BakeSceneStyledLightsInfoEXT styled{};
    styled.reflections = reflectionRequest;
    auto &request = styled.reflections.surfaces.bounds.scene;
    request.structSize = sizeof(styled);
    request.version = REMIXAPI_BAKE_SCENE_STYLED_LIGHTS_VERSION_EXT;
    request.state = REMIXAPI_BAKE_SCENE_READY_EXT;
    remixapi_LightInfoSphereEXT shape{};
    shape.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    shape.position = {0, 0, 2};
    shape.radius = .1f;
    remixapi_LightInfo definition{};
    definition.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    definition.pNext = &shape;
    definition.hash = 500;
    definition.radiance = {4, 2, 1};
    remixapi_LightHandle authoredLight, runtimeLight;
    resources.createLight(definition, authoredLight);
    definition.hash = 501;
    definition.radiance = {};
    resources.createLight(definition, runtimeLight);
    remixapi_BakeStyledLightEXT input{authoredLight, runtimeLight, 32, 0};
    styled.styledLightCount = 1;
    styled.styledLights = &input;
    resources.setBakeScene(&request);
    const auto off = resources.bakeScene();
    require(off->lights.size() == 1 && off->lights[0].id == 501 && off->lights[0].style == 32 &&
            off->lights[0].radiance[0] == 4, "Initially off light captures full authored energy and live identity");
    for (float value : {1.f, .5f, 0.f, 2.f}) {
      definition.radiance = {value, value * .5f, value * .25f};
      resources.updateLight(runtimeLight, definition);
      require(resources.bakeLightsCurrent(), "Styled intensity changes retain the authored lighting envelope");
      resources.setBakeScene(&request);
      require(resources.bakeScene() == off, "Runtime style on/off/flicker leaves the captured snapshot unchanged");
    }
    definition.radiance = {0, 1, 0};
    resources.updateLight(runtimeLight, definition);
    require(resources.bakeLightsCurrent(), "Styled live RGB changes do not stale their captured envelope");
    resources.setBakeScene(&request);
    require(resources.bakeScene() == off, "Live colour edits reuse unit direct visibility without rebaking");
    input.style = 33;
    resources.setBakeScene(&request);
    require(resources.bakeScene()->contentHash != off->contentHash && off->lights[0].style == 32,
            "Style ownership edits change content without modifying prior frames");
    auto stableStyle = resources.bakeScene();
    shape.position.x = 1;
    resources.updateLight(runtimeLight, definition);
    require(!resources.bakeLightsCurrent(), "Styled emitter shape edits require new static inputs");
    rejected([&] { resources.setBakeScene(&request); });
    require(resources.bakeScene() == stableStyle, "Live geometry mismatch is rejected atomically");
    shape.position.x = 0;
    resources.updateLight(runtimeLight, definition);
    input.reserved = 1;
    rejected([&] { resources.setBakeScene(&request); });
    input.reserved = 0;
    input.style = 128;
    rejected([&] { resources.setBakeScene(&request); });
    input.style = 32;
    input.runtimeLight = input.authoredLight;
    rejected([&] { resources.setBakeScene(&request); });
    input.runtimeLight = runtimeLight;
    request.lightCount = 1;
    request.lights = &authoredLight;
    rejected([&] { resources.setBakeScene(&request); });
    request.lightCount = 0;
    request.lights = nullptr;
    styled.styledLightCount = 4097;
    rejected([&] { resources.setBakeScene(&request); });
    styled.styledLightCount = 1;
    resources.setBakeScene(nullptr);
    resources.setBakeScene(&request);
    require(resources.bakeScene()->lights[0].style == 32, "Unload clears styled dependencies and permits recapture");
    resources.setBakeScene(&info);
    require(resources.bakeScene()->lights.empty(), "Legacy descriptors do not retain prior styled membership");
  }
  std::puts("Bake scene authored membership, fingerprints, atomic commits and publication lifetimes passed");
}
