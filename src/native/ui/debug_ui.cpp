#include <cstdio>
#include "runtime/runtime.h"
#include "ui/context.h"
#include "api/imgui_compat.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

namespace remix {
  void RemixNativeRuntime::drawUi(VkCommandBuffer commandBuffer) {
    if (!m_gui)
      return;
    ui::UiScope scope(m_gui);
    // Asynchronous presentation: Present built this frame's UI on its thread
    // (commandBuffer null); the render thread records it only if it did.
    if (commandBuffer && m_asyncActive && !m_uiFrameBuilt)
      return;
    if (!commandBuffer)
      m_uiFrameBuilt = false;
    // The bootstrap UI pipeline is prepared by the same worker. Surface
    // presentation and SDL input remain available while its first job runs.
    if (!ui::prepareVulkanPipeline(commandBuffer == VK_NULL_HANDLE))
      return;
    if (commandBuffer) {
      ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
      return;
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    // A host window usually hides the OS cursor on its thread (games do), so
    // draw ImGui's own while the menu is open.
    ImGui::GetIO().MouseDrawCursor = m_window.embedded() && m_uiState != REMIXAPI_UI_STATE_NONE;
    ImGui::NewFrame();
    if (preparationPending() || !m_preparationFailure.empty()) {
      const auto stats = m_pipelines->statistics();
      const auto &io = ImGui::GetIO();
      ImGui::SetNextWindowPos(
          m_loading ? ImVec2(io.DisplaySize.x * .5f, io.DisplaySize.y * .5f)
                    : ImVec2(16, 16),
          ImGuiCond_Always, m_loading ? ImVec2(.5f, .5f) : ImVec2(0, 0));
      ImGui::SetNextWindowBgAlpha(.94f);
      ImGui::Begin(
          "Renderer preparation", nullptr,
          ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration |
              ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
      ImGui::TextUnformatted(m_loading ? "Preparing renderer"
                                       : "Preparing requested settings");
      // The compiles so far as a bar; the scene variants prewarmed in the
      // background keep it going after activation.
      char progress[48];
      std::snprintf(progress, sizeof(progress), "%llu / %llu", (unsigned long long)stats.completed,
                    (unsigned long long)stats.requested);
      ImGui::TextUnformatted("Compiling shaders");
      ImGui::ProgressBar(stats.requested ? float(double(stats.completed) / double(stats.requested)) : 0.f,
                         ImVec2(280, 0), progress);
      if (m_preparation) {
        const auto seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          m_preparation->started)
                .count();
        ImGui::Text("Elapsed: %.1f s", seconds);
      }
      if (!m_preparationFailure.empty())
        ImGui::TextWrapped("%s", m_preparationFailure.c_str());
      ImGui::Text("%s: settings and diagnostics", m_profileUi.menuShortcut.c_str());
      ImGui::End();
    }
    if (m_uiState != REMIXAPI_UI_STATE_NONE) {
      const bool open =
          ui::drawPanel(m_profiles, m_profileUi, this, [](void *user, int tab, config::Settings &settings) {
            auto &runtime = *static_cast<RemixNativeRuntime *>(user);
            switch (tab) {
            case 0:
              runtime.drawRenderingTab(settings);
              break;
            case 1:
              runtime.drawLightingTab(settings);
              break;
            case 2:
              runtime.drawSkyTab(settings);
              break;
            case 3:
              runtime.drawPostProcessingTab(settings);
              break;
            case 4:
              runtime.drawDiagnosticsTab(settings);
              break;
            case 5:
              runtime.drawTexturesTab(settings);
              break;
            }
          });
      if (!open)
        setUiState(REMIXAPI_UI_STATE_NONE);
    }
    remixapi_imgui_InvokeOverlayCallback();
    ImGui::Render();
    const auto &io = ImGui::GetIO();
    m_window.setGuiCapture(m_uiState != REMIXAPI_UI_STATE_NONE && io.WantCaptureKeyboard,
                           m_uiState != REMIXAPI_UI_STATE_NONE && io.WantCaptureMouse);
    m_window.syncTextInput(m_uiState != REMIXAPI_UI_STATE_NONE && io.WantTextInput);
    for (auto *texture : ImGui::GetPlatformIO().Textures) {
      if (texture->Status != ImTextureStatus_OK)
        ImGui_ImplVulkan_UpdateTexture(texture);
    }
    m_uiFrameBuilt = true;
  }

  void RemixNativeRuntime::updateFreeCamera() {
    if (!m_gui) return;
    ui::UiScope scope(m_gui);
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - m_freeCamera.lastTime).count();
    m_freeCamera.lastTime = now;
    if (dt <= 0.0f || dt > 0.1f) dt = 1.0f / 60.0f;

    if (!m_freeCamera.enabled || m_freeCamera.locked) {
      return;
    }

    const auto &io = ImGui::GetIO();
    if (io.WantTextInput) {
      return;
    }

    float speed = m_freeCamera.speed * dt;
    if (ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift)) {
      speed *= 3.0f;
    }
    if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl)) {
      speed *= 0.25f;
    }

    float moveForward = 0.0f;
    float moveRight = 0.0f;
    float moveUp = 0.0f;

    if (ImGui::IsKeyDown(ImGuiKey_W)) moveForward += speed;
    if (ImGui::IsKeyDown(ImGuiKey_S)) moveForward -= speed;
    if (ImGui::IsKeyDown(ImGuiKey_D)) moveRight += speed;
    if (ImGui::IsKeyDown(ImGuiKey_A)) moveRight -= speed;
    if (ImGui::IsKeyDown(ImGuiKey_E)) moveUp += speed;
    if (ImGui::IsKeyDown(ImGuiKey_Q)) moveUp -= speed;

    float turnSpeed = m_freeCamera.turningSpeed * dt * 2.0f;
    const float pitchDir = m_freeCamera.invertY ? -1.0f : 1.0f;

    if (ImGui::IsKeyDown(ImGuiKey_J)) m_freeCamera.yaw -= turnSpeed;
    if (ImGui::IsKeyDown(ImGuiKey_L)) m_freeCamera.yaw += turnSpeed;
    if (ImGui::IsKeyDown(ImGuiKey_I)) m_freeCamera.pitch += pitchDir * turnSpeed;
    if (ImGui::IsKeyDown(ImGuiKey_K)) m_freeCamera.pitch -= pitchDir * turnSpeed;

    if (!io.WantCaptureMouse) {
      if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float mouseSens = m_freeCamera.turningSpeed * 0.005f;
        m_freeCamera.yaw += io.MouseDelta.x * mouseSens;
        m_freeCamera.pitch -= io.MouseDelta.y * mouseSens * pitchDir;
      }
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        m_freeCamera.reset();
      }
    }

    constexpr float maxPitch = 1.55f;
    m_freeCamera.pitch = std::clamp(m_freeCamera.pitch, -maxPitch, maxPitch);

    constexpr float twoPi = 6.28318530718f;
    while (m_freeCamera.yaw > 3.14159265359f) m_freeCamera.yaw -= twoPi;
    while (m_freeCamera.yaw < -3.14159265359f) m_freeCamera.yaw += twoPi;

    if (moveForward != 0.0f || moveRight != 0.0f || moveUp != 0.0f) {
      if (m_raster.cameraValid()) {
        try {
          const auto &baseView = m_raster.view();
          scene::Matrix viewToWorld = scene::inverse(baseView);
          const float upSign = viewToWorld[5] < 0.0f ? -1.0f : 1.0f;
          scene::Matrix rot = scene::getEulerRotationMatrix(upSign * m_freeCamera.pitch, m_freeCamera.yaw);

          scene::Matrix viewToWorldRot = viewToWorld;
          viewToWorldRot[12] = 0.0f; viewToWorldRot[13] = 0.0f; viewToWorldRot[14] = 0.0f; viewToWorldRot[15] = 1.0f;
          scene::Matrix freeCamRot = scene::multiply(viewToWorldRot, rot);

          for (int i = 0; i < 3; ++i) {
            m_freeCamera.positionOffset[i] += moveRight * freeCamRot[i]
                                           + (upSign * moveUp) * freeCamRot[4 + i]
                                           + moveForward * freeCamRot[8 + i];
          }
        } catch (...) {}
      }
    }
  }
} // namespace remix
