#pragma once
#include <remix/remix_bake_ext.h>

namespace remix::ui {
  // Passive overlay: does not open settings or capture game input.
  void drawBakeProgress(const remixapi_BakeStatisticsEXT &statistics);
}
