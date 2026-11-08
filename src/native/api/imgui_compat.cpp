/*
 * Copyright (c) 2024, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "api/imgui_compat.h"
#include <atomic>
#include <imgui.h>

// Static callback storage - allows external plugins/wrappers to register ImGui draw callbacks.
static std::atomic<PFN_remixapi_imgui_DrawCallback> s_drawCallback{nullptr};
static std::atomic<void *> s_drawCallbackUserData{nullptr};

static std::atomic<PFN_remixapi_imgui_DrawCallback> s_overlayCallback{nullptr};
static std::atomic<void *> s_overlayCallbackUserData{nullptr};

// --- Callback Registration ---

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_RegisterDrawCallback(PFN_remixapi_imgui_DrawCallback callback,
                                                                              void *userData) {
  s_drawCallbackUserData.store(userData, std::memory_order_release);
  s_drawCallback.store(callback, std::memory_order_release);
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_UnregisterDrawCallback(void) {
  s_drawCallback.store(nullptr, std::memory_order_release);
  s_drawCallbackUserData.store(nullptr, std::memory_order_release);
}

extern "C" RIMGUI_EXPORT void remixapi_imgui_InvokeDrawCallback() {
  auto cb = s_drawCallback.load(std::memory_order_acquire);
  if (cb) {
    cb(s_drawCallbackUserData.load(std::memory_order_acquire));
  }
}

extern "C" RIMGUI_EXPORT int remixapi_imgui_HasDrawCallback() {
  return s_drawCallback.load(std::memory_order_acquire) != nullptr ? 1 : 0;
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL
remixapi_imgui_RegisterOverlayCallback(PFN_remixapi_imgui_DrawCallback callback, void *userData) {
  s_overlayCallbackUserData.store(userData, std::memory_order_release);
  s_overlayCallback.store(callback, std::memory_order_release);
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_UnregisterOverlayCallback(void) {
  s_overlayCallback.store(nullptr, std::memory_order_release);
  s_overlayCallbackUserData.store(nullptr, std::memory_order_release);
}

extern "C" RIMGUI_EXPORT void remixapi_imgui_InvokeOverlayCallback() {
  auto cb = s_overlayCallback.load(std::memory_order_acquire);
  if (cb) {
    cb(s_overlayCallbackUserData.load(std::memory_order_acquire));
  }
}

extern "C" RIMGUI_EXPORT int remixapi_imgui_HasOverlayCallback() {
  return s_overlayCallback.load(std::memory_order_acquire) != nullptr ? 1 : 0;
}

// --- Windows ---

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_Begin(const char *name, int *p_open, int flags) {
  if (!ImGui::GetCurrentContext() || !name)
    return 0;
  bool open = p_open ? (*p_open != 0) : true;
  bool result = ImGui::Begin(name, p_open ? &open : nullptr, (ImGuiWindowFlags)flags);
  if (p_open)
    *p_open = open ? 1 : 0;
  return result ? 1 : 0;
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_End(void) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::End();
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_BeginChild(const char *str_id, float w, float h, int border,
                                                                   int flags) {
  if (!ImGui::GetCurrentContext() || !str_id)
    return 0;
  return ImGui::BeginChild(str_id, ImVec2(w, h), border != 0, (ImGuiWindowFlags)flags) ? 1 : 0;
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_EndChild(void) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::EndChild();
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_SetNextWindowPos(float x, float y, int cond) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::SetNextWindowPos(ImVec2(x, y), (ImGuiCond)cond);
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_SetNextWindowSize(float w, float h, int cond) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::SetNextWindowSize(ImVec2(w, h), (ImGuiCond)cond);
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_SetNextWindowCollapsed(int collapsed, int cond) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::SetNextWindowCollapsed(collapsed != 0, (ImGuiCond)cond);
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_SetNextWindowFocus(void) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::SetNextWindowFocus();
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_GetWindowSize(float *out_w, float *out_h) {
  if (!ImGui::GetCurrentContext()) {
    if (out_w)
      *out_w = 0.0f;
    if (out_h)
      *out_h = 0.0f;
    return;
  }
  ImVec2 sz = ImGui::GetWindowSize();
  if (out_w)
    *out_w = sz.x;
  if (out_h)
    *out_h = sz.y;
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_GetWindowPos(float *out_x, float *out_y) {
  if (!ImGui::GetCurrentContext()) {
    if (out_x)
      *out_x = 0.0f;
    if (out_y)
      *out_y = 0.0f;
    return;
  }
  ImVec2 pos = ImGui::GetWindowPos();
  if (out_x)
    *out_x = pos.x;
  if (out_y)
    *out_y = pos.y;
}

// --- Layout ---

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_SameLine(float offset_from_start_x, float spacing) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::SameLine(offset_from_start_x, spacing);
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_Separator(void) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::Separator();
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_Spacing(void) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::Spacing();
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_Dummy(float w, float h) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::Dummy(ImVec2(w, h));
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_Indent(float indent_w) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::Indent(indent_w);
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_Unindent(float indent_w) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::Unindent(indent_w);
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_NewLine(void) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::NewLine();
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_BeginGroup(void) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::BeginGroup();
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_EndGroup(void) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::EndGroup();
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_GetContentRegionAvail(float *out_w, float *out_h) {
  if (!ImGui::GetCurrentContext()) {
    if (out_w)
      *out_w = 0.0f;
    if (out_h)
      *out_h = 0.0f;
    return;
  }
  ImVec2 avail = ImGui::GetContentRegionAvail();
  if (out_w)
    *out_w = avail.x;
  if (out_h)
    *out_h = avail.y;
}

// --- ID Stack ---

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_PushID_Str(const char *str_id) {
  if (!ImGui::GetCurrentContext() || !str_id)
    return;
  ImGui::PushID(str_id);
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_PushID_Int(int int_id) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::PushID(int_id);
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_PopID(void) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::PopID();
}

// --- Text ---

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_Text(const char *text) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::TextUnformatted(text ? text : "");
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_TextColored(float r, float g, float b, float a,
                                                                     const char *text) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(r, g, b, a));
  ImGui::TextUnformatted(text ? text : "");
  ImGui::PopStyleColor();
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_TextWrapped(const char *text) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted(text ? text : "");
  ImGui::PopTextWrapPos();
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_BulletText(const char *text) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::Bullet();
  ImGui::TextUnformatted(text ? text : "");
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_LabelText(const char *label, const char *text) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::LabelText(label ? label : "", "%s", text ? text : "");
}

// --- Controls ---

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_Button(const char *label, float w, float h) {
  if (!ImGui::GetCurrentContext())
    return 0;
  return ImGui::Button(label ? label : "", ImVec2(w, h)) ? 1 : 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_SmallButton(const char *label) {
  if (!ImGui::GetCurrentContext())
    return 0;
  return ImGui::SmallButton(label ? label : "") ? 1 : 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_Checkbox(const char *label, int *v) {
  if (!ImGui::GetCurrentContext())
    return 0;
  bool val = v ? (*v != 0) : false;
  bool result = ImGui::Checkbox(label ? label : "", &val);
  if (v)
    *v = val ? 1 : 0;
  return result ? 1 : 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_RadioButton(const char *label, int active) {
  if (!ImGui::GetCurrentContext())
    return 0;
  return ImGui::RadioButton(label ? label : "", active != 0) ? 1 : 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_SliderFloat(const char *label, float *v, float v_min,
                                                                    float v_max, const char *format) {
  if (!ImGui::GetCurrentContext() || !v)
    return 0;
  return ImGui::SliderFloat(label ? label : "", v, v_min, v_max, format ? format : "%.3f") ? 1 : 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_SliderInt(const char *label, int *v, int v_min, int v_max,
                                                                  const char *format) {
  if (!ImGui::GetCurrentContext() || !v)
    return 0;
  return ImGui::SliderInt(label ? label : "", v, v_min, v_max, format ? format : "%d") ? 1 : 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_SliderFloat2(const char *label, float *v, float v_min,
                                                                     float v_max, const char *format) {
  if (!ImGui::GetCurrentContext() || !v)
    return 0;
  return ImGui::SliderFloat2(label ? label : "", v, v_min, v_max, format ? format : "%.3f") ? 1 : 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_SliderFloat3(const char *label, float *v, float v_min,
                                                                     float v_max, const char *format) {
  if (!ImGui::GetCurrentContext() || !v)
    return 0;
  return ImGui::SliderFloat3(label ? label : "", v, v_min, v_max, format ? format : "%.3f") ? 1 : 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_DragFloat(const char *label, float *v, float v_speed,
                                                                  float v_min, float v_max, const char *format) {
  if (!ImGui::GetCurrentContext() || !v)
    return 0;
  return ImGui::DragFloat(label ? label : "", v, v_speed, v_min, v_max, format ? format : "%.3f") ? 1 : 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_DragInt(const char *label, int *v, float v_speed, int v_min,
                                                                int v_max, const char *format) {
  if (!ImGui::GetCurrentContext() || !v)
    return 0;
  return ImGui::DragInt(label ? label : "", v, v_speed, v_min, v_max, format ? format : "%d") ? 1 : 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_InputFloat(const char *label, float *v, float step,
                                                                   float step_fast, const char *format) {
  if (!ImGui::GetCurrentContext() || !v)
    return 0;
  return ImGui::InputFloat(label ? label : "", v, step, step_fast, format ? format : "%.3f") ? 1 : 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_InputInt(const char *label, int *v, int step, int step_fast) {
  if (!ImGui::GetCurrentContext() || !v)
    return 0;
  return ImGui::InputInt(label ? label : "", v, step, step_fast) ? 1 : 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_InputText(const char *label, char *buf, uint32_t buf_size,
                                                                  int flags) {
  if (!ImGui::GetCurrentContext() || !buf)
    return 0;
  return ImGui::InputText(label ? label : "", buf, (size_t)buf_size, (ImGuiInputTextFlags)flags) ? 1 : 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_ColorEdit3(const char *label, float *col) {
  if (!ImGui::GetCurrentContext() || !col)
    return 0;
  return ImGui::ColorEdit3(label ? label : "", col) ? 1 : 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_ColorEdit4(const char *label, float *col, int flags) {
  if (!ImGui::GetCurrentContext() || !col)
    return 0;
  return ImGui::ColorEdit4(label ? label : "", col, (ImGuiColorEditFlags)flags) ? 1 : 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_ColorPicker3(const char *label, float *col) {
  if (!ImGui::GetCurrentContext() || !col)
    return 0;
  return ImGui::ColorPicker3(label ? label : "", col) ? 1 : 0;
}

// --- Combo ---

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_BeginCombo(const char *label, const char *preview_value,
                                                                   int flags) {
  if (!ImGui::GetCurrentContext())
    return 0;
  return ImGui::BeginCombo(label ? label : "", preview_value, (ImGuiComboFlags)flags) ? 1 : 0;
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_EndCombo(void) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::EndCombo();
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_Selectable(const char *label, int selected, int flags, float w,
                                                                   float h) {
  if (!ImGui::GetCurrentContext())
    return 0;
  return ImGui::Selectable(label ? label : "", selected != 0, (ImGuiSelectableFlags)flags, ImVec2(w, h)) ? 1 : 0;
}

// --- Trees ---

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_CollapsingHeader(const char *label, int flags) {
  if (!ImGui::GetCurrentContext())
    return 0;
  return ImGui::CollapsingHeader(label ? label : "", (ImGuiTreeNodeFlags)flags) ? 1 : 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_TreeNode(const char *label) {
  if (!ImGui::GetCurrentContext())
    return 0;
  return ImGui::TreeNode(label ? label : "") ? 1 : 0;
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_TreePop(void) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::TreePop();
}

// --- Tabs ---

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_BeginTabBar(const char *str_id, int flags) {
  if (!ImGui::GetCurrentContext())
    return 0;
  return ImGui::BeginTabBar(str_id ? str_id : "", (ImGuiTabBarFlags)flags) ? 1 : 0;
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_EndTabBar(void) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::EndTabBar();
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_BeginTabItem(const char *label, int *p_open, int flags) {
  if (!ImGui::GetCurrentContext())
    return 0;
  bool open = p_open ? (*p_open != 0) : true;
  bool result = ImGui::BeginTabItem(label ? label : "", p_open ? &open : nullptr, (ImGuiTabItemFlags)flags);
  if (p_open)
    *p_open = open ? 1 : 0;
  return result ? 1 : 0;
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_EndTabItem(void) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::EndTabItem();
}

// --- Tables ---

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_BeginTable(const char *str_id, int column, int flags,
                                                                   float outer_w, float outer_h, float inner_width) {
  if (!ImGui::GetCurrentContext())
    return 0;
  return ImGui::BeginTable(str_id ? str_id : "", column, (ImGuiTableFlags)flags, ImVec2(outer_w, outer_h), inner_width)
             ? 1
             : 0;
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_EndTable(void) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::EndTable();
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_TableNextRow(int row_flags, float min_row_height) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::TableNextRow((ImGuiTableRowFlags)row_flags, min_row_height);
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_TableNextColumn(void) {
  if (!ImGui::GetCurrentContext())
    return 0;
  return ImGui::TableNextColumn() ? 1 : 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_TableSetColumnIndex(int column_n) {
  if (!ImGui::GetCurrentContext())
    return 0;
  return ImGui::TableSetColumnIndex(column_n) ? 1 : 0;
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_TableSetupColumn(const char *label, int flags,
                                                                          float init_width_or_weight,
                                                                          uint32_t user_id) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::TableSetupColumn(label ? label : "", (ImGuiTableColumnFlags)flags, init_width_or_weight, (ImGuiID)user_id);
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_TableHeadersRow(void) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::TableHeadersRow();
}

// --- Tooltips ---

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_BeginTooltip(void) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::BeginTooltip();
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_EndTooltip(void) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::EndTooltip();
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_SetTooltip(const char *text) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::SetTooltip("%s", text ? text : "");
}

// --- Misc ---

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_ProgressBar(float fraction, float w, float h,
                                                                     const char *overlay) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::ProgressBar(fraction, ImVec2(w, h), overlay);
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_IsItemHovered(int flags) {
  if (!ImGui::GetCurrentContext())
    return 0;
  return ImGui::IsItemHovered((ImGuiHoveredFlags)flags) ? 1 : 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_IsItemClicked(int mouse_button) {
  if (!ImGui::GetCurrentContext())
    return 0;
  return ImGui::IsItemClicked((ImGuiMouseButton)mouse_button) ? 1 : 0;
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_SetItemDefaultFocus(void) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::SetItemDefaultFocus();
}

// --- Style ---

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_PushStyleColor(int idx, float r, float g, float b, float a) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::PushStyleColor((ImGuiCol)idx, ImVec4(r, g, b, a));
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_PopStyleColor(int count) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::PopStyleColor(count);
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_PushStyleVar_Float(int idx, float val) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::PushStyleVar((ImGuiStyleVar)idx, val);
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_PushStyleVar_Vec2(int idx, float x, float y) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::PushStyleVar((ImGuiStyleVar)idx, ImVec2(x, y));
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_PopStyleVar(int count) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::PopStyleVar(count);
}

// --- Plotting (ImPlot) Stubs ---

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_PlotBeginPlot(const char *title_id, float w, float h,
                                                                      int flags) {
  return 0;
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_PlotEndPlot(void) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_PlotPlotLine(const char *label_id, const float *values,
                                                                      int count) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_PlotPlotBars(const char *label_id, const float *values,
                                                                      int count, double bar_size) {}

// --- DrawList ---

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_DrawList_AddLine(float x1, float y1, float x2, float y2,
                                                                          uint32_t col, float thickness) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::GetForegroundDrawList()->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), col, thickness);
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_DrawList_AddRect(float x1, float y1, float x2, float y2,
                                                                          uint32_t col, float rounding,
                                                                          float thickness) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::GetForegroundDrawList()->AddRect(ImVec2(x1, y1), ImVec2(x2, y2), col, rounding, 0, thickness);
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_DrawList_AddRectFilled(float x1, float y1, float x2, float y2,
                                                                                uint32_t col, float rounding) {
  if (!ImGui::GetCurrentContext())
    return;
  ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(x1, y1), ImVec2(x2, y2), col, rounding);
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_DrawList_AddText(float x, float y, uint32_t col,
                                                                          const char *text) {
  if (!ImGui::GetCurrentContext() || !text)
    return;
  ImGui::GetForegroundDrawList()->AddText(ImVec2(x, y), col, text);
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_GetDisplaySize(float *out_w, float *out_h) {
  if (!ImGui::GetCurrentContext()) {
    if (out_w)
      *out_w = 0.0f;
    if (out_h)
      *out_h = 0.0f;
    return;
  }
  const auto &io = ImGui::GetIO();
  if (out_w)
    *out_w = io.DisplaySize.x;
  if (out_h)
    *out_h = io.DisplaySize.y;
}
