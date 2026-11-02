#pragma once
#include "scene/types.h"
#include <filesystem>

namespace remix::scene {
  // Largest texture edge accepted on the CPU side. Image creation still checks
  // the device's maxImageDimension2D (16384 on some GPUs).
  inline constexpr uint32_t maxTextureDimension = 32768;
  Pixels decodeTexture(const remixapi_TextureInfo &info);
  Pixels loadTexture(const std::filesystem::path &path);
  // BC1/BC3/BC4/BC5/BC6H/BC7 formats that compressTexture accepts.
  bool isBlockCompressed(remixapi_Format format);
  // R16G16B16A16 or R32G32B32A32 SFLOAT: decoded into Pixels::hdrMips.
  bool isFloatFormat(remixapi_Format format);
  // An IEEE half's value (infinities and NaNs read as 0).
  float halfToFloat(uint16_t bits);
  // Validates BC data like decodeTexture and keeps the blocks for GPU upload.
  // Only mip-0 alpha coverage bits and a <=64x64 preview are decoded.
  Pixels compressTexture(const remixapi_TextureInfo &info);
  // decodeTexture's result for a compressed texture whose upload payload is
  // still present (used when the device cannot sample BC formats).
  Pixels decodeCompressed(const Pixels &pixels);
  // Completes a single-level decoded texture with a box-filtered mip chain
  // (sRGB colors averaged in linear space). Hosts that upload only the top
  // level (D3D9 proxies, on request: remixapi_TextureInfoGenerateMipsEXT;
  // file-loaded textures always) otherwise leave distant surfaces to alias.
  void generateMips(Pixels &pixels);
  // Makes generateMips' levels again where level 0 changed in [x0, x1) x
  // [y0, y1): each level's texels whose box reads the changed ones (8-bit
  // levels; an update of a dynamic texture with generated mips).
  void updateMips(Pixels &pixels, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
  // The region of the next level whose texels read [x0, x1) x [y0, y1) of a
  // width x height level through generateMips' 2x2 box.
  inline void nextMipRegion(uint32_t &x0, uint32_t &y0, uint32_t &x1, uint32_t &y1, uint32_t width,
                            uint32_t height) {
    const uint32_t w = width > 1 ? width / 2 : 1, h = height > 1 ? height / 2 : 1;
    x0 /= 2;
    y0 /= 2;
    x1 = (x1 + 1) / 2 < w ? (x1 + 1) / 2 : w;
    y1 = (y1 + 1) / 2 < h ? (y1 + 1) / 2 : h;
  }
  // A DDS file (DX10 header) that loadTexture reads back: BC blocks verbatim
  // while CompressedPixels::upload is present (else the preview level),
  // otherwise the RGBA8 or RGBA32F levels. Empty: nothing to encode.
  std::vector<uint8_t> encodeDds(const Pixels &pixels);
} // namespace remix::scene
