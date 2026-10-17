#include "forward/lighting.h"
#include "forward/shadows/sun_cache.h"
#include "forward/water/data.h"
#include "runtime/settings.h"
#include "scene/fixtures.h"
#include <cmath>

using namespace remix;
using namespace remix::forward;
using remix::test::require;

int main() {
  {
    scene::Draw body;
    body.surface = std::make_shared<scene::Surface>();
    body.material = std::make_shared<scene::Material>();
    body.material->transmission = true;
    body.material->refractiveIndex = 1.333f;
    body.material->transmittanceDistance = 50;
    body.material->normalEncoding = 5;
    body.material->scrollLayers = 3;
    body.material->scroll[0] = .1f;
    body.material->scroll[3] = -.2f;
    body.material->scroll[5] = .3f;
    require(water::accepts(body) && !accepts(body),
            "Measured water has separate ownership and stays out of opaque shadow/bake policies");
    const auto data = water::materialData(*body.material, 2);
    require(data.transmittance[3] == 50 && data.optical == Vec4{1.333f, 2, 3, 0} &&
                data.scroll01[0] == .1f && data.scroll01[3] == -.2f && data.scroll2[1] == .3f,
            "Water retains measured optics and authored Source normal-scroll rates");
    body.material->transmittanceDistance = 0;
    require(!water::accepts(body), "Thin transmission remains in compatibility rendering");
    body.material->transmittanceDistance = 50;
    body.depthWriteOverride = true;
    body.depthWrite = false;
    require(!water::accepts(body), "Water ownership respects an explicit no-depth-write draw");
    Options a, b;
    b.water = true;
    require(!a.sameTargets(b), "Water enable prepares its independent targets and pipelines");
    a.water = true;
    b.waterDistance = 80;
    require(a.sameTargets(b), "Water trace-parameter edits retain target resources");
  }
  const Bounds localBox{{-1, -2, -3}, {2, 4, 5}, true};
  auto model = scene::identity();
  model[0] = -2; model[4] = .5f; model[12] = 10;
  model[5] = 3; model[9] = -.25f; model[13] = -7;
  model[10] = -1; model[14] = 1;
  const auto worldBox = transformBounds(localBox, model);
  require(worldBox.valid && worldBox.minimum == Vec3({5, -14.25f, -4}) &&
              worldBox.maximum == Vec3({14, 5.75f, 4}),
          "Transformed caster bounds include every corner under negative scale and shear");
  Bounds combined;
  combined.merge(worldBox);
  combined.merge({});
  require(combined.minimum == worldBox.minimum && combined.maximum == worldBox.maximum &&
              !transformBounds({}, model).valid,
          "Empty bounds cannot add an origin or become a fabricated frozen caster");
  shadows::SunCache sunCache;
  std::array<Matrix, cascadeCount> matrices;
  matrices.fill(scene::identity());
  auto source = std::make_shared<scene::BakeScene>();
  auto dirty = sunCache.update(source, true, matrices);
  require(std::count(dirty.begin(), dirty.end(), true) == cascadeCount,
          "Every static sun layer is initialized before sampling");
  dirty = sunCache.update(source, true, matrices);
  require(std::count(dirty.begin(), dirty.end(), true) == 0,
          "Unchanged frozen geometry and projections reuse every static sun cascade");
  matrices[2][12] += 1;
  dirty = sunCache.update(source, true, matrices);
  require(dirty[2] && std::count(dirty.begin(), dirty.end(), true) == 1,
          "A changed cascade only redraws its own static layer");
  source = std::make_shared<scene::BakeScene>(*source);
  dirty = sunCache.update(source, true, matrices);
  require(std::count(dirty.begin(), dirty.end(), true) == cascadeCount,
          "Replacing the frozen scene invalidates cached sun geometry");
  source.reset();
  dirty = sunCache.update(source, true, matrices);
  require(std::count(dirty.begin(), dirty.end(), true) == cascadeCount,
          "Expired frozen ownership still clears all layers on unload");
  sunCache.update(source, false, matrices);
  matrices[0][12] += 1;
  dirty = sunCache.update(source, false, matrices);
  require(std::count(dirty.begin(), dirty.end(), true) == 0,
          "Camera motion cannot make cleared disabled sun layers dirty");
  dirty = sunCache.update(source, true, matrices);
  require(std::count(dirty.begin(), dirty.end(), true) == cascadeCount,
          "Re-enabling sunlight initializes the current projections");
  scene::DistantLight capturedSun;
  capturedSun.isDistant = true;
  capturedSun.angularDiameterDegrees = .5f;
  auto liveSun = capturedSun;
  liveSun.radiance[0] = 100;
  require(matchesSun(capturedSun, liveSun), "Sun colour changes retain geometry-valid baked visibility");
  liveSun.direction[0] += .001f;
  require(!matchesSun(capturedSun, liveSun), "Sun movement rejects stale lightmap shadows");
  liveSun = capturedSun;
  liveSun.angularDiameterDegrees += .001f;
  require(!matchesSun(capturedSun, liveSun), "Changing sun size rejects old penumbrae");
  liveSun = capturedSun;
  liveSun.isDistant = false;
  require(!matchesSun(capturedSun, liveSun), "An absent sun cannot borrow a captured sun bake");
  const DepthRange range{.1f, 1000, 1};
  require(depthSlice(.001f, range) == 0, "Near cluster clamp");
  require(depthSlice(1e6f, range) == clusterSlices - 1, "Far cluster clamp");
  for (uint32_t i = 0; i < clusterSlices; ++i) {
    const float middle = std::sqrt(sliceDepth(i, range) * sliceDepth(i + 1, range));
    require(depthSlice(middle, range) == i, "Logarithmic cluster partition");
  }
  const auto identity = scene::identity();
  const auto origin = transformPoint(identity, {1, 2, 3});
  require(origin == Vec3{1, 2, 3}, "Column-major transform");
  require(intersectsClip({}, identity), "Missing bounds must not remove a caster");
  Bounds enclosing{{-2, -2, -2}, {2, 2, 2}, true};
  require(intersectsClip(enclosing, identity), "A box enclosing the clip volume survives without inside corners");
  for (uint32_t axis = 0; axis < 3; ++axis)
    for (float side : {-1.f, 1.f}) {
      Bounds outside{{-.1f, -.1f, .4f}, {.1f, .1f, .6f}, true};
      outside.minimum[axis] = side < 0 ? -3.f : 2.f;
      outside.maximum[axis] = side < 0 ? -2.f : 3.f;
      require(!intersectsClip(outside, identity), "Each Vulkan clip plane rejects a separated caster");
      outside.minimum[axis] = side < 0 ? -2.f : 1.f;
      outside.maximum[axis] = side < 0 ? (axis == 2 ? 0.f : -1.f) : 2.f;
      require(intersectsClip(outside, identity), "Casters touching a clip plane must survive");
    }
  Matrix oblique = identity;
  oblique[0] = oblique[4] = .70710678f;
  oblique[1] = -.70710678f;
  oblique[5] = .70710678f;
  require(!intersectsClip({{2, 2, .2f}, {3, 3, .8f}, true}, oblique),
          "Rotated clip planes reject a world-space box beyond the diagonal boundary");
  require(intersectsClip({{-.2f, -.2f, -2}, {.2f, .2f, .5f}, true}, identity),
          "An offscreen caster crossing the near plane remains eligible");
  Matrix perspective{};
  perspective[0] = perspective[5] = 1;
  perspective[10] = .5f;
  perspective[11] = 1;
  require(intersectsClip({{-.1f, -.1f, 2}, {.1f, .1f, 3}, true}, perspective) &&
              !intersectsClip({{-.1f, -.1f, -3}, {.1f, .1f, -2}, true}, perspective),
          "Homogeneous clipping keeps forward geometry and rejects geometry behind a perspective camera");
  scene::SphereLight light;
  light.radius = 1;
  light.radiance[0] = light.radiance[1] = light.radiance[2] = 1;
  const float radius = influenceRadius(light, .01f);
  light.radiance[0] = light.radiance[1] = light.radiance[2] = 4;
  require(std::abs(influenceRadius(light, .01f) - 2 * radius) < 1e-4f,
          "Influence radius follows inverse square power");
  Bounds bounds;
  bounds.include({-2, -2, 0});
  bounds.include({2, 2, 3});
  auto cascades = shadowCascades(identity, identity, {.001f, 1, 1}, {0, 0, 1}, bounds, 1, 1024);
  for (uint32_t c = 0; c < cascadeCount; ++c) {
    require(std::isfinite(cascades.splits[c]) && (c == 0 || cascades.splits[c] > cascades.splits[c - 1]),
            "Ordered cascade splits");
    for (float value : cascades.matrices[c])
      require(std::isfinite(value), "Finite shadow transforms");
  }
  scene::Draw draw;
  draw.material = std::make_shared<scene::Material>();
  require(accepts(draw), "Opaque world belongs to Forward+");
  draw.blend.enabled = true;
  require(!accepts(draw), "Transparency stays in compatibility raster");
  {
    auto paint = draw;
    paint.terrain = paint.decal = true;
    paint.classification = REMIXAPI_INSTANCE_STATIC_EXT;
    paint.blend.src = 6;
    paint.blend.dst = 7;
    require(acceptsTerrainLayer(paint) && !accepts(paint),
            "Explicit static terrain paint receives lighting without becoming an opaque caster");
    paint.terrain = false;
    require(!acceptsTerrainLayer(paint), "Ordinary decals retain compatibility ownership");
    paint.terrain = true;
    paint.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
    require(!acceptsTerrainLayer(paint), "Dynamic decals cannot inherit static terrain lighting");
    paint.classification = REMIXAPI_INSTANCE_STATIC_EXT;
    paint.blend.dst = 1;
    require(!acceptsTerrainLayer(paint), "Additive layers do not become terrain source-over paint");
  }
  draw.blend.enabled = false;
  draw.cutGroup = 1;
  require(!accepts(draw), "Mask cut stays in compatibility raster");
  draw.cutGroup = 0;
  draw.viewModel = true;
  require(!accepts(draw), "Viewmodel camera is not world lighting");
  config::Settings settings;
  require(config::set(settings, "rtx.native.backend", "forwardplus") == config::SetResult::Success &&
              settings.backend == 2,
          "Explicit forward backend value");
  require(config::set(settings, "rtx.native.forward.samples", "3") == config::SetResult::Invalid,
          "Reject unsupported MSAA count");
  require(config::set(settings, "rtx.native.forward.samples", "2") == config::SetResult::Success,
          "Accept 2x MSAA");
  require(config::set(settings, "rtx.native.forward.shadowResolution", "1000") == config::SetResult::Invalid,
          "Reject non-power-of-two shadow map");
  require(config::set(settings, "rtx.native.forward.lightCutoff", "nan") == config::SetResult::Invalid,
          "Reject nonfinite light cutoff");
  require(config::set(settings, "rtx.native.forward.ssrScale", "3") == config::SetResult::Invalid,
          "Reject unsupported reflection resolution");
  require(config::set(settings, "rtx.native.forward.ssrScale", "4") == config::SetResult::Success &&
              config::set(settings, "rtx.native.forward.ssrSteps", "256") == config::SetResult::Success &&
              config::set(settings, "rtx.native.forward.ssrDistance", "32") == config::SetResult::Success,
          "Reflection settings persist alongside lighting settings");
  Options changed = settings.forward;
  changed.ssrThickness *= 2;
  require(changed.sameTargets(settings.forward), "Trace edits keep existing reflection targets");
  changed.ssrScale = 1;
  require(!changed.sameTargets(settings.forward), "Reflection resolution prepares new targets");
  config::Settings restored;
  require(config::set(settings, "rtx.native.forward.bakeProbes", "63") == config::SetResult::Invalid &&
              config::set(settings, "rtx.native.forward.bakeSamples", "8193") == config::SetResult::Invalid &&
              config::set(settings, "rtx.native.forward.bakeTextureResolution", "48") ==
                  config::SetResult::Invalid,
          "Reject unsupported bake budgets and atlas dimensions");
  require(config::set(settings, "rtx.native.forward.bake", "0") == config::SetResult::Success &&
              config::set(settings, "rtx.native.forward.bakeSpacing", "3.5") == config::SetResult::Success &&
              config::set(settings, "rtx.native.forward.bakeSamples", "512") == config::SetResult::Success,
          "Bake controls participate in runtime configuration");
  config::UnknownSettings unknown;
  require(config::set(settings, "rtx.native.forward.reflectionProbes", "33") == config::SetResult::Invalid &&
              config::set(settings, "rtx.native.forward.reflectionResolution", "48") ==
                  config::SetResult::Invalid,
          "Reflection probe limits and power-of-two dimensions are enforced");
  require(config::set(settings, "rtx.native.forward.reflectionProbes", "32") == config::SetResult::Success &&
              config::set(settings, "rtx.native.forward.reflectionResolution", "64") ==
                  config::SetResult::Success,
          "Reflection capture settings participate in profile persistence");
  require(config::set(settings, "rtx.native.forward.reflectionDetailTolerance", "-0.1") ==
                  config::SetResult::Invalid &&
              config::set(settings, "rtx.native.forward.reflectionDetailTolerance", "nan") ==
                  config::SetResult::Invalid &&
              config::set(settings, "rtx.native.forward.reflectionDetailTolerance", "2.1") ==
                  config::SetResult::Invalid,
          "Reflection detail tolerance rejects nonfinite and out-of-range values");
  require(config::set(settings, "rtx.native.forward.reflectionDetailTolerance", "0") ==
                  config::SetResult::Success &&
              config::set(settings, "rtx.native.forward.reflectionDetailTolerance", "0.35") ==
                  config::SetResult::Success,
          "Strict and tolerant reflection fitting participate in profile persistence");
  require(config::set(settings, "rtx.native.forward.lightmapPageSize", "96") == config::SetResult::Invalid &&
              config::set(settings, "rtx.native.forward.lightmapLuxelSize", "0") == config::SetResult::Invalid &&
              config::set(settings, "rtx.native.forward.lightmapTexels", "1024") ==
                  config::SetResult::Invalid,
          "Surface atlas controls enforce supported page sizes and texel budgets");
  require(config::set(settings, "rtx.native.forward.lightmapPageSize", "128") == config::SetResult::Success &&
              config::set(settings, "rtx.native.forward.lightmapLuxelSize", "0.125") == config::SetResult::Success &&
              config::set(settings, "rtx.native.forward.lightmapTexels", "524288") ==
                  config::SetResult::Success,
          "Surface atlas quality settings participate in profile persistence");
  require(config::set(settings, "rtx.native.forward.lightmapPageSize", "4096") == config::SetResult::Success &&
              config::set(settings, "rtx.native.forward.lightmapTexels", "16777216") == config::SetResult::Success &&
              config::set(settings, "rtx.native.forward.lightmapPageSize", "8192") == config::SetResult::Invalid &&
              config::set(settings, "rtx.native.forward.lightmapTexels", "16777217") == config::SetResult::Invalid,
          "4K atlas controls persist and reject dimensions or budgets above their limits");
  auto pageBudget = settings.forward.baking;
  require(pageBudget.lightmapPageSizeForBudget() == 4096,
          "A complete 4K page fits its full texel budget");
  pageBudget.lightmapTexels = 4194304;
  require(pageBudget.lightmapPageSizeForBudget() == 2048,
          "A 4K page request respects a smaller budget without increasing memory");
  require(
      config::set(settings, "rtx.native.forward.localShadowResolution", "96") == config::SetResult::Invalid &&
          config::set(settings, "rtx.native.forward.localShadowLights", "9") == config::SetResult::Invalid &&
          config::set(settings, "rtx.native.forward.localShadowBias", "nan") == config::SetResult::Invalid,
      "Local shadow quality rejects invalid dimensions and budgets");
  require(
      config::set(settings, "rtx.native.forward.localShadowResolution", "256") ==
              config::SetResult::Success &&
          config::set(settings, "rtx.native.forward.localShadowLights", "2") == config::SetResult::Success &&
          config::set(settings, "rtx.native.forward.localShadowBias", "0.005") == config::SetResult::Success,
      "Local shadow controls participate in profile persistence");
  changed = settings.forward;
  changed.localShadowBias *= 2;
  require(changed.sameTargets(settings.forward), "Shadow bias updates reuse targets");
  changed.localShadowResolution *= 2;
  require(!changed.sameTargets(settings.forward), "Shadow resolution changes replace targets");
  require(
      config::set(settings, "rtx.native.forward.rtTextureResolution", "48") == config::SetResult::Invalid &&
          config::set(settings, "rtx.native.forward.rtReflectionBounces", "9") == config::SetResult::Invalid,
      "Hardware reflection transport quality enforces bounded dimensions and bounce counts");
  require(config::set(settings, "rtx.native.forward.rtReflections", "1") == config::SetResult::Success &&
              config::set(settings, "rtx.native.forward.rtReflectionDistance", "32") ==
                  config::SetResult::Success &&
              config::set(settings, "rtx.native.forward.rtTextureResolution", "128") ==
                  config::SetResult::Success,
          "Optional hardware reflection controls persist in profiles");
  changed = settings.forward;
  changed.rtReflections = false;
  require(!changed.sameTargets(settings.forward),
          "Hardware reflection enable prepares replacement resources");
  std::vector<std::string> warnings;
  require(config::set(settings, "rtx.native.forward.bakeDirectDiffuse", "0") == config::SetResult::Success &&
              !settings.forward.baking.directDiffuse,
          "Baked direct-diffuse policy participates in independent profile persistence");
  require(config::set(settings, "rtx.native.forward.bakeCache", "0") == config::SetResult::Success &&
              !settings.forward.baking.diskCache,
          "Disk bake cache preference participates in profile persistence");
  require(config::set(settings, "rtx.native.forward.rtGISamples", "0") == config::SetResult::Invalid &&
              config::set(settings, "rtx.native.forward.rtGISamples", "65") == config::SetResult::Invalid &&
              config::set(settings, "rtx.native.forward.rtGIBounces", "9") == config::SetResult::Invalid &&
              config::set(settings, "rtx.native.forward.rtGIDistance", "nan") == config::SetResult::Invalid,
          "Live GI budgets reject invalid sample counts, bounces and ranges");
  require(config::set(settings, "rtx.native.forward.rtGI", "1") == config::SetResult::Success &&
              config::set(settings, "rtx.native.forward.rtGISamples", "32") == config::SetResult::Success,
          "Live GI configuration persists independently of reflections");
  changed = settings.forward;
  changed.rtGI = false;
  require(!changed.sameTargets(settings.forward), "GI enable changes prepare replacement pipelines");
  std::string error;
  require(config::set(settings, "rtx.native.forward.shadowBias", "-0.001") == config::SetResult::Invalid &&
              config::set(settings, "rtx.native.forward.shadowBias", "nan") == config::SetResult::Invalid &&
              config::set(settings, "rtx.native.forward.shadowBias", "0.003") == config::SetResult::Success,
          "World-metric sun bias is finite, nonnegative and participates in profile persistence");
  changed = settings.forward;
  changed.shadowBias *= 2;
  require(changed.sameTargets(settings.forward), "Sun comparison bias updates reuse existing targets");
  require(config::parse(config::serialize(settings), restored, unknown, warnings, error) &&
              restored == settings,
          "Forward settings persist exactly");
  std::puts("Forward+ cluster ranges, shadows, draw ownership and configuration passed");
}
