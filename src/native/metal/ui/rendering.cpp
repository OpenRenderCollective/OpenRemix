// Metal builds' copy of ui/rendering.cpp: the same tab, with what is Vulkan's read through
// the runtime's ui* queries (metal/ui/status.h). Port upstream changes to ui/rendering.cpp here.
#include "metal/runtime.h"
#include "ui/widgets.h"
#include "pathtrace/sparse_policy.h"
#include <algorithm>
#include <iterator>
#include <cmath>
#include <imgui.h>
#include <utility>

namespace remix {
  void RemixNativeRuntime::drawRenderingTab(config::Settings &settings) {
    auto &options = settings.trace;
    const bool rrActive = m_pathTracer && metal::denoises(m_pathTracer->upscalingStatistics().backend);
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
    if (settings.asyncPresent && isHeadless())
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
      const auto choices = uiReconstructionChoices();
      int upscaler = choices.vendorModes ? int(options.upscaler) : std::min(int(options.upscaler), 1);
      if (ui::Combo("Requested upscaler", &upscaler, choices.upscalers))
        options.upscaler = upscale::Mode(upscaler);
      const auto fsr = m_pathTracer ? m_pathTracer->upscalingStatistics() : metal::ReconstructionStatistics{};
      if (options.upscaler != upscale::Mode::Off) {
        const bool amd = options.upscaler == upscale::Mode::Fsr31;
        auto &savedQuality = amd ? options.fsrQuality : options.dlssQuality;
        int quality = int(savedQuality);
        if (ui::Combo("Quality preset", &quality,
                      "Native AA / DLAA\0Quality\0Balanced\0Performance\0Ultra Performance\0"))
          savedQuality = uint32_t(quality);
        if (amd && choices.sharpening)
          ui::SliderFloat("FSR sharpening", &options.fsrSharpness, 0.f, 1.f, "%.2f");
        else if (!amd)
          ui::Checkbox(choices.denoiser, &options.dlssRayReconstruction);
      }
      if (rrActive) {
        ui::Checkbox("Sparse rendering", &options.sparseRendering);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Lights only a blue-noise selection of the pixels each frame and lets the MetalFX\n"
                            "denoiser rebuild the rest. Faster at high render resolutions; turns itself off where\n"
                            "the render resolution is too low for it to pay.");
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
      ImGui::TextWrapped("Active: %s", metal::name(fsr.backend));
      ImGui::TextWrapped("%ux%u -> %ux%u | reconstruction %.2f ms", fsr.render.width, fsr.render.height,
                         fsr.display.width, fsr.display.height, fsr.milliseconds);
      if (!fsr.failure.empty())
        ImGui::TextWrapped("%s", fsr.failure.c_str());
      if (rrActive)
        ImGui::TextWrapped("Ray Reconstruction manages denoising and independent sampling. Saved settings "
                           "resume when it is disabled.");
      const auto generation = uiFrameGeneration();
      ImGui::BeginDisabled(!generation.lowLatencySupported || settings.frameGeneration);
      bool lowLatency = generation.lowLatencySupported && (settings.lowLatency || settings.frameGeneration);
      if (ui::Checkbox("NVIDIA Reflex low latency", &lowLatency))
        settings.lowLatency = lowLatency;
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", !generation.lowLatencySupported
                                    ? generation.lowLatencyUnavailable
                                    : settings.frameGeneration
                                          ? "Always on with frame generation."
                                          : "The driver holds the game back so that frames are not queued ahead of\n"
                                            "the GPU: input reaches the screen sooner when the GPU is the limit.");
      ui::Checkbox("Frame generation", &settings.frameGeneration);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", choices.frameGenerationTooltip);
      if (settings.frameGeneration && choices.frameGenerationBackends) {
        int backend = int(settings.frameGenerationBackend);
        if (ui::Combo("Frame generation backend", &backend, choices.frameGenerationBackends))
          settings.frameGenerationBackend = upscale::FrameGenerationMode(backend);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Auto: DLSS Frame Generation where the GPU supports it, FSR 3.1 otherwise.");
      }
      if (settings.frameGeneration && choices.multiFrameGeneration) {
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
                           generation.status.empty() ? "applies at the next frame" : generation.status.c_str(),
                           (unsigned long long)generation.generated, (unsigned long long)generation.duplicated,
                           (unsigned long long)generation.ungenerated, generation.renderedInterval,
                           generation.renderedInterval > 0 ? 1000.0 / generation.renderedInterval : 0.0,
                           generation.presentImages, generation.presentMode);
      else if (!generation.unavailable.empty())
        ImGui::TextWrapped("%s", generation.unavailable.c_str());
    }
    if (ImGui::CollapsingHeader("Memory")) {
      ui::Checkbox("Keep BC textures compressed", &settings.compressedTextures);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Upload BC1-BC7 textures as GPU BC images instead of decoding them to RGBA on the CPU.\n"
                          "Uses 4-8x less VRAM and system memory and removes the decode from CreateTexture.\n"
                          "Applies to textures created after the change; no restart needed.");
      if (settings.compressedTextures && !textureCompressionSupported())
        ImGui::TextWrapped("%s", "This GPU cannot sample BC formats: textures are decoded once at upload.");
      ui::Checkbox("Report VRAM statistics", &settings.vramStatistics);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Answer GetVramStats with OpenRemix's texture, geometry, acceleration-structure\n"
                          "and denoiser allocations, plus the driver's device-local usage and budget.");
      const auto memory = rasterMemoryStatistics();
      ImGui::TextDisabled("Textures: %llu, %.1f MiB (%.1f MiB BC) | geometry %.1f MiB",
                          (unsigned long long)memory.textureCount, memory.textureBytes / 1048576.0,
                          memory.compressedTextureBytes / 1048576.0, memory.geometryBytes / 1048576.0);
    }
    // Shown to a host that uses host passes (remixapi_GetHostPassesEnabledEXT).
    if (m_hostPasses.asked() && ImGui::CollapsingHeader("Game", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::BeginDisabled();
      ui::Checkbox("Pass-through game shaders (experimental)", &settings.hostPasses);
      ImGui::EndDisabled();
      ImGui::TextWrapped("Not available with the Metal backend yet: the game's shader passes are not replayed.");
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
