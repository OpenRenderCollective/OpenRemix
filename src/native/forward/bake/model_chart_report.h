#pragma once
#include "scene/bake_scene.h"
#include <filesystem>
#include <string>

namespace remix::forward::bake {
  // Bounded mesh export for offline chart-cost studies. Fresh OBJ only;
  // positions are metres relative to the first corner, preserving UVs/normals.
  void writeModelChartMesh(const scene::BakeScene &source, uint32_t draw,
      const std::filesystem::path &path, std::stop_token cancelled = {});
  // REMIX_DEBUG_BAKE_MODEL_CHART_MESH opts into at most eight OBJ exports on
  // failure. The largest uses that path; others append .draw-<index>.obj.
  std::string modelChartBudgetFailure(const scene::BakeScene &source,
      const std::vector<scene::BakeSurfaceChart> &charts, uint32_t draw, uint32_t first,
      std::stop_token cancelled = {});
}
