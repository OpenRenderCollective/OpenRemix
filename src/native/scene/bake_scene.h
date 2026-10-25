#pragma once
#include "scene/light_visibility.h"
#include "scene/types.h"
#include <stop_token>

namespace remix::scene {
  struct BakeSurfaceChart {
    uint32_t draw = 0, firstIndex = 0, width = 0, height = 0;
    std::vector<std::array<float, 2>> texcoords; // one per indexed corner
  };
  // Throws INVALID_ARGUMENTS for invalid bounds, index ownership or UV data.
  void validateBakeCharts(const std::vector<Draw> &draws, const std::vector<BakeSurfaceChart> &charts,
                          std::stop_token cancelled = {});
  // Weak identity checks for binding a completed bake to live retained draws.
  // Transport never reads through these references; it uses frozen Draws.
  struct BakeDrawBinding {
    std::weak_ptr<Surface> surface;
    std::weak_ptr<Material> material;
  };
  // CPU-only, immutable inputs shared with a background GPU bake. No dependency
  // on a camera, frame visibility, automatic settling, or caller-owned arrays.
  struct BakeScene {
    uint64_t sceneId = 0, revision = 0, contentHash = 0;
    float sceneScale = 1;
    bool ready = false;
    bool hasBounds = false;
    std::array<float, 3> minimum{}, maximum{};
    std::shared_ptr<const VisibilityPartition> partition;
    std::vector<Draw> draws;
    // Empty for ordinary scenes; otherwise one role per draw. Receiver-only
    // geometry supplies sample locations, never transport triangles/materials.
    std::vector<uint8_t> receiverDraws;
    std::vector<uint64_t> receiverInstances; // request identity, not content
    bool receiverOnly(size_t draw) const { return !receiverDraws.empty() && receiverDraws.at(draw) != 0; }
    std::vector<BakeDrawBinding> bindings;
    uint64_t chartRevision = 0; // scheduling identity, excluded from content hash
    std::vector<BakeSurfaceChart> charts;
    uint64_t reflectionRevision = 0;
    std::vector<std::array<float, 3>> reflectionHints;
    std::vector<SphereLight> lights;
    DistantLight sun;
    DomeLight dome;
  };
  // Copies geometry/material state; shares only immutable texture assets.
  // Rejects inputs whose transport would depend on animation or mutable texels.
  void freezeBakeScene(BakeScene &scene);
  // True only for an unchanged, explicitly retained member of this snapshot.
  bool matchesBakeDraw(const BakeScene &scene, uint32_t index, const Draw &draw);
} // namespace remix::scene
