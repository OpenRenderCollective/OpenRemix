#pragma once
#include "forward/lightmap/data.h"
#include "forward/bake/policy.h"
#include "forward/mixed/occlusion.h"
#include "forward/mixed/masks.h"
#include <span>
#include <unordered_map>
namespace remix::forward::mixed {
  constexpr uint32_t channels = 4, none = ~0u;
  using Task = bake::VisibilityTask;
  // Small immutable table retained by the runtime after tracing finishes.
  // The per-texel work list stays with the temporary bake plan.
  struct Bindings {
    std::shared_ptr<const scene::BakeScene> source;
    std::unordered_multimap<uint64_t, uint32_t> identities;
    bake::DirectPolicy directLighting;
    bool bakedDiffuse(uint32_t captured) const;
    bool retainsVisibility(uint32_t captured) const;
    uint32_t match(const scene::SphereLight &light) const;
  };
  struct Plan {
    Bindings lights;
    std::vector<bake::UInt4> charts;       // captured light indices; none is an unassigned channel
    std::vector<bake::UInt4> directCharts; // independent owners of retained unit-radiance SH
    std::vector<Task> tasks;
    uint32_t overflow = 0; // chart/light links using the ordinary direct-light fallback
    uint64_t hemisphereRejectedPairs = 0; // planning diagnostic; no visibility proof is stored
    uint64_t coneRejectedPairs = 0; // finite-emitter cone cannot reach the biased chart bounds
    Occlusion occlusion;
    Masks masks;
    std::vector<std::vector<uint32_t>> remaining;
    std::vector<uint32_t> next;
  };
  // receiverBias is the transport's maximum origin displacement in host units.
  Plan plan(std::shared_ptr<const scene::BakeScene> source, const lightmap::Layout &layout,
            float cutoff = .001f, std::stop_token cancelled = {}, float receiverBias = 0,
            const bake::DirectPolicy &directLighting = {});
  // Constant visibility uses compact metadata, freeing filtered shadow channels.
  // Positive lights can retain direct SH independently before scratch channels are reused.
  enum class DirectCapture { Immediate, Deferred };
  bool refine(Plan &plan, const lightmap::Layout &layout, std::span<const Vec4> visibility,
              std::stop_token cancelled = {}, std::span<const lightmap::Direct> scratch = {},
              std::span<lightmap::Direct> direct = {}, DirectCapture capture = DirectCapture::Immediate);
} // namespace remix::forward::mixed
