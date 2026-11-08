#pragma once
#include <imgui.h>

namespace remix::ui {
  bool prepareVulkanPipeline(bool request);
  void resetTextureUiCache();
  class UiScope {
  public:
    explicit UiScope(ImGuiContext *context) : m_previous(ImGui::GetCurrentContext()) {
      ImGui::SetCurrentContext(context);
    }
    ~UiScope() { ImGui::SetCurrentContext(m_previous); }

  private:
    ImGuiContext *m_previous;
  };

} // namespace remix::ui
