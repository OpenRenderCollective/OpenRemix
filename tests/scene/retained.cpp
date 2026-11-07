#include "scene/fixtures.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

using namespace remix::test;
namespace {
  size_t frame(remix::NativeResources &r, const float *origin = nullptr) {
    r.finishFrame();
    r.drawRetained(origin);
    return r.pendingDrawCount();
  }

  remixapi_SceneInstanceChangeEXT moveTo(uint64_t handle, float x) {
    remixapi_SceneInstanceChangeEXT u = {};
    u.instance = handle;
    u.fields = REMIXAPI_SCENE_INSTANCE_TRANSFORM_EXT;
    u.transform.matrix[0][0] = u.transform.matrix[1][1] = u.transform.matrix[2][2] = 1;
    u.transform.matrix[0][3] = x;
    return u;
  }

  remixapi_SceneInstanceChangeEXT show(uint64_t handle, bool visible) {
    remixapi_SceneInstanceChangeEXT u = {};
    u.instance = handle;
    u.fields = REMIXAPI_SCENE_INSTANCE_VISIBLE_EXT;
    u.visible = visible;
    return u;
  }

  void matchesDrawInstance() {
    remix::NativeResources r;
    auto red = material(r, 1, 1, 0, 0);
    auto mesh = quad(r, 2, red);
    remixapi_InstanceInfoBlendEXT blend = {};
    blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
    blend.alphaBlendEnabled = 1;
    blend.srcColorBlendFactor = 6;
    blend.dstColorBlendFactor = 7;
    blend.alphaTestEnabled = 1;
    blend.alphaTestCompareOp = 4;
    blend.alphaTestReferenceValue = 64;
    remixapi_Transform locals[3] = {};
    for (int i = 0; i < 3; ++i) {
      locals[i].matrix[0][0] = locals[i].matrix[1][1] = locals[i].matrix[2][2] = 1;
      locals[i].matrix[1][3] = float(i);
    }
    remixapi_InstanceInfoGpuInstancingEXT instancing = {};
    instancing.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_GPU_INSTANCING_EXT;
    instancing.pNext = &blend;
    instancing.instanceTransforms_values = locals;
    instancing.instanceTransforms_count = 3;
    auto info = instance(mesh, 2);
    info.pNext = &instancing;
    info.doubleSided = 1;
    info.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC;
    r.drawInstance(info);
    const std::vector<remix::scene::Draw> immediate = r.draws();
    r.finishFrame();
    const uint64_t handle = r.createRetained(info);
    require(handle != 0, "Retained handle is zero");
    locals[0].matrix[0][3] = 99; // The registration must not refer to caller memory.
    require(frame(r) == immediate.size() && immediate.size() == 3, "Retained draw count differs from DrawInstance");
    for (size_t i = 0; i < immediate.size(); ++i) {
      const auto &a = immediate[i], &b = r.draws()[i];
      require(a.mesh == b.mesh && a.surface == b.surface && a.material == b.material && a.model == b.model,
              "Retained record resolved differently");
      require(a.instanceIndex == b.instanceIndex && a.surfaceIndex == b.surfaceIndex &&
                  a.blend.enabled == b.blend.enabled && a.blend.src == b.blend.src && a.doubleSided == b.doubleSided &&
                  a.decal == b.decal && a.depthBias == b.depthBias && a.alphaOverride == b.alphaOverride &&
                  a.compare == b.compare && a.reference == b.reference,
              "Retained record state differs from DrawInstance");
      require(b.instanceId != 0 && b.instanceId == r.draws()[0].instanceId, "Retained identity missing");
      require(b.classification == 0 && a.classification == 1, "Retained instance is not static");
    }
    const uint64_t id = r.draws()[0].instanceId;
    require(frame(r) == 3 && r.draws()[0].instanceId == id, "Retained identity is not stable");
    const float origin[3] = {1, 2, 3};
    frame(r, origin);
    require(r.draws()[2].model[12] == immediate[2].model[12] - 1 &&
                r.draws()[2].model[13] == immediate[2].model[13] - 2 &&
                r.draws()[2].model[14] == immediate[2].model[14] - 3,
            "Scene origin was not subtracted");
    const float bad[3] = {0, std::numeric_limits<float>::infinity(), 0};
    rejected([&] { r.drawRetained(bad); });
  }

  void updatesAndLifetime() {
    remix::NativeResources r;
    auto red = material(r, 1, 1, 0, 0);
    auto mesh = quad(r, 2, red);
    const uint64_t a = r.createRetained(instance(mesh));
    const uint64_t b = r.createRetained(instance(mesh, 1));
    require(a && b && a != b, "Retained handles are not unique");
    require(frame(r) == 2, "Retained instances not drawn");

    // Visibility: hidden entries are skipped; a re-description keeps it.
    auto hide = show(a, false);
    r.updateRetained(&hide, 1);
    require(frame(r) == 1, "Hidden retained instance was drawn");
    auto redescribed = instance(mesh);
    redescribed.doubleSided = 1;
    r.updateRetained(a, redescribed);
    require(frame(r) == 1, "Re-description changed visibility");
    require(r.retainedStatistics().moving == 0, "A re-description in place counted as a move");
    auto unhide = show(a, true);
    r.updateRetained(&unhide, 1);
    require(frame(r) == 2, "Shown retained instance was not drawn");

    // A transform update is a move: dynamic until the instance settles.
    auto move = moveTo(b, 5);
    r.updateRetained(&move, 1);
    frame(r);
    const auto *moved = &r.draws()[0];
    for (const auto &draw : r.draws())
      if (draw.model[12] == 5)
        moved = &draw;
    require(moved->model[12] == 5 && moved->classification == 1, "Moved instance kept static class or old transform");
    require(r.retainedStatistics().moving == 1, "Moving count");
    for (uint64_t i = 0; i < remix::NativeResources::retainedSettleFrames; ++i)
      frame(r);
    for (const auto &draw : r.draws())
      require(draw.classification == 0, "Settled instance did not return to static");
    auto same = moveTo(b, 5);
    r.updateRetained(&same, 1);
    frame(r);
    require(r.retainedStatistics().moving == 0, "An unchanged transform counted as a move");

    // Batches are validated before anything is applied.
    remixapi_SceneInstanceChangeEXT batch[2] = {moveTo(a, 7), moveTo(0, 1)};
    rejected([&] { r.updateRetained(batch, 2); });
    batch[1] = moveTo(b, 1);
    batch[1].transform.matrix[0][0] = std::numeric_limits<float>::quiet_NaN();
    rejected([&] { r.updateRetained(batch, 2); });
    frame(r);
    for (const auto &draw : r.draws())
      require(draw.model[12] != 7, "Rejected batch was partly applied");
    rejected([&] { r.updateRetained(static_cast<const remixapi_SceneInstanceChangeEXT *>(nullptr), 1); });
    r.updateRetained(static_cast<const remixapi_SceneInstanceChangeEXT *>(nullptr), 0);
    auto unknown = moveTo(12345, 1);
    r.updateRetained(&unknown, 1); // Unknown handles are ignored.

    // Mesh destroyed before its retained instances: nothing dangles, nothing is
    // drawn, and a mesh created again under the hash is picked up.
    r.destroyMesh(mesh);
    require(frame(r) == 0, "Retained instance of a destroyed mesh was drawn");
    mesh = quad(r, 2, red, 0xff00ff00);
    require(frame(r) == 2, "Recreated mesh was not picked up");
    require(r.draws()[0].surface->vertices[0].color[1] == 1 && r.draws()[0].surface->vertices[0].color[0] == 0,
            "Retained instance kept the destroyed mesh");

    // Material destroyed: default material; replaced: the new one.
    const auto before = r.draws()[0].material;
    r.destroyMaterial(red);
    require(frame(r) == 2 && r.draws()[0].material != before && r.draws()[0].material->tint[0] == 1 &&
                r.draws()[0].material->tint[1] == 1,
            "Destroyed material did not fall back to the default material");
    red = material(r, 1, 0, 0, 1);
    require(frame(r) == 2 && r.draws()[0].material->tint[2] == 1 && r.draws()[0].material->tint[0] == 0,
            "Recreated material was not picked up");
    material(r, 1, 0, 1, 0); // Same handle, new content.
    require(frame(r) == 2 && r.draws()[0].material->tint[1] == 1 && r.draws()[0].material->tint[2] == 0,
            "Replaced material was not picked up");

    // Destroy: unknown and repeated handles are ignored.
    const uint64_t doomed[] = {a, a, 777};
    r.destroyRetained(doomed, 3);
    require(frame(r) == 1 && r.retainedCount() == 1, "Destroy");
    rejected([&] { r.destroyRetained(nullptr, 1); });
    r.destroyRetained(nullptr, 0);

    // As in the fork, re-describing an unknown handle registers it; creation
    // then never hands that handle out.
    r.updateRetained(1000, instance(mesh));
    require(frame(r) == 2 && r.createRetained(instance(mesh)) == 1001, "Upsert or handle counter");

    // Validation at registration.
    auto hidden = instance(mesh);
    hidden.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN;
    r.createRetained(hidden);
    require(frame(r) == 3, "Hidden category drawn");
    auto missing = instance(reinterpret_cast<remixapi_MeshHandle>(uintptr_t(55)));
    const uint64_t pending = r.createRetained(missing); // Fork: a mesh may be registered later.
    require(frame(r) == 3, "Instance of a missing mesh drawn");
    quad(r, 55, red);
    require(frame(r) == 4, "Mesh registered after its instance was not picked up");
    const uint64_t drop[] = {pending};
    r.destroyRetained(drop, 1);
    rejected([&] { r.createRetained(instance(nullptr)); });
    auto nan = instance(mesh);
    nan.transform.matrix[0][0] = std::numeric_limits<float>::quiet_NaN();
    rejected([&] { r.createRetained(nan); });
    rejected([&] { r.updateRetained(0, instance(mesh)); });

    // Caller identities keep their class and join the duplicate check.
    remixapi_InstanceIdentityEXT identity = {};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.instanceId = 42;
    identity.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
    identity.rasterVisible = 1;
    auto identified = instance(mesh);
    identified.pNext = &identity;
    r.createRetained(identified);
    frame(r);
    bool found = false;
    for (const auto &draw : r.draws())
      found |= draw.instanceId == 42 && draw.classification == 1;
    require(found, "Caller identity was not kept");
    rejected([&] { r.drawInstance(identified); });

    r.reset();
    require(r.retainedCount() == 0 && frame(r) == 0, "Reset kept retained instances");
    red = material(r, 1, 1, 0, 0);
    mesh = quad(r, 2, red);
    require(r.createRetained(instance(mesh)) > 1001, "Handles were reused after reset");
  }

  // Per-instance CPU cost of a static scene: DrawInstance per placement versus
  // one replay. Informational; printed, not asserted.
  void cost() {
    remix::NativeResources r;
    auto red = material(r, 1, 1, 0, 0);
    auto mesh = quad(r, 2, red);
    constexpr uint32_t count = 20000, frames = 20;
    std::vector<remixapi_InstanceInfo> infos;
    std::vector<remixapi_InstanceInfoBlendEXT> blends(count);
    for (uint32_t i = 0; i < count; ++i) {
      infos.push_back(instance(mesh, float(i)));
      blends[i].sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
      infos.back().pNext = &blends[i];
    }
    using clock = std::chrono::steady_clock;
    auto measure = [&](auto &&submit) {
      submit();
      r.finishFrame();
      const auto start = clock::now();
      for (uint32_t f = 0; f < frames; ++f) {
        submit();
        r.finishFrame();
      }
      return std::chrono::duration<double, std::nano>(clock::now() - start).count() / (double(count) * frames);
    };
    const double immediate = measure([&] {
      for (const auto &info : infos)
        r.drawInstance(info);
    });
    for (const auto &info : infos)
      r.createRetained(info);
    const double retained = measure([&] { r.drawRetained(nullptr); });
    require(r.retainedStatistics().draws == count, "Replay count");
    std::printf("per instance: DrawInstance %.1f ns, retained replay %.1f ns (%u instances, 1 surface)\n", immediate,
                retained, count);
  }
} // namespace

int main() {
  matchesDrawInstance();
  updatesAndLifetime();
  cost();
  return 0;
}
