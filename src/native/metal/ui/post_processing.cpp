// Metal builds' copy of ui/post_processing.cpp: the same tab, with what is Vulkan's read through
// the runtime's ui* queries (metal/ui/status.h). Port upstream changes to ui/post_processing.cpp here.
#include "metal/runtime.h"
#include "ui/widgets.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <utility>

namespace remix {
  void RemixNativeRuntime::drawPostProcessingTab(config::Settings &settings) {
    auto &options = settings.trace;
    const bool rrActive = m_pathTracer && metal::denoises(m_pathTracer->upscalingStatistics().backend);
    const auto fsr = m_pathTracer ? m_pathTracer->upscalingStatistics() : metal::ReconstructionStatistics{};
    if (ImGui::CollapsingHeader("Denoising and temporal filtering", ImGuiTreeNodeFlags_DefaultOpen)) {
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
      ImGui::EndDisabled();
      ImGui::BeginDisabled(fsr.active);
      ui::Checkbox("Native resolution TAA", &options.taa);
      ImGui::EndDisabled();
    }
    if (ImGui::CollapsingHeader("Tonemapping and brightness", ImGuiTreeNodeFlags_DefaultOpen)) {
      ui::Checkbox("Automatic exposure", &options.autoExposure);
      ui::SliderFloat("Manual exposure", &options.exposure, .05f, 8.f, "%.2f", ImGuiSliderFlags_Logarithmic);
      ui::SliderFloat("Meter target", &options.autoExposureTarget, .02f, .6f, "%.3f");
      ui::SliderFloat("Adaptation rate (1/s)", &options.autoExposureSpeed, .1f, 16.f, "%.2f");
      ui::SliderFloat("Centre weight", &options.autoExposureCentre, 0, 1, "%.2f");
      ui::SliderFloat("Minimum exposure EV", &options.autoExposureMinEv, -24, 0, "%.1f");
      ui::SliderFloat("Maximum exposure EV", &options.autoExposureMaxEv, 0, 24, "%.1f");
      ui::Checkbox("Bloom", &options.bloom);
      ui::SliderFloat("Bloom blend", &options.bloomIntensity, 0, 1, "%.2f");
      ui::SliderFloat("Bloom spread", &options.bloomRadius, 0, 1, "%.2f");
      const auto post = uiPost();
      if (post.available) {
        ImGui::TextWrapped("Automatic %.4g | target %.4g | %s", post.exposure, post.target,
          post.limit < 0 ? "minimum EV limit" : post.limit > 0 ? "maximum EV limit" : "within limits");
        ImGui::TextWrapped("Active exposure %.4g", post.activeExposure);
        ImGui::TextWrapped("Exposure/bloom %.3f ms | %.1f MiB", post.milliseconds, double(post.memoryBytes) / (1024 * 1024));
      } else
        ImGui::TextDisabled("%s", "Automatic exposure and bloom are not available on this backend (manual exposure applies).");
      if (!post.failure.empty()) ImGui::TextWrapped("%s", post.failure.c_str());
      ui::Checkbox("Prism tonemapping", &options.prism);
      ui::Checkbox("Gamma 2.2 output", &options.gamma22);
      for (auto [label, threshold] : {std::pair{"Local direct clamp", &options.localDirectClamp},
                                      std::pair{"Indirect clamp", &options.indirectClamp}}) {
        float value = *threshold;
        if (ui::InputFloat(label, &value, 0.f, 0.f, "%.6g") && std::isfinite(value))
          *threshold = std::clamp(value, 0.f, 1000000.f);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Linear scene luminance, 0 disables (maximum 1,000,000).\nLower values suppress "
                            "more brightness, including legitimate highlights.");
      }
      auto denoising = denoisingStatistics();
      const bool fireflyFilterActive =
          denoising.activeDenoiser && !rrActive && (options.localDenoiser || options.bounces > 0) &&
          options.localLights && options.localIntensity > 0 && localLightStatistics().activeLights > 0;
      ImGui::BeginDisabled(!fireflyFilterActive);
      ui::Checkbox("PBR local-light firefly filter (REBLUR)", &options.localFireflyFilter);
      ImGui::EndDisabled();
      if (!fireflyFilterActive)
        ImGui::TextWrapped(
            "%s", "PBR local-light firefly filter inactive: requires REBLUR receiving local lighting.");
      if (options.localDirectClamp > 0 || options.indirectClamp > 0)
        ImGui::TextWrapped("%s", "Clamping bypasses experimental PBR lighting reuse.");
      if (options.indirectClamp > 0)
        ImGui::TextWrapped("%s", "Indirect clamping bypasses water irradiance sharing.");
      if (denoising.unavailableReason[0])
        ImGui::TextWrapped("%s", denoising.unavailableReason);
      ImGui::TextWrapped("NRD %s, TAA %s | history %llu, resets %llu",
                         denoising.activeDenoiser ? "on" : "off", denoising.activeTaa ? "on" : "off",
                         (unsigned long long)denoising.historyFrames,
                         (unsigned long long)denoising.historyResets);
      ImGui::TextWrapped("Confidence %.2f / NRD %.2f / TAA %.2f ms | %.1f MiB",
                         denoising.gpuConfidenceMilliseconds, denoising.gpuDenoiseMilliseconds,
                         denoising.gpuTaaMilliseconds, double(denoising.memoryBytes) / (1024 * 1024));
    }
  }
} // namespace remix
