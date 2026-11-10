// Independent CPU reference for the shared Prism transform, without output
// encoding (the numeric HDR fixtures render to a linear UNORM attachment).
std::array<float, 3> prismDisplay(std::array<float, 3> color, float peak = 1) {
  const float inset[3][3] = {{.72634649f, .16665483f, .10699868f},
                             {.13423698f, .70013888f, .16562414f},
                             {.07263955f, .06012051f, .86723994f}};
  const float outset[3][3] = {{1.45021560f, -.33533138f, -.11488422f},
                              {-.25347057f, 1.51071102f, -.25724045f},
                              {-.10389769f, -.07664130f, 1.18053899f}};
  auto multiply = [](const auto &matrix, const auto &value) {
    std::array<float, 3> result{};
    for (uint32_t row = 0; row < 3; ++row)
      for (uint32_t column = 0; column < 3; ++column)
        result[row] += matrix[row][column] * value[column];
    return result;
  };
  for (auto &value : color) value = std::max(value, 0.f);
  color = multiply(inset, color);
  for (auto &value : color) {
    if (value <= .18f) continue;
    const double range = double(peak) - .18;
    const double t = (double(value) - .18) / range;
    value = float(.18 + range * t / (1 + t * std::exp(-.01 / (t * t))));
  }
  color = multiply(outset, color);
  for (auto &value : color) value = std::clamp(value, 0.f, peak);
  return color;
}
