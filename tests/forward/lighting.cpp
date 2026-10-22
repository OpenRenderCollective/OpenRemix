#include "forward/lighting.h"
#include "forward/light_response.h"
#include "forward/highlight_selection.h"
#include "forward/shadows/sun_cache.h"
#include "forward/water/data.h"
#include "forward/viewmodel/lighting.h"
#include "runtime/settings.h"
#include "scene/fixtures.h"
#include <cmath>
#include <limits>

using namespace remix;
using namespace remix::forward;
using remix::test::require;

int main() {
  {
    for (float scale : {1.f, .25f, 4.f}) {
      auto view = scene::identity();
      view[0] = scale;
      view[4] = .75f; // shear, plus nonuniform scaling and a reflection
      view[5] = -2;
      view[12] = 100;
      view[13] = -50;
      const Vec3 centre{2, -3, 4};
      const auto bounds = clusterSphereBounds(view, centre, 3, viewRadiusScale(view));
      for (int x = -4; x <= 4; ++x)
        for (int y = -4; y <= 4; ++y)
          for (int z = -4; z <= 4; ++z) {
            const auto direction = normalized(Vec3{float(x), float(y), float(z)});
            Vec3 sample;
            for (uint32_t axis = 0; axis < 3; ++axis) sample[axis] = centre[axis] + 3 * direction[axis];
            sample = transformPoint(view, sample);
            float distance2 = 0;
            for (uint32_t axis = 0; axis < 3; ++axis) distance2 += std::pow(sample[axis] - bounds[axis], 2);
            require(distance2 <= bounds[3] * bounds[3],
                    "Cluster spheres conservatively enclose translated, scaled, sheared and mirrored emitters");
          }
    }
  }
  {
    Bounds weapon, world;
    weapon.include({-.3f, -.4f, 1});
    weapon.include({.3f, .4f, 2});
    world.include({-10000, -10000, -100});
    world.include({10000, 10000, 100});
    for (const auto direction : {Vec3{0, 0, 1}, Vec3{1, 2, 3}, Vec3{0, 1, 0}}) {
      const auto projection = viewmodel::sunProjection(weapon, world, direction, 1024);
      for (uint32_t corner = 0; corner < 8; ++corner) {
        Vec3 p;
        for (uint32_t c = 0; c < 3; ++c)
          p[c] = corner & (1u << c) ? weapon.maximum[c] : weapon.minimum[c];
        const auto q = transformPoint(projection, p);
        require(std::abs(q[0]) < 1 && std::abs(q[1]) < 1 && q[2] > 0 && q[2] < 1,
                "Close-up sun cascade contains every viewmodel receiver corner");
      }
    }
    const auto close = viewmodel::sunProjection(weapon, weapon, {0, 0, 1}, 1024);
    const auto extended = viewmodel::sunProjection(weapon, world, {0, 0, 1}, 1024);
    require(close[0] == extended[0] && close[5] == extended[5],
            "A large world caster extent cannot dilute viewmodel shadow texel density");
    const auto offscreen = transformPoint(extended, {0, 0, -50});
    require(offscreen[2] > 0 && offscreen[2] < 1,
            "Close-up cascade depth retains upstream world occluders");
  }
  {
    HighlightSelection selection;
    std::array<HighlightCandidate, 2> candidates{{{20, 1}, {10, 1}}};
    auto weights = selection.update(candidates, 1, 0, 2);
    require(weights.size() == 1 && weights[0].key == 10 && weights[0].value == 0,
            "Highlight ties use captured identity and start a timed fade");
    weights = selection.update(candidates, 1, 1, 2);
    require(weights[0].value == .5f, "Highlight fade uses elapsed seconds");
    std::swap(candidates[0], candidates[1]);
    weights = selection.update(candidates, 1, .5, 2);
    require(weights[0].key == 10 && weights[0].value == .5f,
            "Reordered input and a backwards clock do not restart or reverse a fade");
    weights = selection.update(candidates, 1, 2, 2);
    require(weights[0].value == 1, "Backwards time does not extend the next interval");
    candidates[1].priority = 2; // Key 20 now wins; key 10 must fade before replacement.
    weights = selection.update(candidates, 1, 3, 2);
    require(weights.size() == 1 && weights[0].key == 10 && weights[0].value == .5f,
            "Outgoing highlights keep their identity and cannot exceed the cost budget");
    weights = selection.update(candidates, 1, 4, 2);
    require(weights.size() == 1 && weights[0].key == 20 && weights[0].value == 0,
            "A new highlight never inherits the outgoing light's weight");
    weights = selection.update(candidates, 1, 5, 2);
    require(weights[0].value == .5f, "Replacement fades in independently of shadow selection");
    require(selection.update(candidates, 0, 6, 2).empty(), "Disabling highlights enforces zero work");
    selection.reset();
    weights = selection.update(candidates, 2, 0, 0);
    require(weights.size() == 2 && weights[0].value == 1 && weights[1].value == 1,
            "Reset and zero duration allow immediate selection without shadow maps");
    candidates[0].priority = std::numeric_limits<float>::quiet_NaN();
    weights = selection.update(candidates, 2, 1, 0);
    require(weights.size() == 1 && weights[0].key == 20,
            "Invalid priorities remove stale highlight assignments");
    std::vector<HighlightCandidate> many;
    for (uint64_t i = 0; i < 400; ++i) many.push_back({i, float(i + 1)});
    weights = selection.update(many, 400, 2, 0);
    require(weights.size() == HighlightSelection::maximum,
            "Highlight selection stays bounded independently of the submitted light count");
    selection.reset();
    const std::array<HighlightCandidate, 2> duplicate{{{10, 1}, {10, 2}}};
    bool rejected = false;
    try { selection.update(duplicate, 2, 0, 0); }
    catch (const std::invalid_argument &) { rejected = true; }
    require(rejected, "Shared adapter IDs cannot silently alias distinct captured highlights");
    weights = selection.update(candidates, 1, 0, 2);
    weights = selection.update(candidates, 1, std::numeric_limits<double>::quiet_NaN(), 2);
    require(weights[0].value == 0, "A non-finite clock cannot poison highlight fades");
    weights = selection.update(candidates, 1, 2, 2);
    require(weights[0].value == 1, "Valid time resumes after a non-finite clock");
  }
  {
    LightResponseInput input;
    require(needsUnitDirect(LightResponse::Analytic, false, 0),
            "Analytic and styled diffuse retain unit-direct fields without a shadow slot");
    require(!needsUnitDirect(LightResponse::SelectedAggregate, false, 0) &&
                !needsUnitDirect(LightResponse::Aggregate, true, 0),
            "Highlight-only and fully faded aggregate responses need no diffuse correction");
    require(needsUnitDirect(LightResponse::SelectedAggregate, true, .01f) &&
                needsUnitDirect(LightResponse::Aggregate, true, 1) &&
                needsUnitDirect(LightResponse::Aggregate, true, std::numeric_limits<float>::quiet_NaN()),
            "Active mixed corrections and unknown fade values preserve unit-direct fields");
    require(!needsUnitDirect(LightResponse::BakedFallback, true, 1),
            "Uncovered-only fallback lights consume no surface correction on covered receivers");
    const uint8_t needed[] = {0, 1, 0};
    const uint32_t idleOwners[] = {0, 2}, activeOwners[] = {1}, unknownOwners[] = {20};
    require(!drawNeedsUnitDirect(idleOwners, needed) && drawNeedsUnitDirect(activeOwners, needed) &&
                drawNeedsUnitDirect(unknownOwners, needed) && drawNeedsUnitDirect(activeOwners, {}),
            "Draw eligibility keeps active or unknown ownership and skips only proven unused fields");
    require(lightResponse(input).mode == LightResponse::Analytic,
            "Uncaptured and moving lights keep analytic lighting");
    input.aggregate = true;
    input.selectedOnly = true;
    input.shadowSelected = true;
    input.shadowWeight = .4f;
    require(lightResponse(input).mode == LightResponse::BakedFallback,
            "A shadow slot cannot manufacture attributed data for a pure baked light");
    input.attributed = true;
    auto response = lightResponse(input);
    require(response.mode == LightResponse::SelectedAggregate && response.highlightWeight == .4f,
            "Selected mixed shadows retain their legacy highlight fade");
    input.independentHighlights = true;
    response = lightResponse(input);
    require(response.mode == LightResponse::SelectedAggregate && response.highlightWeight == 0,
            "A shadow-only assignment must not enable an unselected highlight");
    input.shadowSelected = false;
    input.highlightWeight = 0;
    response = lightResponse(input);
    require(response.mode == LightResponse::SelectedAggregate && response.highlightWeight == 0,
            "An incoming highlight retains cluster membership at the start of its fade");
    input.highlightWeight = .7f;
    require(lightResponse(input).highlightWeight == .7f,
            "Highlights do not require a shadow slot or inherit its strength");
    input.attributed = false;
    require(lightResponse(input).mode == LightResponse::BakedFallback,
            "Highlight selection cannot promote an emitter without attributed visibility");
    input.attributed = true;
    input.highlightWeight = -1;
    require(lightResponse(input).mode == LightResponse::BakedFallback,
            "An unselected aggregate light is excluded from the covered-receiver prefix");
    input.requireMixed = true;
    require(lightResponse(input).mode == LightResponse::Aggregate,
            "Required mixed lights remain independent of selection budgets");
    input.requireMixed = false;
    input.selectedOnly = false;
    require(lightResponse(input).mode == LightResponse::Aggregate,
            "Exhaustive comparison mode preserves every attributed response");
    input.selectedOnly = true;
    input.highlightWeight = std::numeric_limits<float>::quiet_NaN();
    require(lightResponse(input).mode == LightResponse::BakedFallback,
            "A non-finite highlight weight cannot add clustered work");
    input.highlightWeight = 2;
    require(lightResponse(input).highlightWeight == 1,
            "Selection weights cannot add more energy than the captured emitter");
  }
  {
    Options options;
    options.baking.fullyBakedLights = true; // A custom profile must survive baked mode on/off.
    options.baking.samples = 37;
    const auto requested = options.baking;
    auto expected = requested;
    expected.aggregateDiffuse = true;
    expected.fullyBakedLights = false;
    expected.bakedGeometryLights = true;
    expected.groupedStyledDirect = true;
    const auto ordinary = options;
    options.bakedLighting = true;
    require(options.bakeConfiguration(false) == expected && options.baking == requested &&
                options.selectBakedLights(false) && options.sameTargets(ordinary),
            "Baked mode derives aggregate/geometry/selected lighting without changing quality, preferences or targets");
    require(!options.selectBakedLights(true) && !options.bakedLightingActive(true),
            "Supported live GI pauses baked mode and its selected responses");
    options.baking.directDiffuse = false;
    require(!options.bakedLightingActive(false) && options.bakeConfiguration(false) == options.baking,
            "Baked mode pauses when baked direct diffuse is disabled");
    options.baking = requested;
    options.baking.enabled = false;
    require(!options.bakedLightingActive(false), "Baked mode does not implicitly enable map baking");
    options.baking = requested;
    options.bakedLighting = false;
    require(options.bakeConfiguration(false) == requested && !options.selectBakedLights(false),
            "Disabling baked mode restores the exact requested custom lighting policy");
  }
  {
    Options options;
    options.baking.aggregateDiffuse = true;
    options.baking.fullyBakedLights = true;
    options.baking.bakedGeometryLights = true;
    options.baking.samples = 37;
    options.baking.styledIndirect = true;
    const auto requested = options.baking;
    auto expected = requested;
    expected.aggregateDiffuse = false;
    expected.fullyBakedLights = false;
    expected.bakedGeometryLights = false;
    require(options.bakeConfiguration(true) == expected && options.baking == requested,
            "Live GI retains attributed visibility without changing requested bake quality or preferences");
    options.rtGI = true;
    require(options.bakeConfiguration(false) == requested,
            "Inactive or unsupported live GI preserves the requested aggregate bake policy");
  }
  require(clusterListCapacity(1, 12240, 128u << 20) == 64 &&
          clusterListCapacity(175, 12240, 128u << 20) == 256 &&
          clusterListCapacity(457, 12240, 128u << 20) == 512,
          "Adaptive lists fit sparse and dense light sets without the legacy64-light fallback");
  require(clusterListCapacity(4096, 12240, 128u << 20) == 512 &&
          clusterListCapacity(457, 12240, 128u << 20, 64) == 64 &&
          uint64_t(clusterListCapacity(457, 100000, 128u << 20) + clusterHeaderWords) * 100000 * 4 <= (32u << 20) &&
          uint64_t(clusterListCapacity(457, 12240, 4u << 20) + clusterHeaderWords) * 12240 * 4 <= (4u << 20),
          "Cluster capacity obeys total memory/device limits and retains explicit comparison overrides");
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
    require(a.water, "New Forward+ settings enable measured water");
    a.water = false;
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
  {
    shadows::SunCache stable;
    matrices.fill(scene::identity());
    stable.update(source, true, matrices, 1024);
    auto request = matrices;
    request[0][0] = std::nextafter(1.f, 2.f);
    request[1][12] += .125f / 1024 * .5f;
    request[2][14] += 1e-7f;
    dirty = stable.update(source, true, request, 1024);
    require(std::count(dirty.begin(), dirty.end(), true) == 0 && request == matrices,
            "Subtexel fit noise retains the exact projection used to render cached sunlight");
    request[0][12] += 2.f / 1024;
    request[2][14] += .001f;
    dirty = stable.update(source, true, request, 1024);
    require(dirty[0] && dirty[2] && std::count(dirty.begin(), dirty.end(), true) == 2,
            "Pixel-scale camera movement and meaningful depth changes still redraw their layers");
    dirty = stable.update(source, true, request, 2048);
    require(std::count(dirty.begin(), dirty.end(), true) == cascadeCount,
            "Changed resolution invalidates the texel-dependent projection cache");
    request[0][3] = 1e-9f;
    dirty = stable.update(source, true, request, 2048);
    require(dirty[0], "A non-affine projection cannot use orthographic reuse bounds");
    request[1] = scene::identity();
    request[1][0] = 0;
    dirty = stable.update(source, true, request, 2048);
    require(dirty[1], "A singular projection rejects reuse without throwing");
  }
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
  light.radius *= .0254f;
  require(std::abs(influenceRadius(light, .01f) - 2 * radius * .0254f) < 1e-5f,
          "Changing length units scales emitter influence without changing radiance");
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
  {
    shadows::SunCache fittedCache;
    auto fitted = shadowCascades(identity, identity, {.001f, 1, 1}, {.3f, .7f, 1}, bounds, 1, 2048).matrices;
    fittedCache.update(source, true, fitted, 2048);
    auto request = fitted;
    for (auto &projection : request) {
      // Projection-space perturbations preserve the real rotated light basis
      // and different cascade extents, unlike an identity-only cache fixture.
      Matrix noise = scene::identity();
      noise[12] = .125f / 2048 * .25f;
      noise[14] = 1e-7f;
      projection = scene::multiply(noise, projection);
    }
    dirty = fittedCache.update(source, true, request, 2048);
    require(std::count(dirty.begin(), dirty.end(), true) == 0 && request == fitted,
            "Rotated fitted cascades retain their exact rendered matrices under subtexel fit noise");
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
    require(acceptsSurfaceLayer(paint) && !accepts(paint),
            "Explicit static terrain paint receives lighting without becoming an opaque caster");
    paint.terrain = false;
    require(acceptsSurfaceLayer(paint), "Static source-over decals receive surface lighting");
    paint.terrain = true;
    paint.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
    require(!acceptsSurfaceLayer(paint), "Dynamic decals retain compatibility ownership until receiver tracking is available");
    paint.classification = REMIXAPI_INSTANCE_STATIC_EXT;
    paint.blend.dst = 1;
    require(!acceptsSurfaceLayer(paint), "Additive layers do not become terrain source-over paint");
  }
  draw.blend.enabled = false;
  draw.cutGroup = 1;
  require(!accepts(draw), "Mask cut stays in compatibility raster");
  draw.cutGroup = 0;
  draw.viewModel = true;
  require(!accepts(draw), "Viewmodel camera is not world lighting");
  config::Settings settings;
  require(settings.forward.autoExposure, "Forward+ runtime profiles enable automatic exposure by default");
  require(settings.forward.prism && settings.forward.gamma22,
          "Forward+ defaults to the shared path-traced Prism and gamma display transform");
  const auto beforeExposure = settings.forward;
  require(config::set(settings, "rtx.native.forward.prism", "false") == config::SetResult::Success &&
              config::set(settings, "rtx.native.forward.gamma22", "false") == config::SetResult::Success &&
              beforeExposure.sameTargets(settings.forward) && beforeExposure.baking == settings.forward.baking,
          "Display transform controls update without replacing targets or rebaking lighting");
  require(config::set(settings, "rtx.native.forward.autoExposure", "false") == config::SetResult::Success &&
              config::set(settings, "rtx.native.forward.autoExposureTarget", "0.24") == config::SetResult::Success &&
              config::set(settings, "rtx.native.forward.autoExposureSpeed", "3") == config::SetResult::Success &&
              config::set(settings, "rtx.native.forward.autoExposureMinEv", "-25") == config::SetResult::Invalid &&
              config::set(settings, "rtx.native.forward.autoExposureMaxEv", "25") == config::SetResult::Invalid &&
              beforeExposure.sameTargets(settings.forward) && beforeExposure.baking == settings.forward.baking,
          "Exposure controls validate and update without replacing targets or invalidating map lighting");
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
  require(Options{}.ssrScale == 1, "SSR defaults to full resolution");
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
  require(config::set(settings, "rtx.native.forward.reflectionProbes", "65") == config::SetResult::Invalid &&
              config::set(settings, "rtx.native.forward.reflectionResolution", "48") ==
                  config::SetResult::Invalid,
          "Reflection probe limits and power-of-two dimensions are enforced");
  require(config::set(settings, "rtx.native.forward.reflectionProbes", "64") == config::SetResult::Success &&
              config::set(settings, "rtx.native.forward.reflectionResolution", "64") ==
                  config::SetResult::Success,
          "Reflection capture settings participate in profile persistence");
  require(config::set(settings, "rtx.native.forward.reflectionResolution", "256") == config::SetResult::Success &&
              settings.forward.baking.reflectionResolution == 256 &&
              config::set(settings, "rtx.native.forward.reflectionResolution", "512") == config::SetResult::Success &&
              settings.forward.baking.reflectionResolution == 512 &&
              config::set(settings, "rtx.native.forward.reflectionResolution", "1024") == config::SetResult::Invalid &&
              config::set(settings, "rtx.native.forward.reflectionResolution", "64") == config::SetResult::Success,
          "HDR reflection image settings expose 256/512 while retaining a bounded supported range");
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
  require(config::set(settings, "rtx.native.forward.localShadowFade", "nan") == config::SetResult::Invalid &&
          config::set(settings, "rtx.native.forward.localShadowFade", "-1") == config::SetResult::Invalid &&
          config::set(settings, "rtx.native.forward.localShadowFade", "11") == config::SetResult::Invalid &&
          config::set(settings, "rtx.native.forward.localShadowFade", "2") == config::SetResult::Success,
          "Shadow fade duration is finite and bounded");
  changed = settings.forward;
  changed.localShadowFade = 0;
  require(changed.sameTargets(settings.forward), "Fade edits reuse shadow targets");
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
  require(config::set(settings, "rtx.native.forward.experimentalBakedHighlightLights", "0") ==
              config::SetResult::Success && settings.forward.bakedHighlightLights == 0 &&
              config::set(settings, "rtx.native.forward.experimentalBakedHighlightLights", "32") ==
                  config::SetResult::Success && settings.forward.bakedHighlightLights == 32 &&
              config::set(settings, "rtx.native.forward.experimentalBakedHighlightFade", "1.5") ==
                  config::SetResult::Success && settings.forward.bakedHighlightFade == 1.5f,
          "Highlight budget and duration persist independently of shadow settings");
  require(config::Settings{}.forward.bakedLighting && config::Settings{}.forward.baking.styledIndirect &&
              config::Settings{}.forward.baking.styleStorageMiB == 256 &&
              config::set(settings, "rtx.native.forward.experimentalBakedLightingPreview", "1") ==
                  config::SetResult::Success && settings.forward.bakedLighting &&
              config::set(settings, "rtx.native.forward.experimentalBakedLightingPreview", "0") ==
                  config::SetResult::Success && !settings.forward.bakedLighting,
          "Baked lighting defaults on in runtime profiles and the legacy key remains reversible");
  {
    config::Settings migrated;
    config::UnknownSettings extra;
    std::vector<std::string> notices;
    std::string problem;
    require(config::parse("profile.version=1\nrtx.native.forward.experimentalBakedLightingPreview=0\n"
                           "rtx.native.forward.styledIndirect=0\nrtx.native.forward.styleStorageMiB=128\n",
                           migrated, extra, notices, problem) && migrated.forward.bakedLighting &&
                migrated.forward.baking.styledIndirect && migrated.forward.baking.styleStorageMiB == 256 && !notices.empty(),
            "Old preview-off defaults migrate to completed baked lighting");
    require(config::parse("profile.version=1\nrtx.native.forward.bakedLighting=0\n"
                           "rtx.native.forward.experimentalBakedLightingPreview=1\n",
                           migrated, extra, notices, problem) && !migrated.forward.bakedLighting,
            "Canonical analytic opt-out wins over the legacy preview alias regardless of order");
    const auto saved = config::serialize(migrated);
    require(saved.find("experimentalBakedLightingPreview") == std::string::npos &&
                saved.find("rtx.native.forward.bakedLighting = 0") != std::string::npos,
            "Profiles save one canonical runtime policy and preserve explicit opt-out");
  }
  require(!settings.forward.selectedBakedLights &&
              config::set(settings, "rtx.native.forward.experimentalSelectedBakedLights", "1") ==
                  config::SetResult::Success && settings.forward.selectedBakedLights &&
              config::set(settings, "rtx.native.forward.experimentalSelectedBakedLights", "0") ==
                  config::SetResult::Success && !settings.forward.selectedBakedLights,
          "Selected baked response is an explicit reversible runtime profile preference");
  require(!settings.forward.baking.aggregateDiffuse &&
              config::set(settings, "rtx.native.forward.experimentalAggregateDiffuse", "1") ==
                  config::SetResult::Success && settings.forward.baking.aggregateDiffuse &&
              config::set(settings, "rtx.native.forward.experimentalAggregateDiffuse", "0") ==
                  config::SetResult::Success && !settings.forward.baking.aggregateDiffuse,
          "Aggregate map experiments require an explicit profile opt-in and can return to the default path");
  require(!settings.forward.baking.fullyBakedLights &&
              config::set(settings, "rtx.native.forward.experimentalFullyBakedLights", "1") ==
                  config::SetResult::Success && settings.forward.baking.fullyBakedLights &&
              config::set(settings, "rtx.native.forward.experimentalFullyBakedLights", "0") ==
                  config::SetResult::Success && !settings.forward.baking.fullyBakedLights,
          "Fully baked lighting policy is explicit and leaves the default mixed path unchanged");
  require(!settings.forward.baking.bakedGeometryLights &&
              config::set(settings, "rtx.native.forward.experimentalBakedGeometryLights", "1") ==
                  config::SetResult::Success && settings.forward.baking.bakedGeometryLights &&
              config::set(settings, "rtx.native.forward.experimentalBakedGeometryLights", "0") ==
                  config::SetResult::Success && !settings.forward.baking.bakedGeometryLights,
          "Baked geometry selection is an independent opt-in with a reversible default");
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
  require(remix::forward::bake::Config{}.denoiseLightmaps,
          "New bake configurations request indirect lightmap denoising by default");
  require(settings.forward.debugView == 0 &&
              config::set(settings, "rtx.native.forward.debugView", "7") == config::SetResult::Invalid &&
              config::set(settings, "rtx.native.forward.debugView", "6") == config::SetResult::Success &&
              config::set(settings, "rtx.native.forward.debugView", "4") == config::SetResult::Success &&
              config::set(settings, "rtx.native.forward.debugView", "5") == config::SetResult::Success &&
              config::set(settings, "rtx.native.forward.debugView", "2") == config::SetResult::Success,
          "Lighting diagnostics default off and reject unknown modes");
  changed = settings.forward;
  changed.debugView = 1;
  require(!changed.sameTargets(settings.forward) && changed.baking == settings.forward.baking,
          "Lighting views prepare specialized pipelines with identical bake inputs");
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
  require(config::set(settings, "rtx.native.forward.water", "0") == config::SetResult::Success &&
              config::parse(config::serialize(settings), restored, unknown, warnings, error) &&
              !restored.forward.water,
          "An explicitly saved compatibility-water preference survives the new default");
  std::puts("Forward+ cluster ranges, shadows, draw ownership and configuration passed");
}
