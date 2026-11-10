#pragma once
#include "forward/lighting.h"
#include "scene/bake_scene.h"
#include <memory>
#include <unordered_map>
#include <utility>

namespace remix::forward::shadows {
  constexpr uint32_t maximumLights = 8, faces = 6;
  struct View {
    uint64_t id = 0;
    Vec3 position{};
    float nearPlane = 0, farPlane = 0;
    std::array<Matrix, faces> matrices{};
  };
  View lightView(const scene::SphereLight &light, float range, float sceneScale);
  bool sameView(const View &a, const View &b);
  bool intersects(const Bounds &bounds, const View &view);
  // Union of submitted moving-caster bounds in each face's [-1, 1] domain.
  // An inverted rectangle is empty; unknown/near-plane bounds cover the face.
  using Coverage = std::array<Vec4, faces>;
  Coverage emptyCoverage();
  // Shared projection bound for orthographic sun and perspective local maps.
  class CoverageProjection {
  public:
    explicit CoverageProjection(const Matrix &matrix, float nearPlane = 0)
        : m_matrix(matrix), m_frustum(matrix), m_nearPlane(nearPlane) {}
    void include(Vec4 &coverage, const Bounds &bounds) const;

  private:
    Matrix m_matrix;
    ClipFrustum m_frustum;
    float m_nearPlane;
  };
  template <size_t Count>
  std::array<CoverageProjection, Count> coverageProjections(const std::array<Matrix, Count> &matrices,
                                                              float nearPlane = 0) {
    return [&]<size_t... Index>(std::index_sequence<Index...>) {
      return std::array{CoverageProjection(matrices[Index], nearPlane)...};
    }(std::make_index_sequence<Count>{});
  }
  void includeCoverage(Vec4 &coverage, const Bounds &bounds, const Matrix &projection, float nearPlane = 0);
  void includeCoverage(Coverage &coverage, const Bounds &bounds, const View &view);
  // Selection is bounded by map memory/render cost, with stable slots for
  // retained lights. An unselected light still illuminates the scene.
  struct Selection {
    std::array<View, maximumLights> views{};
    std::array<bool, maximumLights> active{}, dirty{};
    std::array<float, maximumLights> strength{};
    std::vector<uint32_t> slots; // input light index -> slot, or UINT32_MAX
    uint32_t overflow = 0;
  };
  class Planner {
  public:
    // Monotonic seconds drive transitions independently of frame rate. A zero
    // duration keeps immediate selection for callers that do not supply a clock.
    // Optional nonzero fallback entries retain ordinary shadow coverage, but
    // yield slots to lights needing realtime mixed/dynamic receiver shadows.
    Selection update(std::span<const scene::SphereLight> lights, Vec3 eye, uint32_t budget, float cutoff,
                     float distance, float sceneScale, std::shared_ptr<const scene::BakeScene> source,
                     double seconds = 0, float fadeSeconds = 0,
                     std::span<const uint8_t> fallbackLights = {}, std::span<const LightRange> ranges = {});
    bool captured(const scene::Draw &draw) const;
    const std::shared_ptr<const scene::BakeScene> &source() const { return m_source; }

  private:
    std::array<View, maximumLights> m_views{};
    std::array<bool, maximumLights> m_valid{};
    std::array<bool, maximumLights> m_assigned{};
    std::array<float, maximumLights> m_strength{};
    double m_seconds = 0;
    bool m_clockStarted = false;
    std::shared_ptr<const scene::BakeScene> m_source;
    std::unordered_multimap<uint64_t, uint32_t> m_drawIndex;
  };
} // namespace remix::forward::shadows
