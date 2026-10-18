// Asynchronous presentation's frame capture (NativeResources::publishFrame):
// the published frame holds exactly what the frame drew, the API side is a
// finished frame, resources destroyed or replaced after publication stay alive
// for the published frame and are released with it, and a fake render thread
// reading published frames while the next frames' API calls run sees each
// frame exactly as a synchronous Present would have. CPU only.
#include "platform/render_thread.h"
#include "scene/emission.h"
#include "scene/fixtures.h"
#include <atomic>
#include <cstring>
#include <filesystem>
#include <map>
#include <vector>

using namespace remix;
using namespace remix::test;

namespace {
  remixapi_LightHandle sphereLight(NativeResources &r, uint64_t hash, float x) {
    remixapi_LightInfoSphereEXT sphere{};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.radius = .25f;
    sphere.position = {x, 1, 2};
    remixapi_LightInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.pNext = &sphere;
    info.hash = hash;
    info.radiance = {4, 2, 1};
    remixapi_LightHandle handle{};
    r.createLight(info, handle);
    return handle;
  }

  remixapi_InstanceInfo placed(remixapi_MeshHandle mesh, float x, float y = 0) {
    auto info = instance(mesh, 0);
    info.transform.matrix[0][3] = x;
    info.transform.matrix[1][3] = y;
    return info;
  }

  // The frame's content, from values only (independent of object identity).
  double checksum(const std::vector<scene::Draw> &draws) {
    double sum = 0;
    double weight = 1;
    for (const auto &draw : draws) {
      weight += 1;
      for (const float v : draw.model)
        sum += weight * v;
      for (const auto &vertex : draw.surface->vertices)
        sum += weight * (vertex.position[0] + 2 * vertex.position[1] + 3 * vertex.position[2]);
      for (const float v : draw.material->tint)
        sum += weight * v;
      sum += weight * double(draw.instanceId % 1024) + draw.classification + draw.surfaceIndex;
    }
    return sum;
  }

  // The published frame holds what the frame drew; the API side is a
  // finished frame and its next frame does not reach the published one.
  void snapshot() {
    NativeResources api, frame;
    camera(api);
    auto red = material(api, 1, 1, 0, 0);
    auto mesh = quad(api, 2, red);
    api.drawInstance(placed(mesh, 1));
    api.drawInstance(placed(mesh, 2));
    auto light = sphereLight(api, 10, 3);
    api.drawLight(light);
    const auto draws = api.draws();
    const auto lights = api.sphereLights();
    require(lights.size() == 1, "Fixture light");
    api.publishFrame(frame);
    require(api.draws().empty() && api.pendingDrawCount() == 0 && !api.hasSphereLights() &&
                api.sphereLights().empty(),
            "The API frame was not finished");
    require(frame.draws().size() == 2 && frame.pendingDrawCount() == 2, "Published draw count");
    for (size_t i = 0; i < draws.size(); ++i)
      require(frame.draws()[i].mesh == draws[i].mesh && frame.draws()[i].material == draws[i].material &&
                  frame.draws()[i].model == draws[i].model,
              "Published draws differ");
    require(frame.camera() == api.camera() && frame.view() == api.view() && frame.cameraValid(),
            "Published camera differs");
    require(frame.hasSphereLights() && frame.sphereLights() == lights, "Published lights differ");
    // Lights resolve at publication: destroying one afterwards does not
    // reach the published frame.
    api.destroyLight(light);
    require(frame.sphereLights() == lights, "A destroyed light left the published frame");
    api.drawInstance(placed(mesh, 3));
    require(frame.draws().size() == 2, "The next API frame reached the published one");
    frame.finishFrame();
    require(frame.draws().empty() && !frame.hasSphereLights() && frame.sphereLights().empty(),
            "A finished published frame kept its draws or lights");
  }

  // Resources destroyed or replaced after publication stay alive for the
  // published frame and are released with it.
  void lifetime() {
    NativeResources api, frame;
    camera(api);
    auto red = material(api, 1, 1, 0, 0);
    auto mesh = quad(api, 2, red);
    api.drawInstance(placed(mesh, 1));
    api.publishFrame(frame);
    const std::weak_ptr<scene::Mesh> held = frame.draws()[0].mesh;
    const std::weak_ptr<scene::Material> heldMaterial = frame.draws()[0].material;
    const auto releases = scene::meshReleases.load();
    api.destroyMesh(mesh);
    api.destroyMaterial(red);
    require(!held.expired() && !heldMaterial.expired() && scene::meshReleases.load() == releases,
            "A destroyed resource left the published frame");
    require(frame.draws()[0].surface->vertices.size() == 4 && frame.draws()[0].material->tint[0] == 1,
            "A published draw lost its data");
    // Replacements under the same handles are new objects.
    red = material(api, 1, 0, 1, 0);
    mesh = quad(api, 2, red);
    api.drawInstance(placed(mesh, 1));
    require(api.draws()[0].mesh != held.lock() && api.draws()[0].material != heldMaterial.lock(),
            "A replacement reused the published objects");
    frame.finishFrame();
    require(held.expired() && heldMaterial.expired() && scene::meshReleases.load() == releases + 1,
            "The published frame did not release destroyed resources");
  }

  remixapi_MaterialHandle emissive(NativeResources &r, uint64_t hash, float red) {
    const auto path = std::filesystem::path("0xa");
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {red, 1, 1};
    opaque.opacityConstant = 1;
    opaque.alphaTestType = 7;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.hash = hash;
    info.pNext = &opaque;
    info.emissiveColorConstant = {1, .5f, .25f};
    info.emissiveTexture = path.c_str();
    info.emissiveIntensity = 3;
    remixapi_MaterialHandle handle;
    r.createMaterial(info, handle);
    return handle;
  }

  // A replaced material's triangle emission cache is carried over only while
  // frames render on the API's thread: a render thread may be filling it.
  void emissionCache() {
    for (const bool concurrent : {false, true}) {
      NativeResources api;
      api.setConcurrentConsumer(concurrent);
      const uint8_t white[] = {255, 255, 255, 255};
      remixapi_TextureInfo texture{};
      texture.hash = 0xa;
      texture.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
      texture.width = texture.height = texture.depth = texture.mipLevels = 1;
      texture.data = white;
      texture.dataSize = 4;
      remixapi_TextureHandle textureHandle;
      api.createTexture(texture, textureHandle);
      auto glow = emissive(api, 5, 1);
      auto mesh = quad(api, 6, glow);
      api.drawInstance(instance(mesh));
      const float transform[] = {1, 1, 0, 0};
      const auto radiance = scene::triangleEmission(api.draws()[0], 0, transform);
      require(radiance[0] > 0 && !api.draws()[0].material->emissionCache.empty(), "Fixture emission");
      api.finishFrame();
      emissive(api, 5, .5f); // Same emission, another albedo: a replacement.
      api.drawInstance(instance(mesh));
      const bool carried = !api.draws()[0].material->emissionCache.empty();
      require(carried == !concurrent, concurrent ? "A cache a render thread may write was read"
                                                 : "The emission cache was not carried over");
      require(scene::triangleEmission(api.draws()[0], 0, transform) == radiance, "Recomputed emission differs");
    }
  }

  // A consumer switching tables starts a new static epoch.
  void restart() {
    NativeResources api;
    camera(api);
    auto red = material(api, 1, 1, 0, 0);
    auto mesh = quad(api, 2, red);
    const uint64_t handle = api.createRetained(placed(mesh, 0));
    api.setPersistentStatic(true);
    api.drawRetained(nullptr);
    const auto epoch = api.staticEpoch();
    api.restartStatic();
    std::vector<uint64_t> changes;
    api.takeStaticChanges(changes);
    require(api.staticEpoch() != epoch && changes.empty() && api.staticDrawn() && api.retained(handle)->member,
            "A restart changed membership or kept stale changes");
  }

  // The scene driven identically into the published API (persistent static
  // set on) and a legacy one (set off, traced from its draw list).
  struct Churn {
    std::vector<uint64_t> handles;
    void frame(NativeResources &r, int f) {
      // Materials and meshes replaced under the same handles, destroyed and
      // recreated: the published frame keeps the versions it drew.
      auto red = material(r, 1, 1, float(f % 3) * .25f, 0);
      auto green = material(r, 2, 0, 1, float(f % 5) * .2f);
      auto a = quad(r, 10, red);
      auto b = quad(r, 11 + f % 4, green, 0xffffffff);
      if (f % 7 == 3)
        r.destroyMesh(reinterpret_cast<remixapi_MeshHandle>(uintptr_t(11 + (f + 1) % 4)));
      remixapi_TextureInfo texture{};
      const uint8_t pixels[] = {uint8_t(f), 255, 255, 255};
      texture.hash = 0x100 + f % 3;
      texture.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
      texture.width = texture.height = texture.depth = texture.mipLevels = 1;
      texture.data = pixels;
      texture.dataSize = 4;
      remixapi_TextureHandle textureHandle;
      r.createTexture(texture, textureHandle);
      if (f % 3 == 2)
        r.destroyTexture(textureHandle);
      // Retained instances: registered, moved, hidden and destroyed.
      if (handles.size() < 24)
        handles.push_back(r.createRetained(placed(a, float(handles.size()), 1)));
      if (f % 5 == 1) {
        remixapi_SceneInstanceChangeEXT u{};
        u.instance = handles[size_t(f) % handles.size()];
        u.fields = REMIXAPI_SCENE_INSTANCE_TRANSFORM_EXT;
        u.transform.matrix[0][0] = u.transform.matrix[1][1] = u.transform.matrix[2][2] = 1;
        u.transform.matrix[0][3] = float(f);
        r.updateRetained(&u, 1);
      }
      if (f % 11 == 4) {
        remixapi_SceneInstanceChangeEXT u{};
        u.instance = handles[size_t(f * 7) % handles.size()];
        u.fields = REMIXAPI_SCENE_INSTANCE_VISIBLE_EXT;
        u.visible = (f / 11) % 2;
        r.updateRetained(&u, 1);
      }
      if (f % 13 == 6 && handles.size() > 4) {
        const uint64_t gone = handles[2];
        r.destroyRetained(&gone, 1);
        handles.erase(handles.begin() + 2);
      }
      r.drawRetained(nullptr);
      // Frame-local draws.
      r.drawInstance(placed(a, -1, float(f % 4)));
      r.drawInstance(placed(b, -2));
    }
  };

  // A fake render thread traces published frames while the next frame's API
  // calls run: each published frame, expanded, is the legacy replay's draw
  // list by value, and every resource it names is alive.
  void concurrent() {
    NativeResources api, legacy, frame;
    camera(api);
    camera(legacy);
    api.setConcurrentConsumer(true);
    api.setPersistentStatic(true);
    Churn churn, legacyChurn;
    RenderThread render;
    std::atomic<int> checked{0}, staticFrames{0};
    std::atomic<bool> mismatch{false};
    for (int f = 0; f < 300; ++f) {
      if (f == 100)
        api.restartStatic(); // A presentation-mode switch.
      if (f == 150 || f == 151)
        api.setPersistentStatic(f == 151); // Off for a frame: a new epoch after.
      churn.frame(api, f);
      legacyChurn.frame(legacy, f);
      const double expected = checksum(legacy.draws());
      const size_t expectedDraws = legacy.draws().size();
      legacy.finishFrame();
      render.wait(); // Present: the render thread finished the previous frame.
      api.publishFrame(frame);
      // Render thread idle: the replica holds exactly the table's members.
      std::map<uint64_t, double> members, replica;
      api.forEachStatic([&](const scene::RetainedInstance &entry) { members[entry.handle] = checksum(entry.records); });
      frame.forEachStatic([&](const scene::RetainedInstance &entry) { replica[entry.handle] = checksum(entry.records); });
      require(members == replica, "The static set replica differs from the table's members");
      render.submit([&, expected, expectedDraws] {
        // The tracer's view of the static set: its epoch, changes and members.
        std::vector<uint64_t> changes;
        if (frame.staticDrawn()) {
          ++staticFrames;
          frame.takeStaticChanges(changes);
          for (const uint64_t handle : changes)
            if (const auto *entry = frame.retained(handle))
              mismatch = mismatch || !entry->member || entry->records.empty();
          size_t members = 0;
          frame.forEachStatic([&](const scene::RetainedInstance &entry) {
            ++members;
            for (const auto &record : entry.records)
              mismatch = mismatch || record.surface->vertices.size() != 4;
          });
          // Each member here has one record; placed members are a subset.
          mismatch = mismatch || members < frame.staticDrawCount();
        }
        frame.expandStatic();
        if (frame.draws().size() != expectedDraws || checksum(frame.draws()) != expected)
          mismatch = true;
        frame.finishFrame();
        ++checked;
      });
    }
    render.wait();
    require(!mismatch, "A published frame differed from the synchronous replay");
    require(checked == 300 && staticFrames > 250, "Scenario coverage");
  }
} // namespace

namespace {
  void bakeLightDependencies() {
    NativeResources api, original, missing, replaced;
    auto light = sphereLight(api, 9800, 0);
    remixapi_BakeSceneInfoEXT request{};
    request.structSize = sizeof(request);
    request.version = REMIXAPI_BAKE_SCENE_VERSION_EXT;
    request.sceneId = request.revision = 1;
    request.sceneScale = 1;
    request.state = REMIXAPI_BAKE_SCENE_READY_EXT;
    request.lightCount = 1;
    request.lights = &light;
    api.setBakeScene(&request);
    api.publishFrame(original); // No drawLight call: culling is not removal.
    require(original.bakeLightsCurrent() && original.sphereLights().empty(),
            "Published static validity does not require a camera-visible light");
    api.destroyLight(light);
    require(!api.bakeLightsCurrent() && original.bakeLightsCurrent(),
            "Destroying a captured light invalidates future frames without mutating earlier publication");
    api.publishFrame(missing);
    require(!missing.bakeLightsCurrent() && missing.bakeScene() == original.bakeScene(),
            "A render replica receives stale-light state even though it owns no definition registry");
    light = sphereLight(api, 9800, 1);
    require(!api.bakeLightsCurrent(), "Reusing a destroyed light handle cannot revive old aggregate data");
    api.setBakeScene(&request);
    api.publishFrame(replaced);
    require(replaced.bakeLightsCurrent() && !missing.bakeLightsCurrent() && original.bakeLightsCurrent() &&
                replaced.bakeScene()->contentHash != original.bakeScene()->contentHash,
            "Recaptured replacements publish their own validity and content identity");
    api.setBakeScene(nullptr);
    api.publishFrame(replaced);
    require(!replaced.bakeLightsCurrent() && !replaced.bakeScene(),
            "Unload cannot retain a prior publication's light validity");
    missing.reset();
    require(!missing.bakeLightsCurrent(), "Reset clears published stale/current dependency state");
  }
}

int main() {
  bakeLightDependencies();
  snapshot();
  lifetime();
  emissionCache();
  restart();
  concurrent();
  std::puts("published frames and resource lifetime");
}
