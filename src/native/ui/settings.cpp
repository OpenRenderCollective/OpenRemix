#if !defined(REMIX_METAL)
#include "forward/lightmap/denoise.h"
#endif
#if !defined(REMIX_METAL)
#include "forward/lightmap/data.h"
#endif
// Every settings tab of the panel, in tab order (ui::Tab). Each tab is one function;
// each setting is one widget row inside a ui::Panel. The tool tabs (Diagnostics,
// Textures, Profiles, Mods) and the sky/weather rows (sky.cpp) live in their own files.
#include "pathtrace/sparse_policy.h"
#include "pathtrace/volume_policy.h"
#include "ui/tabs.h"
#include "ui/sky.h"
#include "ui/widgets.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <iterator>
#include <string>
#include <utility>

namespace remix {
  // The panel's settings tabs (ui::drawPanel's DrawSettingsTab), in both builds.
  void RemixNativeRuntime::drawSettingsTab(int tab, config::Settings &settings) {
    switch (ui::Tab(tab)) {
    case ui::Tab::Display:
      drawDisplayTab(settings);
      break;
    case ui::Tab::Lighting:
      drawLightingTab(settings);
      break;
    case ui::Tab::Materials:
      drawMaterialsTab(settings);
      break;
    case ui::Tab::Sky:
      drawSkyTab(settings);
      break;
    case ui::Tab::PostProcess:
      drawPostProcessTab(settings);
      break;
    case ui::Tab::Performance:
      drawPerformanceTab(settings);
      break;
    case ui::Tab::Diagnostics:
      drawDiagnosticsTab(settings);
      break;
    case ui::Tab::Textures:
      drawTexturesTab(settings);
      break;
    case ui::Tab::Capture:
      drawCaptureTab(settings);
      break;
    default:
      break;
    }
  }

  // How the frame reaches the screen: the renderer, upscaling, frame generation, HDR10.
  void RemixNativeRuntime::drawDisplayTab(config::Settings &settings) {
    auto &options = settings.trace;
    const auto fsr = uiReconstruction();
    const bool rrActive = fsr.denoises;
    const auto choices = uiReconstructionChoices();
    const auto generation = uiFrameGeneration();
    if (ui::Panel panel{"Renderer", ImGuiTreeNodeFlags_DefaultOpen}) {
      int backend = int(settings.backend);
#if defined(REMIX_METAL)
      if (ui::Combo("Requested backend", &backend, "Raster\0Path trace\0"))
        settings.backend = uint32_t(backend);
      ImGui::TextWrapped("Active backend: %s", m_backend ? "Path trace" : "Raster");
#else
      if (ui::Combo("Requested backend", &backend, "Raster\0Path trace\0Forward+ (experimental)\0"))
        settings.backend = uint32_t(backend);
      ImGui::TextWrapped("Active backend: %s", config::Backend::name(m_backend));
      if (!m_forwardFailure.empty()) ImGui::TextWrapped("%s", m_forwardFailure.c_str());
#endif
      if (!m_traceUnavailable.empty())
        ImGui::TextWrapped("Path tracing unavailable: %s", m_traceUnavailable.c_str());
      int primary = options.rasterPrimary ? 0 : 1;
      if (ui::Combo("Primary visibility", &primary, "Rasterized G-buffer\0Ray traced\0"))
        options.rasterPrimary = primary == 0;
      ui::Checkbox("VSync", &settings.vsync);
      ui::Checkbox("Asynchronous present", &settings.asyncPresent);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Present captures the frame and returns; OpenRemix's render thread prepares, records,\n"
                          "submits and presents it while the game starts its next frame. At most one frame is\n"
                          "in flight: the next Present waits only for that handoff. Statistics describe the last\n"
                          "completed frame. Off: the whole frame runs inside Present, as before.\n"
                          "Applies at the next Present (the render thread is drained first).");
      if (settings.asyncPresent && isHeadless())
        ImGui::TextDisabled("%s", "The headless editor always presents synchronously.");
      ImGui::BeginDisabled(!generation.lowLatencySupported || settings.frameGeneration);
      bool lowLatency = generation.lowLatencySupported && (settings.lowLatency || settings.frameGeneration);
      if (ui::Checkbox("NVIDIA Reflex low latency", &lowLatency))
        settings.lowLatency = lowLatency;
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", !generation.lowLatencySupported ? generation.lowLatencyUnavailable
                                : settings.frameGeneration
                                    ? "Always on with frame generation."
                                    : "The driver holds the game back so that frames are not queued ahead of\n"
                                      "the GPU: input reaches the screen sooner when the GPU is the limit.");
    }
    // Only while Windows has HDR on for the window's display: SDR has no HDR10 settings.
    if (const auto hdr = uiHdr(); hdr.available) {
      if (ui::Panel panel{"HDR10 output", ImGuiTreeNodeFlags_DefaultOpen}) {
        ui::Checkbox("HDR10 output", &settings.hdr);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("BT.2020 primaries, ST.2084 PQ, 10-bit swapchain. Off: SDR.");
        if (hdr.active) {
          ui::SliderFloat("Peak brightness (nits)", &settings.hdrPeakNits, 80, 10000, "%.0f",
                          ImGuiSliderFlags_Logarithmic);
          ui::SliderFloat("Paper white (nits)", &settings.hdrPaperWhiteNits, 80, 1000, "%.0f");
          ui::SliderFloat("UI brightness (nits)", &settings.hdrUiNits, 80, 1000, "%.0f");
        } else {
          ImGui::TextWrapped("HDR10 output inactive: %s", hdr.inactive.c_str());
        }
      }
    }
    if (ui::Panel panel{"Upscaling", ImGuiTreeNodeFlags_DefaultOpen}) {
      int upscaler = choices.vendorModes ? int(options.upscaler) : std::min(int(options.upscaler), 1);
      if (ui::Combo("Requested upscaler", &upscaler, choices.upscalers))
        options.upscaler = upscale::Mode(upscaler);
      if (options.upscaler != upscale::Mode::Off) {
        const bool amd = choices.vendorModes && options.upscaler == upscale::Mode::Fsr31;
        const bool intel = choices.vendorModes && options.upscaler == upscale::Mode::Xess;
        auto &savedQuality = amd ? options.fsrQuality : intel ? options.xessQuality : options.dlssQuality;
        int quality = int(savedQuality);
        // Each upscaler's own names; XeSS has seven presets (the XeSS-SR
        // developer guide's). Without vendor modes one upscaler takes every mode.
        if (ui::Combo("Quality preset", &quality,
                      intel ? "Native Anti-Aliasing\0Ultra Quality Plus\0Ultra Quality\0Quality\0Balanced\0"
                              "Performance\0Ultra Performance\0"
                      : amd || !choices.vendorModes ? "Native AA\0Quality\0Balanced\0Performance\0Ultra Performance\0"
                                                    : "DLAA\0Quality\0Balanced\0Performance\0Ultra Performance\0"))
          savedQuality = uint32_t(quality);
        if (amd) {
          if (choices.sharpening)
            ui::SliderFloat("FSR sharpening", &options.fsrSharpness, 0.f, 1.f, "%.2f");
        } else if (!intel) {
          ui::Checkbox(choices.denoiser, &options.dlssRayReconstruction);
          if (options.dlssRayReconstruction && choices.denoiserModels) {
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
      // Reconstruction is temporal: progressive accumulation and the SHARC
      // debug view render without it (pathtrace/temporal.cpp, metal/tracer.mm).
      if (options.upscaler != upscale::Mode::Off && (options.accumulate || options.sharcDebug))
        ImGui::TextWrapped("%s", options.accumulate
                                     ? "Progressive accumulation is on: reconstruction resumes when it is off "
                                       "(Diagnostics tab)."
                                     : "The SHARC debug view renders without reconstruction.");
      ImGui::TextWrapped("Active: %s", fsr.name.c_str());
      if (fsr.models)
        ImGui::TextWrapped("Ray Reconstruction: %s, preset %c", fsr.cnn ? "CNN (DLSS 310.2.1)" : "transformer",
                           fsr.preset >= 1 && fsr.preset <= 6 ? char('A' + fsr.preset - 1) : '-');
      if (!fsr.note.empty())
        ImGui::TextWrapped("%s", fsr.note.c_str());
      ImGui::TextWrapped("%ux%u -> %ux%u | reconstruction %.2f ms", fsr.renderWidth, fsr.renderHeight, fsr.displayWidth,
                         fsr.displayHeight, fsr.milliseconds);
      if (!fsr.failure.empty())
        ImGui::TextWrapped("%s", fsr.failure.c_str());
      if (rrActive)
        ImGui::TextWrapped("%s manages denoising and independent sampling. Saved settings resume when it is "
                           "disabled.",
                           choices.denoiser);
      if (ui::Panel sparse{"Sparse rendering"}) {
        if (!rrActive)
          ImGui::TextDisabled("Needs %s.", choices.denoiser);
        if (rrActive) {
          ui::Checkbox("Sparse rendering", &options.sparseRendering);
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Lights only a blue-noise selection of the pixels each frame and lets %s\n"
                              "rebuild the rest. Faster at high render resolutions; turns itself off where the\n"
                              "render resolution is too low for it to pay.",
                              choices.denoiser);
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
      }
    }
    if (ui::Panel panel{"Frame generation"}) {
      ui::Checkbox("Frame generation", &settings.frameGeneration);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", choices.frameGenerationTooltip);
      if (settings.frameGeneration && choices.frameGenerationBackends) {
        int backend = int(settings.frameGenerationBackend);
        if (ui::Combo("Frame generation backend", &backend, choices.frameGenerationBackends))
          settings.frameGenerationBackend = upscale::FrameGenerationMode(backend);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Auto: DLSS Frame Generation where the GPU supports it, FSR otherwise.");
      }
      if (settings.frameGeneration && choices.multiFrameGeneration) {
        // FSR generates one frame whatever is asked.
        ImGui::BeginDisabled(settings.frameGenerationBackend == upscale::FrameGenerationMode::Fsr31);
        int frames = int(settings.frameGenerationFrames) - 1;
        if (ui::Combo("Generated frames", &frames,
                      "2x (one generated)\0003x (two generated)\0004x (three generated)\0"))
          settings.frameGenerationFrames = uint32_t(frames) + 1;
        ImGui::EndDisabled();
      }
      if (settings.frameGeneration)
        ImGui::TextWrapped("Frame generation: %s | generated %llu, shown twice instead %llu, rendered frames short of "
                           "generated ones (no free swapchain image) %llu | rendered frame interval %.1f ms (%.0f fps "
                           "rendered) | swapchain: %zu images, %s",
                           generation.status.empty() ? "applies at the next frame" : generation.status.c_str(),
                           (unsigned long long)generation.generated, (unsigned long long)generation.duplicated,
                           (unsigned long long)generation.ungenerated, generation.renderedInterval,
                           generation.renderedInterval > 0 ? 1000.0 / generation.renderedInterval : 0.0,
                           generation.presentImages, generation.presentMode);
      else if (!generation.unavailable.empty())
        ImGui::TextWrapped("%s", generation.unavailable.c_str());
    }
#if REMIX_HAS_DLSS5
    if (ui::Panel panel{"Neural Uplift (DLSS-NR)"}) {
      ui::Checkbox("Enable Neural Uplift (DLSS-NR)", &options.neuralUplift);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("DLSS 5 Neural Uplift (Feature 18) runs as a post-process on the tonemapped,\n"
                          "display-encoded LDR frame, enhancing structure, details and tone.");
      const auto nuStats = m_pathTracer ? m_pathTracer->neuralUpliftStatistics() : upscale::NeuralUpliftStatistics{};
      if (!nuStats.supported) {
        ImGui::TextDisabled("Neural Uplift: unavailable - %s", nuStats.notLoadedReason.empty()
                                                                   ? "nvngx_dlssnr.dll not found"
                                                                   : nuStats.notLoadedReason.c_str());
      } else {
        ImGui::Text("Neural Uplift: %s (feature id %u, preset %u, inits %u)",
                    nuStats.statusReason.empty() ? "idle" : nuStats.statusReason.c_str(), options.neuralUpliftFeatureId,
                    options.neuralUpliftPreset, nuStats.initCount);
        if (options.neuralUplift && !(nuStats.lastHadDepth && nuStats.lastHadMotionVectors)) {
          ImGui::TextWrapped("Inputs: %s, %s - running spatially only. Presets that rely on temporal "
                             "reprojection cannot be judged in this state.",
                             nuStats.lastHadDepth ? "depth" : "NO depth",
                             nuStats.lastHadMotionVectors ? "motion vectors" : "NO motion vectors");
        }
      }
      if (options.neuralUplift) {
        ImGui::Indent();
        int passes = int(options.neuralUpliftPasses);
        if (ui::SliderInt("Passes", &passes, 1, 7))
          options.neuralUpliftPasses = uint32_t(passes);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Number of sequential DLSS 5 passes executed over the image (1 to 7).\n"
                            "Higher passes apply iterative neural enhancement.");
        int style = int(options.neuralUpliftStyle);
        if (ui::SliderInt("Style", &style, 0, 2))
          options.neuralUpliftStyle = uint32_t(style);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("0 is neutral; 1 and 2 apply different baked structure/tone biases.\n"
                            "The snippet only ships these three - higher values are clamped to 2.");
        ui::SliderFloat("Style Strength", &options.neuralUpliftStyleStrength, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("How far the selected style is blended in from neutral. 0 makes it a no-op.\n"
                            "This is the parameter NVIDIA named DLSSNR.LocalToneStrength, which is a style\n"
                            "blend weight rather than the tone control its name suggests.");
        ui::SliderFloat("Intensity", &options.neuralUpliftIntensity, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Blend of the enhanced image against the original. Below 1 the snippet keeps\n"
                            "an extra copy of the input to blend against, so it also costs slightly more.");
        ui::Checkbox("Auto Mask", &options.neuralUpliftAutoMask);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Lets the snippet decide per pixel where to apply the effect. The two structure\n"
                            "strengths below are applied through this mask, so turning it off disables them.");
        ImGui::BeginDisabled(!options.neuralUpliftAutoMask);
        ui::SliderFloat("Local Structure Strength", &options.neuralUpliftLocalStructureStrength, 0.0f, 2.0f, "%.2f");
        ui::SliderFloat("Skin Structure Strength", &options.neuralUpliftSkinStructureStrength, -1.0f, 2.0f, "%.2f");
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("-1 means: use Local Structure Strength for skin too. That is the default, and is\n"
                            "not the same as 0, which explicitly disables structure enhancement on skin.");
        ImGui::EndDisabled();

        if (ui::Panel bringUp{"Bring-up (developer)"}) {
          int preset = int(options.neuralUpliftPreset);
          if (ui::SliderInt("Model Preset", &preset, 0, 7))
            options.neuralUpliftPreset = uint32_t(preset);
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Selects weights hint for the model. Changing it recreates the feature and\n"
                              "resets temporal history.");
          ui::Checkbox("Depth Inverted", &options.neuralUpliftDepthInverted);
          ui::SliderFloat("Motion Vector Scale", &options.neuralUpliftMotionVectorScale, 0.01f, 10.0f, "%.2f");
          int featId = int(options.neuralUpliftFeatureId);
          if (ui::DragInt("NGX Feature ID", &featId, 1.0f, 0, 63))
            options.neuralUpliftFeatureId = uint32_t(featId);
          ui::Checkbox("Bypass Snippet Caller Check", &options.neuralUpliftBypassCallerCheck);
        }
        ImGui::Unindent();
      }
    }
#endif // REMIX_HAS_DLSS5
  }

  // How the scene is lit: denoising, path sampling, the sun and sky, local lights, radiance
  // caches, volumetrics, and the game's scale and shadow offsets.
  void RemixNativeRuntime::drawLightingTab(config::Settings &settings) {
#if !defined(REMIX_METAL)
    if (settings.backend == config::Backend::ForwardPlus) {
      const auto statistics = forwardStatistics();
      ImGui::Text("Active MSAA: %ux | prepared draw records: %u | lights: %u | clusters: %u",
                  statistics.samples, statistics.draws, statistics.lights, statistics.clusters);
      const auto &calls = statistics.drawCalls;
      ImGui::TextDisabled("Geometry calls: depth %u | color %u | terrain %u | water %u",
                          calls.worldDepth, calls.worldColor, calls.terrain, calls.water);
      ImGui::TextDisabled("Depth sources: %u logical | %u in static batch",
                          calls.worldDepthLogical, calls.batchedDepthSources);
      ImGui::TextDisabled("Shadow calls: sun %u static + %u moving | local %u static + %u moving",
                          calls.staticSunShadow, calls.movingSunShadow,
                          calls.staticLocalShadow, calls.movingLocalShadow);
      ImGui::TextDisabled("Viewmodel calls: depth %u | color %u | sun %u | local %u",
                          calls.viewModelDepth, calls.viewModelColor,
                          calls.viewModelSunShadow, calls.viewModelLocalShadow);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Geometry API calls recorded for the last reported frame.\n"
                          "Excludes fullscreen/post-processing, raster overlays and UI.");
      ImGui::TextDisabled("Local light lists: %u runtime | %u baked fallback | %u build candidates",
                          statistics.clusteredLights, statistics.fullyBakedLights, statistics.clusterBuildLights);
      ImGui::TextDisabled("Selected baked roles: %u highlights | %u mixed shadows",
                          statistics.selectedHighlightLights, statistics.selectedMixedShadowLights);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Counts overlap when a light has both roles. Positive response weights only;\n"
                          "pending fades can also retain clustered entries.");
      auto &forward = settings.forward;
      const bool source2Preset = remix::forward::presets::isSource2(forward);
      const char *source2Name = remix::forward::presets::source2Name.data();
      ui::field("Lighting preset", [&](const char *label) {
        if (!ImGui::BeginCombo(label, source2Preset ? source2Name : "Custom")) return false;
        bool changed = false;
        if (ImGui::Selectable(source2Name, source2Preset)) {
          remix::forward::presets::applySource2(forward);
          changed = true;
        }
        ImGui::BeginDisabled();
        ImGui::Selectable("Custom (individual techniques)", !source2Preset);
        ImGui::EndDisabled();
        ImGui::EndCombo();
        return changed;
      });
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Source 2 selects baked surface/probe lighting, mixed shadows and cubemap + SSR reflections.\n"
                          "Quality settings stay unchanged. Individual technique changes display Custom.\n"
                          "If cubemaps were disabled with a zero probe budget, the default budget is restored.");
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
      ui::Checkbox("Batch static depth draws", &forward.staticDepthBatch);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Combines static opaque depth draws with a bounded buffer per frame slot.\n"
                          "Cutout and dynamic draws keep their normal paths; does not rebake lighting.");
      ui::Checkbox("GPU opaque occlusion culling", &forward.opaqueOcclusionCulling);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Uses depth prepass coverage to skip hidden opaque draws in larger views.");
      ui::Checkbox("Bicubic lightmaps", &forward.bicubicLightmaps);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Smooth baked texel transitions with chart-safe cubic B-spline filtering.\n"
                          "Costs additional lighting samples; does not rebake or remove capture noise.");
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
      ImGui::Text("Moving local maps: %u updated, %u reused", statistics.movingLocalUpdates,
                  statistics.movingLocalReuses);
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
      ImGui::Text("Posed uploads: %llu | unchanged poses reused: %llu",
                  static_cast<unsigned long long>(statistics.posedUploads),
                  static_cast<unsigned long long>(statistics.posedUploadReuses));
      ui::Checkbox("Screen-space reflections", &forward.ssr);
      ui::Checkbox("Water refraction and reflections", &forward.water);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Uses the material's tint, absorption and refractive index.\n"
                          "Disabling restores compatibility water rendering.");
      auto selectedTechniques = remix::forward::techniques::requested(forward);
      int diffuseTechnique = int(selectedTechniques.diffuse);
      if (ui::Combo("Diffuse lighting", &diffuseTechnique,
                    "Surface lightmaps + volume probes\0Live ray-query GI\0Disabled\0Live incoming + baked direct\0"))
        remix::forward::techniques::selectDiffuse(forward,
            remix::forward::techniques::Diffuse(diffuseTechnique));
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Selects diffuse transport independently of material shading.\n"
                          "Live incoming retains the selected baked direct technique; full live GI replaces the baked field.\n"
                          "Cubemap captures use an independent owner when live incoming is active.");
      if (forward.rtGI) {
        if (!statistics.rayGI.available)
          ImGui::TextWrapped("Fragment ray queries unavailable; using baked indirect lighting.");
        else ImGui::TextWrapped("%s", statistics.rayGI.status);
        int samples = int(forward.rtGISamples), bounces = int(forward.rtGIBounces);
        if (ui::SliderInt("GI rays per shaded pixel", &samples, 1, 64)) forward.rtGISamples = uint32_t(samples);
        if (ui::SliderInt("GI secondary bounces", &bounces, 1, 8)) forward.rtGIBounces = uint32_t(bounces);
        ui::DragFloat("GI ray distance (m)", &forward.rtGIDistance, 1, .1f, 10000);
      }
      int reflectionTechnique = int(remix::forward::techniques::requested(forward).reflections);
      if (ui::Combo("Reflection fallback", &reflectionTechnique,
                    "Disabled\0Parallax cubemaps\0Ray queries\0Ray queries + cubemaps\0"))
        remix::forward::techniques::selectReflections(forward,
            remix::forward::techniques::Reflections(reflectionTechnique));
      if (forward.rtReflections) {
        if (!statistics.rayReflections.available)
          ImGui::TextWrapped("Hardware ray queries unavailable; using enabled fallback techniques.");
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
      ui::Checkbox("Cache baked lighting on disk", &forward.baking.diskCache);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Reuses finished lighting when map inputs, materials, quality and GPU match.\n"
                          "Cache changes affect subsequent map loads; disabling keeps the active lighting.");
      int directTechnique = int(remix::forward::techniques::requested(forward).direct);
      if (ui::Combo("Static direct lighting", &directTechnique,
                    "Analytic\0Attributed bake\0Baked diffuse + mixed shadows\0"))
        remix::forward::techniques::selectDirect(forward,
            remix::forward::techniques::Direct(directTechnique));
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Uses baked diffuse for fixed lights and reduces their per-frame work.\n"
                          "Lightmaps and probes provide diffuse; selected highlights and moving shadows stay live.\n"
                          "Switching can prepare another bake; disabling restores your previous policy.");
      if (forward.bakedLighting) {
        ImGui::TextWrapped("Fixed-light diffuse comes from lightmaps and probes. "
                           "Other reflections still use the enabled reflection effects.");
        if (!forward.baking.enabled)
          ImGui::TextWrapped("Select surface lightmaps + volume probes to use baked lighting.");
        else if (!forward.baking.directDiffuse)
          ImGui::TextWrapped("Enable baked direct diffuse to use baked lighting.");
        else if (forward.rtGI && statistics.rayGI.available)
          ImGui::TextWrapped(forward.bakedDirectWithLiveGI
              ? "Direct lighting stays baked; incoming lighting is evaluated live."
              : "Live ray-traced GI replaces the baked field. Baked lighting resumes when GI is off.");
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
      ui::Checkbox("Lightmap static models", &forward.baking.modelLightmaps);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Bakes eligible opaque static mesh surfaces into the surface atlas.\n"
                          "Dynamic objects retain probe lighting. Changing this rebuilds lighting once.");
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
#endif
    auto &options = settings.trace;
    const auto fsr = uiReconstruction();
    const bool rrActive = fsr.denoises;
    if (ui::Panel panel{"Denoising", ImGuiTreeNodeFlags_DefaultOpen}) {
      const auto denoising = denoisingStatistics();
      ImGui::BeginDisabled(rrActive);
      ui::Checkbox("REBLUR diffuse SH", &options.denoiser);
      ui::Checkbox("SIGMA sun shadows", &options.sigma);
      ui::Checkbox("Blue noise sun shadows", &options.blueNoiseShadows);
      ImGui::EndDisabled();
      const auto shadows = shadowStatistics();
      ImGui::TextWrapped("SIGMA: %s", shadows.active ? "active" : "inactive");
      if (shadows.active && shadows.gpuTimingsAvailable)
        ImGui::TextWrapped("Sun shadow filter %.3f ms", shadows.gpuSigmaMilliseconds);
      if (shadows.unavailableReason[0])
        ImGui::TextWrapped("%s", shadows.unavailableReason);
      ImGui::BeginDisabled(rrActive);
      ui::Checkbox("Denoise local lighting", &options.localDenoiser);
      ImGui::BeginDisabled(!options.localDenoiser);
      int localHistory = int(options.localHistoryFrames);
      if (ui::SliderInt("Local light history", &localHistory, 2, 64))
        options.localHistoryFrames = uint32_t(localHistory);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Frames REBLUR averages local lighting over. Longer is cleaner; moving shadows\n"
                          "and switched lights take longer to settle.");
      ui::SliderFloat("Local anti-lag sigma", &options.localAntilagSigma, .1f, 16.f, "%.2f",
                      ImGuiSliderFlags_Logarithmic);
      ui::SliderFloat("Local anti-lag sensitivity", &options.localAntilagSensitivity, .1f, 16.f, "%.2f",
                      ImGuiSliderFlags_Logarithmic);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("REBLUR anti-lag for local lighting: lower values drop history sooner where the\n"
                          "light changed (faster shadows, more noise right after a change).");
      ui::SliderFloat("Moving shadow blur", &options.movingShadowBlur, .01f, 1.f, "%.3f", ImGuiSliderFlags_Logarithmic);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Spatial smoothing where a moving object's shadow just arrived or left (the\n"
                          "denoiser restarted those pixels). Lower: sharper, grainier moving shadows.\n"
                          "Higher: smoother, paler. Settles to normal over eight frames.");
      ImGui::EndDisabled();
      ImGui::EndDisabled();
      ImGui::BeginDisabled(fsr.active);
      ui::Checkbox("Native resolution TAA", &options.taa);
      ImGui::EndDisabled();
      int history = int(options.indirectHistoryFrames);
      if (ui::SliderInt("Indirect denoiser history (0 = default)", &history, 0, 63))
        options.indirectHistoryFrames = uint32_t(history);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Frames REBLUR averages bounce light over. The default is half a second of frames.\n"
                          "Longer hides more boiling; lighting changes take longer to settle.");
      if (denoising.unavailableReason[0])
        ImGui::TextWrapped("%s", denoising.unavailableReason);
      ImGui::TextWrapped("NRD %s, TAA %s | history %llu, resets %llu", denoising.activeDenoiser ? "on" : "off",
                         denoising.activeTaa ? "on" : "off", (unsigned long long)denoising.historyFrames,
                         (unsigned long long)denoising.historyResets);
      ImGui::TextWrapped("Confidence %.2f / NRD %.2f / TAA %.2f ms | %.1f MiB", denoising.gpuConfidenceMilliseconds,
                         denoising.gpuDenoiseMilliseconds, denoising.gpuTaaMilliseconds,
                         double(denoising.memoryBytes) / (1024 * 1024));
      if (ui::Panel clamps{"Firefly filtering and clamps"}) {
        for (auto [label, threshold] : {std::pair{"Local direct clamp", &options.localDirectClamp},
                                        std::pair{"Indirect clamp", &options.indirectClamp}}) {
          float value = *threshold;
          if (ui::InputFloat(label, &value, 0.f, 0.f, "%.6g") && std::isfinite(value))
            *threshold = std::clamp(value, 0.f, 1000000.f);
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip(options.relativeClamps
                                  ? "Multiples of middle grey at the current exposure, 0 disables.\nLower values "
                                    "suppress more brightness, including legitimate highlights."
                                  : "Linear scene luminance, 0 disables (maximum 1,000,000).\nLower values suppress "
                                    "more brightness, including legitimate highlights.");
        }
        ui::Checkbox("Clamps relative to exposure", &options.relativeClamps);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("The clamps count in multiples of middle grey (0.18 on screen) at the current\n"
                            "exposure instead of linear scene luminance, so one value suits any scene.");
        const bool fireflyFilterActive = denoising.activeDenoiser && !rrActive &&
                                         (options.localDenoiser || options.bounces > 0) && options.localLights &&
                                         options.localIntensity > 0 && localLightStatistics().activeLights > 0;
        ImGui::BeginDisabled(!fireflyFilterActive);
        ui::Checkbox("PBR local-light firefly filter (REBLUR)", &options.localFireflyFilter);
        ImGui::EndDisabled();
        if (!fireflyFilterActive)
          ImGui::TextWrapped("%s",
                             "PBR local-light firefly filter inactive: requires REBLUR receiving local lighting.");
        if (options.localDirectClamp > 0 || options.indirectClamp > 0)
          ImGui::TextWrapped("%s", "Clamping bypasses experimental PBR lighting reuse.");
        if (options.indirectClamp > 0)
          ImGui::TextWrapped("%s", "Indirect clamping bypasses water irradiance sharing.");
      }
    }

    if (ui::Panel panel{"Sampling", ImGuiTreeNodeFlags_DefaultOpen}) {
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
    }
    if (ui::Panel panel{"Sun and sky", ImGuiTreeNodeFlags_DefaultOpen}) {
      ui::Checkbox("Override daylight", &options.overrideLight);
      if (options.overrideLight) {
        ui::SliderFloat3("Light direction", options.lightDirection, -1, 1);
        ui::ColorEdit3("Light color", options.lightColor);
        ui::SliderFloat("Light intensity", &options.lightIntensity, 0, 20);
      }
      bool overrideSize = options.sunAngularDiameter >= 0;
      if (ui::Checkbox("Override sun size", &overrideSize))
        options.sunAngularDiameter = overrideSize ? .533f : -1.f;
      if (overrideSize)
        ui::SliderFloat("Sun diameter (degrees)", &options.sunAngularDiameter, 0.f, 3.f, "%.3f");
      ui::Checkbox("Sky illumination", &options.sky);
      if (options.sky) {
        int rays = int(options.skySamples);
        if (ui::SliderInt("Sky rays per pixel", &rays, 1, 16))
          options.skySamples = uint32_t(rays);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Sky visibility rays at each pixel's surface, averaged. Indoors few directions\n"
                            "reach an opening, so one ray finds the sky rarely and brightly and the light\n"
                            "boils; more rays steady it. Each is one shadow ray.");
        int sheetRays = int(std::min(options.sheetSkySamples, options.skySamples));
        if (ui::SliderInt("Sky rays on translucent layers", &sheetRays, 1, int(options.skySamples)))
          options.sheetSkySamples = uint32_t(sheetRays);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Sky rays at glass and water surfaces, at most the rays above. Each lit layer\n"
                            "casts them on top of the surface behind it, so fewer keep glass cheap.");
        ImGui::BeginDisabled(options.skySamples < 2);
        ui::Checkbox("Sky guiding", &options.skyGuiding);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Each area learns which directions reach the sky (openings, windows) and the\n"
                            "extra sky rays aim there. Diffuse and legacy surfaces; needs 2 or more rays.");
        if (options.skyGuiding) {
          ui::SliderFloat("Guided share", &options.skyGuideShare, 0.f, .95f, "%.2f");
          ui::DragFloat("Sky guide cell size", &options.skyGuideCellSize, .01f, .05f, 100, "%.2f m");
          options.skyGuideCellSize = std::clamp(options.skyGuideCellSize, .05f, 100.f);
        }
        ImGui::EndDisabled();
      }
    }
    if (ui::Panel panel{"Local lights", ImGuiTreeNodeFlags_DefaultOpen}) {
      ui::Checkbox("Local lights", &options.localLights);
      ui::Checkbox("Torch lights", &options.torchLights);
      ui::Checkbox("Emissive lighting", &options.emissiveLights);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Controls light cast onto the world. Emissive textures remain visible.");
      int samples = int(options.localSamples);
      if (ui::SliderInt("Local light samples", &samples, 1, 16))
        options.localSamples = uint32_t(samples);
      ui::SliderFloat("Local light strength", &options.localIntensity, 0.f, 4.f);
      ui::SliderFloat("Light reach (0 = unlimited)", &options.localRange, 0.f, 64.f, "%.1f");
      if (options.localRange > 0 && options.localRange < .01f)
        options.localRange = .01f;
      ImGui::BeginDisabled(rrActive);
      int separation = int(options.separateLocalLighting);
      const char *separationModes[] = {"Combined", "Separate - shared ray budget", "Separate - samples per family",
                                       "Combined - separate pass"};
      if (ui::Combo("Torch / emissive lighting", &separation, separationModes, 4))
        options.separateLocalLighting = uint32_t(separation);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Separate families use distinct direct sampling and denoiser histories.\nShared budget keeps the "
            "shadow-ray budget. Per-family mode can trace twice as many.\nCombined separate pass retains the "
            "combined sampler and budget in a dedicated dispatch.\nIndirect lighting remains combined.");
      ImGui::EndDisabled();
      if (rrActive)
        ImGui::TextWrapped("%s", "RR samples each pixel; shared family budgets use combined sampling.");
      const auto local = localLightStatistics();
      ImGui::TextWrapped("%u active / %u lights | tree depth %u", local.activeLights, local.lightCount,
                         local.treeDepth);
      ImGui::TextWrapped("BVH builds %llu / refits %llu | %.3f ms CPU", (unsigned long long)local.builds,
                         (unsigned long long)local.refits, local.cpuUpdateMilliseconds);
      const auto set = localLightSetStatistics();
      ImGui::TextWrapped("Static %u (+%u unlit slots) / dynamic %u (%u of %u dynamic instances) | dynamic part "
                         "rebuilt %llu times",
                         set.staticLights, set.retiredSlots, set.dynamicLights, set.dynamicInstanceLights,
                         set.dynamicEmittingInstances, (unsigned long long)set.dynamicBuilds);
      if (set.tableBuilds)
        ImGui::TextWrapped("Visibility table made %llu times, last %.3f ms | %.1f KiB",
                           (unsigned long long)set.tableBuilds, set.tableMilliseconds, double(set.tableBytes) / 1024);
      if (local.denoiserActive && local.gpuTimingsAvailable)
        ImGui::TextWrapped("Local light filter %.3f ms", local.gpuDenoiseMilliseconds);
      if (local.unavailableReason[0])
        ImGui::TextWrapped("%s", local.unavailableReason);
      if (ui::Panel emissive{"Emissive surfaces"}) {
        {
          int cluster = int(std::clamp(options.emissiveClusterSize, 1u, 64u));
          if (ui::SliderInt("Emissive triangles per light cluster", &cluster, 1, 64))
            options.emissiveClusterSize = uint32_t(cluster);
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Neighbouring glowing triangles of a surface are grouped (1: none). Each keeps its\n"
                              "shape and color; larger groups make the light tree cheaper to update and walk, at\n"
                              "some noise close to large glowing surfaces.");
        }
        ui::Checkbox("Stabilize animated emissive lighting", &options.stabilizeEmissiveLighting);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip(
              "Experimental: smooths animation-driven cast light over half a second. Visible emissive textures "
              "keep animating.\nCombined lighting and the shadow-ray budget are preserved.");
        ui::Checkbox("Improve nearby emissive lighting", &options.emissiveSolidAngleSampling);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Experimental: samples nearby area lights with the same shadow-ray count.\nAdds "
                            "some GPU work; disable to compare performance.");
        int candidates = int(options.emissiveCandidates);
        if (ui::SliderInt("Emissive candidates", &candidates, 1, 8))
          options.emissiveCandidates = uint32_t(candidates);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip(
              "Compares unshadowed candidates at opaque primary surfaces in larger emissive light sets.\nKeeps "
              "one shadow ray per local light sample; 1 restores basic BVH sampling.");
        int budget = int(options.emissiveCandidateBudget);
        if (ui::SliderInt("Emissive candidate budget", &budget, 0, 128))
          options.emissiveCandidateBudget = uint32_t(budget);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("At most this many emissive candidates per pixel across all local light samples\n"
                            "(0: no limit). Each candidate is a light-tree walk: at 16 samples, 32 leaves two\n"
                            "candidates per sample, 16 one.");
      }
      if (ui::Panel sampling{"Light sampling"}) {
        ui::SliderFloat("Light selection power exponent", &options.lightPowerExponent, .5f, 1.f, "%.2f");
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("The light tree picks lights by their power to this exponent (1: by power). Lower\n"
                            "values give dim lights more samples and bright ones fewer; brightness is unchanged.");
        ui::Checkbox("Adaptive local-light sampling", &options.adaptiveLocalSampling);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Smoothly raises the local-light sample budget toward four near important light "
                            "clusters. Configured sample and emissive candidate counts remain minimums.");
        int sphereCandidates = int(options.sphereCandidates);
        if (ui::SliderInt("Light candidates (no emissives)", &sphereCandidates, 1, 8))
          options.sphereCandidates = uint32_t(sphereCandidates);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip(
              "Scenes lit only by API lights: compares unshadowed candidates (spot cones included) before the\n"
              "one shadow ray. Helps a dominant nearby light (a flashlight) among many distant ones.");
        ImGui::BeginDisabled(rrActive);
        ui::Checkbox("Blue noise local lights", &options.blueNoiseLocalLights);
        ui::Checkbox("Blue noise bounce and sky directions", &options.blueNoiseSky);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Each pixel's first bounce and sky rays take directions from a blue-noise sequence:\n"
                            "neighbouring pixels and frames cover the sky evenly, which steadies the light\n"
                            "after denoising. No extra rays.");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Uses blue noise for primary torch and emissive sample positions with local "
                            "denoising.\nPreserves candidate and shadow-ray budgets.");
        ui::Checkbox("Presampled light cells (ReGIR)", &options.regir);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Each world cell gets a pool of light samples drawn once per frame. Bounce lighting,\n"
                            "the SHARC update and ReSTIR candidates pick from it instead of walking the light tree.");
        if (options.regir)
          ui::SliderFloat("Light cell size", &options.regirCellSize, .125f, 16.f, "%.2f m");
        ui::Checkbox("Cull lights by visibility sets", &options.lightVisibilityCulling);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("When the host supplies its map's potentially visible sets, lights in rooms that\n"
                            "cannot be seen from a surface are never sampled for it.");
        ui::Checkbox("Guide by recent visibility", &options.localGuiding);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip(
              "Guides opaque direct-light samples while retaining exploration.\nOne active light needs no "
              "visibility history.\nEmissive reservoirs use visible groups while checking shadows afresh.");
        ImGui::BeginDisabled(!options.localGuiding);
        ui::DragFloat("Guide cell size", &options.localGuideCellSize, .01f, .05f, 100, "%.2f m");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Size of a visibility cell in meters (converted with the scene scale).");
        options.localGuideCellSize = std::clamp(options.localGuideCellSize, .05f, 100.f);
        const auto guiding = localLightGuidingStatistics();
        if (guiding.active)
          ImGui::TextWrapped("Guided %u / %u observed samples | %.1f MiB history", guiding.sampledGuided,
                             guiding.sampledQueries, double(guiding.memoryBytes) / (1024 * 1024));
        if (guiding.unavailableReason[0])
          ImGui::TextWrapped("%s", guiding.unavailableReason);
      }
      if (ui::Panel restir{"ReSTIR DI"}) {
        ui::Checkbox("ReSTIR DI (local lights)", &options.restir);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Reservoir resampling of primary local lighting: light samples are reused across\n"
                            "frames and neighboring pixels, and occluded ones discarded. Needs the NRD denoiser\n"
                            "(not used with DLSS Ray Reconstruction). Uses the separate local-lighting pass.");
        if (options.restir) {
          int candidates = int(options.restirCandidates), spatial = int(options.restirSpatialSamples),
              history = int(options.restirTemporalHistory), boost = int(options.restirDisocclusionSamples);
          if (ui::SliderInt("ReSTIR candidates", &candidates, 1, 32))
            options.restirCandidates = uint32_t(candidates);
          if (ui::SliderInt("ReSTIR spatial samples", &spatial, 0, 8))
            options.restirSpatialSamples = uint32_t(spatial);
          if (ui::SliderInt("ReSTIR spatial samples after disocclusion", &boost, 0, 8))
            options.restirDisocclusionSamples = uint32_t(boost);
          ui::SliderFloat("ReSTIR spatial radius", &options.restirSpatialRadius, 1, 100, "%.0f px");
          if (ui::SliderInt("ReSTIR temporal history", &history, 0, 64))
            options.restirTemporalHistory = uint32_t(history);
          ui::SliderFloat("ReSTIR boiling filter", &options.restirBoilingFilter, 0, 1, "%.2f");
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Drops reservoirs far brighter than their neighbors' (0: off), before a rare light\n"
                              "spreads through reuse as a flash. Slightly darkens by design.");
          ui::Checkbox("ReSTIR initial visibility", &options.restirInitialVisibility);
          ui::Checkbox("ReSTIR permutation sampling", &options.restirPermutation);
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Reuses a shuffled neighbor's history each frame: less temporally\n"
                              "correlated (frozen) noise for the denoiser.");
        }
      }
    }
    if (ui::Panel panel{"Radiance caches"}) {
      if (ui::Panel sharc{"SHARC", ImGuiTreeNodeFlags_DefaultOpen}) {
        ui::Checkbox("SHARC", &options.sharc);
        ui::Checkbox("Show cached radiance (raw)", &options.sharcDebug);
        if (ImGui::Button("Reset cache"))
          ++m_sharcReset;
        int scale = int(options.sharcSceneScale), downscale = int(options.sharcDownscale);
        if (ui::SliderInt("Cache scene scale", &scale, 1, 200))
          options.sharcSceneScale = uint32_t(scale);
        if (ui::SliderInt("Update downscale", &downscale, 1, 16))
          options.sharcDownscale = uint32_t(downscale);
        int paths = int(options.sharcMaxUpdatePaths), motion = int(options.sharcMotionFrames);
        if (ui::SliderInt("Most update paths (0: no limit)", &paths, 0, 1000000))
          options.sharcMaxUpdatePaths = uint32_t(paths);
        if (ui::SliderInt("Frames averaged while objects move", &motion, 1, 64))
          options.sharcMotionFrames = uint32_t(motion);
        ui::SliderFloat("Update sample limit (x cell mean, 0: none)", &options.sharcFireflyClamp, 0, 100);
        ui::Checkbox("Cache on moving surfaces", &options.sharcDynamicSurfaces);
        ui::Checkbox("Cache on glossy surfaces", &options.sharcGlossySurfaces);
        ui::Checkbox("Footprint test on cache reads", &options.sharcFootprintTest);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("On: a path reads cached lighting only where it has spread wider than a cache cell.\n"
                            "Off: faster (fewer traced bounces), but coarse distant cells can read too dark.");
        auto cache = sharcStatistics();
        ImGui::TextWrapped("%s | %u / %u entries | %.1f MiB",
                           cache.active ? (cache.updatePaths ? "Caching" : "Enabled (idle)") : "Inactive",
                           cache.occupiedEntries, cache.capacity, double(cache.memoryBytes) / (1024 * 1024));
        ImGui::TextWrapped("Update paths %u | sampled query hits %.1f%% | resets %llu", cache.updatePaths,
                           cache.queryAttempts ? 100.0 * cache.queryHits / cache.queryAttempts : 0.0,
                           (unsigned long long)cache.resets);
        if (cache.gpuTimingsAvailable)
          ImGui::TextWrapped("Update %.3f / resolve %.3f ms", cache.gpuUpdateMilliseconds,
                             cache.gpuResolveMilliseconds);
        if (cache.unavailableReason[0])
          ImGui::TextWrapped("%s", cache.unavailableReason);
        if (cache.active && !cache.updatePaths)
          ImGui::TextWrapped("No cache update paths. Requires opaque surfaces the settings above let hold entries.");
        ImGui::TextWrapped(
            "%s", options.sharcGlossySurfaces
                      ? "Glossy PBR surfaces hold cached lighting as rough ones; paths that are still sharp trace on."
                      : "Sharp PBR surfaces continue tracing; rough PBR surfaces can reuse cached lighting.");
        if (options.accumulate)
          ImGui::TextWrapped("%s", "Progressive reference rendering bypasses SHARC.");
      }
      if (ui::Panel orca{"ORCA (per-frame cache)"}) {
        ui::Checkbox("Per-frame radiance cache (ORCA)", &options.orca);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Traces full indirect paths for one pixel per tile and caches their first-bounce radiance\n"
                            "in voxels rebuilt every frame; other pixels read it after one ray. No lighting lag from\n"
                            "the cache itself. Combine with a shorter indirect history below.");
        if (options.orca) {
          int tile = int(options.orcaTile), levels = int(options.orcaLevels), minimum = int(options.orcaMinSamples);
          if (ui::SliderInt("ORCA tile (one path per tile)", &tile, 2, 8))
            options.orcaTile = uint32_t(tile);
          ui::SliderFloat("ORCA smallest voxel", &options.orcaCellSize, .01f, 1.f, "%.3f m");
          if (ui::SliderInt("ORCA voxel sizes", &levels, 1, 8))
            options.orcaLevels = uint32_t(levels);
          if (ui::SliderInt("ORCA minimum samples", &minimum, 1, 16))
            options.orcaMinSamples = uint32_t(minimum);
          ui::SliderFloat("ORCA maximum voxel error (0: any)", &options.orcaMaxError, 0.f, 2.f, "%.2f");
        }
      }
    }
    if (ui::Panel panel{"Volumetrics"}) {
      ui::Checkbox("Volumetric fog", &options.volumetrics);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Fog and haze lit by the sun, the sky and every local light. Uses the game's fog\n"
                          "unless the override below is on.");
      if (options.volumetrics) {
        ui::Checkbox("Override the game's fog", &options.volumetricOverride);
        if (options.volumetricOverride) {
          ui::SliderFloat("Sight distance (m)", &options.volumetricDistance, 1.f, 2000.f, "%.0f",
                          ImGuiSliderFlags_Logarithmic);
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How far one sees through the medium: 5%% of the light is left at this distance.");
          ui::ColorEdit3("Scattering color", options.volumetricColor);
        }
        ui::SliderFloat("Scattered light", &options.volumetricScattering, 0.f, 8.f, "%.2f");
        ui::SliderFloat("Height falloff (m)", &options.volumetricHeightFalloff, 0.f, 500.f, "%.1f");
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Above the base height the medium thins to 37%% over this height. 0: uniform.");
        if (options.volumetricHeightFalloff > 0) {
          ui::DragFloat("Base height (m)", &options.volumetricBaseHeight, .25f, -100000, 100000, "%.2f");
        }
        int axis = int(std::min(options.volumetricUpAxis, 3u));
        if (ui::Combo("Up axis", &axis, "X\0Y\0Z\0Auto (from the camera)\0"))
          options.volumetricUpAxis = uint32_t(axis);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("The world's up axis, for the medium's height falloff and the sky.");
        ui::SliderFloat("March distance (m)", &options.volumetricMaxDistance, 10.f, 2000.f, "%.0f",
                        ImGuiSliderFlags_Logarithmic);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("The medium fills at most this much of a camera ray, the sky's included.");
        {
          const int count = int(std::size(trace::volumeQualities));
          int preset = count;
          for (int i = 0; i < count; ++i) {
            const auto &q = trace::volumeQualities[i];
            if (std::abs(options.volumetricCellSize - q.cellSize) < 1e-4f && options.volumetricCells == q.cells &&
                options.volumetricLevels == q.levels && options.volumetricSteps == q.steps)
              preset = i;
          }
          if (ui::Combo("Volumetric quality", &preset, "Low\0Medium\0High\0Ultra\0Custom\0") && preset < count) {
            const auto &q = trace::volumeQualities[preset];
            options.volumetricCellSize = q.cellSize;
            options.volumetricCells = q.cells;
            options.volumetricLevels = q.levels;
            options.volumetricSteps = q.steps;
          }
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How finely the medium's light is stored and sampled: sharper shadows in the fog and\n"
                              "smoother gradients from Low to Ultra, at more memory and GPU time.");
        }
        {
          const int count = int(std::size(trace::volumeResponses));
          int preset = count;
          for (int i = 0; i < count; ++i)
            if (options.volumetricUpdateInterval == trace::volumeResponses[i].interval &&
                options.volumetricHistory == trace::volumeResponses[i].history)
              preset = i;
          if (ui::Combo("Volumetric response", &preset, "Smooth\0Balanced\0Fast\0Immediate\0Custom\0") &&
              preset < count) {
            options.volumetricUpdateInterval = trace::volumeResponses[preset].interval;
            options.volumetricHistory = trace::volumeResponses[preset].history;
          }
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How quickly the fog follows a change of lighting. Faster settings trace more rays\n"
                              "each frame and flicker more.");
        }
        const auto volume = trace::g_volumeState;
        if (volume.active)
          ImGui::TextWrapped("Volumetrics: on (%s), sight %.1f m", volume.source, volume.visibilityMeters);
        else
          ImGui::TextWrapped("Volumetrics: off (%s)", volume.reason);
        if (ui::Panel sources{"Scattered light by source"}) {
          ui::SliderFloat("Sun", &options.volumetricSunScale, 0.f, 4.f, "%.2f");
          ui::SliderFloat("Lights", &options.volumetricLightScale, 0.f, 4.f, "%.2f");
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Sphere, rect, disk and cylinder lights (map lights, lamps, the flashlight).\n"
                              "Each light's own volumetric radiance scale applies as well.");
          ui::SliderFloat("Emissive surfaces", &options.volumetricEmissiveScale, 0.f, 4.f, "%.2f");
          ui::SliderFloat("Sky", &options.volumetricSkyScale, 0.f, 4.f, "%.2f");
        }
        if (ui::Panel detail{"Detail"}) {
          ui::SliderFloat("Cell size (m)", &options.volumetricCellSize, .05f, 2.f, "%.2f");
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The finest grid's cells: shadows in the fog are as sharp as these.");
          int cells = int(options.volumetricCells), levels = int(options.volumetricLevels);
          if (ui::SliderInt("Cells per side", &cells, 16, 96))
            options.volumetricCells = uint32_t(cells) & ~1u;
          if (ui::SliderInt("Grid levels", &levels, 1, 6))
            options.volumetricLevels = uint32_t(levels);
          int steps = int(options.volumetricSteps), interval = int(options.volumetricUpdateInterval),
              history = int(options.volumetricHistory);
          if (ui::SliderInt("Samples per camera ray", &steps, 1, 64))
            options.volumetricSteps = uint32_t(steps);
          if (ui::SliderInt("Frames between a cell's updates", &interval, 1, 64))
            options.volumetricUpdateInterval = uint32_t(interval);
          if (ui::SliderInt("Samples a cell averages", &history, 1, 256))
            options.volumetricHistory = uint32_t(history);
          const float cellSize = std::clamp(options.volumetricCellSize, .02f, 16.f);
          const uint32_t side = trace::volumeCells(options), count = trace::volumeLevels(options);
          ImGui::TextWrapped(
              "Grids span %.0f m to %.0f m; %.1f MiB; lighting settles in about %u frames", cellSize * side,
              cellSize * side * float(1u << (count - 1)), double(side) * side * side * count * 32 / (1024 * 1024),
              std::clamp(options.volumetricUpdateInterval, 1u, 64u) * std::clamp(options.volumetricHistory, 1u, 256u));
        }
      }
    }
    if (ui::Panel panel{"Game"}) {
      // World units of the host: converts the tracer's meter-based defaults.
      ui::DragFloat("Scene scale (m per unit)", &options.sceneScale, .0005f, .0001f, 1000, "%.4f",
                    ImGuiSliderFlags_Logarithmic);
      options.sceneScale = std::clamp(options.sceneScale, .0001f, 1000.f);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Meters per world unit (1 for meter-scale hosts, 0.0254 for Source inches).\n"
                          "Converts the denoiser's hit distances and the visibility-guide cell size.");
      // Shown to a host that uses host passes (remixapi_GetHostPassesEnabledEXT).
      if (m_hostPasses.asked()) {
        const bool replayed = hostPassesSupported();
        ImGui::BeginDisabled(!replayed);
        ui::Checkbox("Pass-through game shaders (experimental)", &settings.hostPasses);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Replays the game's screen-space passes (post-processing effects, addon shaders)\n"
                            "on the path-traced frame with the game's own shaders, before the HUD. Off: the\n"
                            "game's shader passes are not replayed and the frame is composed without them.\n"
                            "Applies from the next frame.");
        if (!replayed)
          ImGui::TextWrapped("Not available with this backend yet: the game's shader passes are not replayed.");
      }
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
    }
    if (ui::Panel panel{"Shadow terminator and ray offsets"}) {
      ui::Checkbox("Soften shadow terminator", &options.shadowTerminatorSoften);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sampled light on the diffuse lobe fades out within a few degrees of a triangle's\n"
                          "horizon (as dxvk-remix): a gradient instead of an abrupt cut.");
      ui::Checkbox("Shadow terminator offset", &options.shadowTerminatorLift);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Shadow rays leave smooth-shaded triangles from where their vertex normals put the\n"
                          "smooth surface (Hanika 2021, as dxvk-remix): no triangle-shaped shadows on\n"
                          "low-poly characters and props, and sampled light ends at the smooth surface's\n"
                          "horizon instead of the triangle's. Large triangles are left alone (light leaks).");
      if (options.shadowTerminatorLift) {
        ui::SliderFloat("Offset: largest triangle (sq. m)", &options.shadowTerminatorMaxArea, .005f, 1.f, "%.3f",
                        ImGuiSliderFlags_Logarithmic);
        ui::SliderFloat("Offset: longest offset (m)", &options.shadowTerminatorMaxLength, .002f, .2f, "%.3f",
                        ImGuiSliderFlags_Logarithmic);
      }
      ui::SliderFloat("Ray offset (float steps)", &options.rayOffset, 4.f, 1024.f, "%.0f",
                      ImGuiSliderFlags_Logarithmic);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How far shadow and bounce rays start off a surface, in units in the last place\n"
                          "of its coordinates. Smaller: tighter contact shadows; too small: shadow acne.");
      ui::SliderFloat("Contact gap (m)", &options.contactGap, 0.f, .02f, "%.4f");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Shadow and bounce rays pass surfaces within this distance of where they start.\n"
                          "Hides the contact shadow of model parts pressed a hair apart (collars, folds)\n"
                          "that raster lighting never shadowed. 0: off.");
    }
  }

  // How surfaces respond to light: translucency, subsurface scattering, iridescence and caustics.
  void RemixNativeRuntime::drawMaterialsTab(config::Settings &settings) {
    auto &options = settings.trace;
    const bool rrActive = uiReconstruction().denoises;
    if (ui::Panel panel{"Translucency", ImGuiTreeNodeFlags_DefaultOpen}) {
      ui::Checkbox("Denoise translucent surfaces", &options.translucencyDenoiser);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Filters single-layer surface lighting with its own motion and depth.\n"
                          "Uses NRD, including before DLSS RR; overlapping layers retain their existing rendering.");
      if (options.translucencyDenoiser)
        ImGui::TextDisabled("Surface filter: %s", m_pathTracer && m_pathTracer->translucencyDenoiserActive()
                                                      ? "active"
                                                      : "inactive (requires NRD and translucent surfaces)");
      ui::Checkbox("Shadow and bounce rays ignore blended surfaces", &options.skipBlendedSecondaryRays);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Glass, sprites and other blended layers stay visible to the camera, but cast no\n"
                          "shadows and do not tint light passing through them. Cheaper wherever rays cross them.");
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
    }
    if (ui::Panel panel{"Subsurface scattering", ImGuiTreeNodeFlags_DefaultOpen}) {
      ui::Checkbox("Subsurface scattering", &options.subsurface);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Light that enters skin, wax or leaves and leaves them nearby, on materials that\n"
                          "have it. Off: those materials are lit as plain diffuse surfaces.");
      ImGui::BeginDisabled(!options.subsurface);
      ui::Checkbox("Diffusion profiles", &options.subsurfaceDiffusionProfile);
      ui::Checkbox("Thin opaque surfaces", &options.subsurfaceThinOpaque);
      ui::Checkbox("Subsurface texture maps", &options.subsurfaceTextureMaps);
      ui::Checkbox("Light through thin parts", &options.subsurfaceTransmission);
      ui::DragFloat("Diffusion profile scale", &options.subsurfaceProfileScale, .01f, .001f, 1000, "%.3f",
                    ImGuiSliderFlags_Logarithmic);
      options.subsurfaceProfileScale = std::clamp(options.subsurfaceProfileScale, .001f, 1000.f);
      ui::DragFloat("Surface thickness scale", &options.subsurfaceThicknessScale, .01f, 0, 1000, "%.3f",
                    ImGuiSliderFlags_Logarithmic);
      options.subsurfaceThicknessScale = std::clamp(options.subsurfaceThicknessScale, 0.f, 1000.f);
      ImGui::EndDisabled();
    }
    if (ui::Panel panel{"Iridescence and caustics", ImGuiTreeNodeFlags_DefaultOpen}) {
      ui::Checkbox("Iridescence", &options.iridescence);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Thin-film interference colours on materials that have a film (soap bubbles,\n"
                          "oil slicks, beetle shells). Off: those materials reflect without it.");
      ui::Checkbox("Caustics", &options.caustics);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Light through water and bumpy glass is focused by their normal maps' waves\n"
                          "and bumps into caustics on what it lights.");
      ImGui::BeginDisabled(!options.caustics);
      ui::SliderFloat("Caustics strength", &options.causticsStrength, 0.f, 1.f, "%.2f");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How strongly the surface bends the light (.4: as water and glass do).");
      ui::SliderFloat("Caustics focus depth (m)", &options.causticsDepth, 0.f, 4.f, "%.2f");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Caustics fade in over this depth behind the surface: shallow water and thin\n"
                          "glass have none, as light has not yet been focused. 0: at once.");
      ui::SliderFloat("Caustics contrast", &options.causticsContrast, 0.f, 6.f, "%.2f");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Exaggerates the caustics: past 1, brighter lines and darker gaps\n"
                          "(not physical; for surfaces whose waves barely focus the light).");
      ImGui::EndDisabled();
    }
    if (ui::Panel panel{"Experimental"}) {
      ImGui::BeginDisabled(rrActive || options.localDirectClamp > 0 || options.indirectClamp > 0);
      ui::Checkbox("Experimental PBR lighting reuse", &options.pbrLocalReuse);
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Reuses local lighting on closely matching rough surfaces.\nMay change lighting "
                          "and temporal noise; disabled by default.");
    }
  }

  void RemixNativeRuntime::drawSkyTab(config::Settings &settings) { ui::drawSkyTab(settings, uiSky()); }

  // The image after lighting: exposure, bloom and contrast.
  void RemixNativeRuntime::drawPostProcessTab(config::Settings &settings) {
#if !defined(REMIX_METAL)
    if (settings.backend == config::Backend::ForwardPlus) {
      ImGui::TextUnformatted("Forward+ brightness");
      auto &options = settings.forward;
      ui::Checkbox("Prism tone mapping", &options.prism);
      ui::Checkbox("Gamma 2.2 output", &options.gamma22);
      ui::Checkbox("Automatic exposure", &options.autoExposure);
      ui::DragFloat("Exposure multiplier", &options.exposure, .01f, .0001f, 10000.f);
      ui::SliderFloat("Meter target", &options.autoExposureTarget, .02f, .6f, "%.3f");
      ui::SliderFloat("Adaptation rate (1/s)", &options.autoExposureSpeed, .1f, 16.f, "%.2f");
      ui::SliderFloat("Centre weight", &options.autoExposureCentre, 0, 1, "%.2f");
      ui::SliderFloat("Minimum exposure EV", &options.autoExposureMinEv, -24, 0, "%.1f");
      ui::SliderFloat("Maximum exposure EV", &options.autoExposureMaxEv, 0, 24, "%.1f");
      const auto post = postProcessingStatistics();
      ImGui::TextWrapped("Automatic %.4g | target %.4g | %.3f ms", post.exposure, post.target, post.milliseconds);
      ImGui::TextWrapped("Meters linear world lighting and reflections before tonemapping. Empty background "
                        "and overlays are excluded. Debug lighting views freeze adaptation.");
      return;
    }
#endif
    auto &options = settings.trace;
    if (ui::Panel panel{"Exposure", ImGuiTreeNodeFlags_DefaultOpen}) {
      ui::Checkbox("Automatic exposure", &options.autoExposure);
      ui::SliderFloat("Manual exposure", &options.exposure, .05f, 8.f, "%.2f", ImGuiSliderFlags_Logarithmic);
      ui::SliderFloat("Meter target", &options.autoExposureTarget, .02f, .6f, "%.3f");
      ui::SliderFloat("Adaptation rate (1/s)", &options.autoExposureSpeed, .1f, 16.f, "%.2f");
      ui::SliderFloat("Centre weight", &options.autoExposureCentre, 0, 1, "%.2f");
      ui::SliderFloat("Minimum exposure EV", &options.autoExposureMinEv, -24, 0, "%.1f");
      ui::SliderFloat("Maximum exposure EV", &options.autoExposureMaxEv, 0, 24, "%.1f");
      const auto post = uiPost();
      if (post.available) {
        ImGui::TextWrapped("Automatic %.4g | target %.4g | %s", post.exposure, post.target,
                           post.limit < 0   ? "minimum EV limit"
                           : post.limit > 0 ? "maximum EV limit"
                                            : "within limits");
        ImGui::TextWrapped("Active exposure %.4g", post.activeExposure);
        ImGui::TextWrapped("Exposure/bloom %.3f ms | %.1f MiB", post.milliseconds,
                           double(post.memoryBytes) / (1024 * 1024));
      } else
        ImGui::TextDisabled(
            "%s", "Automatic exposure and bloom are not available on this backend (manual exposure applies).");
      if (!post.failure.empty())
        ImGui::TextWrapped("%s", post.failure.c_str());
    }
    if (ui::Panel panel{"Bloom", ImGuiTreeNodeFlags_DefaultOpen}) {
      ui::Checkbox("Bloom", &options.bloom);
      ui::SliderFloat("Bloom blend", &options.bloomIntensity, 0, 1, "%.2f");
      ui::SliderFloat("Bloom spread", &options.bloomRadius, 0, 1, "%.2f");
    }
    if (ui::Panel panel{"Contrast"}) {
      ui::Checkbox("Contrast before tonemapping", &options.contrast);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Per-channel contrast around middle grey (0.18), applied after exposure:\n"
                          "colour = (colour / 0.18)^contrast * 0.18. Counters a flat, grey look.");
      if (options.contrast)
        ui::SliderFloat("Contrast", &options.contrastAmount, .5f, 2.f, "%.2f");
    }
  }

  // Memory and GPU cost: the same image either way.
  void RemixNativeRuntime::drawPerformanceTab(config::Settings &settings) {
    auto &options = settings.trace;
    if (ui::Panel panel{"Memory"}) {
      ui::Checkbox("Keep BC textures compressed", &settings.compressedTextures);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Upload BC1-BC7 textures as GPU BC images instead of decoding them to RGBA on the CPU.\n"
                          "Uses 4-8x less VRAM and system memory and removes the decode from CreateTexture.\n"
                          "Applies to textures created after the change; no restart needed.");
      if (settings.compressedTextures && !textureCompressionSupported())
        ImGui::TextWrapped("%s", "This GPU cannot sample BC formats: textures are decoded once at upload.");
#if REMIX_HAS_USD
      ui::Checkbox("Retain texture data for captures", &settings.captureRetainTextureData);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Keep BC texture blocks in system memory after upload so USD captures (Mods tab)\n"
            "can write them. Costs about the textures' VRAM size in system memory.\n"
            "Applies to textures uploaded afterwards. Off: captures save BC textures as 64-pixel stand-ins.");
#endif
      ui::Checkbox("Report VRAM statistics", &settings.vramStatistics);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Answer GetVramStats with OpenRemix's texture, geometry, acceleration-structure\n"
                          "and denoiser allocations, plus the driver's device-local usage and budget.");
      const auto memory = rasterMemoryStatistics();
      ImGui::TextDisabled("Textures: %llu, %.1f MiB (%.1f MiB BC) | geometry %.1f MiB",
                          (unsigned long long)memory.textureCount, memory.textureBytes / 1048576.0,
                          memory.compressedTextureBytes / 1048576.0, memory.geometryBytes / 1048576.0);
    }
    if (ui::Panel panel{"Geometry"}) {
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
    if (ui::Panel panel{"Camera culling"}) {
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
    if (ui::Panel panel{"GPU optimizations"}) {
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
      const bool micromaps = opacityMicromapsSupported();
      ImGui::BeginDisabled(!micromaps);
      ui::Checkbox("Opacity micromaps", &options.opacityMicromaps);
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", micromaps
                                    ? "Alpha-tested surfaces (foliage, fences, grates) carry opacity micromaps:\n"
                                      "traversal skips the parts proven cut out and accepts the parts proven\n"
                                      "solid, so fewer rays run the shader's alpha test. Same image. Applies at\n"
                                      "the next frame: alpha-tested meshes are proved again and rebuild their BLAS.\n"
                                      "rtx.native.pathtrace.opacityMicromaps"
                                    : "This GPU or driver does not support VK_EXT_opacity_micromap.\n"
                                      "rtx.native.pathtrace.opacityMicromaps");
      if (micromaps && options.opacityMicromaps) {
        int level = int(options.opacityMicromapLevel);
        if (ui::SliderInt("Micromap detail", &level, 1, 8))
          options.opacityMicromapLevel = uint32_t(level);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("The deepest subdivision of a triangle: up to 4^level micro-triangles, about two\n"
                            "per texel. Higher proves more of large triangles at more memory and baking time.\n"
                            "Limited by the device. rtx.native.pathtrace.opacityMicromapLevel");
      }
    }
  }

  // Screenshots, USD scene captures and the mods replacing the game's assets.
  void RemixNativeRuntime::drawCaptureTab(config::Settings &settings) {
    if (const auto screenshots = uiScreenshots(); screenshots.available) {
      if (ui::Panel panel{"Screenshots", ImGuiTreeNodeFlags_DefaultOpen}) {
        if (ImGui::Button("Take screenshot"))
          uiTakeScreenshot();
        ImGui::SameLine();
        // The shortcut: click, then press it (ui/menu_shortcut.cpp).
        const std::string label = (m_profileUi.capturingScreenshotShortcut ? std::string("Press a shortcut...")
                                                                           : m_profileUi.screenshotShortcut) +
                                  "###Screenshot shortcut";
        if (ImGui::SmallButton(label.c_str()))
          m_profileUi.captureScreenshotShortcut = true;
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Shortcut that takes a screenshot.\nClick, then press a key while holding the "
                            "modifiers to use (Alt, Ctrl, Shift).\nEscape keeps the current shortcut.");
        ImGui::TextWrapped("%s", "Always an SDR PNG. While HDR10 output is active, also:");
        ui::Checkbox("HDR PNG (BT.2020 PQ, 16-bit)", &settings.screenshotHdrPng);
        ui::Checkbox("JPEG XR (scRGB, as Windows' HDR captures)", &settings.screenshotJxr);
        ui::Checkbox("OpenEXR (linear scRGB)", &settings.screenshotExr);
        const auto folder = config::Profiles::defaultDirectory() / "screenshots";
        ImGui::TextWrapped("Folder: %s", folder.string().c_str());
        if (!screenshots.status.empty())
          ImGui::TextWrapped("%s", screenshots.status.c_str());
      }
    }
    ui::drawMods(m_profileUi, settings);
  }
} // namespace remix
