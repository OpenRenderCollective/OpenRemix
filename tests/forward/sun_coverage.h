#pragma once
#include "forward/lightmap/receiver_sampler.h"
#include "scene/fixtures.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace remix::test {
  // Independent polygon area oracle for a straight hard shadow over one luxel.
  inline double shadowCoverage(double nx, double ny, double offset) {
    using P = std::array<double, 2>;
    const std::array<P, 4> square{{{-.5, -.5}, {.5, -.5}, {.5, .5}, {-.5, .5}}};
    std::vector<P> clipped;
    for (uint32_t i = 0; i < square.size(); ++i) {
      const auto a = square[i], b = square[(i + 1) % square.size()];
      const double da = nx * a[0] + ny * a[1] - offset;
      const double db = nx * b[0] + ny * b[1] - offset;
      if (da >= 0)
        clipped.push_back(a);
      if ((da >= 0) != (db >= 0)) {
        const double t = da / (da - db);
        clipped.push_back({a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])});
      }
    }
    double area = 0;
    for (size_t i = 0; i < clipped.size(); ++i) {
      const auto a = clipped[i], b = clipped[(i + 1) % clipped.size()];
      area += a[0] * b[1] - a[1] * b[0];
    }
    return std::abs(area) * .5;
  }
  inline void sunlightCoverage(forward::lightmap::ReceiverSampler &sampler, uint32_t receiver) {
    const auto four = sampler.sample(receiver);
    std::array<forward::bake::Point, 16> sixteen, repeated;
    sampler.sample(receiver, sixteen);
    sampler.sample(receiver, repeated);
    require(std::memcmp(sixteen.data(), repeated.data(), sizeof(sixteen)) == 0,
            "Sixteen sunlight origins remain deterministic");
    double error4 = 0, error16 = 0;
    uint32_t cases = 0;
    for (uint32_t angle = 0; angle < 32; ++angle) {
      const double nx = std::cos(angle * 6.283185307179586 / 32);
      const double ny = std::sin(angle * 6.283185307179586 / 32);
      for (uint32_t step = 0; step < 129; ++step) {
        const double offset = (double(step) / 128 - .5) * 1.5;
        const double exact = shadowCoverage(nx, ny, offset);
        const auto estimate = [&](const auto &sites) {
          uint32_t visible = 0;
          for (const auto &site : sites) {
            const double x = (site.position[0] - 10) / .8;
            const double y = site.position[1] / 1.2;
            require(std::abs(x) <= .50001 && std::abs(y) <= .50001 && std::abs(site.position[2] - .5) < 1e-5,
                    "Sunlight origins stay within the true receiving luxel");
            visible += nx * x + ny * y >= offset;
          }
          return double(visible) / sites.size();
        };
        error4 += std::abs(estimate(four) - exact);
        error16 += std::abs(estimate(sixteen) - exact);
        ++cases;
      }
    }
    std::printf("Sun coverage: %u edges, MAE four %.6f / sixteen %.6f\n", cases, error4 / cases,
                error16 / cases);
    require(error16 < error4 * .65, "More spatial origins reduce hard-shadow coverage error");
  }
} // namespace remix::test
