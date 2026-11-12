#include "ui/tabs.h"
#include "ui/widgets.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <imgui.h>
#include <string>
#include <utility>
#include <vector>

namespace remix {
  namespace {
    // The Latency panel's stage colors (Okabe-Ito; distinct on the dark theme).
    constexpr ImU32 stageColors[latency::StageCount] = {
        IM_COL32(150, 150, 150, 255), IM_COL32(86, 180, 233, 255), IM_COL32(230, 159, 0, 255),
        IM_COL32(240, 228, 66, 255),  IM_COL32(204, 121, 167, 255), IM_COL32(0, 158, 115, 255),
        IM_COL32(213, 94, 0, 255)};
    constexpr ImU32 totalColor = IM_COL32(255, 255, 255, 255);

    bool shownStage(const latency::Summary &s, uint32_t stage) {
      return s.gpu || (stage != latency::Queued && stage != latency::Gpu);
    }
    double total(const latency::Summary &s, const latency::FrameLatency &f) { return s.gpu ? f.toGpuEnd : f.toPresent; }

    // A graph's frame: background, the value range and its labels; hover is
    // the frame under the mouse (-1: none).
    struct Plot {
      ImVec2 min, max;
      double top = 1;
      int hover = -1;
    };
    Plot plotArea(const char *id, float height, size_t frames, double top) {
      Plot p;
      const float width = std::max(ImGui::GetContentRegionAvail().x, 100.0f);
      p.min = ImGui::GetCursorScreenPos();
      p.max = ImVec2(p.min.x + width, p.min.y + height);
      p.top = std::max(top * 1.1, 0.5);
      ImGui::InvisibleButton(id, ImVec2(width, height));
      if (ImGui::IsItemHovered() && frames > 1) {
        const float x = ImGui::GetIO().MousePos.x;
        p.hover = std::clamp(int(std::lround((x - p.min.x) / width * float(frames - 1))), 0, int(frames) - 1);
      }
      auto *draw = ImGui::GetWindowDrawList();
      draw->AddRectFilled(p.min, p.max, IM_COL32(20, 20, 20, 255), 3);
      draw->AddRect(p.min, p.max, IM_COL32(70, 70, 70, 255), 3);
      for (int line = 1; line < 4; ++line) {
        const float y = p.max.y - (p.max.y - p.min.y) * float(line) / 4;
        draw->AddLine(ImVec2(p.min.x, y), ImVec2(p.max.x, y), IM_COL32(50, 50, 50, 255));
      }
      char label[32];
      std::snprintf(label, sizeof(label), "%.1f ms", p.top);
      draw->AddText(ImVec2(p.min.x + 4, p.min.y + 2), IM_COL32(160, 160, 160, 255), label);
      return p;
    }
    ImVec2 plotPoint(const Plot &p, size_t index, size_t frames, double value) {
      const float x = frames > 1 ? p.min.x + (p.max.x - p.min.x) * float(index) / float(frames - 1) : p.min.x;
      const float y = p.max.y - (p.max.y - p.min.y) * float(std::clamp(value / p.top, 0.0, 1.0));
      return ImVec2(x, y);
    }
    template <class Value>
    void plotLine(const Plot &p, const std::vector<latency::FrameLatency> &frames, ImU32 color, Value value,
                  float thickness) {
      std::vector<ImVec2> points;
      points.reserve(frames.size());
      for (size_t i = 0; i < frames.size(); ++i)
        points.push_back(plotPoint(p, i, frames.size(), value(frames[i])));
      ImGui::GetWindowDrawList()->AddPolyline(points.data(), int(points.size()), color, 0, thickness);
    }
    void hoverLine(const Plot &p, size_t frames) {
      if (p.hover < 0)
        return;
      const float x = plotPoint(p, size_t(p.hover), frames, 0).x;
      ImGui::GetWindowDrawList()->AddLine(ImVec2(x, p.min.y), ImVec2(x, p.max.y), IM_COL32(255, 255, 255, 90));
    }
    void legendItem(const char *name, ImU32 color) {
      const float size = ImGui::GetTextLineHeight() * .7f;
      const ImVec2 at = ImGui::GetCursorScreenPos();
      const float offset = (ImGui::GetTextLineHeight() - size) / 2;
      ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x, at.y + offset), ImVec2(at.x + size, at.y + offset + size),
                                                color, 2);
      ImGui::Dummy(ImVec2(size + 2, ImGui::GetTextLineHeight()));
      ImGui::SameLine(0, 2);
      ImGui::TextUnformatted(name);
    }
    void frameTooltip(const latency::Summary &s, const latency::FrameLatency &f) {
      ImGui::BeginTooltip();
      ImGui::Text("Frame %llu", (unsigned long long)f.frame);
      if (s.gpu)
        ImGui::Text("Frame start to GPU done: %.2f ms", f.toGpuEnd);
      ImGui::Text("Frame start to present: %.2f ms", f.toPresent);
      ImGui::Separator();
      for (uint32_t i = 0; i < latency::StageCount; ++i)
        if (shownStage(s, i)) {
          legendItem(latency::stageName(latency::Stage(i)), stageColors[i]);
          ImGui::SameLine(ImGui::GetFontSize() * 13);
          ImGui::Text("%6.2f ms", f.stage[i]);
        }
      ImGui::EndTooltip();
    }

    // The newest frame's stages on one time axis, from the low latency hold
    // before it to the last of its points.
    void drawTimeline(const latency::Summary &s, const latency::FrameLatency &f) {
      int64_t origin = 0, last = 0;
      for (const auto t : f.at)
        if (t) {
          origin = origin ? std::min(origin, t) : t;
          last = std::max(last, t);
        }
      if (!origin || last <= origin)
        return;
      const double span = double(last - origin) / 1e6;
      static const std::pair<latency::Point, latency::Point> bounds[latency::StageCount] = {
          {latency::SleepStart, latency::FrameStart},   {latency::FrameStart, latency::PresentCall},
          {latency::PresentCall, latency::SubmitStart}, {latency::SubmitStart, latency::SubmitEnd},
          {latency::SubmitEnd, latency::GpuStart},      {latency::GpuStart, latency::GpuEnd},
          {latency::PresentStart, latency::PresentEnd}};
      float labelWidth = 0;
      for (uint32_t i = 0; i < latency::StageCount; ++i)
        labelWidth = std::max(labelWidth, ImGui::CalcTextSize(latency::stageName(latency::Stage(i))).x);
      labelWidth += ImGui::GetStyle().ItemSpacing.x * 2;
      const float row = ImGui::GetTextLineHeightWithSpacing();
      const float width = std::max(ImGui::GetContentRegionAvail().x, labelWidth + 100);
      uint32_t rows = 0;
      for (uint32_t i = 0; i < latency::StageCount; ++i)
        rows += shownStage(s, i);
      const ImVec2 start = ImGui::GetCursorScreenPos();
      const float barLeft = start.x + labelWidth, barRight = start.x + width - 4;
      auto x = [&](int64_t t) { return barLeft + (barRight - barLeft) * float(double(t - origin) / 1e6 / span); };
      auto *draw = ImGui::GetWindowDrawList();
      const float height = row * float(rows + 1);
      draw->AddRectFilled(ImVec2(barLeft, start.y), ImVec2(barRight, start.y + row * float(rows)),
                          IM_COL32(20, 20, 20, 255), 3);
      // The frame's start: where input is read.
      if (f.at[latency::FrameStart]) {
        const float fx = x(f.at[latency::FrameStart]);
        draw->AddLine(ImVec2(fx, start.y), ImVec2(fx, start.y + row * float(rows)), IM_COL32(255, 255, 255, 120));
      }
      uint32_t r = 0;
      for (uint32_t i = 0; i < latency::StageCount; ++i) {
        if (!shownStage(s, i))
          continue;
        const float y = start.y + row * float(r++);
        draw->AddText(ImVec2(start.x, y), ImGui::GetColorU32(ImGuiCol_Text), latency::stageName(latency::Stage(i)));
        const auto [from, to] = bounds[i];
        if (!f.at[from] || !f.at[to])
          continue;
        const float a = x(f.at[from]), b = std::max(x(f.at[to]), a + 2);
        draw->AddRectFilled(ImVec2(a, y + 2), ImVec2(b, y + row - 2), stageColors[i], 2);
      }
      char label[48];
      const float axis = start.y + row * float(rows);
      draw->AddText(ImVec2(barLeft, axis), IM_COL32(160, 160, 160, 255), "0");
      std::snprintf(label, sizeof(label), "%.2f ms", span);
      draw->AddText(ImVec2(barRight - ImGui::CalcTextSize(label).x, axis), IM_COL32(160, 160, 160, 255), label);
      if (f.at[latency::FrameStart]) {
        const char *text = "frame start (input)";
        const float fx = x(f.at[latency::FrameStart]), w = ImGui::CalcTextSize(text).x;
        if (fx + w < barRight - ImGui::CalcTextSize(label).x - 8 && fx > barLeft + 12)
          draw->AddText(ImVec2(fx + 3, axis), IM_COL32(200, 200, 200, 255), text);
      }
      ImGui::Dummy(ImVec2(width, height));
    }

    void drawLatency(const ui::LatencyStatus &live) {
      // Paused: the panel keeps the summary it had (the recorder keeps going).
      static bool paused = false;
      static ui::LatencyStatus held;
      if (ImGui::Checkbox("Pause", &paused) && paused)
        held = live;
      const auto &status = paused ? held : live;
      const auto &s = status.summary;
      ImGui::SameLine();
      if (ImGui::SmallButton("Copy as text"))
        ImGui::SetClipboardText(latency::describe(s).c_str());
      if (!*status.mode)
        ImGui::TextWrapped("Low latency mode: none on this device (NVIDIA Reflex or AMD Anti-Lag).");
      else
        ImGui::TextWrapped("Low latency mode: %s, %s", status.mode,
                           status.active ? "holding the host before each frame" : "off");
      if (!status.available) {
        ImGui::TextWrapped("%s", status.unavailable.c_str());
        return;
      }
      if (!status.gpuMissing.empty())
        ImGui::TextWrapped("%s", status.gpuMissing.c_str());
      if (s.frames.empty()) {
        ImGui::TextWrapped("Waiting for completed frames.");
        return;
      }
      const auto &last = s.frames.back();
      ImGui::Text("%s: %.2f ms (average %.2f over %zu frames)",
                  s.gpu ? "Frame start to GPU done" : "Frame start to present", s.gpu ? last.toGpuEnd : last.toPresent,
                  s.gpu ? s.toGpuEnd.average : s.toPresent.average, s.frames.size());
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("From where the host's frame begins (Present returned, after any low latency hold;\n"
                          "where a game typically reads input) to the GPU finishing that frame's commands.\n"
                          "Input read later inside the host's frame is not seen. Like NVIDIA Reflex's\n"
                          "game-to-render latency; display scan-out comes after it.");

      // Statistics table.
      if (ImGui::BeginTable("latencyStats", 5,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Last");
        ImGui::TableSetupColumn("Average");
        ImGui::TableSetupColumn("Min");
        ImGui::TableSetupColumn("Max");
        ImGui::TableHeadersRow();
        auto row = [](const char *name, ImU32 color, const latency::Statistic &v) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          legendItem(name, color);
          for (const double value : {v.last, v.average, v.minimum, v.maximum}) {
            ImGui::TableNextColumn();
            ImGui::Text("%7.2f", value);
          }
        };
        if (s.gpu)
          row("Frame start to GPU done", totalColor, s.toGpuEnd);
        row("Frame start to present", IM_COL32(120, 120, 120, 255), s.toPresent);
        for (uint32_t i = 0; i < latency::StageCount; ++i)
          if (shownStage(s, i))
            row(latency::stageName(latency::Stage(i)), stageColors[i], s.stage[i]);
        ImGui::EndTable();
      }

      // The total over the frames.
      ImGui::TextUnformatted(s.gpu ? "Frame start to GPU done, per frame" : "Frame start to present, per frame");
      {
        const double top = s.gpu ? s.toGpuEnd.maximum : s.toPresent.maximum;
        const auto p = plotArea("##latencyTotal", ImGui::GetFrameHeight() * 4, s.frames.size(), top);
        plotLine(p, s.frames, totalColor, [&](const latency::FrameLatency &f) { return total(s, f); }, 1.5f);
        hoverLine(p, s.frames.size());
        if (p.hover >= 0)
          frameTooltip(s, s.frames[size_t(p.hover)]);
      }

      // Every stage over the frames.
      ImGui::TextUnformatted("Stages, per frame");
      {
        double top = 0;
        for (uint32_t i = 0; i < latency::StageCount; ++i)
          if (shownStage(s, i))
            top = std::max(top, s.stage[i].maximum);
        const auto p = plotArea("##latencyStages", ImGui::GetFrameHeight() * 5, s.frames.size(), top);
        for (uint32_t i = 0; i < latency::StageCount; ++i)
          if (shownStage(s, i))
            plotLine(p, s.frames, stageColors[i], [i](const latency::FrameLatency &f) { return f.stage[i]; }, 1.2f);
        hoverLine(p, s.frames.size());
        if (p.hover >= 0)
          frameTooltip(s, s.frames[size_t(p.hover)]);
      }

      // The newest frame, laid out in time.
      ImGui::Text("Frame %llu in time", (unsigned long long)last.frame);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Each stage's start and end on one clock (GPU times converted with calibrated\n"
                          "timestamps). With asynchronous presentation, the host's next frame overlaps\n"
                          "this one's rendering; the line marks where this frame began.");
      drawTimeline(s, last);

      if (const auto &d = status.driver; d.available) {
        ImGui::Separator();
        ImGui::Text("NVIDIA Reflex driver report, frame %llu", (unsigned long long)d.frame);
        ImGui::Text("Game to render %.2f ms | simulation %.2f, submit %.2f, present %.2f, driver %.2f, "
                    "OS queue %.2f, GPU render %.2f",
                    d.gameToRender, d.simulation, d.submit, d.present, d.driver, d.osQueue, d.gpu);
      }
    }
  } // namespace

  void RemixNativeRuntime::drawGpuPassTimings(bool enabled) {
    if (!enabled)
      return;
    if (!gpuTimersSupported()) {
      ImGui::TextWrapped("Unavailable: the queue family has no timestamp support.");
      return;
    }
    const auto &t = gpuPassTimings();
    if (!t.available) {
      ImGui::TextWrapped("Waiting for a completed frame.");
      return;
    }
    ImGui::TextWrapped("GPU frame %.3f ms = passes %.3f + between passes %.3f (frame %llu, queue family %u)",
                       t.frameMilliseconds, t.passMilliseconds, t.untimedMilliseconds, (unsigned long long)t.frame,
                       t.queueFamily);
    ImGui::TextWrapped("Render %ux%u, output %ux%u", t.renderWidth, t.renderHeight, t.outputWidth, t.outputHeight);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Each pass is timed from the end of the work before it to its own end (bottom of\n"
                        "the pipe), so passes do not overlap and their own times add up to the frame.\n"
                        "Own time excludes nested passes. Passes that wait for the swapchain image or\n"
                        "for presentation include that wait.");
    if (t.truncated)
      ImGui::TextWrapped("%u passes not timed (query limit)", t.truncated);
    // Sorted by own time; the time between passes is a row of its own, so the
    // column adds up to the frame.
    const auto order = gpu::byCost(t);
    const uint32_t untimedRow = uint32_t(t.passes.size());
    std::vector<uint32_t> rows;
    rows.reserve(order.size() + 1);
    bool untimedPlaced = false;
    for (const auto index : order) {
      if (!untimedPlaced && t.untimedMilliseconds >= t.passes[index].selfMilliseconds) {
        rows.push_back(untimedRow);
        untimedPlaced = true;
      }
      rows.push_back(index);
    }
    if (!untimedPlaced)
      rows.push_back(untimedRow);
    const double frame = std::max(t.frameMilliseconds, 1e-9);
    auto size = [](const gpu::PassTiming &pass, char *out, size_t bytes) {
      if (pass.width && pass.height)
        std::snprintf(out, bytes, "%ux%u", pass.width, pass.height);
      else
        std::snprintf(out, bytes, "-");
    };
    if (ImGui::SmallButton("Copy as text")) {
      std::string text;
      char line[160], dims[32];
      std::snprintf(line, sizeof(line),
                    "GPU frame %.3f ms (passes %.3f, between passes %.3f); render %ux%u, output %ux%u\n",
                    t.frameMilliseconds, t.passMilliseconds, t.untimedMilliseconds, t.renderWidth, t.renderHeight,
                    t.outputWidth, t.outputHeight);
      text += line;
      for (const auto row : rows) {
        if (row == untimedRow) {
          std::snprintf(line, sizeof(line), "%-56s %11s %8.3f %8.3f %5.1f%%\n", "Between passes (untimed)", "-",
                        t.untimedMilliseconds, t.untimedMilliseconds, 100 * t.untimedMilliseconds / frame);
        } else {
          const auto &pass = t.passes[row];
          size(pass, dims, sizeof(dims));
          std::snprintf(line, sizeof(line), "%-56s %11s %8.3f %8.3f %5.1f%%\n", pass.name, dims, pass.selfMilliseconds,
                        pass.milliseconds, 100 * pass.selfMilliseconds / frame);
        }
        text += line;
      }
      ImGui::SetClipboardText(text.c_str());
    }
    if (!ImGui::BeginTable("gpuPasses", 5,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit))
      return;
    ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Size");
    ImGui::TableSetupColumn("Own ms");
    ImGui::TableSetupColumn("Incl. ms");
    ImGui::TableSetupColumn("%");
    ImGui::TableHeadersRow();
    auto cells = [&](const char *name, const char *dims, double self, double inclusive, bool nested) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (nested)
        ImGui::TextDisabled("%s", name);
      else
        ImGui::TextUnformatted(name);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(dims);
      ImGui::TableNextColumn();
      ImGui::Text("%7.3f", self);
      ImGui::TableNextColumn();
      ImGui::Text("%7.3f", inclusive);
      ImGui::TableNextColumn();
      ImGui::Text("%5.1f", 100 * self / frame);
    };
    char dims[32];
    for (const auto row : rows) {
      if (row == untimedRow) {
        cells("Between passes (untimed)", "-", t.untimedMilliseconds, t.untimedMilliseconds, false);
        continue;
      }
      const auto &pass = t.passes[row];
      size(pass, dims, sizeof(dims));
      // Containers (own time is what their nested passes leave out) are dimmed.
      cells(pass.name, dims, pass.selfMilliseconds, pass.milliseconds,
            pass.selfMilliseconds + 1e-9 < pass.milliseconds);
    }
    cells("Total", "-", t.frameMilliseconds, t.frameMilliseconds, false);
    ImGui::EndTable();
  }

  // One pixel's paths, as the path tracer recorded them last frame: picked by
  // a click on the scene, listed, and drawn over it (vertices white, light
  // samples green where visible and red where blocked, emission collected
  // by a path yellow).
  void RemixNativeRuntime::drawPathInspector() {
    const auto status = uiPathInspection();
    if (!status.available)
      return;
    const auto &io = ImGui::GetIO();
    const auto &inspection = status.inspection;
    if (m_inspecting && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) && io.DisplaySize.x > 0 && io.DisplaySize.y > 0) {
      const uint32_t width = inspection.width ? inspection.width : status.renderWidth;
      const uint32_t height = inspection.width ? inspection.height : status.renderHeight;
      m_inspectX = int(io.MousePos.x / io.DisplaySize.x * float(width));
      m_inspectY = int(io.MousePos.y / io.DisplaySize.y * float(height));
      uiInspectPixel(m_inspectX, m_inspectY);
    }
    if (ui::Panel panel{"Path inspector"}) {
      if (ui::Checkbox("Inspect a pixel (click the scene)", &m_inspecting) && !m_inspecting)
        uiInspectPixel(-1, -1);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Records the paths of one pixel each frame: its primary surface, every path vertex,\n"
                          "every local-light sample (estimate, selection PDF, visibility) and the emission\n"
                          "paths collected. Drawn over the scene; click elsewhere to pick another pixel.");
      if (m_inspecting) {
        ImGui::Text("Pixel %d, %d: %zu records", m_inspectX, m_inspectY, inspection.records.size());
        if (ImGui::BeginTable("Path records", 5,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                              ImVec2(0, 260))) {
          for (const char *name : {"Kind", "Position", "", "", ""})
            ImGui::TableSetupColumn(name);
          ImGui::TableHeadersRow();
          for (const auto &r : inspection.records) {
            const int kind = int(r[15]);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            static const char *kinds[] = {"primary", "vertex", "light", "emission", "light (path)"};
            ImGui::TextUnformatted(kind >= 0 && kind < 5 ? kinds[kind] : "?");
            ImGui::TableNextColumn();
            ImGui::Text("%.1f %.1f %.1f", r[0], r[1], r[2]);
            ImGui::TableNextColumn();
            if (kind == 0)
              ImGui::Text("t %.1f rough %.2f", r[3], r[7]);
            else if (kind == 1)
              ImGui::Text("bounce %d rough %.2f", int(r[3]), r[7]);
            else if (kind == 2 || kind == 4)
              ImGui::Text("emitter %d pdf %.3g", int(r[3]), r[7]);
            ImGui::TableNextColumn();
            if (kind == 0)
              ImGui::Text("albedo %.2f %.2f %.2f", r[8], r[9], r[10]);
            else if (kind == 1)
              ImGui::Text("throughput %.3g %.3g %.3g", r[8], r[9], r[10]);
            else if (kind == 2 || kind == 4)
              ImGui::Text("estimate %.3g %.3g %.3g", r[4], r[5], r[6]);
            else if (kind == 3)
              ImGui::Text("weight %.3g %.3g %.3g", r[4], r[5], r[6]);
            ImGui::TableNextColumn();
            if (kind == 0)
              ImGui::Text("pixel %.3g %.3g %.3g", r[12], r[13], r[14]);
            else if (kind == 1)
              ImGui::Text("emission %.3g %.3g %.3g", r[12], r[13], r[14]);
            else if (kind == 2 || kind == 4)
              ImGui::Text("visible %.2f at %.1f", r[8], r[11]);
            else if (kind == 3)
              ImGui::Text("emission %.3g %.3g %.3g", r[8], r[9], r[10]);
          }
          ImGui::EndTable();
        }
      }
    } else {
      if (m_inspecting)
        ImGui::TextDisabled("Path inspector: pixel %d, %d", m_inspectX, m_inspectY);
    }
    if (!m_inspecting || inspection.records.empty() || !inspection.width)
      return;
    // World to the display: the recorded camera (column-major, Vulkan
    // framebuffer coordinates) to the window's points.
    const auto &m = inspection.camera;
    const auto project = [&](const float *p, ImVec2 &out) {
      const float x = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
                  y = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
                  w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
      if (w <= 1e-6f)
        return false;
      out = ImVec2((x / w * .5f + .5f) * io.DisplaySize.x, (y / w * .5f + .5f) * io.DisplaySize.y);
      return true;
    };
    auto *draw = ImGui::GetBackgroundDrawList();
    const ImVec2 pixel((float(m_inspectX) + .5f) / float(inspection.width) * io.DisplaySize.x,
                       (float(m_inspectY) + .5f) / float(inspection.height) * io.DisplaySize.y);
    draw->AddCircle(pixel, 6, IM_COL32(255, 255, 255, 255), 12, 2);
    // Vertices follow their path in record order (bounce 1 starts a path at
    // the primary); a path vertex's light samples follow it.
    const float *primary = nullptr, *last = nullptr;
    for (const auto &r : inspection.records)
      if (int(r[15]) == 0)
        primary = r.data();
    for (const auto &r : inspection.records) {
      const int kind = int(r[15]);
      ImVec2 a, b;
      if (kind == 1) {
        const float *from = int(r[3]) == 1 ? primary : last;
        if (from && project(from, a) && project(r.data(), b))
          draw->AddLine(a, b, IM_COL32(255, 255, 255, 220), 1.5f);
        last = r.data();
      } else if (kind == 2 || kind == 4) {
        const float *from = kind == 4 && last ? last : primary;
        if (from && project(from, a) && project(r.data(), b))
          draw->AddLine(a, b, r[8] > 0 ? IM_COL32(60, 230, 90, 200) : IM_COL32(240, 60, 60, 200), 1);
      } else if (kind == 3 && project(r.data(), b))
        draw->AddCircleFilled(b, 4, IM_COL32(255, 220, 40, 255));
    }
  }

  void RemixNativeRuntime::drawDiagnosticsTab(config::Settings &settings) {
    if (const auto s = uiPreparation(); s.available) {
      ImGui::Text("Pipeline preparation: %llu / %llu completed", (unsigned long long)s.completed,
                  (unsigned long long)s.requested);
      ImGui::Text("Created %llu, reused %llu, failed %llu; %.1f ms compiling", (unsigned long long)s.created,
                  (unsigned long long)s.reused, (unsigned long long)s.failed, s.milliseconds);
      ImGui::Text("Pending requests deduplicated: %llu", (unsigned long long)s.deduplicated);
      ImGui::Text("Driver cache: %s; feedback: %s; hits: %llu", s.cacheLoaded ? "loaded" : "cold",
                  s.feedback ? "supported" : "unavailable", (unsigned long long)s.cacheHits);
      if (s.feedback)
        ImGui::Text("Driver creation time: %.1f ms", s.driverMilliseconds);
      if (s.cacheWriteFailed)
        ImGui::TextUnformatted("Pipeline cache could not be saved; rendering remains available.");
      if (s.pending) {
        ImGui::Text("Pending configuration #%llu (profile %s)", (unsigned long long)s.generation,
                    m_profiles.active().c_str());
        ImGui::Text("Elapsed %.1f s; totals include jobs discovered during preparation", s.elapsedSeconds);
      }
      if (!s.failure.empty())
        ImGui::TextWrapped("%s", s.failure.c_str());
      ImGui::Separator();
      ImGui::Text("Longest presentation gap: %.1f ms (includes application idle time)", m_longestPresentationGap);
    }
    auto &options = settings.trace;
    const bool rrActive = uiReconstruction().denoises;
    if (ui::Panel panel{"Debug views", ImGuiTreeNodeFlags_DefaultOpen}) {
      int view = int(options.debugView);
      if (ui::Combo("Debug view", &view,
                    "Final lighting\0Albedo\0Shading normals\0Triangle IDs\0Depth bands\0World position grid\0"
                    "Raw local lighting\0Raw indirect lighting\0Motion vectors\0Motion validity\0"
                    "Indirect lighting confidence\0TAA current-frame weight\0Roughness and metalness\0Local history "
                    "confidence\0"
                    "Translucent layer count\0Shadow terminator offset\0"))
        options.debugView = uint32_t(view);
      static const char *help[] = {
          "Start with Triangle IDs and Shading normals at the problem angle, then compare Raw local "
          "lighting. Compare both Primary visibility options above.",
          "Unlit base color of the first visible surface. Flicker here points to visibility or material "
          "data.",
          "World-space shading normals: RGB = XYZ mapped to 0..1. Includes authored vertex normals and "
          "material normal maps; missing vertex normals fall back to flat faces.",
          "Colors identify submitted triangles. Coplanar surfaces swapping visibility change color. "
          "Remeshing can also change IDs; palette collisions are possible.",
          "Distance from the camera, repeating black to white every 4 world units. Sudden patches can reveal "
          "visibility or position jumps.",
          "World position as RGB, repeating every world unit. Look for moving discontinuities inside a flat "
          "face.",
          "Unfiltered direct local illumination on opaque receivers, including torches and emissives, before "
          "albedo and transparent tint. Uses the current display tonemapper.",
          "Unfiltered indirect illumination before albedo and transparent tint. Includes SHARC when enabled; "
          "zero bounces is black.",
          "Opaque motion guides: red/green = horizontal/vertical pixels, gray = zero, full range = +/-16 "
          "pixels. Magenta = invalid motion.",
          "Opaque motion correspondence: green = available, red = invalid/reset. This precedes per-pixel "
          "depth/normal history rejection.",
          "REBLUR indirect confidence: white = high trust, black = low. This is not the local-light "
          "denoiser's history.",
          "Actual native TAA blend weight: white = current frame only; darker = more history. Unavailable "
          "with FSR or TAA off.",
          "PBR material of the first visible surface: red = roughness, green = metalness. Blue marks "
          "non-PBR materials.",
          "Local-light denoiser history confidence: white = history kept, black = dropped (moved lights, "
          "moved occluders, moving surfaces). Indexed as NRD reads it.",
          "Translucent layers each camera ray passes: black none, blue 1, green 2, yellow 3, red 4, white 5 "
          "or more. Pixels with two or more are lit layer by layer in the multi-sheet pass.",
          "Shadow-ray lift off smooth-shaded triangles: red = lifted (full red at the longest offset allowed), "
          "grey = the sun's shading cosine, blue = sun-lit by the shading normal but past the lighting horizon "
          "(the triangle's, or with the offset the smooth surface's), where sun light is cut off. No red with "
          "the offset off."};
      ImGui::TextWrapped("%s", help[options.debugView]);
      if (options.debugView) {
        ImGui::TextWrapped(
            "Diagnostics display live internal pixels without temporal filtering. Lighting histories and "
            "requested settings keep running. FSR pixels are enlarged without filtering.");
        ImGui::TextWrapped("Purple checkerboard means the selected signal is unavailable. Normal subpixel "
                           "jitter remains visible at edges.");
      }
    }
    int shadowView = int(options.shadowDebug);
    if (ui::Combo("Shadow view", &shadowView, "Final lighting\0Raw visibility\0SIGMA visibility\0"))
      options.shadowDebug = uint32_t(shadowView);
    ImGui::BeginDisabled(rrActive);
    ui::Checkbox("NRD validation", &options.validation);
    ImGui::EndDisabled();
    ui::Checkbox("Progressive accumulation", &options.accumulate);
    if (options.accumulate)
      ImGui::TextWrapped("%s", "Raw reference: requested denoiser and TAA resume afterward.");
    ui::Checkbox("Force dynamic TLAS rebuild", &options.forceRebuild);
    ui::Checkbox("Freeze emissive radiance", &options.freezeEmissiveRadiance);
    drawPathInspector();
    if (ui::Panel panel{"Free Camera", ImGuiTreeNodeFlags_DefaultOpen}) {
      ui::Checkbox("Enable Free Camera", &m_freeCamera.enabled);
      if (m_freeCamera.enabled) {
        ImGui::Indent();
        ui::Checkbox("Lock Free Camera", &m_freeCamera.locked);
        ui::Checkbox("Invert Y (Pitch)", &m_freeCamera.invertY);
        ui::DragFloat3("Position offset", m_freeCamera.positionOffset.data(), 0.1f);
        ui::DragFloat("Yaw", &m_freeCamera.yaw, 0.01f, -3.14159f, 3.14159f, "%.3f rad");
        ui::DragFloat("Pitch", &m_freeCamera.pitch, 0.01f, -1.55f, 1.55f, "%.3f rad");
        ui::DragFloat("Movement Speed", &m_freeCamera.speed, 0.5f, 0.1f, 500.0f, "%.1f");
        ui::DragFloat("Turning Speed", &m_freeCamera.turningSpeed, 0.05f, 0.1f, 10.0f, "%.2f");
        if (ImGui::Button("Reset Camera")) {
          m_freeCamera.reset();
        }
        if (ui::Panel guide{"Controls guide"}) {
          ImGui::BulletText("W / S: Move Forward / Backward");
          ImGui::BulletText("A / D: Move Left / Right");
          ImGui::BulletText("E / Q: Move Up / Down");
          ImGui::BulletText("Shift: Speed boost (3x) | Ctrl: Slow precision (0.25x)");
          ImGui::BulletText("Mouse Left-Drag: Look around (yaw & pitch)");
          ImGui::BulletText("J / L: Turn Yaw | I / K: Tilt Pitch");
          ImGui::BulletText("Mouse Right-Click: Reset position & angles");
        }
        ImGui::Unindent();
      }
    }
    if (ui::Panel panel{"Performance statistics", ImGuiTreeNodeFlags_DefaultOpen}) {
      const auto device = uiDevice();
      ImGui::TextWrapped("%s", device.name.c_str());
      ImGui::TextWrapped("%s", device.api.c_str());
      const auto &io = ImGui::GetIO();
      ImGui::TextWrapped("Presentation: %.2f ms / %.1f FPS", io.Framerate > 0 ? 1000.0f / io.Framerate : 0.0f,
                         io.Framerate);
      ImGui::TextWrapped("Scene: %zu surface draws", pendingDrawCount());
      auto s = tracingStatistics();
      ImGui::TextWrapped("Accumulated frames: %llu", (unsigned long long)s.accumulationCount);
      ImGui::TextWrapped("BLAS builds / refits / compactions: %llu / %llu / %llu", (unsigned long long)s.blasBuilds,
                         (unsigned long long)s.blasRefits, (unsigned long long)s.blasCompactions);
      ImGui::TextWrapped("Static TLAS builds: %llu", (unsigned long long)s.staticTlasBuilds);
      ImGui::TextWrapped("Dynamic TLAS builds / refits: %llu / %llu", (unsigned long long)s.dynamicTlasBuilds,
                         (unsigned long long)s.dynamicTlasRefits);
      ImGui::TextWrapped("Rebuild reason: %u; refits since build: %u", s.dynamicRebuildReason, s.dynamicRefitCount);
      ImGui::TextWrapped("Acceleration memory: %.1f MiB", double(s.accelerationBytes) / (1024 * 1024));
      if (s.gpuTimingsAvailable) {
        ImGui::TextWrapped("GPU builds %.3f ms; tracing %.3f ms", s.gpuBuildMilliseconds, s.gpuTraceMilliseconds);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Tracing includes sun and local shadows, indirect rays, and transparent "
                            "sheets.\nDenoiser timings are reported separately.");
      }
    }
    if (ui::Panel panel{"GPU pass timings", ImGuiTreeNodeFlags_DefaultOpen}) {
      ui::Checkbox("GPU pass timers", &settings.gpuPassTimers);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Timestamps around every GPU pass of the frame (rtx.native.gpuPassTimers).\n"
                          "Off records no extra queries. Results are read after the frame's fence,\n"
                          "without waiting, so they describe the frame completed two frames ago.");
      drawGpuPassTimings(settings.gpuPassTimers);
    }
    if (ui::Panel panel{"Latency", ImGuiTreeNodeFlags_DefaultOpen})
      drawLatency(uiLatency());
    if (ui::Panel panel{"Present CPU breakdown"}) {
      // Previous Present; this panel is drawn while the current one records.
      const auto present = uiPresent();
      const auto &p = present.performance;
      const auto &b = present.breakdown;
      ImGui::TextWrapped("Present %.2f ms: scene %.2f, fence %.2f, acquire %.2f, record %.2f, submit %.2f, "
                         "queue present %.2f",
                         p.presentMilliseconds, p.sceneMilliseconds, p.fenceMilliseconds, p.acquireMilliseconds,
                         p.recordMilliseconds, p.submitMilliseconds, p.queuePresentMilliseconds);
      if (present.async) {
        // Asynchronous presentation: the frame above ran on the render thread;
        // its Present on the caller's thread is split out here. Present, setup
        // and the rows below include the caller's setup and capture, not its
        // wait for the handoff.
        const auto &c = present.caller;
        ImGui::TextWrapped("Caller Present %.2f ms: handoff wait %.2f, setup %.2f (developer UI %.2f), capture "
                           "%.2f | render thread %.2f ms",
                           c.callMilliseconds, c.waitMilliseconds, c.setupMilliseconds, c.uiMilliseconds,
                           c.captureMilliseconds, present.render.renderMilliseconds);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Handoff wait: time Present waited for the render thread to finish the previous\n"
                            "frame (the render thread is the bottleneck when it is large). Capture: publishing\n"
                            "the frame's draw lists, lights and static-set changes to the render thread.\n"
                            "Render thread: scene preparation through queue present, excluding the caller.");
        ImGui::TextWrapped("This Present waited %.2f ms for that frame; handed off %llu, presented %llu",
                           present.current.waitMilliseconds, (unsigned long long)present.current.handedOff,
                           (unsigned long long)present.render.rendered);
      }
      struct Row {
        const char *name;
        double value;
      };
      const Row rows[] = {{"Raster prepare", b.rasterPrepareMilliseconds},
                          {"  raster skinning", b.rasterSkinningMilliseconds},
                          {"Tracer begin", b.traceBeginMilliseconds},
                          {"Tracer setup", b.traceSetupMilliseconds},
                          {"Trace surfaces", b.traceSurfacesMilliseconds},
                          {"  geometry", p.geometryMilliseconds},
                          {"  materials", b.materialMilliseconds},
                          {"  temporal", b.temporalMilliseconds},
                          {"Retire", b.retireMilliseconds},
                          {"Acceleration", b.traceAccelerationMilliseconds},
                          {"Surface table", b.tableMilliseconds},
                          {"Lighting", b.lightingMilliseconds},
                          {"SHARC", b.sharcMilliseconds},
                          {"Scene signature", b.signatureMilliseconds},
                          {"Descriptors", b.descriptorMilliseconds},
                          {"Primary G-buffer", b.primaryMilliseconds},
                          {"Passes", b.passesMilliseconds},
                          {"Post-processing", b.postMilliseconds},
                          {"Composite", b.compositeMilliseconds},
                          {present.async ? "Developer UI draw" : "Developer UI", b.uiMilliseconds},
                          {"Snapshot", b.snapshotMilliseconds},
                          {"Other", b.otherMilliseconds},
                          {"Present: setup", b.presentSetupMilliseconds},
                          {"Present: release", b.presentReleaseMilliseconds}};
      for (const auto &row : rows)
        ImGui::Text("%-18s %7.3f ms", row.name, row.value);
      // The persistent static set: its sync is part of scene, its tables part
      // of trace surfaces.
      const auto &st = present.statics;
      ImGui::Text("%-18s %7.3f ms", "Static sync", st.syncMilliseconds);
      ImGui::Text("%-18s %7.3f ms", "Static tables", st.tableMilliseconds);
      if (st.active)
        ImGui::TextWrapped("Static set: %llu instances, surfaces reused %llu, rebuilt %llu (%llu instances); "
                           "dynamic surfaces %llu; changes %llu, layouts %llu, temporal %llu",
                           (unsigned long long)st.units, (unsigned long long)st.reusedSurfaces,
                           (unsigned long long)st.rebuiltSurfaces, (unsigned long long)st.rebuiltUnits,
                           (unsigned long long)st.dynamicSurfaces, (unsigned long long)st.changes,
                           (unsigned long long)st.relayouts, (unsigned long long)st.temporalSurfaces);
      else
        ImGui::TextWrapped("Static set: not in use (dynamic surfaces %llu)", (unsigned long long)st.dynamicSurfaces);
      if (st.validations)
        ImGui::TextWrapped("Static validation: %llu mismatches%s%s", (unsigned long long)st.mismatches,
                           st.mismatches ? ": " : "", st.mismatches ? st.mismatch.c_str() : "");
      ImGui::TextWrapped("Instances %llu, surfaces %llu, primary draws %llu, raster draws %llu, textures %llu",
                         (unsigned long long)b.traceInstances, (unsigned long long)b.traceSurfaces,
                         (unsigned long long)b.primaryDraws, (unsigned long long)b.rasterDraws,
                         (unsigned long long)b.textureDescriptors);
      ImGui::TextWrapped("Geometry conversions %llu, uploads %.2f MiB (raster %.2f MiB), raster skinned "
                         "vertices %llu",
                         (unsigned long long)p.geometryConversions, double(p.uploadBytes) / (1024 * 1024),
                         double(b.rasterUploadBytes) / (1024 * 1024), (unsigned long long)b.rasterSkinnedVertices);
    }
    if (ui::Panel panel{"Input and clear color", ImGuiTreeNodeFlags_DefaultOpen}) {
      ui::ColorEdit3("Clear color", settings.clearColor.data());
      ui::InputText("Input", uiInput(), uiInputSize());
    }
  }
} // namespace remix
