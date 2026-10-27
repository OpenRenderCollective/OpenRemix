#include "forward/probes/normalization.h"
#include <bit>
#include <cmath>
#include <stdexcept>

namespace remix::forward::probes {
  Normalization captureNormalization(const Data &data, std::stop_token cancelled) {
    auto check = [&] {
      if (cancelled.stop_requested()) throw std::runtime_error("Capture normalization superseded");
    };
    check();
    const auto [count, size, levels, stride] = data.header.parameters;
    Normalization result;
    if (!count) {
      if (size || levels || stride || !data.texels.empty())
        throw std::invalid_argument("Nonempty normalization payload without captures");
      return result;
    }
    if (count > maxProbes || size < 8 || size > 512 || !std::has_single_bit(size) ||
        levels != std::bit_width(size) || stride != levelOffset(size, levels) ||
        data.texels.size() != size_t(count) * stride)
      throw std::invalid_argument("Invalid normalization capture dimensions");
    // Integrate each cubemap texel's solid angle rather than treating its
    // projected area as uniform. The factor two is the cosine-convolved L1
    // basis after division by pi; the constant term is mean radiance.
    auto area = [](double x, double y) { return std::atan2(x * y, std::sqrt(x*x + y*y + 1)); };
    std::vector<Vec4> quadrature(6ull * size * size);
    double solidAngle = 0;
    for (uint32_t y = 0; y < size; ++y) {
      check();
      for (uint32_t x = 0; x < size; ++x) {
        const double x0 = 2.0 * x / size - 1, x1 = 2.0 * (x + 1) / size - 1;
        const double y0 = 2.0 * y / size - 1, y1 = 2.0 * (y + 1) / size - 1;
        const double weight = area(x1,y1) - area(x0,y1) - area(x1,y0) + area(x0,y0);
        solidAngle += 6 * weight;
        for (uint32_t face = 0; face < 6; ++face) {
          const auto n = direction(face, float((x0+x1)*.5), float((y0+y1)*.5));
          quadrature[(face * size + y) * size + x] =
              {float(2*n[0]*weight), float(2*n[1]*weight), float(2*n[2]*weight), float(weight)};
        }
      }
    }
    for (uint32_t probe = 0; probe < count; ++probe) {
      check();
      std::array<double, 4> sum{};
      for (size_t i = 0; i < quadrature.size(); ++i) {
        if ((i & 4095u) == 0) check();
        const auto &color = data.texels[size_t(probe) * stride + i];
        for (uint32_t c = 0; c < 3; ++c)
          if (!std::isfinite(color[c]) || color[c] < 0 || color[c] > 65000)
            throw std::invalid_argument("Invalid normalization capture radiance");
        const double luma = .2126 * color[0] + .7152 * color[1] + .0722 * color[2];
        for (uint32_t c = 0; c < 4; ++c) sum[c] += luma * quadrature[i][c];
      }
      for (uint32_t c = 0; c < 4; ++c) result.coefficients[probe][c] = float(sum[c] / solidAngle);
    }
    return result;
  }
} // namespace remix::forward::probes
