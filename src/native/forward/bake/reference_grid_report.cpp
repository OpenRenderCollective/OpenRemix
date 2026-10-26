#include "forward/bake/reference_capture.h"
#include "forward/bake/grid.h"
#include "forward/bake/spherical_harmonics.h"
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <locale>

namespace remix::forward::bake {
  void ReferenceCapture::reportGrid(const Grid &grid) const noexcept {
    if (!hemisphere && !surface) return;
    try {
      auto path = m_output;
      path.replace_extension(".grid.json");
      if (std::filesystem::exists(path)) throw std::runtime_error("grid report already exists");
      std::ofstream out(path);
      out.exceptions(std::ios::failbit | std::ios::badbit);
      out.imbue(std::locale::classic());
      out << std::setprecision(9) << "{\"version\":1,\"sceneHash\":\"" << sceneHash
          << "\",\"baseGridOnly\":true,\"scope\":\"CPU probe reconstruction at reference sites; normal bias retained; excludes separately attributed styles, analytic sunlight and material response\",\"points\":[";
      auto vector = [&](const Vec3 &value) {
        out << '[' << value[0] << ',' << value[1] << ',' << value[2] << ']';
      };
      for (size_t i = 0; i < points.size(); ++i) {
        if (i) out << ',';
        const auto &point = points[i];
        const Vec3 position{point.position[0], point.position[1], point.position[2]};
        const Vec3 normal = normalized({point.normal[0], point.normal[1], point.normal[2]});
        const auto stencil = probeStencil(grid, position, normal);
        const auto value = sample(grid, position, normal);
        const auto sh = irradianceBasis(normal);
        out << "{\"position\":"; vector(position);
        out << ",\"normal\":"; vector(normal);
        out << ",\"irradiance\":";
        if (value) vector(*value); else out << "null";
        out << ",\"stencilCount\":" << stencil.count << ",\"rootCell\":";
        if (stencil.rootCell == ~0u) out << "null"; else out << stencil.rootCell;
        out << ",\"receiverPositions\":[";
        vector(stencil.receiverPositions[0]); out << ','; vector(stencil.receiverPositions[1]);
        out << "],\"probes\":[";
        bool first = true;
        for (uint32_t slot = 0; slot < stencil.count; ++slot) {
          const float weight = stencil.weights[slot];
          if (weight <= 0) continue;
          if (!first) out << ',';
          first = false;
          const auto index = stencil.indices[slot];
          Vec3 raw{};
          for (uint32_t j = 0; j < 9; ++j) for (uint32_t c = 0; c < 3; ++c)
            raw[c] += grid.probes[index].irradiance[j][c] * sh[j];
          out << "{\"slot\":" << slot << ",\"index\":" << index << ",\"weight\":" << weight
              << ",\"rawIrradiance\":";
          vector(raw); out << '}';
        }
        out << "]}";
      }
      out << "]}\n";
    } catch (const std::exception &error) {
      std::fprintf(stderr, "Reference grid comparison unavailable: %s\n", error.what());
    }
  }
}
