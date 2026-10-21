#include "forward/shadows/sun_cache.h"
#include <cmath>

namespace remix::forward::shadows {
  namespace {
    bool affine(const Matrix &matrix) {
      for (float value : matrix)
        if (!std::isfinite(value)) return false;
      return matrix[3] == 0 && matrix[7] == 0 && matrix[11] == 0 && matrix[15] == 1;
    }
    bool reuseProjection(const Matrix &rendered, const Matrix &requested, uint32_t resolution) {
      if (!resolution || !affine(rendered) || !affine(requested)) return false;
      Matrix inverse;
      try {
        inverse = scene::inverse(requested);
      } catch (remixapi_ErrorCode) {
        return false;
      }
      if (!affine(inverse)) return false;
      const auto previousClip = scene::multiply(rendered, inverse);
      if (!affine(previousClip)) return false;
      // Cascade fitting already pads XY by a texel and Z by one percent.
      // Keep substantially inside those margins: at most 1/16 of an XY texel
      // and two millionths of the depth range over the complete clip box.
      const float xyLimit = .125f / float(resolution);
      for (uint32_t corner = 0; corner < 8; ++corner) {
        const Vec3 point{corner & 1 ? 1.f : -1.f, corner & 2 ? 1.f : -1.f,
                         corner & 4 ? 1.f : 0.f};
        const auto previous = transformPoint(previousClip, point);
        for (uint32_t axis = 0; axis < 3; ++axis)
          if (!std::isfinite(previous[axis]) ||
              std::abs(previous[axis] - point[axis]) > (axis == 2 ? 2e-6f : xyLimit)) return false;
      }
      return true;
    }
  }
  std::array<bool, cascadeCount> SunCache::update(const std::shared_ptr<const scene::BakeScene> &source,
                                                 bool enabled, std::array<Matrix, cascadeCount> &matrices,
                                                 uint32_t resolution) {
    const bool reset = !m_initialized || m_enabled != enabled || m_hasSource != bool(source) ||
                       m_source.lock() != source || m_resolution != resolution;
    std::array<bool, cascadeCount> dirty;
    for (uint32_t c = 0; c < cascadeCount; ++c) {
      if (!reset && enabled && m_matrices[c] != matrices[c] &&
          reuseProjection(m_matrices[c], matrices[c], resolution)) matrices[c] = m_matrices[c];
      dirty[c] = reset || (enabled && m_matrices[c] != matrices[c]);
    }
    m_source = source;
    m_hasSource = bool(source);
    m_enabled = enabled;
    m_matrices = matrices;
    m_resolution = resolution;
    m_initialized = true;
    return dirty;
  }
}
