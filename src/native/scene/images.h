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
  // A DDS file (DX10 header) that loadTexture reads back: BC blocks verbatim
  // while CompressedPixels::upload is present (else the preview level),
  // otherwise the RGBA8 or RGBA32F levels. Empty: nothing to encode.
  std::vector<uint8_t> encodeDds(const Pixels &pixels);
} // namespace remix::scene
