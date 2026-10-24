#pragma once
#include "forward/bake/grid.h"
#include <stop_token>

namespace remix::forward::bake {
  struct ReceiverRay {
    Vec3 origin{}, direction{}; // host units; unit direction
  };
  struct ReceiverHit {
    uint32_t draw = 0, firstIndex = 0;
    float distance = 0;
    Vec3 position{}, normal{}, geometricNormal{};
  };
  // At most sixteen rays against rigid, opaque retained triangles. Both sides
  // are tested; normals face the ray origin, matching the opaque shading path.
  // Alpha texels, normal maps, transmission and skinned geometry are excluded.
  std::vector<std::optional<ReceiverHit>> pickProbeReceivers(const scene::BakeScene &source,
      std::span<const ReceiverRay> rays, std::stop_token cancelled = {});
  // Optional worker-only diagnostic, including cache hits. Input is a count
  // (1..16), then XYZ origins and XYZ unit directions, at most 4 KiB total.
  // REMIX_DEBUG_PROBE_RECEIVER_RAYS names the input; output appends .results.json.
  // Includes at most 256 triangles/charts per hit draw, with truncation flags.
  // Existing output is protected; failures never affect bake publication.
  void dumpProbeReceiverReport(const scene::BakeScene &source, const Grid &grid,
                               std::stop_token cancelled = {}) noexcept;
  // Fresh JSON path in REMIX_DEBUG_BAKE_MODEL_RECEIVERS. Bounded uncharted
  // geometry manifest with current probe estimates, not lighting references.
  void dumpModelReceiverReport(const scene::BakeScene &source, const Grid &grid,
                               std::stop_token cancelled = {}) noexcept;
}
