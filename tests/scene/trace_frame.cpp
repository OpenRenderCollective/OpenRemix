#include "scene/trace_frame.h"
#include "scene/fixtures.h"
#include "scene/opacity.h"
#include "scene/retained.h"
#include <cmath>

using namespace remix::scene;
using namespace remix::test;

int main() {
  SurfaceEmissionCache emissionCache;
  Draw draw;
  draw.instanceId = 5;
  draw.mesh = std::make_shared<Mesh>();
  draw.material = std::make_shared<Material>();
  draw.surface = std::make_shared<Surface>();
  draw.surface->vertices = {{{0, 0, 0}, {.1f, .1f}, {}}, {{1, 0, 0}, {.9f, .1f}, {}}, {{0, 1, 0}, {.1f, .9f}, {}}};
  draw.surface->indices = {0, 1, 2};
  std::fill_n(draw.material->emission, 3, 1.f);
  draw.material->columns = 2;
  draw.material->fps = 2;
  draw.material->emissionTexture = std::make_shared<Texture>();
  draw.material->emissionTexture->pixels = {2, 1, true, {{255, 0, 0, 255, 0, 255, 0, 255}}};

  const auto first = prepareTraceFrame({draw}, .1, true, emissionCache);
  const auto second = prepareTraceFrame({draw}, .6, true, emissionCache);
  const auto wrapped = prepareTraceFrame({draw}, 1.1, true, emissionCache);
  require(first.instances[0][0].animationFrame == 0 && second.instances[0][0].animationFrame == 1,
          "Sprite frame did not advance");
  require(first.flatEmitters().size() == 1 && second.flatEmitters().size() == 1 && wrapped.flatEmitters().size() == 1 &&
              first.flatEmitters()[0].light.radiance[0] == 1 && first.flatEmitters()[0].light.radiance[1] == 0 &&
              second.flatEmitters()[0].light.radiance[0] == 0 && second.flatEmitters()[0].light.radiance[1] == 1 &&
              first.flatEmitters()[0].light == wrapped.flatEmitters()[0].light,
          "Sprite emission reduction does not match the staged UV frame");
  require(first.flatEmitters()[0].animated && first.flatEmitters()[0].layout == second.flatEmitters()[0].layout &&
              first.flatEmitters()[0].light.id == second.flatEmitters()[0].light.id,
          "Animation changed emitter correspondence");
  require(first.flatEmitters()[0].light.radiance[0] == 1 && second.flatEmitters()[0].light.radiance[1] == 1,
          "Preparing another frame mutated pending lighting");

  auto paint = draw;
  paint.decal = true;
  paint.doubleSided = true;
  require(!prepareTraceFrame({paint}, 0, false, emissionCache).instances[0][0].draw.decal,
          "Decal category turned an ordinary opaque wall into paint");
  paint.blend.enabled = true;
  const auto painted = prepareTraceFrame({paint}, 0, false, emissionCache);
  const auto &preparedPaint = painted.instances[0][0];
  OpacityCache opacity;
  require(preparedPaint.draw.decal && !opacity.opaque(preparedPaint.draw, preparedPaint.uv),
          "Decal entered hardware-opaque traversal and cannot resolve its receiver");
  require(paint.blend.enabled && !first.instances[0][0].draw.decal,
          "Preparing paint modified the submitted or pending frame");
  paint.material = std::make_shared<Material>(*paint.material);
  paint.material->translucent = true;
  require(prepareTraceFrame({paint}, 0, false, emissionCache).instances[0][0].draw.decal,
          "Constant-opacity paint was confused with glass");
  paint.material->transmission = true;
  const auto glass = prepareTraceFrame({paint}, 0, false, emissionCache);
  require(!glass.instances[0][0].draw.decal, "Authored glass was converted to paint");

  auto legacy = draw;
  legacy.instanceId = 0;
  auto explicitId = draw;
  explicitId.instanceId = UINT64_MAX;
  auto nextSurface = draw;
  nextSurface.surfaceIndex = 1;
  const auto ordered = prepareTraceFrame({explicitId, legacy, draw, nextSurface}, 0, false, emissionCache);
  require(ordered.instances.size() == 3 && ordered.instances[0].size() == 2 &&
              ordered.instances[0][0].draw.instanceId == 5 &&
              ordered.instances[1][0].draw.instanceId == UINT64_MAX - 1 &&
              !ordered.persistentIds.count(UINT64_MAX - 1) && ordered.persistentIds.count(UINT64_MAX),
          "Grouping, ordering or legacy identities changed");
  require(ordered.flatEmitters()[0].light.id != ordered.flatEmitters()[1].light.id &&
              !ordered.flatEmitters()[0].animated,
          "Surfaces share emitter identity or disabled filtering is staged");
  rejected([&] { prepareTraceFrame({draw, draw}, 0, false, emissionCache); });
  rejected([&] { prepareTraceFrame({nextSurface}, 0, false, emissionCache); });

  auto singular = draw;
  singular.instanceId = 6;
  singular.model[0] = 0;
  auto overlay = draw;
  overlay.classification = 2;
  auto sky = draw;
  sky.camera = REMIXAPI_CAMERA_TYPE_SKY;
  const auto empty = prepareTraceFrame({singular, overlay, sky}, 0, true, emissionCache);
  require(empty.instances.empty() && empty.flatEmitters().empty(),
          "Untraced or singular geometry contributes emission");

  // Replacement snapshots must leave an already prepared frame intact.
  auto owner = draw.material;
  draw.material = std::make_shared<Material>(*owner);
  draw.material->emissionTexture = std::make_shared<Texture>();
  draw.material->emissionTexture->pixels = {2, 1, true, {{0, 0, 255, 255, 0, 0, 0, 255}}};
  draw.material->emissionCache.clear();
  const auto replaced = prepareTraceFrame({draw}, .1, true, emissionCache);
  require(replaced.flatEmitters().size() == 1 && replaced.flatEmitters()[0].light.radiance[2] == 1 &&
              replaced.flatEmitters()[0].light.radiance[0] == 0 && first.instances[0][0].draw.material == owner &&
              first.flatEmitters()[0].light.radiance[0] == 1,
          "Texture replacement reused stale reduction or lost snapshot ownership");

  draw.surface = std::make_shared<Surface>(*draw.surface);
  draw.surface->bonesPerVertex = 1;
  draw.surface->weights = {1, 1, 1};
  draw.surface->bones = {0, 0, 0};
  draw.bones = std::make_shared<std::vector<Matrix>>(1, identity());
  (*draw.bones)[0][12] = 2;
  draw.model[12] = 3;
  draw.doubleSided = true;
  const auto skinned = prepareTraceFrame({draw}, .1, false, emissionCache);
  require(skinned.flatEmitters()[0].light.corners[0][0] == 5 && skinned.flatEmitters()[0].light.triangle == 2,
          "Prepared emitter lost skinning, world transform or sidedness");
  // Albedo animation and instance state must reuse reductions while producing
  // fresh world-space light geometry. Replacing emission/UV snapshots must not.
  const auto uv = skinned.instances[0][0].uv;
  const auto reduction = emissionCache.reduce(draw, uv);
  draw.material = std::make_shared<Material>(*draw.material);
  draw.material->texture = std::make_shared<Texture>();
  require(emissionCache.reduce(draw, uv) == reduction, "Unrelated albedo invalidated emission reduction");
  draw.model[12] = 7;
  draw.instanceId = 42;
  draw.doubleSided = false;
  draw.bones = std::make_shared<std::vector<Matrix>>(1, identity());
  (*draw.bones)[0][12] = 4;
  const auto moved = prepareTraceFrame({draw}, .1, true, emissionCache);
  require(moved.flatEmitters()[0].light.corners[0][0] == 11 && moved.flatEmitters()[0].light.triangle == 1 &&
              moved.flatEmitters()[0].light.id != skinned.flatEmitters()[0].light.id &&
              skinned.flatEmitters()[0].light.corners[0][0] == 5 && emissionCache.reduce(draw, uv) == reduction,
          "Cached reduction froze instance identity, transform, skinning or sidedness");
  auto compareFresh = [&](double seconds) {
    SurfaceEmissionCache fresh;
    const auto expected = prepareTraceFrame({draw}, seconds, true, fresh);
    const auto actual = prepareTraceFrame({draw}, seconds, true, emissionCache);
    require(actual.flatEmitters().size() == expected.flatEmitters().size(),
            "Cached emitter count differs from fresh reduction");
    for (size_t i = 0; i < actual.flatEmitters().size(); ++i)
      require(actual.flatEmitters()[i].light == expected.flatEmitters()[i].light &&
                  actual.flatEmitters()[i].layout == expected.flatEmitters()[i].layout,
              "Cached emission differs from fresh reduction");
    return actual;
  };
  // A previously black sprite becomes luminous, without changing geometry.
  require(compareFresh(.6).flatEmitters().empty(), "Black sprite entered light hierarchy");
  draw.material = std::make_shared<Material>(*draw.material);
  draw.material->emissionTexture = std::make_shared<Texture>();
  draw.material->emissionTexture->pixels = {2, 1, true, {{0, 0, 0, 255, 255, 255, 255, 255}}};
  draw.material->emissionCache.clear();
  require(compareFresh(.6).flatEmitters().size() == 1 && reduction->front().radiance[2] == 1,
          "Emission replacement reused black cache or mutated pending reduction");
  draw.material = std::make_shared<Material>(*draw.material);
  draw.material->emission[0] = 3;
  draw.material->emissionCache.clear();
  require(compareFresh(.6).flatEmitters()[0].light.radiance[0] == 3, "Changed emission constant reused stale radiance");
  draw.material = std::make_shared<Material>(*draw.material);
  draw.material->emissionTexture.reset();
  require(compareFresh(.6).flatEmitters()[0].light.radiance[0] == 3, "Constant emission inherited textured reduction");
  draw.surface = std::make_shared<Surface>(*draw.surface);
  draw.surface->indices.insert(draw.surface->indices.end(), {0, 2, 1});
  require(compareFresh(.6).flatEmitters().size() == 2, "Replacement geometry reused old triangle list");
  std::weak_ptr<Surface> removed = draw.surface;
  draw.surface.reset();
  emissionCache.prune();
  require(removed.expired(), "Emission cache retained retired geometry");

  // remixapi_InstanceInfoTexcoordTransformEXT: scale and offset before the
  // sprite cell, the offset scrolled by the clock.
  remixapi_InstanceInfoTexcoordTransformEXT texcoord{
      REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_TEXCOORD_TRANSFORM_EXT, nullptr, {2, 1}, {.25f, 0}, {.5f, 0}};
  remixapi_InstanceInfo info{};
  info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  info.pNext = &texcoord;
  info.mesh = reinterpret_cast<remixapi_MeshHandle>(uintptr_t(1));
  info.transform.matrix[0][0] = info.transform.matrix[1][1] = info.transform.matrix[2][2] = 1;
  InstanceDescription described;
  describeInstance(info, described);
  Draw scrolled;
  scrolled.instanceId = 9;
  scrolled.mesh = std::make_shared<Mesh>();
  scrolled.material = std::make_shared<Material>();
  scrolled.material->columns = 2;
  scrolled.material->fps = 2;
  scrolled.surface = std::make_shared<Surface>();
  scrolled.surface->vertices = {{{0, 0, 0}, {.1f, .1f}, {}}, {{1, 0, 0}, {.9f, .1f}, {}}, {{0, 1, 0}, {.1f, .9f}, {}}};
  scrolled.surface->indices = {0, 1, 2};
  scrolled.texcoord = described.texcoord;
  scrolled.texcoordVelocity = described.texcoordVelocity;
  auto sameUv = [](const std::array<float, 4> &a, std::array<float, 4> b) {
    for (unsigned i = 0; i < 4; ++i)
      if (std::abs(a[i] - b[i]) > 1e-4f)
        return false;
    return true;
  };
  uint32_t cell = 0;
  // Cell 1 of 2 at .6 s: u x 2 x .5 + (.25 + .5 x .6) x .5 + .5.
  require(sameUv(surfaceTexcoord(scrolled, .6, cell), {1, 1, .775f, 0}) && cell == 1,
          "Texcoord transform not composed with the sprite cell");
  require(sameUv(surfaceTexcoord(scrolled, .6, cell, false), {1, 1, .5f, 0}), "Resting texcoords moved");
  require(sameUv(surfaceTexcoord(scrolled, 1e6 + .6, cell), {1, 1, .775f, 0}),
          "A long-running scroll lost its precision");
  texcoord.offset[1] = NAN;
  rejected([&] { describeInstance(info, described); });

  scrolled.classification = REMIXAPI_INSTANCE_STATIC_EXT;
  scrolled.material->fps = 0;
  auto resting = scrolled;
  resting.texcoordVelocity = {};
  auto driven = resting; // a host that gives the offset itself
  resting.texcoord = {2, 1, 0, 0};
  require(staticTraceEligible({resting}) && !staticTraceEligible({scrolled}),
          "A scrolling instance entered the persistent static set");

  // Alpha test of an opaque texture: proven at rest, not while it scrolls,
  // and one cache entry for every offset.
  for (auto *d : {&scrolled, &resting}) {
    d->doubleSided = true;
    d->material = scrolled.material;
  }
  scrolled.material->compare = 6;
  scrolled.material->reference = .5f;
  scrolled.material->texture = std::make_shared<Texture>();
  scrolled.material->texture->pixels = {2, 1, true, {{255, 0, 0, 255, 0, 255, 0, 255}}};
  OpacityCache coverage;
  const auto still = coverage.partition(resting, surfaceTexcoord(resting, .6, cell));
  const auto moving = coverage.partition(scrolled, surfaceTexcoord(scrolled, .6, cell));
  require(still->opaque(0) && !moving->opaque(0) && moving->reasons[OpacityPartition::Scrolling] == 1,
          "Scrolling coverage was proven from one offset");
  require(coverage.partition(scrolled, surfaceTexcoord(scrolled, 1.6, cell)) == moving,
          "Scrolling coverage was proven again for a new offset");
  driven.doubleSided = true;
  driven.material = scrolled.material;
  const auto drivenFirst = coverage.partition(driven, surfaceTexcoord(driven, 0, cell));
  driven.texcoord[2] = .4f;
  require(!drivenFirst->opaque(0) && coverage.partition(driven, surfaceTexcoord(driven, 0, cell)) == drivenFirst,
          "A host-driven offset was proven, or proven again for every offset");

  // Its emitters: one estimate at rest, whatever the time.
  std::fill_n(scrolled.material->emission, 3, 1.f);
  scrolled.material->emissionTexture = scrolled.material->texture;
  const auto early = prepareTraceFrame({scrolled}, .6, true, emissionCache);
  const auto late = prepareTraceFrame({scrolled}, 1.3, true, emissionCache);
  require(early.emitters.size() == 1 && early.emitters[0].reduction == late.emitters[0].reduction &&
              !sameUv(early.instances[0][0].uv, late.instances[0][0].uv),
          "Scrolling emitters were estimated again, or the surface did not scroll");
  std::puts("CPU tracing snapshots, identities, animation, emitters and "
            "ownership passed");
}
