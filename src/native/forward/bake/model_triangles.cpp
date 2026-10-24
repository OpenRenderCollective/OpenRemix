#include "forward/bake/model_triangles.h"
#include "forward/bake/probe_receiver_geometry.h"
#include "forward/material.h"
#include <cmath>
#include <stdexcept>

namespace remix::forward::bake {
  void forEachModelTriangle(const scene::BakeScene &source,
      const std::function<void(const ModelTriangle &)> &visit, std::stop_token cancelled) {
    if (!visit) throw std::invalid_argument("Missing model triangle visitor");
    if (!std::isfinite(source.sceneScale) || source.sceneScale <= 0)
      throw std::invalid_argument("Invalid model chart scene scale");
    const auto ranges = unchartedReceiverRanges(source, cancelled);
    for (const auto &range : ranges) {
      const auto &draw = source.draws[range.draw];
      if (draw.classification != REMIXAPI_INSTANCE_STATIC_EXT) continue;
      for (size_t first = range.firstIndex; first < size_t(range.firstIndex) + range.indexCount; first += 3) {
        if (cancelled.stop_requested()) throw std::runtime_error("Model chart generation superseded");
        std::array<Vec3, 3> positions;
        for (uint32_t corner = 0; corner < 3; ++corner) {
          const int64_t index = int64_t(draw.surface->indices[first + corner]) + draw.vertexOffset;
          if (index < 0 || uint64_t(index) >= draw.surface->vertices.size())
            throw std::invalid_argument("Invalid model chart vertex index");
          const auto &vertex = draw.surface->vertices[size_t(index)];
          positions[corner] = transformPoint(draw.model, {vertex.position[0], vertex.position[1], vertex.position[2]});
          for (float value : positions[corner])
            if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite model chart position");
        }
        std::array<double, 3> a, b, cross;
        for (uint32_t axis = 0; axis < 3; ++axis) {
          a[axis] = double(positions[1][axis]) - positions[0][axis];
          b[axis] = double(positions[2][axis]) - positions[0][axis];
        }
        for (uint32_t axis = 0; axis < 3; ++axis)
          cross[axis] = a[(axis + 1) % 3] * b[(axis + 2) % 3] - a[(axis + 2) % 3] * b[(axis + 1) % 3];
        // Match the world-metric parameterizer's degeneracy threshold.
        const double area = std::hypot(cross[0], cross[1], cross[2]) *
                            double(source.sceneScale) * source.sceneScale;
        const double edges = std::hypot(a[0], a[1], a[2]) * std::hypot(b[0], b[1], b[2]);
        if (!(area > 1e-12) || std::hypot(cross[0], cross[1], cross[2]) <= edges * 1e-6) continue;
        if (!playableReceiverTriangle(source, positions, cancelled)) continue;
        visit(ModelTriangle{range.draw, uint32_t(first), positions, area * .5});
      }
    }
  }
}
