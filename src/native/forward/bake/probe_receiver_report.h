#pragma once
#include "forward/bake/grid.h"
#include <stop_token>

namespace remix::forward::bake {
  // Diagnostic only: preserve a clear normal bias, otherwise stop before the
  // first solid/unplayable interval of a closed host partition. Unknown stays absent.
  std::optional<float> partitionContainedBias(const scene::BakeScene &source,
      Vec3 position, Vec3 normal, float bias);
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
  // Optional excludedDraw omits that draw only, not other submeshes of its instance.
  std::vector<std::optional<ReceiverHit>> pickProbeReceivers(const scene::BakeScene &source,
      std::span<const ReceiverRay> rays, std::stop_token cancelled = {}, uint32_t excludedDraw = UINT32_MAX);
  // Intersection-limited normal bias for 1..16 surface-origin/unit-normal rays.
  // Endpoints exclude 0.1 mm; this does not prove a closed free-space domain
  // or account for alpha/transmission. Never used by normal shading.
  std::vector<float> opaqueBiasLimits(const scene::BakeScene &source,
      std::span<const ReceiverRay> normalRays, float bias, std::stop_token cancelled = {});
  // Optional worker-only diagnostic, including cache hits. Input is a count
  // (1..16), then XYZ origins and XYZ unit directions, at most 4 KiB total.
  // REMIX_DEBUG_PROBE_RECEIVER_RAYS names the input; output appends .results.json.
  // Includes at most 256 triangles/charts per hit draw, with truncation flags.
  // At most 16 segments per original/partition/opaque-limited stencil test blockers
  // against the exact biased query endpoint, excluding 0.1 mm at each end.
  // Visible-connection SH oracles retain missing support as null, never black.
  // Unique hit draws also report unbiased AABB-midpoint stencils, excluding the
  // same draw from blockers. Bounds scan at most 32,768 triangles; unknown stays null.
  // Surface candidates rank complete playable triangles by area per signed dominant
  // normal axis, using a 2 mm offset. This does not prove segment connectivity.
  // A missing opaque blocker does not establish alpha/transmission visibility.
  // Existing output is protected; failures never affect bake publication.
  void dumpProbeReceiverReport(const scene::BakeScene &source, const Grid &grid,
                               std::stop_token cancelled = {}) noexcept;
  // Fresh JSON path in REMIX_DEBUG_BAKE_MODEL_RECEIVERS. Bounded uncharted
  // geometry manifest with current probe estimates, not lighting references.
  void dumpModelReceiverReport(const scene::BakeScene &source, const Grid &grid,
                               std::stop_token cancelled = {}) noexcept;
}
