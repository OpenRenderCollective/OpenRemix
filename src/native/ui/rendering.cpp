#include "runtime/runtime.h"
#include "forward/lightmap/data.h"
#include "forward/lightmap/denoise.h"
#include "ui/widgets.h"
#include "pathtrace/block_costs.h"
#include "pathtrace/sparse_policy.h"
#include <iterator>
#include "upscale/ngx.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <utility>

namespace remix {
  void RemixNativeRuntime::drawRenderingTab(config::Settings &settings) {
    auto &options = settings.trace;
    const bool rrActive =
        m_pathTracer && m_pathTracer->upscalingStatistics().backend == upscale::Backend::DlssRr;
    int backend = int(settings.backend);
    if (ui::Combo("Requested backend", &backend, "Raster\0Path trace\0Forward+ (experimental)\0"))
      settings.backend = uint32_t(backend);
    ImGui::TextWrapped("Active backend: %s", config::Backend::name(m_backend));
    if (!m_forwardFailure.empty()) ImGui::TextWrapped("%s", m_forwardFailure.c_str());
    if (!m_traceUnavailable.empty())
      ImGui::TextWrapped("Path tracing unavailable: %s", m_traceUnavailable.c_str());
    ui::Checkbox("VSync", &settings.vsync);
    ui::Checkbox("Asynchronous present", &settings.asyncPresent);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Present captures the frame and returns; OpenRemix's render thread prepares, records,\n"
                        "submits and presents it while the game starts its next frame. At most one frame is\n"
                        "in flight: the next Present waits only for that handoff. Statistics describe the last\n"
                        "completed frame. Off: the whole frame runs inside Present, as before.\n"
                        "Applies at the next Present (the render thread is drained first).");
    if (settings.asyncPresent && m_headless)
      ImGui::TextDisabled("%s", "The headless editor always presents synchronously.");
    if (settings.backend == config::Backend::ForwardPlus) {
      const auto statistics = forwardStatistics();
      ImGui::Text("Active MSAA: %ux | opaque draws: %u | lights: %u | clusters: %u",
                  statistics.samples, statistics.draws, statistics.lights, statistics.clusters);
      ImGui::TextDisabled("Local light lists: %u runtime | %u baked fallback | %u build candidates",
                          statistics.clusteredLights, statistics.fullyBakedLights, statistics.clusterBuildLights);
      ImGui::TextDisabled("Selected baked roles: %u highlights | %u mixed shadows",
                          statistics.selectedHighlightLights, statistics.selectedMixedShadowLights);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Counts overlap when a light has both roles. Positive response weights only;\n"
                          "pending fades can also retain clustered entries.");
      auto &forward = settings.forward;
      int lightingView = int(forward.debugView);
      if (ui::Combo("Lighting view", &lightingView,
                    "Shaded\0Baked irradiance (neutral)\0Analytic direct diffuse\0Direct specular\0"
                    "Albedo (unlit)\0Baked irradiance (no normal maps)\0Bake coverage\0Total diffuse\0"))
        forward.debugView = uint32_t(lightingView);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Separates opaque-world lighting using the current bake.\n"
                          "Neutral baked irradiance includes aggregate direct lighting, excluding material colour, emission and reflections.\n"
                          "Analytic diffuse excludes direct light already in the bake. Albedo excludes lighting.\n"
                          "Total diffuse retains material colour, baked and live diffuse, excluding emission and specular.\n"
                          "Unmapped irradiance excludes normal maps. Direct views exclude screen-space and probe reflections.\n"
                          "Bake coverage: blue = surface lightmap, green = valid probe, red = uncovered opaque receiver.\n"
                          "Does not rebake.");
      int samples = forward.samples == 4 ? 2 : forward.samples == 2 ? 1 : 0;
      if (ui::Combo("MSAA", &samples, "Off\0 2x\0 4x\0")) forward.samples = 1u << samples;
      ui::Checkbox("Sun shadows", &forward.shadows);
      ui::Checkbox("Local light shadows", &forward.localShadows);
      int shadowLights = int(forward.localShadowLights);
      if (ui::SliderInt("Shadowed local lights", &shadowLights, 0, 8))
        forward.localShadowLights = uint32_t(shadowLights);
      int shadowResolution = 0;
      while ((64u << shadowResolution) < forward.localShadowResolution) ++shadowResolution;
      if (ui::Combo("Local shadow face size", &shadowResolution, "64\0 128\0 256\0 512\0 1024\0"))
        forward.localShadowResolution = 64u << shadowResolution;
      ui::DragFloat("Local shadow distance (m)", &forward.localShadowDistance, 1, .1f, 10000);
      ImGui::Text("Local shadows: %u | unshadowed fallback: %u | static map updates: %u",
                  statistics.shadowedLights, statistics.shadowOverflow, statistics.staticShadowUpdates);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Each selected light has cached static geometry and a separate live caster map.\n"
                          "Lights beyond the budget keep illuminating the scene without local shadow maps.");
      ImGui::Text("Sun static cascade updates: %u | captured casters: %u | live candidates: %u",
                  statistics.staticSunUpdates, statistics.capturedCasters, statistics.liveCasters);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Captured geometry is excluded from moving-caster maps.\n"
                          "Frozen depth maps are fallback shadows for receivers without a matching lightmap.\n"
                          "Static sun cascades are reused until their projection or frozen scene changes.");
      ImGui::Text("Frozen caster draws prepared this frame: %u", statistics.staticCasterPreparations);
      ImGui::Text("Geometry uploads: %llu created, %llu reused | spare buffers %.2f MiB",
                  static_cast<unsigned long long>(statistics.rigidUploadCreates),
                  static_cast<unsigned long long>(statistics.rigidUploadReuses),
                  statistics.spareGeometryBytes / 1048576.0);
      ui::Checkbox("Screen-space reflections", &forward.ssr);
      ui::Checkbox("Water refraction and reflections", &forward.water);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Uses the material's tint, absorption and refractive index.\n"
                          "Disabling restores compatibility water rendering.");
      ui::Checkbox("Hardware ray-traced GI", &forward.rtGI);
      if (forward.rtGI) {
        if (!statistics.rayGI.available)
          ImGui::TextWrapped("Fragment ray queries unavailable; using baked indirect lighting.");
        else ImGui::TextWrapped("%s", statistics.rayGI.status);
        int samples = int(forward.rtGISamples), bounces = int(forward.rtGIBounces);
        if (ui::SliderInt("GI rays per shaded pixel", &samples, 1, 64)) forward.rtGISamples = uint32_t(samples);
        if (ui::SliderInt("GI secondary bounces", &bounces, 1, 8)) forward.rtGIBounces = uint32_t(bounces);
        ui::DragFloat("GI ray distance (m)", &forward.rtGIDistance, 1, .1f, 10000);
      }
      ui::Checkbox("Hardware ray-traced reflections", &forward.rtReflections);
      if (forward.rtReflections) {
        if (!statistics.rayReflections.available)
          ImGui::TextWrapped("Hardware ray queries unavailable; using SSR and reflection probes.");
        else if (statistics.rayReflections.status[0]) {
          ImGui::TextWrapped("%s", statistics.rayReflections.status);
          ImGui::Text("Reflection ray slots: %u | scene builds: %u", statistics.rayReflections.rays,
                      statistics.rayReflections.sceneBuilds);
        }
        ui::DragFloat("Hardware reflection distance (m)", &forward.rtReflectionDistance, 1, .1f, 10000);
        int bounces = int(forward.rtReflectionBounces);
        if (ui::SliderInt("Hardware reflection bounces", &bounces, 1, 8))
          forward.rtReflectionBounces = uint32_t(bounces);
      }
      ui::Checkbox("Bake static lighting on map load", &forward.baking.enabled);
      ui::Checkbox("Cache baked lighting on disk", &forward.baking.diskCache);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Reuses finished lighting when map inputs, materials, quality and GPU match.\n"
                          "Cache changes affect subsequent map loads; disabling keeps the active lighting.");
      ui::Checkbox("Bake per-light direct diffuse", &forward.baking.directDiffuse);
      ui::Checkbox("Use baked lighting", &forward.bakedLighting);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Uses baked diffuse for fixed lights and reduces their per-frame work.\n"
                          "Lightmaps and probes provide diffuse; selected highlights and moving shadows stay live.\n"
                          "Switching can prepare another bake; disabling restores your previous policy.");
      if (forward.bakedLighting) {
        ImGui::TextWrapped("Fixed-light diffuse comes from lightmaps and probes. "
                           "Other reflections still use the enabled reflection effects.");
        if (!forward.baking.enabled)
          ImGui::TextWrapped("Enable map baking to use baked lighting.");
        else if (!forward.baking.directDiffuse)
          ImGui::TextWrapped("Enable baked direct diffuse to use baked lighting.");
        else if (forward.rtGI && statistics.rayGI.available)
          ImGui::TextWrapped("Live ray-traced GI uses its own lighting policy. Baked lighting resumes when GI is off.");
      }
      ImGui::BeginDisabled(!remix::forward::lightmap::denoiserVersion());
      ui::Checkbox("Denoise indirect lightmaps", &forward.baking.denoiseLightmaps);
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Filters baked indirect light while preserving direct-light and shadow masks.\n"
                          "Adds a filtering stage to new bakes; cached lighting reuses the result.\n"
                          "Requires an Open Image Denoise enabled build; otherwise raw lighting is retained.");
      ui::DragFloat("Bake GPU budget (ms)", &forward.baking.gpuBudgetMs, .25f, 0, 16);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Adjusts batch size from completed GPU timings. Complex batches can exceed the target.\n"
                          "Higher values finish first bakes faster and can reduce frame rate while baking.\n"
                          "Zero uses the fixed bakeBatch setting. Scheduling edits preserve baked lighting.");
      ui::DragFloat("Bake probe spacing (m)", &forward.baking.spacing, .1f, .1f, 100);
      if (statistics.irradianceGrid.slots)
        ImGui::Text("Active probe grid: %.2f m, %u / %u usable", statistics.irradianceGrid.spacingMetres,
                    statistics.irradianceGrid.usable, statistics.irradianceGrid.slots);
      ui::Checkbox("Antialias baked sunlight edges", &forward.baking.antialiasSun);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Refines detected static sun-shadow edges across each luxel's surface area.\n"
                          "Adds a bounded sampling pass to new bakes; cached lighting reuses the result.");
      ui::Checkbox("Fit surface detail to atlas budget", &forward.baking.lightmapFitBudget);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Selects the finest consistent surface density that fits the atlas.\n"
                          "Larger budgets add detail, including on small maps.");
      if (!forward.baking.lightmapFitBudget)
        ui::DragFloat("Manual surface luxel size (m)", &forward.baking.lightmapLuxelSize, .01f, .01f, 16);
      int surfaceBudget = int(forward.baking.lightmapTexels / 1024);
      if (ui::SliderInt("Surface lightmap budget (k texels)", &surfaceBudget, 64, 16384))
        forward.baking.lightmapTexels = uint32_t(surfaceBudget) * 1024;
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("More texels preserve smaller baked lighting details. Automatic density fills this budget.");
      ImGui::Text("Surface atlas memory limit: %.1f MiB",
                  double(forward.baking.lightmapTexels) * remix::forward::lightmap::atlasBytesPerTexel / (1024 * 1024));
      int surfacePage = 0;
      while ((64u << surfacePage) < forward.baking.lightmapPageSize) ++surfacePage;
      if (ui::Combo("Maximum surface atlas page size", &surfacePage, "64\0 128\0 256\0 512\0 1024\0 2048\0 4096\0"))
        forward.baking.lightmapPageSize = 64u << surfacePage;
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Larger pages let large faces retain finer luxels. Pages shrink if one would exceed the texel budget.\n"
                          "Smaller pages allocate memory in smaller increments.");
      int probeCount = int(forward.baking.reflectionProbes);
      if (ui::SliderInt("Reflection probes", &probeCount, 0, int(remix::forward::probes::maxProbes)))
        forward.baking.reflectionProbes = uint32_t(probeCount);
      const auto activeProbes = statistics.reflectionProbes.parameters[0];
      ui::Checkbox("Automatic fallback outside authored volumes", &forward.authoredReflectionFallback);
      ImGui::Text("Active reflection probes: %u (%s)", activeProbes,
                  statistics.authoredReflections.active ?
                    (statistics.authoredReflections.automatic ? "authored + automatic" : "authored") : "automatic");
      if (statistics.authoredReflections.active && activeProbes && !forward.authoredReflectionFallback)
        ImGui::TextWrapped("Authored volumes replace the automatic set for this map. "
                           "Areas outside these volumes have no local cubemap. "
                           "Clear the host's override to restore automatic probes.");
      if (statistics.authoredReflections.active && forward.authoredReflectionFallback && activeProbes)
        ImGui::Text("Automatic fallback: %u retained, %u covered, %u omitted by capacity",
                    statistics.authoredReflections.automatic, statistics.authoredReflections.covered,
                    statistics.authoredReflections.omitted);
      if (statistics.authoredReflections.active && !activeProbes)
        ImGui::TextWrapped("The authored volume set is empty, so local cubemaps are disabled. "
                           "Clear the host's override to use automatic probes.");
      ui::DragFloat("Reflection trim tolerance (m)", &forward.baking.reflectionDetailTolerance, .01f, 0, 2);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Allows small bevels and interior clutter in fitted reflection boxes. Zero uses strict empty space.\n"
                          "Thin walls remain barriers; this is an approximate room fit.");
      int probeResolution = 0;
      while ((8u << probeResolution) < forward.baking.reflectionResolution) ++probeResolution;
      if (ui::Combo("Cubemap face size", &probeResolution, "8\0 16\0 32\0 64\0 128\0 256\0 512\0"))
        forward.baking.reflectionResolution = 8u << probeResolution;
      if (ImGui::IsItemHovered()) {
        const uint64_t size = forward.baking.reflectionResolution;
        // Six faces, their full mip chain, and eight bytes per HDR texel.
        const uint64_t perCapture = 6 * (4*size*size-1) / 3 * 8;
        ImGui::SetTooltip("Sharper faces require more memory and capture work.\n"
                          "Image texels at this capture budget: about %.1f MiB.",
                          double(perCapture * forward.baking.reflectionProbes) / (1024*1024));
      }
      ImGui::TextWrapped("%s", statistics.baking.status[0] ? statistics.baking.status : "Waiting for static map inputs");
      if (statistics.baking.probes)
        ImGui::Text("Bake probes: %u / %u", statistics.baking.completed, statistics.baking.probes);
      int ssrScale = forward.ssrScale == 4 ? 2 : forward.ssrScale == 2 ? 1 : 0;
      if (ui::Combo("SSR resolution", &ssrScale, "Full\0Half\0Quarter\0"))
        forward.ssrScale = 1u << ssrScale;
      ui::DragFloat("Shadow distance (m)", &forward.shadowDistance, 1, 1, 10000);
      ui::DragFloat("Scene scale (m per unit)", &forward.sceneScale, .0005f, .0001f, 1000, "%.4f", ImGuiSliderFlags_Logarithmic);
      ui::DragFloat("Ambient fill", &forward.ambient, .001f, 0, 100);
      ui::DragFloat("Exposure", &forward.exposure, .01f, .0001f, 10000);
      ImGui::TextWrapped("Experimental Forward+ renderer with mixed static and dynamic local shadows. Map baking supplies static indirect light, soft static shadows and "
                         "box-projected reflections. Transparency and game effects use compatibility rasterization.");
      return;
    }
    // World units of the host: converts the tracer's meter-based defaults.
    ui::DragFloat("Scene scale (m per unit)", &options.sceneScale, .0005f, .0001f, 1000, "%.4f",
                  ImGuiSliderFlags_Logarithmic);
    options.sceneScale = std::clamp(options.sceneScale, .0001f, 1000.f);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Meters per world unit (1 for meter-scale hosts, 0.0254 for Source inches).\n"
                        "Converts the denoiser's hit distances and the visibility-guide cell size.");
    if (ImGui::CollapsingHeader("Sampling and visibility", ImGuiTreeNodeFlags_DefaultOpen)) {
      int primary = options.rasterPrimary ? 0 : 1;
      if (ui::Combo("Primary visibility", &primary, "Rasterized G-buffer\0Ray traced\0"))
        options.rasterPrimary = primary == 0;
      int samples = int(options.samples), bounces = int(options.bounces);
      if (ui::SliderInt("Samples / pixel", &samples, 1, 16))
        options.samples = uint32_t(samples);
      if (ui::SliderInt("Maximum indirect bounces", &bounces, 0, 16))
        options.bounces = uint32_t(bounces);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Maximum traced indirect surface interactions per sample.\nMisses and cache hits "
                          "can end tracing earlier; SHARC may supply a longer cached tail.");
      ui::Checkbox("Adaptive indirect termination", &options.russianRoulette);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Probabilistically stop low-contribution paths and compensate survivors.\nMay "
                          "reduce tracing work but increase noise; the maximum bounce limit always applies.");
      ImGui::BeginDisabled(!options.russianRoulette);
      int minimumBounces = int(options.rouletteMinBounces);
      if (ui::SliderInt("Bounces before adaptive termination", &minimumBounces, 1, 16))
        options.rouletteMinBounces = uint32_t(minimumBounces);
      ImGui::EndDisabled();
      if (options.russianRoulette && options.rouletteMinBounces >= options.bounces)
        ImGui::TextWrapped("%s", "Adaptive termination inactive: minimum reaches the maximum bounce limit.");
      if (options.russianRoulette && m_pathTracer && !m_pathTracer->adaptiveTerminationAvailable())
        ImGui::TextWrapped("%s", "Adaptive termination unavailable: using fixed-limit tracing.");
      ui::Checkbox("Denoise translucent surfaces", &options.translucencyDenoiser);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Filters single-layer surface lighting with its own motion and depth.\n"
                         "Uses NRD, including before DLSS RR; overlapping layers retain their existing rendering.");
      if (options.translucencyDenoiser)
        ImGui::TextDisabled("Surface filter: %s", m_pathTracer && m_pathTracer->translucencyDenoiserActive()
                                               ? "active" : "inactive (requires NRD and translucent surfaces)");
      ui::Checkbox("Shadow and bounce rays ignore blended surfaces", &options.skipBlendedSecondaryRays);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Glass, sprites and other blended layers stay visible to the camera, but cast no\n"
                          "shadows and do not tint light passing through them. Cheaper wherever rays cross them.");
      ui::Checkbox("Path-trace view models", &options.viewModel);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("First-person view models (VIEW_MODEL camera instances) are path-traced in the\n"
                          "world camera's view, always in front of the scene, instead of rasterized with\n"
                          "their own camera. They shadow themselves but not the world, let the player's\n"
                          "light through and stay out of mirrors.");
      ImGui::BeginDisabled(!options.viewModel);
      ui::SliderFloat("View model scale", &options.viewModelScale, .05f, 1, "%.2f");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Shrinks view models toward the eye: the same image, nearer, so their own shadow\n"
                          "and bounce rays start inside the player's space instead of in a wall they reach\n"
                          "into. 1: their true size.");
      ImGui::EndDisabled();
      ui::Checkbox("Primary surface replacement (mirrors)", &options.primarySurfaceReplacement);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Smooth metal and smooth black surfaces show the surface they reflect as if it were\n"
                          "directly visible: full direct lighting and denoising instead of a noisy reflection.\n"
                          "Requires raster primary visibility and a denoiser.");
      ui::Checkbox("Trace indirect light from translucent layers", &options.translucentIndirect);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Off: each transparent layer takes the denoised indirect light of the surface behind it,\n"
                          "and deeper layers reuse the front layer's direct light (much cheaper).\n"
                          "On: every layer traces its own lighting paths.");
      ImGui::BeginDisabled(options.translucentIndirect);
      ui::Checkbox("Local lights on glossy translucent layers", &options.sheetLocalLights);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Samples torches and other local lights at water and glossy glass surfaces.\n"
                          "Off: sun and sky only there; reflections still show locally lit surroundings.");
      ImGui::EndDisabled();
      ui::Checkbox("Deeper translucent layers reuse the front layer's light", &options.layerReuse);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Layers behind the first translucent layer of a pixel (the back of a glass pane,\n"
                          "a window seen through another) take its light instead of sampling lights and\n"
                          "tracing paths of their own: one lit layer per pixel. Off: every layer is lit.");
      ui::Checkbox("Short reflection paths on translucent layers", &options.shortSheetPaths);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Paths from glass and other translucent layers end at their first hit, lit there by\n"
                          "the radiance cache or by its direct light. Off: they bounce on like an opaque\n"
                          "surface's paths (deeper bounces in glass reflections, much slower).");
      ImGui::BeginDisabled(rrActive || options.indirectClamp > 0);
      ui::Checkbox("Reuse opaque lighting through translucency", &options.translucencyReuse);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Uses filtered lighting at closely matching opaque ray hits.\n"
                         "Requires NRD; unmatched surfaces and raw references keep full tracing.\n"
                         "Optional: its performance benefit depends on the scene.");
      ui::Checkbox("Reuse water indirect samples", &options.waterIndirectReuse);
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Shares diffuse lighting across small flat water patches with TAA or FSR.\nEdges, "
                          "overlapping layers and raw reference rendering retain full sampling.");
      ImGui::BeginDisabled(rrActive || options.localDirectClamp > 0 || options.indirectClamp > 0);
      ui::Checkbox("Experimental PBR lighting reuse", &options.pbrLocalReuse);
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Reuses local lighting on closely matching rough surfaces.\nMay change lighting "
                          "and temporal noise; disabled by default.");
    }
    if (ImGui::CollapsingHeader("Reconstruction", ImGuiTreeNodeFlags_DefaultOpen)) {
      int upscaler = int(options.upscaler);
      if (ui::Combo("Requested upscaler", &upscaler, "Off\0Auto\0FSR 3.1\0DLSS\0"))
        options.upscaler = upscale::Mode(upscaler);
      const auto fsr = m_pathTracer ? m_pathTracer->upscalingStatistics() : upscale::Statistics{};
      if (options.upscaler != upscale::Mode::Off) {
        const bool amd = options.upscaler == upscale::Mode::Fsr31;
        auto &savedQuality = amd ? options.fsrQuality : options.dlssQuality;
        int quality = int(savedQuality);
        if (ui::Combo("Quality preset", &quality,
                      "Native AA / DLAA\0Quality\0Balanced\0Performance\0Ultra Performance\0"))
          savedQuality = uint32_t(quality);
        if (amd)
          ui::SliderFloat("FSR sharpening", &options.fsrSharpness, 0.f, 1.f, "%.2f");
        else {
          ui::Checkbox("DLSS Ray Reconstruction", &options.dlssRayReconstruction);
          if (options.dlssRayReconstruction) {
            int model = int(options.dlssRayReconstructionModel);
            if (ui::Combo("Ray Reconstruction model", &model, "Transformer\0CNN\0"))
              options.dlssRayReconstructionModel = uint32_t(model);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Transformer: the current model (DLSS 310.9.1), sharper reflections and detail.\n"
                                "CNN: the earlier convolutional model (DLSS 310.2.1), softer, about half the\n"
                                "cost on RTX 20 series GPUs. Changing it restarts DLSS (not while DLSS frame\n"
                                "generation runs).");
            ui::Checkbox("Choose the model preset", &options.dlssRayReconstructionPresetOverride);
            if (options.dlssRayReconstructionPresetOverride) {
              // NGX presets 1-3 (A-C) are the CNN library's, 4-6 (D-F) the transformer's.
              const bool cnn = options.dlssRayReconstructionModel == 1;
              const uint32_t first = cnn ? 1 : 4;
              int preset = int(options.dlssRayReconstructionPreset) - int(first);
              if (preset < 0 || preset > 2)
                preset = cnn ? 2 : 1;
              if (ui::Combo("Model preset", &preset, cnn ? "A\0B\0C\0" : "D\0E\0F (experimental)\0") ||
                  options.dlssRayReconstructionPreset < first || options.dlssRayReconstructionPreset > first + 2)
                options.dlssRayReconstructionPreset = first + uint32_t(preset);
              if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", cnn ? "A-C: the CNN presets of DLSS 310.2.1; C is the default."
                                            : "E: the latest transformer (the default). D: the first transformer.\n"
                                              "F: DLSS 310.9.1's new default; Vulkan validation reports hazards\n"
                                              "in it on Linux 615 drivers.");
            }
          }
        }
      }
      if (rrActive) {
        ui::Checkbox("Sparse rendering", &options.sparseRendering);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Lights only a blue-noise selection of the pixels each frame and lets Ray Reconstruction\n"
                            "rebuild the rest. Faster at high render resolutions; turns itself off where the\n"
                            "render resolution is too low for it to pay.");
        if (options.sparseRendering) {
          ui::Checkbox("Sparse rate follows render resolution", &options.sparseAdaptiveRate);
          if (options.sparseAdaptiveRate) {
            // The presets' rate and exponent are tuned as pairs; other values
            // come from the configuration only.
            const int count = int(std::size(trace::sparsePresets));
            int preset = count;
            for (int i = 0; i < count; ++i)
              if (std::abs(options.sparseSamplingRate - trace::sparsePresets[i].samplingRate) < 1e-4f &&
                  std::abs(options.sparseResolutionExponent - trace::sparsePresets[i].resolutionExponent) < 1e-4f)
                preset = i;
            if (ui::Combo("Sparse preset", &preset, "Ultra\0High\0Medium\0Low\0Custom (configuration)\0") &&
                preset < count) {
              options.sparseSamplingRate = trace::sparsePresets[preset].samplingRate;
              options.sparseResolutionExponent = trace::sparsePresets[preset].resolutionExponent;
            }
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("How many pixels are lit at each render resolution: fewer from Ultra to Low.");
            ui::SliderFloat("Sparse quality", &options.sparseQuality, 0.f, 1.f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Fine adjustment of the preset: 0.5 leaves it, higher lights more pixels.");
          } else {
            ui::SliderFloat("Sparse sampling rate", &options.sparseSamplingRate, 1.f / 16, 1.f, "%.3f");
          }
          const auto sparse = trace::g_sparseState;
          if (sparse.launched)
            ImGui::TextWrapped("Sparse rendering: %.1f%% of the pixels lit each frame", sparse.rate * 100);
          else
            ImGui::TextWrapped("Sparse rendering: off (%s)", sparse.reason);
        }
      }
      ImGui::TextWrapped("Active: %s", upscale::name(fsr.backend));
      if (fsr.backend == upscale::Backend::DlssRr)
        ImGui::TextWrapped("Ray Reconstruction: %s, preset %c", fsr.rrCnn ? "CNN (DLSS 310.2.1)" : "transformer",
                           fsr.rrPreset >= 1 && fsr.rrPreset <= 6 ? char('A' + fsr.rrPreset - 1) : '-');
      if (!fsr.rrNote.empty())
        ImGui::TextWrapped("%s", fsr.rrNote.c_str());
      ImGui::TextWrapped("%ux%u -> %ux%u | reconstruction %.2f ms", fsr.render.width, fsr.render.height,
                         fsr.display.width, fsr.display.height, fsr.milliseconds);
      if (!fsr.failure.empty())
        ImGui::TextWrapped("%s", fsr.failure.c_str());
      if (rrActive)
        ImGui::TextWrapped("Ray Reconstruction manages denoising and independent sampling. Saved settings "
                           "resume when it is disabled.");
      ImGui::BeginDisabled(!m_lowLatencySupported || settings.frameGeneration);
      bool lowLatency = m_lowLatencySupported && (settings.lowLatency || settings.frameGeneration);
      if (ui::Checkbox("NVIDIA Reflex low latency", &lowLatency))
        settings.lowLatency = lowLatency;
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", !m_lowLatencySupported
                                    ? "Needs an NVIDIA GPU and driver with VK_NV_low_latency2, and the path tracer."
                                    : settings.frameGeneration
                                          ? "Always on with frame generation."
                                          : "The driver holds the game back so that frames are not queued ahead of\n"
                                            "the GPU: input reaches the screen sooner when the GPU is the limit.");
      ui::Checkbox("Frame generation", &settings.frameGeneration);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Shows generated frames between each pair of rendered frames: DLSS Frame Generation\n"
                          "(2x to 4x, a supported RTX GPU) or FSR 3.1 frame generation (2x, any GPU).\n"
                          "Needs the path tracer and an upscaler (DLSS or FSR; their native-resolution preset\n"
                          "counts). Adds latency: the rendered frame is shown after its generated ones.");
      if (settings.frameGeneration) {
        int backend = int(settings.frameGenerationBackend);
        if (ui::Combo("Frame generation backend", &backend, "Auto\0DLSS\0FSR 3.1\0"))
          settings.frameGenerationBackend = upscale::FrameGenerationMode(backend);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Auto: DLSS Frame Generation where the GPU supports it, FSR 3.1 otherwise.");
        // FSR generates one frame whatever is asked.
        ImGui::BeginDisabled(settings.frameGenerationBackend == upscale::FrameGenerationMode::Fsr31);
        int frames = int(settings.frameGenerationFrames) - 1;
        if (ui::Combo("Generated frames", &frames, "2x (one generated)\0003x (two generated)\0004x (three generated)\0"))
          settings.frameGenerationFrames = uint32_t(frames) + 1;
        ImGui::EndDisabled();
      }
      if (settings.frameGeneration)
        ImGui::TextWrapped("Frame generation: %s | generated %llu, shown twice instead %llu, rendered frames short of "
                           "generated ones (no free swapchain image) %llu | rendered frame interval %.1f ms (%.0f fps "
                           "rendered) | swapchain: %zu images, %s",
                           m_frameGenerationStatus.empty() ? "applies at the next frame" : m_frameGenerationStatus.c_str(),
                           (unsigned long long)m_generatedFrames, (unsigned long long)m_duplicatedFrames,
                           (unsigned long long)m_ungeneratedFrames, m_renderedInterval,
                           m_renderedInterval > 0 ? 1000.0 / m_renderedInterval : 0.0, m_swapchainImages.size(),
                           m_presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR  ? "immediate"
                           : m_presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox"
                           : m_presentMode == VK_PRESENT_MODE_FIFO_KHR    ? "FIFO (vsync)"
                                                                          : "other present mode");
      else if (m_ngx) {
        const auto ngx = m_ngx->capabilities();
        if (ngx.initializationComplete && !ngx.fg)
          ImGui::TextWrapped("DLSS Frame Generation unavailable: %s", ngx.fgReason.c_str());
      }
    }
    if (ImGui::CollapsingHeader("Memory")) {
      ui::Checkbox("Keep BC textures compressed", &settings.compressedTextures);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Upload BC1-BC7 textures as GPU BC images instead of decoding them to RGBA on the CPU.\n"
                          "Uses 4-8x less VRAM and system memory and removes the decode from CreateTexture.\n"
                          "Applies to textures created after the change; no restart needed.");
      if (settings.compressedTextures && !m_textureCompression)
        ImGui::TextWrapped("%s", "This GPU cannot sample BC formats: textures are decoded once at upload.");
      ui::Checkbox("Report VRAM statistics", &settings.vramStatistics);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Answer GetVramStats with OpenRemix's texture, geometry, acceleration-structure\n"
                          "and denoiser allocations, plus the driver's device-local usage and budget.");
      const auto memory = renderer().memoryStatistics();
      ImGui::TextDisabled("Textures: %llu, %.1f MiB (%.1f MiB BC) | geometry %.1f MiB",
                          (unsigned long long)memory.textureCount, memory.textureBytes / 1048576.0,
                          memory.compressedTextureBytes / 1048576.0, memory.geometryBytes / 1048576.0);
    }
    // Shown to a host that uses host passes (remixapi_GetHostPassesEnabledEXT).
    if (m_hostPasses.asked() && ImGui::CollapsingHeader("Game", ImGuiTreeNodeFlags_DefaultOpen)) {
      ui::Checkbox("Pass-through game shaders (experimental)", &settings.hostPasses);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Replays the game's screen-space passes (post-processing effects, addon shaders)\n"
                          "on the path-traced frame with the game's own shaders, before the HUD. Off: the\n"
                          "game's shader passes are not replayed and the frame is composed without them.\n"
                          "Applies from the next frame.");
    }
    if (ImGui::CollapsingHeader("Geometry")) {
      ui::Checkbox("Refresh mesh geometry in place", &settings.refreshGeometry);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Honour remixapi_MeshInfoDeformEXT: a host refreshing a mesh's vertices\n"
                          "(face morphs, particles) keeps its identity, so the path tracer refits the\n"
                          "existing BLAS and motion vectors follow the surface. Off: the refresh replaces\n"
                          "the mesh (full BLAS build). Applies to refreshes after the change.");
      ui::Checkbox("Persistent static scene", &settings.persistentStatic);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Static scene instances (DrawSceneInstancesEXT) stay in the path tracer's own\n"
                          "copy, updated only where they change: unchanged ones cost no per-frame work in\n"
                          "the draw list, scene preparation, surface table, temporal history or scene\n"
                          "signature. The same output as without it. Path tracing only; applies from the\n"
                          "next frame.");
      ImGui::BeginDisabled(!settings.persistentStatic);
      ui::Checkbox("Validate persistent static scene", &settings.persistentStaticValidation);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Diagnostic: every frame, rebuild the kept static tables from scratch and compare\n"
                          "them. The first difference is logged and shown in Diagnostics > Present CPU\n"
                          "breakdown. Costs about what the setting saves.");
      ImGui::EndDisabled();
    }
    // remixapi_InstanceInfoCameraCullingEXT: what the camera does not see of
    // instances that ask; lighting is the same either way.
    if (ImGui::CollapsingHeader("Camera culling")) {
      ui::Checkbox("Hide back faces from the camera", &options.cameraBackFaceCulling);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Instances the host marks for it (a map's walls, floors and props) show the camera\n"
                          "their front faces only, as the game's rasterizer does. Shadows, bounces and\n"
                          "reflections still meet both sides, so no light leaks.\n"
                          "rtx.native.pathtrace.cameraBackFaceCulling");
      ui::Checkbox("Hide what the camera's cluster cannot see", &options.cameraVisibilityCulling);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Marked instances are not shown where they lie in a cluster of the host's visibility\n"
                          "partition that is not potentially visible from the camera's (the game's PVS).\n"
                          "Outside the world everything is shown. Lighting is unchanged.\n"
                          "rtx.native.pathtrace.cameraVisibilityCulling");
    }
    // Path tracing GPU cost; each is off by default and renders the same image.
    if (ImGui::CollapsingHeader("GPU optimizations")) {
      ui::Checkbox("Hardware face culling", &options.hardwareFaceCulling);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("One-sided surfaces without alpha test or blending become hardware-opaque in their\n"
                          "acceleration structures, and ray traversal rejects the back faces of one-sided\n"
                          "instances instead of the shader. Same image. Applies at the next frame: changed\n"
                          "meshes rebuild their BLAS (both variants stay in memory until a mesh is released).\n"
                          "rtx.native.pathtrace.hardwareFaceCulling");
      ui::Checkbox("Combined acceleration structure", &options.combinedTlas);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Static and dynamic instances share one top-level acceleration structure, so every\n"
                          "ray traverses once instead of twice. Moving instances refit it; static changes and\n"
                          "added or removed instances rebuild it over all instances (\"TLAS builds\" in the\n"
                          "GPU pass timings). Same image. Applies at the next frame.\n"
                          "rtx.native.pathtrace.combinedTlas");
      ui::Checkbox("Shadow ray early-out", &options.shadowEarlyOut);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Shadow, sky and local-light visibility rays stop at the first blocker instead of\n"
                          "searching for the nearest hit; translucent sheets on the way are still multiplied\n"
                          "in, as before. SIGMA's sun shadow ray keeps its nearest-blocker search. Same image.\n"
                          "Applies at the next frame (the specialized shaders compile in the background).\n"
                          "rtx.native.pathtrace.shadowEarlyOut");
    }
  }
} // namespace remix
