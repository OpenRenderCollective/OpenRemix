#include "ui/profiles.h"
#include "ui/theme.h"
#include <imgui.h>

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif

namespace remix::ui {
  namespace {
    // Inter Regular 4.1 (SIL OFL 1.1, third_party/inter).
    const unsigned char kInter[] = {
#embed "../../../third_party/inter/Inter-Regular.ttf"
    };
  } // namespace

  void toolkitFont() {
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char *>(kInter), int(sizeof(kInter)),
                                               theme::fontSize, &config);
  }
} // namespace remix::ui
