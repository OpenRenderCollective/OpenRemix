#pragma once
#include "scene/bake_scene.h"
#include "forward/lightmap/layout.h"
#include <filesystem>

namespace remix::forward::bake {
  // Eligibility inventory only: no charts or GPU work. Fresh JSON, bounded to
  // 32768 draws, 4M triangles and 2M source vertices per draw. Vertex sets
  // stream one draw at a time; total source-vertex references are counts only.
  void writeModelPreflight(const scene::BakeScene &source, const std::filesystem::path &path,
                           std::stop_token cancelled = {});
  // Full authored+model CPU layout study under the supplied atlas budget.
  // No lighting transport, GPU upload, cache publication or live-scene mutation.
  void writeModelAtlasPreflight(const scene::BakeScene &source, lightmap::Options options,
      const std::filesystem::path &path, std::stop_token cancelled = {});
}
