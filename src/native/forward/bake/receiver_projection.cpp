#include "forward/bake/receiver_projection.h"
#include "forward/lightmap/data.h"
#include <algorithm>
#include <cmath>
namespace remix::forward::bake {
  ReceiverProjection projectReceivers(const scene::BakeScene &source, const lightmap::Layout &layout,
      const std::array<Vec3, 3> &basis, std::stop_token cancelled,
      std::span<const ReceiverTileOptions> options) {
    if (!std::isfinite(source.sceneScale) || source.sceneScale <= 0)
      throw std::invalid_argument("Invalid receiver projection scale");
    for (uint32_t axis = 0; axis < 3; ++axis) {
      if (std::abs(dot(basis[axis], basis[axis]) - 1) > 1e-4f ||
          !std::all_of(basis[axis].begin(), basis[axis].end(), [](float v) { return std::isfinite(v); }))
        throw std::invalid_argument("Invalid receiver projection basis");
      for (uint32_t other = 0; other < axis; ++other)
        if (std::abs(dot(basis[axis], basis[other])) > 1e-4f)
          throw std::invalid_argument("Nonorthogonal receiver projection basis");
    }
    if (options.size() > 8) throw std::invalid_argument("Too many receiver tile trials");
    std::vector<ReceiverTiles> tiles;
    for (const auto &option : options) tiles.emplace_back(option);
    ReceiverProjection result;
    for (const auto &mesh : lightmap::meshes(source, layout, cancelled)) {
      const auto &draw = source.draws.at(mesh.draw);
      for (size_t first = 0; first + 2 < mesh.indices.size(); first += 3) {
        if (cancelled.stop_requested()) throw std::runtime_error("Receiver projection superseded");
        const auto chart = mesh.vertices[mesh.indices[first]].coordinate[3];
        if (chart < .5f || chart != mesh.vertices[mesh.indices[first + 1]].coordinate[3] ||
            chart != mesh.vertices[mesh.indices[first + 2]].coordinate[3]) continue;
        ++result.triangles;
        std::array<Vec3, 3> triangle;
        for (uint32_t corner = 0; corner < 3; ++corner) {
          const auto &p = mesh.vertices[mesh.indices[first + corner]].surface.position;
          const auto world = transformPoint(draw.model, {p[0], p[1], p[2]});
          Vec3 projected;
          for (uint32_t axis = 0; axis < 3; ++axis) {
            projected[axis] = dot(basis[axis], world) * source.sceneScale;
            if (!std::isfinite(projected[axis])) throw std::invalid_argument("Nonfinite receiver projection");
          }
          result.bounds.include(projected);
          triangle[corner] = projected;
        }
        for (auto &trial : tiles) trial.include(triangle, cancelled);
      }
    }
    for (const auto &trial : tiles) {
      result.tiles.push_back(trial.coverage());
      const std::vector<std::array<int32_t, 2>> coordinates(trial.coordinates().begin(), trial.coordinates().end());
      result.depthLayouts.push_back(fitSunDepthLayout(coordinates, {}, cancelled));
    }
    return result;
  }
}
