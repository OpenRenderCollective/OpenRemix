#pragma once
#include "forward/lighting.h"
#include "scene/bake_scene.h"
#include <memory>
#include <unordered_map>

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
  // Selection is bounded by map memory/render cost, with stable slots for
  // retained lights. An unselected light still illuminates the scene.
  struct Selection {
    std::array<View, maximumLights> views{};
    std::array<bool, maximumLights> active{}, dirty{};
    std::vector<uint32_t> slots; // input light index -> slot, or UINT32_MAX
    uint32_t overflow = 0;
  };
  class Planner {
  public:
    Selection update(std::span<const scene::SphereLight> lights, Vec3 eye, uint32_t budget, float cutoff,
                     float distance, float sceneScale, std::shared_ptr<const scene::BakeScene> source);
    bool captured(const scene::Draw &draw) const;
    const std::shared_ptr<const scene::BakeScene> &source() const { return m_source; }

  private:
    std::array<View, maximumLights> m_views{};
    std::array<bool, maximumLights> m_valid{};
    std::shared_ptr<const scene::BakeScene> m_source;
    std::unordered_multimap<uint64_t, uint32_t> m_instances;
  };
} // namespace remix::forward::shadows
