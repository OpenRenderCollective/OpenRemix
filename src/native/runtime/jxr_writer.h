#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace remix {
  // Writes width x height tightly packed RGBA half floats, scRGB (linear BT.709,
  // 1.0 = 80 nits, alpha 1), as a lossless JPEG XR file in the 64bpp RGBA half
  // format Windows uses for HDR captures. On failure returns false and sets error.
  bool writeJxrScRgb(const std::filesystem::path &path, uint32_t width, uint32_t height,
    const uint16_t *rgbaHalf, std::string &error);
} // namespace remix
