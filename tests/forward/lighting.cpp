#include "forward/lighting.h"
#include "forward/provider_plan.h"
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
    const auto defaults = config::Settings{}.forward;
    auto specialized = defaults;
    specialized.bicubicLightmaps = !defaults.bicubicLightmaps;
    require(!defaults.sameTargets(specialized) && defaults.baking == specialized.baking,
            "Lightmap filtering changes rebuild specialized pipelines without invalidating bakes");
    specialized = defaults;
    specialized.depthPrepass = !defaults.depthPrepass;
    require(!defaults.sameTargets(specialized) && defaults.baking == specialized.baking,
            "Depth-prepass changes rebuild shader and depth-write policy together");
    for (auto view : {LightingView::BakedIrradiance, LightingView::Albedo,
                      LightingView::IrradianceWithoutNormalMaps, LightingView::BakeCoverage}) {
      auto diagnostic = defaults;
      diagnostic.debugView = uint32_t(view);
      diagnostic.baking.reflectionProbes = 0;
      auto captured = diagnostic;
      captured.baking.reflectionProbes = 1;
      require(diagnostic.sameTargets(captured) &&
                  !techniques::resolveResources(captured, {}).reflectionResolve,
              "Diagnostics without reflectance retain their targets when capture is enabled");
    }
    for (auto direct : {techniques::Direct::Analytic, techniques::Direct::AttributedBake,
                        techniques::Direct::AggregateBake})
      for (auto diffuse : {techniques::Diffuse::SurfaceAndProbes, techniques::Diffuse::RayQueries,
                           techniques::Diffuse::Disabled, techniques::Diffuse::RayIncoming})
        for (auto reflection : {techniques::Reflections::Disabled, techniques::Reflections::Cubemaps,
                                techniques::Reflections::RayQueries, techniques::Reflections::RayQueriesAndCubemaps}) {
          auto options = defaults;
          options.samples = 8;
          options.baking.samples = 512;
          options.baking.reflectionResolution = 256;
          options.baking.reflectionProbes = 7;
          options.baking.reflectionDetailTolerance = diffuse == techniques::Diffuse::Disabled ? 0 : .75f;
          options.authoredReflectionFallback = direct != techniques::Direct::Analytic;
          const techniques::Selection requested{direct, diffuse, reflection, false, false, true};
          techniques::apply(options, requested);
          require(techniques::requested(options) == requested,
                  "Technique selections round-trip through existing saved options");
          require(options.samples == 8 && options.baking.samples == 512 &&
                      options.baking.reflectionResolution == 256,
                  "Technique selection preserves quality settings");
          for (uint32_t support = 0; support < 8; ++support) {
            const auto before = options;
            const techniques::Capabilities capabilities{bool(support & 1), bool(support & 2), bool(support & 4)};
            const auto resources = techniques::resolveResources(options, capabilities);
            require(options == before, "Resolving device dependencies preserves requested techniques and quality");
            require(resources.rayDiffuse() == (options.rtGI && capabilities.rayQueries && capabilities.fragmentGI &&
                        (!options.water || capabilities.waterRayQueries)) &&
                        resources.rayReflections == (options.rtReflections && capabilities.rayQueries),
                    "Supported requested providers activate, while unsupported ray effects remain unallocated");
            require(!resources.rayScene || capabilities.rayQueries,
                    "Unsupported hardware cannot allocate a live ray scene");
            require(resources.rayScene == (resources.rayDiffuse() || resources.rayReflections),
                    "Diffuse and reflection providers share exactly one requested live scene");
            require(!resources.rayDiffuse() || (capabilities.fragmentGI && options.rtGI),
                    "Fragment GI pipelines require support and an explicit diffuse request");
            require(!resources.rayWater || (resources.rayReflections && options.water && capabilities.waterRayQueries),
                    "Water continuation requires its reflection provider and water capability");
            const auto &capturePolicy = resources.capturePolicy;
            require(capturePolicy.automaticCount == options.baking.reflectionProbes &&
                        capturePolicy.detailTolerance == options.baking.reflectionDetailTolerance &&
                        capturePolicy.authoredFallback == options.authoredReflectionFallback,
                    "Capture policy carries resolved ownership and capture-only selection settings");
            const auto &lighting = resources.receiverLighting;
            require(lighting.baking.samples == 512 && resources.captureConfig.samples == 512 &&
                        resources.captureConfig.reflectionResolution == 256 &&
                        resources.captureConfig.enabled == (options.baking.reflectionProbes != 0),
                    "Resolved producer inputs preserve quality independently of ownership and device fallback");
            require(resources.sharedCaptures() == (lighting.baking.enabled && !resources.liveDiffuse()),
                    "Capture source and shared transport admission follow the resolved provider, including fallback");
            auto disabledCaptures = options;
            disabledCaptures.baking.reflectionProbes = 0;
            require(techniques::resolveResources(disabledCaptures, capabilities).capturePolicy.transport == resources.capturePolicy.transport,
                    "Disabling images preserves the capture retirement owner");
            require(resources.composesBakedDirect() == (resources.liveDiffuse() && lighting.bakedReceivers),
                    "Live incoming composes only with an explicitly selected baked direct field");
            require(!resources.composesBakedDirect() ||
                        (lighting.baking.irradianceContents == bake::IrradianceContents::ReceiverDirect &&
                         lighting.baking.bounces == 1 && !lighting.baking.styledIndirect &&
                         !lighting.baking.reflectionProbes && lighting.selectedMixedLights),
                    "Composed resources require direct-only transport and independent capture ownership");
            if (resources.rayDiffuse())
              require(resources.diffuse == (lighting.bakedReceivers ? techniques::Diffuse::RayIncoming
                                                                    : techniques::Diffuse::RayQueries),
                      "Resolved diffuse technique identifies the live shader family");
            else
              require(resources.diffuse == (options.baking.enabled ? techniques::Diffuse::SurfaceAndProbes
                                                                   : techniques::Diffuse::Disabled),
                      "Unavailable live GI retains the configured baked or disabled fallback");
            require(resources.screenReflections == options.ssr,
                    "Resolved screen source selection is independent of ray and baked capabilities");
            auto resolveCapabilities = capabilities;
            resolveCapabilities.forceReflectionResolve = true;
            const auto resolve = techniques::resolveResources(options, resolveCapabilities);
            const bool inlineCubes = options.baking.reflectionProbes != 0 && !resources.screenReflections &&
                                     !resources.rayReflections && !resources.liveDiffuse();
            require(!resolve.inlineReflections &&
                        resolve.reflectionResolve == (options.ssr || resources.rayReflections || options.baking.reflectionProbes != 0),
                    "Only supported reflection sources allocate guides and resolve");
            require(resources.inlineReflections == inlineCubes &&
                        resources.reflectionResolve == (resolve.reflectionResolve && !inlineCubes) &&
                        resources.capturePolicy == resolve.capturePolicy,
                    "Inline cubemaps replace only the resolve and retain the selected capture provider");
          }
          const auto live = options.lightingPlan(options.rtGI);
          require(!options.rtGI || !live.bakedReceivers ||
                      live.baking.irradianceContents == bake::IrradianceContents::ReceiverDirect,
                  "Live incoming never consumes combined baked irradiance");
        }
    auto opaqueLive = defaults;
    opaqueLive.rtGI = true;
    opaqueLive.water = false;
    const techniques::Capabilities opaqueOnly{true, true, false};
    require(techniques::resolveResources(opaqueLive, opaqueOnly).rayDiffuse(),
            "Opaque live diffuse does not require an inactive water pass's descriptor capability");
    opaqueLive.water = true;
    require(techniques::resolveResources(opaqueLive, opaqueOnly).bakedDiffuse(),
            "Unsupported enabled live-water dependencies retain the configured baked fallback");
    auto rayOnly = defaults;
    rayOnly.ssr = false;
    rayOnly.rtReflections = true;
    rayOnly.baking.reflectionProbes = 0;
    auto cubeFallback = rayOnly;
    cubeFallback.baking.reflectionProbes = 1;
    require(!rayOnly.sameTargets(cubeFallback),
            "Cube fallback changes prepare targets even when unsupported rays were requested");
    auto screenRay = rayOnly;
    screenRay.ssr = true;
    auto screenCube = cubeFallback;
    screenCube.ssr = true;
    require(screenRay.sameTargets(screenCube), "SSR already supplies the reflection attachment boundary");
    cubeFallback.baking.reflectionProbes = 2;
    auto cubeBudget = cubeFallback;
    cubeBudget.baking.reflectionProbes = 3;
    require(cubeFallback.sameTargets(cubeBudget), "Nonzero cube budget edits preserve target resources");
    require(defaults == presets::source2() && presets::source2Name == "Source 2" &&
                !defaults.ssr && !defaults.rtGI && !defaults.rtReflections &&
                defaults.baking.reflectionProbes > 0 && defaults.lightingPlan(false).bakedReceivers,
            "Runtime Source 2 defaults select baked lighting, probe volumes and cubemap reflections");
    auto quality = defaults;
    quality.samples = 1;
    quality.ssrScale = 2;
    quality.shadowResolution = 256;
    quality.baking.samples = 97;
    quality.baking.reflectionProbes = 7;
    quality.baking.reflectionResolution = 256;
    quality.baking.lightmapTexels = 123456;
    quality.baking.diskCache = false;
    quality.baking.modelLightmaps = false;
    quality.baking.lightmapFitBudget = false;
    quality.exposure = 3;
    quality.authoredReflectionFallback = false;
    require(presets::isSource2(quality), "Quality, display and authored-volume choices do not change technique identity");
    auto custom = quality;
    custom.rtGI = custom.rtReflections = true;
    custom.ssr = true;
    custom.shadows = custom.localShadows = false;
    custom.bakedLighting = custom.baking.enabled = custom.baking.directDiffuse = false;
    custom.baking.styledIndirect = false;
    require(!presets::isSource2(custom), "Changing techniques derives Custom without separate stored preset state");
    presets::applySource2(custom);
    require(custom == quality, "Selecting Source 2 restores techniques while preserving all quality and user preferences");
    custom.baking.reflectionProbes = 0;
    require(!presets::isSource2(custom), "Disabled cubemaps do not match the Source 2 preset");
    presets::applySource2(custom);
    require(custom.baking.reflectionProbes == bake::Config{}.reflectionProbes && presets::isSource2(custom),
            "Selecting Source 2 enables disabled cubemaps with a valid default budget");
    for (bool rayGi : {false, true}) {
      config::Settings profile, restored;
      profile.forward = quality;
      profile.forward.rtGI = rayGi;
      config::UnknownSettings unknown;
      std::vector<std::string> warnings;
      std::string error;
      require(config::parse(config::serialize(profile), restored, unknown, warnings, error) &&
                  restored.forward == profile.forward && presets::isSource2(restored.forward) == !rayGi,
              "Saved component settings restore both Source 2 and Custom identity without a preset key");
    }
    {
      config::Settings profile, restored;
      profile.forward = quality;
      techniques::selectDiffuse(profile.forward, techniques::Diffuse::RayIncoming);
      config::UnknownSettings unknown;
      std::vector<std::string> warnings;
      std::string error;
      require(config::parse(config::serialize(profile), restored, unknown, warnings, error) &&
                  restored.forward == profile.forward &&
                  techniques::requested(restored.forward).diffuse == techniques::Diffuse::RayIncoming,
              "Explicit direct/incoming composition persists without changing legacy live-GI requests");
      presets::applySource2(restored.forward);
      require(!restored.forward.bakedDirectWithLiveGI && presets::isSource2(restored.forward),
              "Source 2 resets explicit composition while retaining the normal baked preset");
    }
    for (uint32_t combination = 0; combination < 64; ++combination) {
      bake::Config requested;
      requested.enabled = (combination & 1) != 0;
      requested.directDiffuse = (combination & 2) != 0;
      requested.samples = 37;
      requested.reflectionResolution = 256;
      const auto original = requested;
      const bool live = (combination & 4) != 0;
      const lighting::Request request{(combination & 8) != 0, (combination & 16) != 0, (combination & 32) != 0};
      const auto plan = lighting::resolve(requested, request, live);
      require(requested == original && plan.baking.samples == 37 &&
                  plan.baking.reflectionResolution == (live && plan.bakedReceivers ? 8u : 256u),
              "Resolving techniques preserves requested settings and independent quality budgets");
      if (live && request.composeBakedDirect && request.baked && requested.enabled && requested.directDiffuse)
        require(plan.baking.enabled && plan.bakedReceivers && plan.selectedMixedLights &&
                    plan.baking.irradianceContents == bake::IrradianceContents::ReceiverDirect &&
                    plan.baking.bounces == 1 && !plan.baking.styledIndirect && !plan.baking.reflectionProbes,
                "Selected baked direct composes independently with live incoming across request combinations");
      else if (live)
        require(!plan.baking.enabled && !plan.bakedReceivers && !plan.selectedMixedLights && !plan.baking.aggregateDiffuse &&
                    !plan.baking.groupedStyledDirect && !plan.baking.bakedGeometryLights,
                "Live GI replaces aggregate ownership and selected baked responses for every request combination");
      if (!requested.enabled || !requested.directDiffuse)
        require(!plan.bakedReceivers, "Unavailable baked transport cannot claim receiver ownership");
      if (!live && request.selectedMixedLights)
        require(plan.selectedMixedLights, "Explicit mixed-light selection remains available independently of baked mode");
    }
  }
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
    Bounds pose, jittered;
    pose.include({1200.371f, -813.27f, 1});
    pose.include({1201.013f, -812.4f, 2});
    jittered.include({1200.3711f, -813.2701f, 1});
    jittered.include({1201.0129f, -812.3999f, 2});
    const auto stable = viewmodel::sunProjection(pose, world, {0, 0, 1}, 1024);
    require(stable == viewmodel::sunProjection(jittered, world, {0, 0, 1}, 1024),
            "Subtexel viewmodel pose jitter preserves the fitted sun projection");
    const auto lower = transformPoint(stable, jittered.minimum);
    const auto upper = transformPoint(stable, jittered.maximum);
    for (uint32_t axis = 0; axis < 2; ++axis)
      require(std::max(std::abs(lower[axis]), std::abs(upper[axis])) <= 1 - 4.f / 1024,
              "Snapped viewmodel projection retains padding for the shadow footprint");
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
    require(options.lightingPlan(false).baking == expected && options.baking == requested &&
                options.lightingPlan(false).selectedMixedLights && options.sameTargets(ordinary),
            "Baked mode derives aggregate/geometry/selected lighting without changing quality, preferences or targets");
    require(!options.lightingPlan(true).bakedReceivers && !options.lightingPlan(true).selectedMixedLights,
            "Legacy live GI retains whole-field replacement for saved profiles");
    options.bakedDirectWithLiveGI = true;
    auto composed = expected;
    composed.irradianceContents = bake::IrradianceContents::ReceiverDirect;
    composed.bounces = 1;
    composed.styledIndirect = false;
    composed.reflectionProbes = 0;
    composed.reflectionResolution = 8;
    composed.reflectionDetailTolerance = 0;
    require(options.lightingPlan(true).selectedMixedLights && options.lightingPlan(true).bakedReceivers &&
                options.lightingPlan(true).baking == composed && options.baking == requested,
            "Live incoming preserves baked direct ownership while excluding baked indirect/captures");
    auto liveTargets = options;
    liveTargets.rtGI = true;
    auto analyticTargets = liveTargets;
    analyticTargets.bakedLighting = false;
    require(!liveTargets.sameTargets(analyticTargets),
            "Changing composed versus analytic direct ownership rebuilds specialized live pipelines");
    options.baking.directDiffuse = false;
    require(!options.lightingPlan(false).bakedReceivers && options.lightingPlan(false).baking == options.baking,
            "Baked mode pauses when baked direct diffuse is disabled");
    options.baking = requested;
    options.baking.enabled = false;
    require(!options.lightingPlan(false).bakedReceivers, "Baked mode does not implicitly enable map baking");
    options.baking = requested;
    options.bakedLighting = false;
    require(options.lightingPlan(false).baking == requested && !options.lightingPlan(false).selectedMixedLights,
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
    expected.enabled = false;
    expected.aggregateDiffuse = false;
    expected.fullyBakedLights = false;
    expected.bakedGeometryLights = false;
    require(options.lightingPlan(true).baking == expected && options.baking == requested,
            "Live GI stops unused bake work without changing requested bake quality or preferences");
    options.rtGI = true;
    require(options.lightingPlan(false).baking == requested,
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
  source = std::make_shared<scene::BakeScene>();
  dirty = sunCache.update(source, false, matrices);
  require(std::count(dirty.begin(), dirty.end(), true) == cascadeCount,
          "Replacing a disabled sun source still initializes every layer");
  dirty = sunCache.update(source, false, matrices);
  require(std::count(dirty.begin(), dirty.end(), true) == 0,
          "A replaced disabled source needs no repeated initialization");
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
  const ClipFrustum frustum(identity);
  require(intersectsClip({}, identity) && frustum.intersects({}), "Missing bounds must not remove a caster");
  Bounds enclosing{{-2, -2, -2}, {2, 2, 2}, true};
  require(intersectsClip(enclosing, identity), "A box enclosing the clip volume survives without inside corners");
  const auto unchangedBounds = transformBounds(enclosing, identity);
  require(unchangedBounds.valid && unchangedBounds.minimum == enclosing.minimum &&
              unchangedBounds.maximum == enclosing.maximum && !transformBounds({}, identity).valid,
          "Identity geometry preserves exact world bounds while unknown bounds stay unknown");
  Matrix translated = identity;
  translated[12] = 3;
  const auto translatedBounds = transformBounds(enclosing, translated);
  require(translatedBounds.minimum == Vec3{1, -2, -2} && translatedBounds.maximum == Vec3{5, 2, 2},
          "Translation shifts both bounds without changing their extent");
  const float largest = std::numeric_limits<float>::max();
  translated[12] = largest;
  const auto overflowBounds = transformBounds({{0, 0, 0}, {largest, 1, 1}, true}, translated);
  require(overflowBounds.valid && overflowBounds.minimum == Vec3{largest, 0, 0} &&
              overflowBounds.maximum == Vec3{largest, 1, 1},
          "An overflowing translation retains finite corners");
  const Bounds asymmetric{{-1.25f, .31f, -3.2f}, {4.4f, 2.7f, .125f}, true};
  const auto checkCornerBounds = [&](const Matrix &model) {
    Bounds reference;
    for (uint32_t corner = 0; corner < 8; ++corner) {
      const Vec3 point{corner & 1u ? asymmetric.maximum[0] : asymmetric.minimum[0],
                       corner & 2u ? asymmetric.maximum[1] : asymmetric.minimum[1],
                       corner & 4u ? asymmetric.maximum[2] : asymmetric.minimum[2]};
      reference.include(transformPoint(model, point));
    }
    const auto result = transformBounds(asymmetric, model);
    require(result.valid == reference.valid && result.minimum == reference.minimum &&
                result.maximum == reference.maximum,
            "Fast bounds match the corner path, including mirroring, collapse and overflow");
  };
  for (float scale : {-3.1f, 0.f, .73f, 2.8f, largest}) {
    auto model = identity;
    model[0] = scale;
    model[5] = -.4f;
    model[10] = 1.1f;
    model[12] = .37f;
    checkCornerBounds(model);
    model[1] = .71f;
    model[4] = -.36f;
    model[8] = .93f;
    checkCornerBounds(model);
    model[3] = .025f;
    checkCornerBounds(model);
  }
  for (uint32_t axis = 0; axis < 3; ++axis)
    for (float side : {-1.f, 1.f}) {
      Bounds outside{{-.1f, -.1f, .4f}, {.1f, .1f, .6f}, true};
      outside.minimum[axis] = side < 0 ? -3.f : 2.f;
      outside.maximum[axis] = side < 0 ? -2.f : 3.f;
      require(!intersectsClip(outside, identity) && !frustum.intersects(outside),
              "Each Vulkan clip plane rejects a separated caster");
      outside.minimum[axis] = side < 0 ? -2.f : 1.f;
      outside.maximum[axis] = side < 0 ? (axis == 2 ? 0.f : -1.f) : 2.f;
      require(intersectsClip(outside, identity) && frustum.intersects(outside),
              "Casters touching a clip plane must survive");
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
  for (float outer : {-.5f, 0.f, .5f, .9999f}) {
    require(std::abs(coneInnerCosine(outer, 0) - outer) < 2e-6f &&
                coneInnerCosine(outer, 1) == 1,
            "Spotlight softness endpoints preserve the outer angle and full angular transition");
    require(std::abs(coneInnerCosine(outer, .5f) - std::sqrt((1 + outer) * .5f)) < 2e-6f,
            "Half softness follows the half-angle identity rather than a cosine-space blend");
    require(coneInnerCosine(outer, -1) == coneInnerCosine(outer, 0) &&
                coneInnerCosine(outer, 2) == coneInnerCosine(outer, 1),
            "Prepared softness retains the shader's clamping convention");
  }
  require(coneInnerCosine(-1, .5f) == -1, "Unrestricted point lights need no angular conversion");
  const float radius = influenceRadius(light, .01f);
  light.radiance[0] = light.radiance[1] = light.radiance[2] = 4;
  require(std::abs(influenceRadius(light, .01f) - 2 * radius) < 1e-4f,
          "Influence radius follows inverse square power");
  light.forwardCutoff = radius;
  const auto bounded = localLightRange(light, .01f);
  require(bounded.influence == radius && std::abs(bounded.fade - 2 * radius) < 1e-4f,
          "Explicit cutoff shortens admission without moving the intensity fade");
  light.forwardCutoff = 0;
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
    for (float depthSign : {-1.f, 1.f}) {
      // Keep a fixed lens while turning and translating the camera. A tight
      // light-space AABB breathes under rotation even when its centre is snapped.
      Matrix lens{};
      lens[0] = .75f; lens[5] = 1.25f;
      lens[10] = depthSign * 200.f / 199.9f; lens[11] = depthSign;
      lens[14] = -.1f * 200.f / 199.9f;
      const DepthRange range{.1f, 200, depthSign};
      std::array<float, cascadeCount> reference{};
      for (uint32_t pose = 0; pose < 8; ++pose) {
        const float angle = pose * .23f;
        Matrix view = scene::identity();
        view[0] = view[10] = std::cos(angle);
        view[2] = -std::sin(angle); view[8] = std::sin(angle);
        view[12] = pose * .031f; view[13] = -float(pose) * .017f;
        const auto camera = scene::multiply(lens, view);
        const auto fitted = shadowCascades(scene::inverse(camera), view, range, {.3f, .7f, 1}, bounds, 100, 2048);
        for (uint32_t c = 0; c < cascadeCount; ++c) {
          const auto &m = fitted.matrices[c];
          const float scale = std::sqrt(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]);
          if (!pose) reference[c] = scale;
          require(std::abs(scale - reference[c]) <= reference[c] * 1e-5f,
                  "Turning a fixed-lens camera preserves cascade texel size");
          for (uint32_t axis = 0; axis < 2; ++axis) {
            const float texelOrigin = m[12 + axis] * 1024;
            require(std::abs(texelOrigin - std::round(texelOrigin)) < .001f,
                    "Sun cascade translation stays on the world-anchored texel lattice");
          }
          const auto inverse = scene::inverse(camera);
          for (float x : {-1.f, 1.f}) for (float y : {-1.f, 1.f}) {
            const auto nearPoint = transformPoint(inverse, {x, y, 0});
            const auto farPoint = transformPoint(inverse, {x, y, .9999f});
            const float az = transformPoint(view, nearPoint)[2] * depthSign, bz = transformPoint(view, farPoint)[2] * depthSign;
            for (float depth : {c ? fitted.splits[c - 1] : range.nearPlane, fitted.splits[c]}) {
              Vec3 p;
              for (uint32_t axis = 0; axis < 3; ++axis)
                p[axis] = nearPoint[axis] + (farPoint[axis] - nearPoint[axis]) * ((depth - az) / (bz - az));
              const auto q = transformPoint(m, p);
              require(std::abs(q[0]) <= 1.0001f && std::abs(q[1]) <= 1.0001f,
                      "Stable sun cascades retain every receiver-frustum corner");
            }
          }
        }
      }
    }
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
  require(settings.forward.baking.modelLightmaps && !bake::Config{}.modelLightmaps,
          "Runtime profiles lightmap static meshes while low-level baking retains explicit eligibility control");
  const auto withModels = settings.forward.baking;
  require(config::set(settings, "rtx.native.forward.lightmapModels", "0") == config::SetResult::Success &&
              !settings.forward.baking.modelLightmaps &&
              !settings.forward.baking.sameLighting(withModels),
          "Disabling static mesh lightmaps changes the bake policy");
  require(config::set(settings, "rtx.native.forward.lightmapModels", "1") == config::SetResult::Success &&
              config::set(settings, "rtx.native.forward.lightmapModels", "2") == config::SetResult::Invalid,
          "Static mesh lightmap settings persist with boolean validation");
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
              !beforeExposure.sameTargets(settings.forward) && beforeExposure.baking == settings.forward.baking,
          "Automatic exposure validates controls and replaces its guide targets without invalidating map lighting");
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
  require(Options{}.authoredReflectionFallback,
          "Automatic reflections fill areas outside authored volumes by default");
  const auto beforeFallback = settings.forward.baking;
  require(config::set(settings, "rtx.native.forward.authoredReflectionFallback", "true") == config::SetResult::Success &&
              settings.forward.authoredReflectionFallback && settings.forward.baking.sameLighting(beforeFallback),
          "Automatic reflection fallback can change without invalidating the light bake");
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
              config::set(settings, "rtx.native.forward.bakeProbeSamples", "8193") == config::SetResult::Invalid &&
              config::set(settings, "rtx.native.forward.bakeTextureResolution", "48") ==
                  config::SetResult::Invalid,
          "Reject unsupported bake budgets and atlas dimensions");
  require(config::set(settings, "rtx.native.forward.bake", "0") == config::SetResult::Success &&
              config::set(settings, "rtx.native.forward.bakeSpacing", "3.5") == config::SetResult::Success &&
              config::set(settings, "rtx.native.forward.bakeSamples", "512") == config::SetResult::Success,
          "Bake controls participate in runtime configuration");
  require(config::set(settings, "rtx.native.forward.bakeProbeSamples", "1024") == config::SetResult::Success &&
              settings.forward.baking.probeSampleCount() == 1024 && settings.forward.baking.samples == 512,
          "Volume probe quality changes independently of surface quality");
  require(config::set(settings, "rtx.native.forward.bakeProbeSamples", "0") == config::SetResult::Success &&
              settings.forward.baking.probeSampleCount() == 512,
          "Zero probe sample override inherits the existing surface budget");
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
  require(config::set(settings, "rtx.native.forward.bakedHighlightLights", "0") ==
              config::SetResult::Success && settings.forward.bakedHighlightLights == 0 &&
              config::set(settings, "rtx.native.forward.bakedHighlightLights", "32") ==
                  config::SetResult::Success && settings.forward.bakedHighlightLights == 32 &&
              config::set(settings, "rtx.native.forward.bakedHighlightFade", "1.5") ==
                  config::SetResult::Success && settings.forward.bakedHighlightFade == 1.5f,
          "Highlight budget and duration persist independently of shadow settings");
  require(config::Settings{}.forward.bakedLighting && config::Settings{}.forward.baking.styledIndirect &&
              config::Settings{}.forward.baking.styleStorageMiB == 512 &&
              config::set(settings, "rtx.native.forward.bakedLighting", "1") ==
                  config::SetResult::Success && settings.forward.bakedLighting &&
              config::set(settings, "rtx.native.forward.bakedLighting", "0") ==
                  config::SetResult::Success && !settings.forward.bakedLighting,
          "Baked lighting defaults on and can be disabled explicitly");
  {
    config::Settings parsed;
    config::UnknownSettings extra;
    std::vector<std::string> notices;
    std::string problem;
    require(config::parse("profile.version=1\nrtx.native.forward.bakedLighting=0\n"
                           "rtx.native.forward.styledIndirect=0\nrtx.native.forward.styleStorageMiB=128\n",
                           parsed, extra, notices, problem) && !parsed.forward.bakedLighting &&
                !parsed.forward.baking.styledIndirect && parsed.forward.baking.styleStorageMiB == 128,
            "Profiles preserve explicit baked lighting and style choices");
    require(config::serialize(parsed).find("rtx.native.forward.bakedLighting = 0") != std::string::npos,
            "Profiles save the explicit baked lighting opt-out");
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
              config::set(settings, "rtx.native.forward.debugView", "8") == config::SetResult::Invalid &&
              config::set(settings, "rtx.native.forward.debugView", "7") == config::SetResult::Success &&
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
