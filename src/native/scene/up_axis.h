#pragma once
#include "scene/types.h"
#include <cmath>
#include <cstdint>

namespace remix::scene {
  // The world's up axis for rtx.native.pathtrace.volumetricUpAxis Auto (3):
  // game cameras do not roll, so their right axis stays horizontal and their
  // up axis leans toward the world's. Per world axis, averaged over about a
  // second: how far the camera's up axis points along it, less how far its
  // right axis does; only a clear difference moves the result (1 Y, 2 Z).
  class UpAxisDetector {
  public:
    void observe(const Matrix &viewToWorld) {
      const float right = std::hypot(viewToWorld[0], viewToWorld[1], viewToWorld[2]),
                  up = std::hypot(viewToWorld[4], viewToWorld[5], viewToWorld[6]);
      if (!(right > 1e-6f && up > 1e-6f))
        return;
      for (int i = 0; i < 3; ++i) {
        const float score = std::abs(viewToWorld[4 + i]) / up - std::abs(viewToWorld[i]) / right;
        m_score[i] += (score - m_score[i]) * .02f;
      }
      if (m_axis == 2 && m_score[1] > m_score[2] + .2f)
        m_axis = 1;
      else if (m_axis == 1 && m_score[2] > m_score[1] + .2f)
        m_axis = 2;
    }
    // A setting's axis: itself, or the detected one for Auto.
    uint32_t resolve(uint32_t setting) const { return setting > 2 ? m_axis : setting; }

  private:
    float m_score[3] = {};
    uint32_t m_axis = 2;
  };
} // namespace remix::scene
