#pragma once
#include <imgui.h>

struct ImGuiWindow;

// Blender-like controls (ui/widgets.cpp). The capitalized functions are property rows: the label in
// a right-aligned column on the left and the control filling the rest, stacked when the panel is too
// narrow. The lowercase ones draw only the control, under an ID ("##..."), for custom layouts.
namespace remix::ui {
  // Draws the label column of a row and calls draw(id) for the control beside it.
  bool field(const char *label, bool (*draw)(const char *id, void *user), void *user);
  template <typename Draw> bool field(const char *label, Draw draw) {
    return field(label, [](const char *id, void *user) { return (*static_cast<Draw *>(user))(id); }, &draw);
  }

  // A number field: drag to scrub (Shift: finer, Ctrl: snap), click or Ctrl+click to type, Escape
  // reverts; the arrows at its ends step. An empty or inverted range does not clamp, as in ImGui.
  bool dragScalar(const char *id, ImGuiDataType type, void *value, float speed, const void *min = nullptr,
                  const void *max = nullptr, const char *format = nullptr, ImGuiSliderFlags flags = 0);
  // A slider: its fill shows the value in [min, max]; dragging moves it relative to the press.
  bool sliderScalar(const char *id, ImGuiDataType type, void *value, const void *min, const void *max,
                    const char *format = nullptr, ImGuiSliderFlags flags = 0);
  // Components stacked as one aligned column, each labeled X, Y, Z, W.
  bool dragScalarN(const char *id, ImGuiDataType type, void *value, int components, float speed,
                   const void *min = nullptr, const void *max = nullptr, const char *format = nullptr,
                   ImGuiSliderFlags flags = 0);
  bool sliderScalarN(const char *id, ImGuiDataType type, void *value, int components, const void *min, const void *max,
                     const char *format = nullptr, ImGuiSliderFlags flags = 0);
  // A menu button whose popup lists the items.
  bool combo(const char *id, int *current, const char *(*item)(void *user, int index), void *user, int count);
  bool combo(const char *id, int *current, const char *itemsSeparatedByZeros);
  bool combo(const char *id, int *current, const char *const items[], int count);
  // A color swatch; clicking it opens a picker.
  bool colorSwatch(const char *id, float rgb[3], ImGuiColorEditFlags flags = 0);
  // An option box followed by the label's visible text.
  bool checkbox(const char *label, bool *value);

  inline bool SliderFloat(const char *label, float *v, float min, float max, const char *format = "%.3f",
                          ImGuiSliderFlags flags = 0) {
    return field(label,
                 [&](const char *id) { return sliderScalar(id, ImGuiDataType_Float, v, &min, &max, format, flags); });
  }
  inline bool SliderInt(const char *label, int *v, int min, int max, const char *format = "%d",
                        ImGuiSliderFlags flags = 0) {
    return field(label,
                 [&](const char *id) { return sliderScalar(id, ImGuiDataType_S32, v, &min, &max, format, flags); });
  }
  inline bool SliderFloat3(const char *label, float v[3], float min, float max, const char *format = "%.3f",
                           ImGuiSliderFlags flags = 0) {
    return field(
        label, [&](const char *id) { return sliderScalarN(id, ImGuiDataType_Float, v, 3, &min, &max, format, flags); });
  }
  inline bool DragFloat(const char *label, float *v, float speed = 1, float min = 0, float max = 0,
                        const char *format = "%.3f", ImGuiSliderFlags flags = 0) {
    return field(label, [&](const char *id) {
      return dragScalar(id, ImGuiDataType_Float, v, speed, &min, &max, format, flags);
    });
  }
  inline bool DragInt(const char *label, int *v, float speed = 1, int min = 0, int max = 0, const char *format = "%d",
                      ImGuiSliderFlags flags = 0) {
    return field(
        label, [&](const char *id) { return dragScalar(id, ImGuiDataType_S32, v, speed, &min, &max, format, flags); });
  }
  inline bool DragFloat3(const char *label, float v[3], float speed = 1, float min = 0, float max = 0,
                         const char *format = "%.3f", ImGuiSliderFlags flags = 0) {
    return field(label, [&](const char *id) {
      return dragScalarN(id, ImGuiDataType_Float, v, 3, speed, &min, &max, format, flags);
    });
  }
  template <typename Items, typename... Count>
  bool Combo(const char *label, int *current, Items items, Count... count) {
    return field(label, [&](const char *id) { return combo(id, current, items, count...); });
  }
  inline bool ColorEdit3(const char *label, float rgb[3], ImGuiColorEditFlags flags = 0) {
    return field(label, [&](const char *id) { return colorSwatch(id, rgb, flags); });
  }
  template <typename... Args> bool InputFloat(const char *label, Args... args) {
    return field(label, [&](const char *id) { return ImGui::InputFloat(id, args...); });
  }
  template <typename... Args> bool InputText(const char *label, Args... args) {
    return field(label, [&](const char *id) { return ImGui::InputText(id, args...); });
  }
  // The option box sits in the control column, the label after it.
  bool Checkbox(const char *label, bool *value);

  // Property search (drawPanel's search box). While a search is set, rows whose label lacks one of
  // its words are hidden, panels show only while they hold a match (open, whatever their state),
  // and a panel whose title matches shows all of its rows. nullptr or "" ends the search.
  void setSearch(const char *text);
  // Rows and panel titles that matched since setSearch().
  int searchMatches();

  // A Blender panel: a header with a disclosure triangle; while it is open, what its scope draws is
  // its contents. A panel inside another is a subpanel (an indented header, no backdrop of its own).
  //   if (ui::Panel panel{"Name", ImGuiTreeNodeFlags_DefaultOpen}) { ... }
  // or, to return early while it is closed: ui::Panel panel{"Name"}; if (!panel) return;
  class Panel {
  public:
    explicit Panel(const char *label, ImGuiTreeNodeFlags flags = 0);
    ~Panel();
    Panel(const Panel &) = delete;
    Panel &operator=(const Panel &) = delete;
    explicit operator bool() const { return m_open; }

  private:
    bool m_open = false, m_backdrop = false, m_scoped = false;
    float m_indent = 0;
    // During a search: the panel's place in the search state, and, for a panel without matches,
    // the layout its hidden (clipped) contents leave as they found it.
    int m_searchDepth = -1;
    bool m_hidden = false;
    ImVec2 m_cursor, m_cursorMax, m_lineSize, m_prevLine, m_prevLineSize;
    // A top-level panel's backdrop: drawn at last frame's height under the header, its bottom
    // vertices moved to where the contents end.
    ImGuiWindow *m_window = nullptr;
    ImGuiID m_id = 0;
    int m_vtxBegin = 0, m_vtxEnd = 0;
    float m_top = 0, m_guessBottom = 0;
  };
} // namespace remix::ui
