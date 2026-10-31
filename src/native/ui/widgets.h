#pragma once
#include <imgui.h>
#include <utility>

namespace remix::ui {
  // Stack labels when a narrow or high-DPI panel cannot fit an inline field.
  template <typename Draw> bool field(const char *label, Draw draw) {
    if (ImGui::CalcItemWidth() + ImGui::CalcTextSize(label).x + ImGui::GetStyle().ItemInnerSpacing.x <=
        ImGui::GetContentRegionAvail().x)
      return draw(label);
    ImGui::TextWrapped("%s", label);
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(-1);
    const bool changed = draw("##value");
    ImGui::PopID();
    return changed;
  }
#define REMIX_FIELD(name)                                                                                    \
  template <typename... Args> bool name(const char *label, Args &&...args) {                                 \
    return field(label, [&](const char *id) { return ImGui::name(id, std::forward<Args>(args)...); });       \
  }
  REMIX_FIELD(Combo)
  REMIX_FIELD(SliderInt)
  REMIX_FIELD(DragInt)
  REMIX_FIELD(SliderFloat)
  REMIX_FIELD(SliderFloat3)
  REMIX_FIELD(DragFloat)
  REMIX_FIELD(DragFloat3)
  REMIX_FIELD(InputFloat)
  REMIX_FIELD(InputText)
  REMIX_FIELD(ColorEdit3)
#undef REMIX_FIELD
  inline bool Checkbox(const char *label, bool *value) {
    if (ImGui::GetFrameHeight() + ImGui::CalcTextSize(label).x + ImGui::GetStyle().ItemInnerSpacing.x <=
        ImGui::GetContentRegionAvail().x)
      return ImGui::Checkbox(label, value);
    ImGui::BeginGroup();
    ImGui::PushID(label);
    const bool changed = ImGui::Checkbox("##value", value);
    ImGui::PopID();
    ImGui::SameLine();
    ImGui::TextWrapped("%s", label);
    ImGui::EndGroup();
    return changed;
  }
} // namespace remix::ui
