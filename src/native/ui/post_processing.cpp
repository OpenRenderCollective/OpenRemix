#include "ui/tabs.h"
#include "ui/widgets.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <utility>

namespace remix {
  void RemixNativeRuntime::drawPostProcessingTab(config::Settings &settings) {
    auto &options = settings.trace;
    const bool rrActive = reconstructionDenoises();
    const auto reconstruction = uiReconstruction();
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
      ImGui::BeginDisabled(reconstruction.active);
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
      ui::Checkbox("Contrast before tonemapping", &options.contrast);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Per-channel contrast around middle grey (0.18), applied after exposure:\n"
                          "colour = (colour / 0.18)^contrast * 0.18. Counters a flat, grey look.");
      if (options.contrast)
        ui::SliderFloat("Contrast", &options.contrastAmount, .5f, 2.f, "%.2f");
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
    // Only while Windows has HDR on for the window's display: SDR has no HDR10 settings.
    const auto hdr = uiHdr();
    if (hdr.available && ImGui::CollapsingHeader("HDR10 output", ImGuiTreeNodeFlags_DefaultOpen)) {
      ui::Checkbox("HDR10 output", &settings.hdr);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("BT.2020 primaries, ST.2084 PQ, 10-bit swapchain. Off: SDR.");
      if (hdr.active) {
        ui::SliderFloat("Peak brightness (nits)", &settings.hdrPeakNits, 80, 10000, "%.0f", ImGuiSliderFlags_Logarithmic);
        ui::SliderFloat("Paper white (nits)", &settings.hdrPaperWhiteNits, 80, 1000, "%.0f");
        ui::SliderFloat("UI brightness (nits)", &settings.hdrUiNits, 80, 1000, "%.0f");
      } else {
        ImGui::TextWrapped("HDR10 output inactive: %s", hdr.inactive.c_str());
      }
    }
    const auto screenshots = uiScreenshots();
    if (screenshots.available && ImGui::CollapsingHeader("Screenshots")) {
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
} // namespace remix
