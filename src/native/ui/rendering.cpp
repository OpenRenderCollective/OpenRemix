#include "runtime/runtime.h"
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
    if (ui::Combo("Requested backend", &backend, "Raster\0Path trace\0"))
      settings.backend = uint32_t(backend);
    ImGui::TextWrapped("Active backend: %s", m_backend ? "Path trace" : "Raster");
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
#if REMIX_HAS_DLSS5
      ImGui::Separator();
      ui::Checkbox("Enable Neural Uplift (DLSS-NR)", &options.neuralUplift);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("DLSS 5 Neural Uplift (Feature 18) runs as a post-process on the tonemapped,\n"
                          "display-encoded LDR frame, enhancing structure, details and tone.");
      const auto nuStats = m_pathTracer ? m_pathTracer->neuralUpliftStatistics() : upscale::NeuralUpliftStatistics{};
      if (!nuStats.supported) {
        ImGui::TextDisabled("Neural Uplift: unavailable - %s",
                            nuStats.notLoadedReason.empty() ? "nvngx_dlssnr.dll not found" : nuStats.notLoadedReason.c_str());
      } else {
        ImGui::Text("Neural Uplift: %s (feature id %u, preset %u, inits %u)",
                    nuStats.statusReason.empty() ? "idle" : nuStats.statusReason.c_str(),
                    options.neuralUpliftFeatureId, options.neuralUpliftPreset, nuStats.initCount);
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

        if (ImGui::TreeNode("Inputs / Bring-up")) {
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
          ImGui::TreePop();
        }
        ImGui::Unindent();
      }
#endif // REMIX_HAS_DLSS5
    }
    if (ImGui::CollapsingHeader("Memory")) {
      ui::Checkbox("Keep BC textures compressed", &settings.compressedTextures);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Upload BC1-BC7 textures as GPU BC images instead of decoding them to RGBA on the CPU.\n"
                          "Uses 4-8x less VRAM and system memory and removes the decode from CreateTexture.\n"
                          "Applies to textures created after the change; no restart needed.");
      if (settings.compressedTextures && !m_textureCompression)
        ImGui::TextWrapped("%s", "This GPU cannot sample BC formats: textures are decoded once at upload.");
#if REMIX_HAS_USD
      ui::Checkbox("Retain texture data for captures", &settings.captureRetainTextureData);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Keep BC texture blocks in system memory after upload so USD captures (Mods tab)\n"
                          "can write them. Costs about the textures' VRAM size in system memory.\n"
                          "Applies to textures uploaded afterwards. Off: captures save BC textures as 64-pixel stand-ins.");
#endif
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
