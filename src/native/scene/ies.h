#pragma once
#include <array>
#include <string_view>

namespace remix::scene {
  // IES photometric profiles (remixapi_LightInfoIesProfileEXT): a light's
  // emission by the angle from its axis, kIesBins samples from 0 to 180
  // degrees (both ends included), the brightest 1. The shaders read them in
  // emitterShaping (light_bvh.glsl).
  constexpr unsigned kIesBins = 256;
  using IesTable = std::array<float, kIesBins>;
  // An IESNA LM-63 file's contents as a table: candela by vertical angle,
  // averaged over the horizontal angles (profiles are taken as rotationally
  // symmetric). False when the text is not such a file, or emits nothing.
  bool parseIes(std::string_view text, IesTable &table);
} // namespace remix::scene
