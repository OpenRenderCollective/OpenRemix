#pragma once
#include "scene/types.h"
#include <remix/remix_native_ext.h>
#include <array>
#include <span>

namespace remix::forward {
  using Vec3 = std::array<float, 3>;
  using Vec4 = std::array<float, 4>;
  using scene::Matrix;
  constexpr uint32_t clusterSlices = 24, clusterCapacity = 64, cascadeCount = 4;
  constexpr uint32_t sunShadowLayers = cascadeCount * 2; // frozen geometry, then moving casters
  // Bounded adaptive lists avoid evaluating every uploaded light when a
  // cluster exceeds the legacy64 entries. Overflow still falls back correctly.
  uint32_t clusterListCapacity(uint32_t lights, uint32_t cells, uint64_t storageRange, uint32_t requested = 0);
  bool matchesSun(const scene::DistantLight &captured, const scene::DistantLight &live);

  Vec3 transformPoint(const Matrix &matrix, const Vec3 &point);
  Vec3 transformVector(const Matrix &matrix, const Vec3 &vector);
  Vec3 normalized(Vec3 vector);
  float dot(const Vec3 &a, const Vec3 &b);
  Vec3 cross(const Vec3 &a, const Vec3 &b);

  struct DepthRange {
    float nearPlane = .1f, farPlane = 1000;
    float sign = 1; // supports cameras looking along either view-space Z axis
  };
  DepthRange depthRange(const Matrix &projection);
  uint32_t depthSlice(float depth, const DepthRange &range);
  float sliceDepth(uint32_t slice, const DepthRange &range);
  inline bool acceptsOpaque(const scene::Draw &d) {
    // Share ownership of a draw with the compatibility renderer explicitly.
    return d.traced() && !d.blend.enabled && !d.material->translucent && !d.material->transmission &&
           !d.decal && !d.cutGroup && !d.wireframe && d.depth && (!d.depthWriteOverride || d.depthWrite) &&
           !d.spriteFlags && d.material->normalEncoding != 5 && d.blend.mask == 15 &&
           (d.depthCompare == 1 || d.depthCompare == 3);
  }
  inline bool accepts(const scene::Draw &d) { return !d.viewModel && acceptsOpaque(d); }
  inline bool acceptsViewModel(const scene::Draw &d) { return d.viewModel && acceptsOpaque(d); }
  inline bool acceptsSurfaceLayer(const scene::Draw &d) {
    return d.decal && d.classification == REMIXAPI_INSTANCE_STATIC_EXT && d.traced() &&
           d.blend.enabled && d.blend.src == 6 && d.blend.dst == 7 && d.blend.op == 0 &&
           d.blend.mask == 15 && !d.material->translucent && !d.material->transmission &&
           !d.cutGroup && !d.wireframe && d.depth && !d.viewModel && !d.spriteFlags &&
           d.material->normalEncoding != 5 && (d.depthCompare == 1 || d.depthCompare == 3);
  }

  struct Bounds {
    Vec3 minimum{}, maximum{};
    bool valid = false;
    void include(const Vec3 &point);
    void merge(const Bounds &bounds);
  };
  Bounds transformBounds(const Bounds &bounds, const Matrix &model);
  struct Cascades {
    std::array<Matrix, cascadeCount> matrices;
    Vec4 splits{};
  };
  // Conservative Vulkan clip-volume test. Unknown bounds remain submitted.
  bool intersectsClip(const Bounds &bounds, const Matrix &clip);
  Cascades shadowCascades(const Matrix &camera, const Matrix &view, DepthRange range, Vec3 sunDirection,
                          const Bounds &casters, float distance, uint32_t resolution);
  float influenceRadius(const scene::SphereLight &light, float cutoff);
} // namespace remix::forward
