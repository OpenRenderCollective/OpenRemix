// Copyright 2016 Pixar. Adapted from OpenUSD usdLux/blackbody.cpp.
// Licensed under the Tomorrow Open Source Technology License 1.0;
// see OpenUSD-LICENSE.txt beside this file.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
namespace remix::scene {
  inline std::array<float, 3> blackbody(float temperature) {
    static constexpr float knots[][3] = {
        {1.000000f, 0.027490f, 0.000000f}, {1.000000f, 0.027490f, 0.000000f}, {1.000000f, 0.149664f, 0.000000f},
        {1.000000f, 0.256644f, 0.008095f}, {1.000000f, 0.372033f, 0.067450f}, {1.000000f, 0.476725f, 0.153601f},
        {1.000000f, 0.570376f, 0.259196f}, {1.000000f, 0.653480f, 0.377155f}, {1.000000f, 0.726878f, 0.501606f},
        {1.000000f, 0.791543f, 0.628050f}, {1.000000f, 0.848462f, 0.753228f}, {1.000000f, 0.898581f, 0.874905f},
        {1.000000f, 0.942771f, 0.991642f}, {0.906947f, 0.890456f, 1.000000f}, {0.828247f, 0.841838f, 1.000000f},
        {0.765791f, 0.801896f, 1.000000f}, {0.715255f, 0.768579f, 1.000000f}, {0.673683f, 0.740423f, 1.000000f},
        {0.638992f, 0.716359f, 1.000000f}, {0.609681f, 0.695588f, 1.000000f}, {0.609681f, 0.695588f, 1.000000f},
        {0.609681f, 0.695588f, 1.000000f},

    };
    static constexpr float basis[4][4] = {
        {-.5f, 1.5f, -1.5f, .5f}, {1, -2.5f, 2, -.5f}, {-.5f, 0, .5f, 0}, {0, 1, 0, 0}};
    float x = std::clamp((temperature - 1000) / 9000, 0.f, 1.f) * (std::size(knots) - 4);
    int segment = int(std::floor(x));
    float u = x - segment;
    std::array<float, 3> rgb{};
    for (int c = 0; c < 3; ++c) {
      float a[4]{};
      for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
          a[i] += basis[i][j] * knots[segment + j][c];
      rgb[c] = ((a[0] * u + a[1]) * u + a[2]) * u + a[3];
    }
    float luminance = rgb[0] * .2126f + rgb[1] * .7152f + rgb[2] * .0722f;
    for (auto &c : rgb)
      c = std::max(c / luminance, 0.f);
    return rgb;
  }
} // namespace remix::scene
