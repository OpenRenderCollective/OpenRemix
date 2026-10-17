#include "ui/bake_progress.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>

namespace remix::ui {
  void drawBakeProgress(const remixapi_BakeStatisticsEXT &statistics) {
    const bool busy = statistics.state == REMIXAPI_BAKE_PREPARING_EXT ||
                      statistics.state == REMIXAPI_BAKE_TRACING_EXT;
    const bool failed = statistics.state == REMIXAPI_BAKE_FAILED_EXT;
    if (!busy && !failed)
      return;

    const auto *viewport = ImGui::GetMainViewport();
    const float width = std::max(1.f, std::min(420.f, viewport->WorkSize.x - 32.f));
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 16.f,
                                 viewport->WorkPos.y + 16.f), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowSize(ImVec2(width, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(.9f);
    constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("Map lighting progress", nullptr, flags)) {
      if (busy) {
        const float size = ImGui::GetTextLineHeight();
        const auto origin = ImGui::GetCursorScreenPos();
        const ImVec2 centre(origin.x + size * .5f, origin.y + size * .5f);
        const float angle = float(std::fmod(ImGui::GetTime() * 5, 6.28318530718));
        auto *draw = ImGui::GetWindowDrawList();
        draw->PathArcTo(centre, size * .35f, angle, angle + 4.71238898038f, 24);
        draw->PathStroke(ImGui::GetColorU32(ImGuiCol_CheckMark), 0, 2.f);
        ImGui::Dummy(ImVec2(size, size));
        ImGui::SameLine();
      }
      ImGui::TextUnformatted(failed ? "Lighting bake failed" : "Baking map lighting");
      // The controller reports the current stage's real work counts. Probe
      // completion alone must not become a misleading overall percentage:
      // lightmaps, visibility, cubemaps and cache publication can still follow.
      ImGui::PushTextWrapPos(0);
      ImGui::TextUnformatted(statistics.status[0] ? statistics.status : "Preparing lighting");
      ImGui::PopTextWrapPos();
    }
    ImGui::End();
  }
}
