#pragma once
#include <array>
#include <string_view>

namespace remix::config {
  // Sky quality presets: the NUMOS settings that set the sky's GPU cost. High
  // is NUMOS's own defaults. A profile whose values match no row shows Custom.
  struct SkyQualityValue {
    std::string_view key, value;
  };
  struct SkyQuality {
    const char *name;
    std::array<SkyQualityValue, 5> values;
  };
  inline constexpr SkyQuality skyQualities[] = {
      {"Low",
       {{{"rtx.atmosphere.cloudMarchRate", "2"},
         {"rtx.atmosphere.cloudPerformancePreset", "1"},
         {"rtx.atmosphere.cloudViewSamplesMax", "64"},
         {"rtx.atmosphere.aerialPerspectiveFroxelSizePixels", "16"},
         {"rtx.atmosphere.aerialPerspectiveFroxelDepthSlices", "48"}}}},
      {"Medium",
       {{{"rtx.atmosphere.cloudMarchRate", "2"},
         {"rtx.atmosphere.cloudPerformancePreset", "1"},
         {"rtx.atmosphere.cloudViewSamplesMax", "96"},
         {"rtx.atmosphere.aerialPerspectiveFroxelSizePixels", "12"},
         {"rtx.atmosphere.aerialPerspectiveFroxelDepthSlices", "64"}}}},
      {"High",
       {{{"rtx.atmosphere.cloudMarchRate", "0"},
         {"rtx.atmosphere.cloudPerformancePreset", "0"},
         {"rtx.atmosphere.cloudViewSamplesMax", "128"},
         {"rtx.atmosphere.aerialPerspectiveFroxelSizePixels", "8"},
         {"rtx.atmosphere.aerialPerspectiveFroxelDepthSlices", "96"}}}},
      {"Ultra",
       {{{"rtx.atmosphere.cloudMarchRate", "1"},
         {"rtx.atmosphere.cloudPerformancePreset", "0"},
         {"rtx.atmosphere.cloudViewSamplesMax", "192"},
         {"rtx.atmosphere.aerialPerspectiveFroxelSizePixels", "6"},
         {"rtx.atmosphere.aerialPerspectiveFroxelDepthSlices", "128"}}}},
  };
} // namespace remix::config
