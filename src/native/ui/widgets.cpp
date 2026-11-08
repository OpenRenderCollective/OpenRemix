#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/widgets.h"
#include "ui/theme.h"
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>

namespace remix::ui {
  namespace {
    float scale() { return ImGui::GetStyle().FontScaleDpi; }
    ImU32 color(const ImVec4 &value) { return ImGui::GetColorU32(value); }

    // --- Values -------------------------------------------------------------------------------------
    // Number widgets work in double and store through the data type.

    bool isFloat(ImGuiDataType type) { return type == ImGuiDataType_Float || type == ImGuiDataType_Double; }

    double load(ImGuiDataType type, const void *data) {
      switch (type) {
      case ImGuiDataType_S8:
        return *static_cast<const int8_t *>(data);
      case ImGuiDataType_U8:
        return *static_cast<const uint8_t *>(data);
      case ImGuiDataType_S16:
        return *static_cast<const int16_t *>(data);
      case ImGuiDataType_U16:
        return *static_cast<const uint16_t *>(data);
      case ImGuiDataType_S32:
        return *static_cast<const int32_t *>(data);
      case ImGuiDataType_U32:
        return *static_cast<const uint32_t *>(data);
      case ImGuiDataType_S64:
        return double(*static_cast<const int64_t *>(data));
      case ImGuiDataType_U64:
        return double(*static_cast<const uint64_t *>(data));
      case ImGuiDataType_Float:
        return *static_cast<const float *>(data);
      case ImGuiDataType_Double:
        return *static_cast<const double *>(data);
      default:
        return 0;
      }
    }

    template <typename T> bool storeAs(void *data, double value) {
      T stored;
      if constexpr (std::is_floating_point_v<T>)
        stored = T(value);
      else
        stored = T(std::clamp(std::round(value), double(std::numeric_limits<T>::lowest()),
                              double(std::numeric_limits<T>::max())));
      if (std::memcmp(data, &stored, sizeof(T)) == 0)
        return false;
      std::memcpy(data, &stored, sizeof(T));
      return true;
    }

    // Whether the stored value changed.
    bool store(ImGuiDataType type, void *data, double value) {
      switch (type) {
      case ImGuiDataType_S8:
        return storeAs<int8_t>(data, value);
      case ImGuiDataType_U8:
        return storeAs<uint8_t>(data, value);
      case ImGuiDataType_S16:
        return storeAs<int16_t>(data, value);
      case ImGuiDataType_U16:
        return storeAs<uint16_t>(data, value);
      case ImGuiDataType_S32:
        return storeAs<int32_t>(data, value);
      case ImGuiDataType_U32:
        return storeAs<uint32_t>(data, value);
      case ImGuiDataType_S64:
        return storeAs<int64_t>(data, value);
      case ImGuiDataType_U64:
        return storeAs<uint64_t>(data, value);
      case ImGuiDataType_Float:
        return storeAs<float>(data, value);
      case ImGuiDataType_Double:
        return storeAs<double>(data, value);
      default:
        return false;
      }
    }

    // A float rounded to the precision its format shows, as ImGui's drags and sliders do.
    double roundToFormat(double value, const char *format) {
      char trimmed[64], text[64];
      ImParseFormatTrimDecorations(format, trimmed, sizeof(trimmed));
      std::snprintf(text, sizeof(text), trimmed, value);
      return std::strtod(text, nullptr);
    }

    struct Range {
      double min = 0, max = 0;
      bool bounded = false; // min < max: drags clamp, sliders map
    };
    Range range(ImGuiDataType type, const void *min, const void *max) {
      Range r;
      if (min && max) {
        r.min = load(type, min);
        r.max = load(type, max);
        r.bounded = r.min < r.max;
      }
      return r;
    }

    // Logarithmic sliders map the ratio exponentially; ranges reaching zero use a small epsilon
    // the size of the format's last shown digit.
    double logEpsilon(const char *format) { return std::pow(.1, std::max(ImParseFormatPrecision(format, 3), 1)); }
    double ratioOf(double value, const Range &r, bool logarithmic, const char *format) {
      if (!r.bounded)
        return 0;
      if (logarithmic && r.min >= 0) {
        const double lo = std::max(r.min, logEpsilon(format)), v = std::clamp(value, lo, r.max);
        return r.max <= lo ? 0 : std::log(v / lo) / std::log(r.max / lo);
      }
      return std::clamp((value - r.min) / (r.max - r.min), 0.0, 1.0);
    }
    double valueOf(double ratio, const Range &r, bool logarithmic, const char *format) {
      ratio = std::clamp(ratio, 0.0, 1.0);
      if (logarithmic && r.min >= 0) {
        const double lo = std::max(r.min, logEpsilon(format));
        return ratio <= 0 && r.min < lo ? r.min : lo * std::pow(r.max / lo, ratio);
      }
      return r.min + (r.max - r.min) * ratio;
    }

    // Ctrl snapping: a tenth of the range's magnitude for sliders, ten steps for drags.
    double snap(double value, double step) { return step > 0 ? std::round(value / step) * step : value; }
    double sliderSnapStep(const Range &r, ImGuiDataType type) {
      const double step = std::pow(10.0, std::floor(std::log10(std::max(r.max - r.min, 1e-9))) - 1);
      return isFloat(type) ? step : std::max(1.0, std::round(step));
    }

    // --- The drag interaction shared by number fields and sliders -----------------------------------

    struct Drag {
      ImGuiID id = 0;
      double start = 0, travel = 0; // the value at the press; pointer travel in pixels (Shift-scaled)
      bool moved = false;
    };
    Drag &drag() {
      static thread_local Drag state;
      return state;
    }

    enum class Kind { Number, Slider };

    struct Number {
      Kind kind;
      ImGuiDataType type;
      void *data;
      double speed;
      Range range;
      const char *format;
      ImGuiSliderFlags flags;
      ImDrawFlags corners;
      const char *prefix; // a vector component's name, drawn on the left
    };

    void drawChevron(ImDrawList *list, ImVec2 center, float size, ImGuiDir dir, ImU32 col) {
      const float h = size * .5f, t = 1.25f * scale();
      if (dir == ImGuiDir_Left || dir == ImGuiDir_Right) {
        const float s = dir == ImGuiDir_Left ? 1.f : -1.f;
        const ImVec2 points[] = {ImVec2(center.x + s * h * .5f, center.y - h), ImVec2(center.x - s * h * .5f, center.y),
                                 ImVec2(center.x + s * h * .5f, center.y + h)};
        list->AddPolyline(points, 3, col, ImDrawFlags_None, t);
      } else {
        const float s = dir == ImGuiDir_Up ? 1.f : -1.f;
        const ImVec2 points[] = {ImVec2(center.x - h, center.y + s * h * .5f), ImVec2(center.x, center.y - s * h * .5f),
                                 ImVec2(center.x + h, center.y + s * h * .5f)};
        list->AddPolyline(points, 3, col, ImDrawFlags_None, t);
      }
    }

    bool numberWidget(const char *label, const Number &n, ImVec2 size) {
      ImGuiWindow *window = ImGui::GetCurrentWindow();
      if (window->SkipItems)
        return false;
      ImGuiContext &g = *GImGui;
      const ImGuiID id = window->GetID(label);
      const ImRect bb(window->DC.CursorPos, window->DC.CursorPos + size);
      const bool allowInput = !(n.flags & ImGuiSliderFlags_NoInput);
      ImGui::ItemSize(bb, g.Style.FramePadding.y);
      if (!ImGui::ItemAdd(bb, id, &bb, allowInput ? ImGuiItemFlags_Inputable : 0))
        return false;
      const char *format = n.format ? n.format : ImGui::DataTypeGetInfo(n.type)->PrintFmt;
      const bool hovered = ImGui::ItemHoverable(bb, id, g.LastItemData.ItemFlags);
      const bool logarithmic = (n.flags & ImGuiSliderFlags_Logarithmic) != 0;
      const float arrow = std::min(size.y * .8f, size.x * .2f);
      auto &state = drag();
      bool editing = allowInput && ImGui::TempInputIsActive(id), changed = false;

      if (!editing) {
        const bool clicked = hovered && ImGui::IsMouseClicked(0, ImGuiInputFlags_None, id);
        const bool doubleClicked =
            hovered && g.IO.MouseClickedCount[0] == 2 && ImGui::TestKeyOwner(ImGuiKey_MouseLeft, id);
        if (clicked || doubleClicked)
          ImGui::SetKeyOwner(ImGuiKey_MouseLeft, id);
        const bool navInput = g.NavActivateId == id && (g.NavActivateFlags & ImGuiActivateFlags_PreferInput);
        if (allowInput && ((clicked && g.IO.KeyCtrl) || doubleClicked || navInput)) {
          editing = true;
        } else if (clicked) {
          ImGui::SetActiveID(id, window);
          ImGui::SetFocusID(id, window);
          ImGui::FocusWindow(window);
          state = {id, load(n.type, n.data), 0, false};
          std::memcpy(&g.ActiveIdValueOnActivation, n.data, ImGui::DataTypeGetInfo(n.type)->Size);
        }
      }

      if (!editing && g.ActiveId == id && state.id == id) {
        if (g.IO.MouseDown[0]) {
          const float threshold = g.IO.MouseDragThreshold * scale();
          if (!state.moved && ImGui::IsMouseDragPastThreshold(0, threshold))
            state.moved = true;
          else if (state.moved)
            state.travel += g.IO.MouseDelta.x * (g.IO.KeyShift ? (n.kind == Kind::Slider ? .05f : .1f) : 1.f);
          if (state.moved) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            double value;
            if (n.kind == Kind::Slider && n.range.bounded) {
              const double ratio =
                  ratioOf(state.start, n.range, logarithmic, format) + state.travel / std::max(1.f, size.x);
              value = valueOf(ratio, n.range, logarithmic, format);
              if (g.IO.KeyCtrl)
                value = snap(value, sliderSnapStep(n.range, n.type));
            } else if (logarithmic && state.start > 0) {
              value = state.start * std::exp(state.travel * .01); // e per 100 pixels
            } else {
              value = state.start + state.travel * n.speed;
              if (g.IO.KeyCtrl)
                value = snap(value, n.speed * 10);
            }
            if (isFloat(n.type) && !(n.flags & ImGuiSliderFlags_NoRoundToFormat))
              value = roundToFormat(value, format);
            if (n.range.bounded)
              value = std::clamp(value, n.range.min, n.range.max);
            changed = store(n.type, n.data, value);
          }
          if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            changed = store(n.type, n.data, state.start);
            ImGui::ClearActiveID();
          }
        } else {
          if (!state.moved && hovered) {
            const float x = g.IO.MousePos.x;
            const int step = n.kind == Kind::Number ? (x < bb.Min.x + arrow ? -1 : x > bb.Max.x - arrow ? 1 : 0) : 0;
            if (step) {
              double value =
                  load(n.type, n.data) + step * (isFloat(n.type) ? n.speed : std::max(1.0, std::round(n.speed)));
              if (n.range.bounded)
                value = std::clamp(value, n.range.min, n.range.max);
              changed = store(n.type, n.data, value);
            } else if (allowInput) {
              g.NavActivateId = id;
              g.NavActivateFlags = ImGuiActivateFlags_PreferInput;
              editing = true;
            }
          }
          if (!editing)
            ImGui::ClearActiveID();
        }
      }

      if (editing) {
        const bool clamp = n.range.bounded && (n.flags & ImGuiSliderFlags_ClampOnInput);
        alignas(8) unsigned char min[8] = {}, max[8] = {};
        store(n.type, min, n.range.min);
        store(n.type, max, n.range.max);
        return ImGui::TempInputScalar(bb, id, label, n.type, n.data, format, clamp ? min : nullptr,
                                      clamp ? max : nullptr);
      }
      if (changed)
        ImGui::MarkItemEdited(id);

      // Draw.
      auto *list = window->DrawList;
      const bool active = g.ActiveId == id;
      const float rounding = theme::rounding * scale();
      ImVec4 inner = active && state.moved && n.kind == Kind::Number ? theme::fieldActive
                     : hovered || active                             ? theme::fieldHover
                                                                     : theme::field;
      ImGui::RenderNavCursor(bb, id);
      list->AddRectFilled(bb.Min, bb.Max, color(inner), rounding, n.corners);
      if (n.kind == Kind::Slider && n.range.bounded) {
        const float fill =
            bb.Min.x + float(ratioOf(load(n.type, n.data), n.range, logarithmic, format)) * bb.GetWidth();
        if (fill > bb.Min.x) {
          list->PushClipRect(bb.Min, ImVec2(fill, bb.Max.y), true);
          list->AddRectFilled(bb.Min, bb.Max,
                              color(hovered || active ? theme::colors().accentHover : theme::colors().accent), rounding,
                              n.corners);
          list->PopClipRect();
        }
      }
      const bool arrows = n.kind == Kind::Number && hovered && !active && size.x > arrow * 4;
      if (arrows) {
        const float c = bb.GetCenter().y, w = size.y * .45f;
        drawChevron(list, ImVec2(bb.Min.x + arrow * .5f, c), w, ImGuiDir_Left, color(theme::text));
        drawChevron(list, ImVec2(bb.Max.x - arrow * .5f, c), w, ImGuiDir_Right, color(theme::text));
      }
      char text[64];
      const char *end = text + ImGui::DataTypeFormatString(text, int(sizeof(text)), n.type, n.data, format);
      const float pad = (arrows || n.kind == Kind::Number ? arrow : size.y * .4f);
      const ImVec2 min(bb.Min.x + pad, bb.Min.y), max(bb.Max.x - pad, bb.Max.y);
      ImGui::PushStyleColor(ImGuiCol_Text, n.kind == Kind::Slider ? theme::textSelected : theme::text);
      if (n.prefix) {
        ImGui::RenderTextClipped(ImVec2(bb.Min.x + size.y * .4f, min.y), max, n.prefix, nullptr, nullptr,
                                 ImVec2(0, .5f));
        ImGui::RenderTextClipped(min, ImVec2(bb.Max.x - size.y * .4f, max.y), text, end, nullptr, ImVec2(1, .5f));
      } else {
        ImGui::RenderTextClipped(min, max, text, end, nullptr, ImVec2(.5f, .5f));
      }
      ImGui::PopStyleColor();
      return changed;
    }

    ImVec2 controlSize() { return ImVec2(ImGui::CalcItemWidth(), ImGui::GetFrameHeight()); }

    bool numberColumn(const char *id, Kind kind, ImGuiDataType type, void *value, int components, float speed,
                      const void *min, const void *max, const char *format, ImGuiSliderFlags flags) {
      static const char *const names[] = {"X", "Y", "Z", "W"};
      const size_t stride = ImGui::DataTypeGetInfo(type)->Size;
      const ImVec2 size = controlSize();
      bool changed = false;
      ImGui::BeginGroup();
      ImGui::PushID(id);
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, std::round(scale())));
      for (int i = 0; i < components; ++i) {
        ImDrawFlags corners = ImDrawFlags_RoundCornersNone;
        if (i == 0)
          corners |= ImDrawFlags_RoundCornersTop;
        if (i == components - 1)
          corners |= ImDrawFlags_RoundCornersBottom;
        ImGui::PushID(i);
        const Number n{kind,
                       type,
                       static_cast<unsigned char *>(value) + stride * size_t(i),
                       speed,
                       range(type, min, max),
                       format,
                       flags,
                       corners,
                       i < 4 ? names[i] : nullptr};
        changed |= numberWidget("##component", n, size);
        ImGui::PopID();
      }
      ImGui::PopStyleVar();
      ImGui::PopID();
      ImGui::EndGroup();
      return changed;
    }

    // --- Panels --------------------------------------------------------------------------------------

    // How many panels enclose what is drawn now: a panel inside another is a subpanel.
    int &panelDepth() {
      static thread_local int depth = 0;
      return depth;
    }

    // --- Property search -----------------------------------------------------------------------------

    struct Search {
      static constexpr int maxDepth = 16;
      std::string text; // lower case; empty: no search
      int matches = 0;
      // Per enclosing panel (by depth): matches found inside it, and whether its title matched
      // (its own or an enclosing panel's), which shows all of its rows.
      int counts[maxDepth] = {};
      bool showAll[maxDepth] = {};
    };
    Search &search() {
      static thread_local Search state;
      return state;
    }
    // The storage key of a panel's match count last frame.
    ImGuiID searchKey(ImGuiID id) { return id ^ 0x5ea4c4u; }
    // The storage key of a top-level panel's height last frame (its own id holds whether it is open).
    ImGuiID heightKey(ImGuiID id) { return id ^ 0x4e1647u; }

    // Whether the label holds every word of the search, ignoring case.
    bool matchesSearch(const char *label, const char *end) {
      const auto &text = search().text;
      const auto contains = [&](const char *word, size_t length) {
        for (const char *p = label; p + length <= end; ++p) {
          size_t i = 0;
          while (i < length && std::tolower((unsigned char)p[i]) == word[i])
            ++i;
          if (i == length)
            return true;
        }
        return false;
      };
      for (size_t start = 0; start < text.size();) {
        const size_t stop = std::min(text.find(' ', start), text.size());
        if (stop > start && !contains(text.data() + start, stop - start))
          return false;
        start = stop + 1;
      }
      return true;
    }
    // A match counts in every enclosing panel, which keeps them shown.
    void countMatch() {
      auto &state = search();
      ++state.matches;
      for (int i = 0; i < std::min(panelDepth(), Search::maxDepth); ++i)
        ++state.counts[i];
    }
    // Whether a row shows: with no search, or when it matches or its panel's title did.
    bool rowShown(const char *label, const char *end) {
      auto &state = search();
      if (state.text.empty())
        return true;
      if (matchesSearch(label, end)) {
        countMatch();
        return true;
      }
      const int depth = std::min(panelDepth(), Search::maxDepth);
      return depth > 0 && state.showAll[depth - 1];
    }
    // A hidden row leaves no last item: a tooltip check after it finds nothing hovered.
    void hideRow() { GImGui->LastItemData.StatusFlags = ImGuiItemStatusFlags_None; }
  } // namespace

  void setSearch(const char *text) {
    auto &state = search();
    state = {};
    for (const char *p = text; p && *p; ++p)
      state.text += char(std::tolower((unsigned char)*p));
    // Words only: no leading or trailing spaces.
    while (!state.text.empty() && state.text.back() == ' ')
      state.text.pop_back();
    state.text.erase(0, std::min(state.text.find_first_not_of(' '), state.text.size()));
  }
  int searchMatches() { return search().matches; }

  bool field(const char *label, bool (*draw)(const char *id, void *user), void *user) {
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
      return false;
    const auto &style = ImGui::GetStyle();
    const char *end = ImGui::FindRenderedTextEnd(label);
    if (!rowShown(label, end)) {
      hideRow();
      return false;
    }
    const float available = ImGui::GetContentRegionAvail().x;
    const float column = std::floor(available * theme::labelColumn);
    ImGui::PushID(label);
    bool changed;
    if (column < 70 * scale()) {
      // Too narrow for two columns: the label above the control.
      if (end != label)
        ImGui::TextWrapped("%.*s", int(end - label), label);
      ImGui::SetNextItemWidth(-FLT_MIN);
      changed = draw("##value", user);
    } else {
      // Right-aligned label lines, the first beside the control's text.
      const ImVec2 origin = window->DC.CursorPos;
      const float wrap = column - style.ItemInnerSpacing.x * 2;
      ImFont *font = ImGui::GetFont();
      const float fontSize = ImGui::GetFontSize();
      float y = origin.y + style.FramePadding.y;
      ImGui::PushStyleColor(ImGuiCol_Text, theme::text);
      for (const char *line = label; line < end;) {
        const char *lineEnd = font->CalcWordWrapPosition(fontSize, line, end, wrap);
        if (lineEnd == line)
          ++lineEnd;
        const char *trimmed = lineEnd;
        while (trimmed > line && trimmed[-1] == ' ')
          --trimmed;
        const float width = ImGui::CalcTextSize(line, trimmed).x;
        ImGui::RenderText(ImVec2(origin.x + column - style.ItemInnerSpacing.x - width, y), line, trimmed, false);
        y += fontSize + style.ItemSpacing.y;
        line = lineEnd;
        while (line < end && *line == ' ')
          ++line;
      }
      ImGui::PopStyleColor();
      const float labelBottom = y - style.ItemSpacing.y + style.FramePadding.y;
      window->DC.CursorPos = ImVec2(origin.x + column, origin.y);
      window->DC.CurrLineSize.y = 0;
      ImGui::SetNextItemWidth(available - column);
      changed = draw("##value", user);
      // A label longer than the control keeps the next row below it.
      if (window->DC.CursorPos.y < labelBottom + style.ItemSpacing.y) {
        window->DC.CursorPos.y = labelBottom + style.ItemSpacing.y;
        window->DC.CursorMaxPos.y = std::max(window->DC.CursorMaxPos.y, labelBottom);
      }
    }
    ImGui::PopID();
    return changed;
  }

  bool dragScalar(const char *id, ImGuiDataType type, void *value, float speed, const void *min, const void *max,
                  const char *format, ImGuiSliderFlags flags) {
    const Number n{Kind::Number, type, value, speed, range(type, min, max), format, flags, ImDrawFlags_RoundCornersAll,
                   nullptr};
    return numberWidget(id, n, controlSize());
  }
  bool sliderScalar(const char *id, ImGuiDataType type, void *value, const void *min, const void *max,
                    const char *format, ImGuiSliderFlags flags) {
    const Number n{Kind::Slider, type, value, 1, range(type, min, max), format, flags, ImDrawFlags_RoundCornersAll,
                   nullptr};
    return numberWidget(id, n, controlSize());
  }
  bool dragScalarN(const char *id, ImGuiDataType type, void *value, int components, float speed, const void *min,
                   const void *max, const char *format, ImGuiSliderFlags flags) {
    return numberColumn(id, Kind::Number, type, value, components, speed, min, max, format, flags);
  }
  bool sliderScalarN(const char *id, ImGuiDataType type, void *value, int components, const void *min, const void *max,
                     const char *format, ImGuiSliderFlags flags) {
    return numberColumn(id, Kind::Slider, type, value, components, 1, min, max, format, flags);
  }

  bool combo(const char *id, int *current, const char *(*item)(void *user, int index), void *user, int count) {
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
      return false;
    const ImVec2 size = controlSize(), pos = window->DC.CursorPos;
    const ImRect bb(pos, pos + size);
    auto *list = window->DrawList;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::menu);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, theme::menuHover);
    ImGui::SetNextItemWidth(size.x);
    const bool open = ImGui::BeginCombo(id, "", ImGuiComboFlags_NoArrowButton);
    ImGui::PopStyleColor(2);
    // The preview and the menu arrow, on the button the combo just drew.
    const char *preview = *current >= 0 && *current < count ? item(user, *current) : "";
    const float pad = ImGui::GetStyle().FramePadding.x, arrow = size.y * .35f;
    ImGui::RenderTextEllipsis(list, ImVec2(bb.Min.x + pad, bb.Min.y + ImGui::GetStyle().FramePadding.y),
                              ImVec2(bb.Max.x - pad - arrow * 2, bb.Max.y), bb.Max.x - pad - arrow * 2, preview,
                              nullptr, nullptr);
    drawChevron(list, ImVec2(bb.Max.x - pad - arrow * .5f, bb.GetCenter().y), arrow, ImGuiDir_Down, color(theme::text));
    if (!open)
      return false;
    bool changed = false;
    for (int i = 0; i < count; ++i) {
      ImGui::PushID(i);
      const bool selected = i == *current;
      if (ImGui::Selectable(item(user, i), selected) && !selected) {
        *current = i;
        changed = true;
      }
      if (selected)
        ImGui::SetItemDefaultFocus();
      ImGui::PopID();
    }
    ImGui::EndCombo();
    if (changed)
      ImGui::MarkItemEdited(window->GetID(id));
    return changed;
  }
  bool combo(const char *id, int *current, const char *const items[], int count) {
    return combo(
        id, current, [](void *user, int i) { return static_cast<const char *const *>(user)[i]; },
        const_cast<const char **>(items), count);
  }
  bool combo(const char *id, int *current, const char *itemsSeparatedByZeros) {
    // Items are looked up by walking the list; menus here have a handful of entries.
    int count = 0;
    for (const char *p = itemsSeparatedByZeros; *p; p += std::strlen(p) + 1)
      ++count;
    return combo(
        id, current,
        [](void *user, int i) {
          const char *p = static_cast<const char *>(user);
          while (i-- > 0)
            p += std::strlen(p) + 1;
          return p;
        },
        const_cast<char *>(itemsSeparatedByZeros), count);
  }

  bool colorSwatch(const char *id, float rgb[3], ImGuiColorEditFlags flags) {
    const ImVec2 size = controlSize();
    ImGui::PushID(id);
    const bool clicked =
        ImGui::ColorButton(id, ImVec4(rgb[0], rgb[1], rgb[2], 1), flags | ImGuiColorEditFlags_NoAlpha, size);
    if (clicked)
      ImGui::OpenPopup("picker");
    bool changed = false;
    if (ImGui::BeginPopup("picker")) {
      changed = ImGui::ColorPicker3("##picker", rgb,
                                    (flags & (ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)) |
                                        ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_PickerHueWheel);
      ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
  }

  bool checkbox(const char *label, bool *value) {
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
      return false;
    const auto &style = ImGui::GetStyle();
    const ImGuiID id = window->GetID(label);
    const char *end = ImGui::FindRenderedTextEnd(label);
    const float height = ImGui::GetFrameHeight(), box = std::round(height * .7f);
    const float textX = box + style.ItemInnerSpacing.x;
    const float wrap = std::max(1.f, ImGui::GetContentRegionAvail().x - textX);
    const ImVec2 text = end != label ? ImGui::CalcTextSize(label, end, false, wrap) : ImVec2(0, 0);
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect bb(pos, pos + ImVec2(textX + text.x, std::max(height, text.y + style.FramePadding.y * 2)));
    ImGui::ItemSize(bb, style.FramePadding.y);
    if (!ImGui::ItemAdd(bb, id))
      return false;
    bool hovered, held;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, ImGuiButtonFlags_PressedOnClick);
    if (pressed) {
      *value = !*value;
      ImGui::MarkItemEdited(id);
    }
    auto *list = window->DrawList;
    const ImVec2 boxMin(pos.x, pos.y + std::round((height - box) * .5f));
    const ImVec4 inner = *value ? (hovered ? theme::colors().accentHover : theme::colors().accent)
                                : (hovered ? theme::fieldHover : theme::field);
    ImGui::RenderNavCursor(bb, id);
    list->AddRectFilled(boxMin, boxMin + ImVec2(box, box), color(inner), theme::rounding * scale() * .75f);
    if (*value)
      ImGui::RenderCheckMark(list, boxMin + ImVec2(box * .2f, box * .2f), color(theme::textSelected), box * .6f);
    if (end != label)
      ImGui::RenderTextWrapped(ImVec2(pos.x + textX, pos.y + style.FramePadding.y), label, end, wrap);
    return pressed;
  }

  bool Checkbox(const char *label, bool *value) {
    if (!ImGui::GetCurrentWindow()->SkipItems && !rowShown(label, ImGui::FindRenderedTextEnd(label))) {
      hideRow();
      return false;
    }
    const float available = ImGui::GetContentRegionAvail().x;
    const float column = std::floor(available * theme::labelColumn);
    if (column >= 70 * scale())
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + column);
    return checkbox(label, value);
  }

  Panel::Panel(const char *label, ImGuiTreeNodeFlags flags) {
    const int depth = panelDepth()++;
    const bool subpanel = depth > 0;
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
      return;
    const auto &style = ImGui::GetStyle();
    const float s = scale(), rounding = theme::rounding * s;
    const ImGuiID id = window->GetID(label);
    m_window = window;
    m_id = id;
    bool open = ImGui::TreeNodeUpdateNextOpen(id, flags);
    auto &state = search();
    if (!state.text.empty() && depth < Search::maxDepth) {
      // Searching: shown, and open, while it holds a match (last frame's count) or its title
      // matches; otherwise its contents still run, clipped away, to count their matches.
      m_searchDepth = depth;
      const bool titleMatch = matchesSearch(label, ImGui::FindRenderedTextEnd(label));
      if (titleMatch)
        countMatch();
      state.showAll[depth] = titleMatch || (depth > 0 && state.showAll[depth - 1]);
      state.counts[depth] = 0;
      if (!state.showAll[depth] && window->StateStorage.GetInt(searchKey(id), 0) == 0) {
        m_hidden = m_open = true;
        m_cursor = window->DC.CursorPos;
        m_cursorMax = window->DC.CursorMaxPos;
        m_lineSize = window->DC.CurrLineSize;
        m_prevLine = window->DC.CursorPosPrevLine;
        m_prevLineSize = window->DC.PrevLineSize;
        ImGui::PushClipRect(m_cursor, m_cursor, false);
        ImGui::PushID(id);
        m_scoped = true;
        return;
      }
      open = true;
    }
    // Panels reach into the window padding (up to the half ImGui leaves unclipped), so their
    // contents line up with rows outside panels; subpanels sit inside their panel's contents.
    const float inset = subpanel ? 0 : std::min(theme::panelPadding * s, std::floor(window->WindowPadding.x * .5f));
    if (window->DC.CursorPos.y > window->DC.CursorStartPos.y)
      window->DC.CursorPos.y += (subpanel ? theme::panelGap * .5f : theme::panelGap) * s;
    const ImVec2 pos = window->DC.CursorPos;
    const float headerHeight = std::round(ImGui::GetFrameHeight() * (subpanel ? 1.f : 1.2f));
    const ImRect bb(ImVec2(pos.x - inset, pos.y), ImVec2(window->WorkRect.Max.x + inset, pos.y + headerHeight));
    ImGui::ItemSize(ImVec2(bb.GetWidth() - inset, headerHeight), style.FramePadding.y);
    const bool visible = ImGui::ItemAdd(bb, id);
    bool hovered = false, held = false;
    if (visible && ImGui::ButtonBehavior(bb, id, &hovered, &held)) {
      open = !open;
      window->DC.StateStorage->SetInt(id, open);
    }
    auto *list = window->DrawList;
    const auto &colors = theme::colors();
    if (subpanel) {
      list->AddRectFilled(bb.Min, bb.Max, color(colors.subpanel), rounding);
    } else if (open) {
      // The backdrop under the header and the contents to follow, at last frame's height.
      const float guess = std::max(window->StateStorage.GetFloat(heightKey(id), 0), headerHeight + rounding * 2 + 2);
      m_backdrop = true;
      m_top = pos.y;
      m_guessBottom = pos.y + guess;
      m_vtxBegin = list->VtxBuffer.Size;
      list->AddRectFilled(bb.Min, ImVec2(bb.Max.x, m_guessBottom), color(colors.panel), rounding);
      m_vtxEnd = list->VtxBuffer.Size;
    } else {
      list->AddRectFilled(bb.Min, bb.Max, color(colors.panel), rounding);
    }
    if (hovered)
      list->AddRectFilled(bb.Min, bb.Max, color(theme::rgb(0xffffff, .04f)), rounding,
                          open && !subpanel ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersAll);
    ImGui::RenderNavCursor(bb, id);
    const float arrow = headerHeight * .3f, cy = bb.GetCenter().y, textX = bb.Min.x + inset + (subpanel ? arrow : 0);
    drawChevron(list, ImVec2(textX + arrow * .5f, cy), arrow, open ? ImGuiDir_Down : ImGuiDir_Right,
                color(theme::text));
    const char *end = ImGui::FindRenderedTextEnd(label);
    ImGui::RenderTextClipped(ImVec2(textX + arrow * 2, bb.Min.y), bb.Max, label, end, nullptr, ImVec2(0, .5f));
    m_open = open;
    if (!open)
      return;
    window->DC.CursorPos.y += std::round(2 * s);
    // The contents' IDs are the panel's own, so a row may share the panel's label.
    ImGui::PushID(id);
    m_scoped = true;
    if (subpanel) {
      m_indent = std::round(theme::panelPadding * s);
      ImGui::Indent(m_indent);
    }
  }

  Panel::~Panel() {
    --panelDepth();
    if (m_searchDepth >= 0)
      m_window->StateStorage.SetInt(searchKey(m_id), search().counts[m_searchDepth]);
    if (m_scoped)
      ImGui::PopID();
    if (m_hidden) {
      ImGui::PopClipRect();
      m_window->DC.CursorPos = m_cursor;
      m_window->DC.CursorMaxPos = m_cursorMax;
      m_window->DC.CurrLineSize = m_lineSize;
      m_window->DC.CursorPosPrevLine = m_prevLine;
      m_window->DC.PrevLineSize = m_prevLineSize;
      return;
    }
    if (m_indent > 0) {
      ImGui::Unindent(m_indent);
      ImGui::Dummy(ImVec2(0, std::round(theme::panelGap * .5f * scale())));
    }
    if (!m_backdrop)
      return;
    // The backdrop's bottom moves to where the contents ended.
    ImGuiWindow *window = m_window;
    const float padding = theme::panelGap * scale();
    const float bottom =
        std::max(window->DC.CursorPos.y - GImGui->Style.ItemSpacing.y + padding, m_top + ImGui::GetFrameHeight());
    const float split = (m_top + m_guessBottom) * .5f, delta = bottom - m_guessBottom;
    auto &vertices = window->DrawList->VtxBuffer;
    for (int i = m_vtxBegin; i < m_vtxEnd && i < vertices.Size; ++i)
      if (vertices[i].pos.y > split)
        vertices[i].pos.y += delta;
    window->StateStorage.SetFloat(heightKey(m_id), bottom - m_top);
    window->DC.CursorPos.y = bottom + GImGui->Style.ItemSpacing.y;
    window->DC.CursorMaxPos.y = std::max(window->DC.CursorMaxPos.y, bottom);
  }
} // namespace remix::ui
