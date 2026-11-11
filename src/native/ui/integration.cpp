#include "runtime/runtime.h"
#include "ui/context.h"
#include "ui/status.h"

#include <algorithm>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

namespace remix {
  using ui::UiScope;
  namespace {
    void checkVulkan(VkResult result) {
      if (result < 0) {
        throw result;
      }
    }
  } // namespace

  bool RemixNativeRuntime::initializeUi() {
    UiScope scope(ImGui::GetCurrentContext());
    IMGUI_CHECKVERSION();
    m_gui = ImGui::CreateContext();
    if (!m_gui) {
      return false;
    }
    ImGui::SetCurrentContext(m_gui);
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ui::toolkitStyle();
    ui::toolkitFont();
    if (!ImGui_ImplSDL3_InitForVulkan(m_window.handle())) {
      return false;
    }
    // The backend's IME update calls SDL's text input from ImGui::EndFrame, on
    // whichever thread presents; in a host's window that may not be the
    // window's thread, and SDL's IME calls wait on it (a deadlock). The window
    // starts and stops text input for the panel itself (syncTextInput).
    if (m_window.embedded())
      ImGui::GetPlatformIO().Platform_SetImeDataFn = nullptr;
    const float scale = SDL_GetWindowDisplayScale(m_window.handle());
    ImGui::GetStyle().ScaleAllSizes(scale > 0 ? scale : 1.0f);
    ImGui::GetStyle().FontScaleDpi = scale > 0 ? scale : 1.0f;
    m_window.setEventHandler(processEvent, this);
    return true;
  }

  bool RemixNativeRuntime::initializeUiVulkan() {
    UiScope scope(m_gui);
    ui::toolkitColors();
    ImGui_ImplVulkan_InitInfo info = {};
    info.ApiVersion = VK_API_VERSION_1_1;
    info.Instance = m_instance;
    info.PhysicalDevice = m_physicalDevice;
    info.Device = m_device;
    info.QueueFamily = m_queueFamily;
    info.Queue = m_queue;
    info.DescriptorPoolSize = 4096;
    info.MinImageCount = 2;
    // A headless shared frame is one image; the backend wants at least two.
    info.ImageCount = std::max<uint32_t>(2, static_cast<uint32_t>(m_swapchainImages.size()));
    info.PipelineInfoMain.RenderPass = m_renderPass;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.CheckVkResultFn = checkVulkan;
    return ImGui_ImplVulkan_Init(&info);
  }

  void RemixNativeRuntime::shutdownUiVulkan() {
    if (!m_gui) {
      return;
    }
    UiScope scope(m_gui);
    ui::resetTextureUiCache();
    if (ImGui::GetIO().BackendRendererUserData) {
      ImGui_ImplVulkan_Shutdown();
    }
  }

  void RemixNativeRuntime::shutdownUi() {
    if (!m_gui) {
      return;
    }
    UiScope scope(ImGui::GetCurrentContext() == m_gui ? nullptr : ImGui::GetCurrentContext());
    ImGui::SetCurrentContext(m_gui);
    if (ImGui::GetIO().BackendPlatformUserData) {
      ImGui_ImplSDL3_Shutdown();
    }
    if (!m_window.embedded())
      SDL_StopTextInput(m_window.handle());
    ImGui::DestroyContext(m_gui);
    m_gui = nullptr;
    m_window.setEventHandler(nullptr, nullptr);
    m_window.setGuiCapture(false, false);
  }

  bool RemixNativeRuntime::processEvent(const SDL_Event *event, void *userData) {
    auto &runtime = *static_cast<RemixNativeRuntime *>(userData);
    UiScope scope(runtime.m_gui);
    return ImGui_ImplSDL3_ProcessEvent(event);
  }
} // namespace remix

// ui/status.h: the Vulkan ImGui renderer's textures.
namespace remix::ui {
  void updateTexture(ImTextureData *texture) { ImGui_ImplVulkan_UpdateTexture(texture); }

  ImTextureID addTexture(const void *view) {
    return ImTextureID(
        ImGui_ImplVulkan_AddTexture(VkImageView(const_cast<void *>(view)), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
  }

  void removeTexture(ImTextureID texture) {
    if (texture != ImTextureID_Invalid)
      ImGui_ImplVulkan_RemoveTexture(VkDescriptorSet(texture));
  }
} // namespace remix::ui
