#pragma once
#include "forward/shadows/plan.h"
#include "forward/bindings.h"

namespace remix::forward::shadows {
  struct AlphaCoverage {
    std::weak_ptr<const scene::Texture> texture;
    Vec4 tint{}, uv{}, factor{};
    std::array<uint32_t, 4> state{};
    uint64_t sampler = 0;
    std::array<uint8_t, 3> samplerState{};
    bool cacheable = false, textured = false, decodeColor = false;
  };
  AlphaCoverage alphaCoverage(const DrawData &data, const scene::Material &material, uint64_t sampler);
  struct MovingCaster {
    std::weak_ptr<const scene::Surface> surface;
    std::shared_ptr<const std::vector<Matrix>> bones;
    Matrix model{};
    uint32_t firstIndex = 0, indexCount = 0;
    int32_t vertexOffset = 0;
    bool opaque = false;
    AlphaCoverage alpha;
  };
  // Surfaces and pose vectors are immutable submitted scene snapshots.
  // Like static shadow caches, this follows queue-ordered recording/submission.
  // The owner must discard it with targets after a failed/abandoned recording.
  // Alpha reuse requires immutable texels and exact prepared coverage inputs.
  class MovingCache {
  public:
    static constexpr size_t maximumCasters = 4096, maximumPoseMatrices = 16384;
    bool update(const View &view, std::span<const MovingCaster> casters);
    void reset();

  private:
    View m_view;
    std::vector<MovingCaster> m_casters;
    bool m_valid = false;
  };
} // namespace remix::forward::shadows
