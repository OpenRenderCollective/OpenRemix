#include "runtime/runtime.h"
#include "ui/widgets.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <utility>

namespace remix {
  void RemixNativeRuntime::drawPostProcessingTab(config::Settings &settings) {
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
    auto &options = settings.trace;
    const bool rrActive =
        m_pathTracer && m_pathTracer->upscalingStatistics().backend == upscale::Backend::DlssRr;
    const auto fsr = m_pathTracer ? m_pathTracer->upscalingStatistics() : upscale::Statistics{};
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
      const auto post = postProcessingStatistics();
      ImGui::TextWrapped("Automatic %.4g | target %.4g | %s", post.exposure, post.target,
        post.limit < 0 ? "minimum EV limit" : post.limit > 0 ? "maximum EV limit" : "within limits");
      ImGui::TextWrapped("Active exposure %.4g", m_postOptions.exposure * (m_postOptions.autoExposure ? post.exposure : 1.f));
      ImGui::TextWrapped("Exposure/bloom %.3f ms | %.1f MiB", post.milliseconds, double(post.memoryBytes) / (1024 * 1024));
      if (!m_postFailure.empty()) ImGui::TextWrapped("%s", m_postFailure.c_str());
      ui::Checkbox("Contrast before tonemapping", &options.contrast);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Per-channel contrast around middle grey (0.18), applied after exposure:\n"
                          "colour = (colour / 0.18)^contrast * 0.18. Counters a flat, grey look.");
      if (options.contrast)
        ui::SliderFloat("Contrast", &options.contrastAmount, .5f, 2.f, "%.2f");
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
