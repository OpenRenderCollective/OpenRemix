#include "scene/images.h"
#include <remix/remix_native_ext.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_ONLY_HDR
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 32768 // scene::maxTextureDimension
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <image/stb_image.h>
#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#include <image/bcdec.h>

namespace remix::scene {
  namespace {
    constexpr auto invalid = REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    constexpr auto unavailable = REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    uint32_t word(const std::vector<uint8_t> &data, size_t offset) {
      if (offset + 4 > data.size()) {
        throw invalid;
      }
      return uint32_t(data[offset]) | uint32_t(data[offset + 1]) << 8 | uint32_t(data[offset + 2]) << 16 |
             uint32_t(data[offset + 3]) << 24;
    }
    Pixels decodeFloat(uint32_t width, uint32_t height, uint32_t mips, unsigned channels, bool half,
                       const uint8_t *data, size_t size) {
      if (!width || !height || width > maxTextureDimension || height > maxTextureDimension || !data || channels < 1 || channels > 4)
        throw invalid;
      unsigned levels = 0;
      for (auto n = std::max(width, height); n; n >>= 1)
        ++levels;
      if (mips > levels)
        throw invalid;
      Pixels out{width, height, false, {}};
      size_t offset = 0, total = 0;
      for (unsigned mip = 0; mip < std::max(1u, mips); ++mip) {
        size_t count = size_t(width) * height, bytes = count * channels * (half ? 2 : 4);
        total += count * 20;
        if (bytes > size - offset || total > 512u * 1024 * 1024)
          throw invalid;
        auto &floats = out.hdrMips.emplace_back(count * 4);
        auto &pixels = out.mips.emplace_back(count * 4);
        for (size_t i = 0; i < count; ++i)
          for (unsigned c = 0; c < 4; ++c) {
            float value = c == 3 ? 1 : 0;
            if (c < channels || (channels == 1 && c < 3)) {
              const size_t source = offset + (i * channels + (channels == 1 ? 0 : c)) * (half ? 2 : 4);
              if (half) {
                const unsigned bits = unsigned(data[source]) | (unsigned(data[source + 1]) << 8),
                               exponent = (bits >> 10) & 31, fraction = bits & 1023;
                value = exponent == 31 ? 0
                                       : std::ldexp(float(fraction + (exponent ? 1024 : 0)),
                                                    (exponent ? int(exponent) - 15 : -14) - 10);
                if (bits & 0x8000)
                  value = -value;
              } else {
                std::memcpy(&value, data + source, 4);
                if (!std::isfinite(value))
                  value = 0;
              }
            }
            floats[i * 4 + c] = value;
            pixels[i * 4 + c] = uint8_t(std::clamp(value, 0.f, 1.f) * 255);
          }
        offset += bytes;
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
      }
      return out;
    }
    Pixels decode(uint32_t width, uint32_t height, uint32_t mips, bool srgb, uint32_t block, bool bgra,
                  const uint8_t *data, size_t size) {
      if (!width || !height || width > maxTextureDimension || height > maxTextureDimension || !data) {
        throw invalid;
      }
      uint32_t levels = 0;
      for (auto dimension = std::max(width, height); dimension; dimension >>= 1) {
        ++levels;
      }
      mips = std::max(1u, mips);
      if (mips > levels) {
        throw invalid;
      }
      Pixels out{width, height, srgb, {}};
      out.twoChannel = block == 5;
      size_t offset = 0, total = 0;
      for (uint32_t mip = 0; mip < mips; ++mip) {
        const size_t bytes =
            block ? size_t((width + 3) / 4) * ((height + 3) / 4) * ((block == 1 || block == 4) ? 8 : 16)
                  : size_t(width) * height * 4;
        const size_t decodedSize = size_t(width) * height * 4;
        total += decodedSize * ((block == 6 || block == 16) ? 5 : 1);
        if (offset > size || bytes > size - offset || total > 512u * 1024 * 1024) {
          throw invalid;
        }
        auto &pixels = out.mips.emplace_back(decodedSize);
        if (block == 6 || block == 16)
          out.hdrMips.emplace_back(decodedSize);
        if (!block) {
          std::memcpy(pixels.data(), data + offset, bytes);
          if (bgra) {
            for (size_t i = 0; i < pixels.size(); i += 4) {
              std::swap(pixels[i], pixels[i + 2]);
            }
          }
        } else {
          size_t cursor = offset;
          for (uint32_t y = 0; y < height; y += 4) {
            for (uint32_t x = 0; x < width; x += 4) {
              alignas(8) uint8_t source[16] = {}, result[64] = {};
              std::memcpy(source, data + cursor, (block == 1 || block == 4) ? 8 : 16);
              if (block == 1) {
                bcdec_bc1(source, result, 16);
              } else if (block == 2) {
                bcdec_bc2(source, result, 16);
              } else if (block == 3) {
                bcdec_bc3(source, result, 16);
              } else if (block == 4) {
                uint8_t scalar[16];
                bcdec_bc4(source, scalar, 4);
                for (int i = 0; i < 16; ++i) {
                  result[i * 4] = result[i * 4 + 1] = result[i * 4 + 2] = scalar[i];
                  result[i * 4 + 3] = 255;
                }
              } else if (block == 6 || block == 16) {
                float rgb[48];
                bcdec_bc6h_float(source, rgb, 12, block == 16);
                for (uint32_t row = 0; row < std::min(4u, height - y); ++row)
                  for (uint32_t col = 0; col < std::min(4u, width - x); ++col) {
                    const auto dst = (size_t(y + row) * width + x + col) * 4;
                    for (unsigned c = 0; c < 3; ++c) {
                      const float value = rgb[(row * 4 + col) * 3 + c];
                      out.hdrMips.back()[dst + c] = std::isfinite(value) ? value : 0;
                      result[(row * 4 + col) * 4 + c] =
                          uint8_t(std::clamp(out.hdrMips.back()[dst + c], 0.f, 1.f) * 255);
                    }
                    out.hdrMips.back()[dst + 3] = 1;
                    result[(row * 4 + col) * 4 + 3] = 255;
                  }
              } else if (block == 5) {
                uint8_t rg[32];
                bcdec_bc5(source, rg, 8);
                for (int i = 0; i < 16; ++i) {
                  result[i * 4] = rg[i * 2];
                  result[i * 4 + 1] = rg[i * 2 + 1];
                  result[i * 4 + 3] = 255;
                }
              } else if (block == 7) {
                bcdec_bc7(source, result, 16);
              } else {
                throw unavailable;
              }
              for (uint32_t row = 0; row < std::min(4u, height - y); ++row) {
                std::memcpy(pixels.data() + (size_t(y + row) * width + x) * 4, result + row * 16,
                            std::min(4u, width - x) * 4);
              }
              cursor += (block == 1 || block == 4) ? 8 : 16;
            }
          }
        }
        offset += bytes;
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
      }
      return out;
    }
  } // namespace

  Pixels decodeTexture(const remixapi_TextureInfo &info) {
    if (info.depth > 1 || info.dataSize > SIZE_MAX) {
      throw invalid;
    }
    uint32_t block = 0;
    bool srgb = false, bgra = false, bc1Alpha = false;
    switch (uint32_t(info.format)) {
    case REMIXAPI_FORMAT_R8G8B8A8_SRGB:
      srgb = true;
      break;
    case REMIXAPI_FORMAT_R8G8B8A8_UNORM:
      break;
    case REMIXAPI_FORMAT_B8G8R8A8_SRGB:
      srgb = true;
      bgra = true;
      break;
    case REMIXAPI_FORMAT_B8G8R8A8_UNORM:
      bgra = true;
      break;
    case REMIXAPI_FORMAT_BC1_RGB_SRGB:
      srgb = true;
      [[fallthrough]];
    case REMIXAPI_FORMAT_BC1_RGB_UNORM:
      block = 1;
      break;
    case REMIXAPI_FORMAT_BC1_RGBA_SRGB_EXT:
      srgb = true;
      [[fallthrough]];
    case REMIXAPI_FORMAT_BC1_RGBA_UNORM_EXT:
      block = 1;
      bc1Alpha = true;
      break;
    case REMIXAPI_FORMAT_BC3_SRGB:
      srgb = true;
      [[fallthrough]];
    case REMIXAPI_FORMAT_BC3_UNORM:
      block = 3;
      break;
    case REMIXAPI_FORMAT_BC4_UNORM_EXT:
      block = 4;
      break;
    case REMIXAPI_FORMAT_BC6H_UFLOAT_EXT:
      block = 6;
      break;
    case REMIXAPI_FORMAT_BC6H_SFLOAT_EXT:
      block = 16;
      break;
    case REMIXAPI_FORMAT_BC5_UNORM:
      block = 5;
      break;
    case REMIXAPI_FORMAT_BC7_SRGB:
      srgb = true;
      [[fallthrough]];
    case REMIXAPI_FORMAT_BC7_UNORM:
      block = 7;
      break;
    default:
      throw unavailable;
    }
    auto result = decode(info.width, info.height, info.mipLevels, srgb, block, bgra,
                         static_cast<const uint8_t *>(info.data), size_t(info.dataSize));
    // The raw API exposes BC1 RGB, whereas DDS DXT1 can also carry one-bit alpha.
    if (block == 1 && !bc1Alpha) {
      for (auto &mip : result.mips) {
        for (size_t i = 3; i < mip.size(); i += 4) {
          mip[i] = 255;
        }
      }
    }
    return result;
  }

  namespace {
    size_t blockBytes(uint32_t block) { return (block == 1 || block == 4) ? 8 : 16; }
    size_t levelBytes(uint32_t block, uint32_t width, uint32_t height) {
      return size_t((width + 3) / 4) * ((height + 3) / 4) * blockBytes(block);
    }
    // One 4x4 block exactly as decode()/decodeTexture() produce it: RGBA8 rows of
    // 16 bytes; BC6H also writes sanitized linear RGB (3 floats per texel).
    void decodeBlock(uint32_t block, bool bc1Alpha, const uint8_t *data, uint8_t rgba[64], float *rgb) {
      alignas(8) uint8_t source[16] = {};
      std::memcpy(source, data, blockBytes(block));
      std::memset(rgba, 0, 64);
      if (block == 1) {
        bcdec_bc1(source, rgba, 16);
        if (!bc1Alpha)
          for (int i = 0; i < 16; ++i)
            rgba[i * 4 + 3] = 255;
      } else if (block == 3) {
        bcdec_bc3(source, rgba, 16);
      } else if (block == 4) {
        uint8_t scalar[16];
        bcdec_bc4(source, scalar, 4);
        for (int i = 0; i < 16; ++i) {
          rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = scalar[i];
          rgba[i * 4 + 3] = 255;
        }
      } else if (block == 5) {
        uint8_t rg[32];
        bcdec_bc5(source, rg, 8);
        for (int i = 0; i < 16; ++i) {
          rgba[i * 4] = rg[i * 2];
          rgba[i * 4 + 1] = rg[i * 2 + 1];
          rgba[i * 4 + 3] = 255;
        }
      } else if (block == 6 || block == 16) {
        float values[48];
        bcdec_bc6h_float(source, values, 12, block == 16);
        for (int i = 0; i < 16; ++i) {
          for (int c = 0; c < 3; ++c) {
            const float value = std::isfinite(values[i * 3 + c]) ? values[i * 3 + c] : 0;
            if (rgb)
              rgb[i * 3 + c] = value;
            rgba[i * 4 + c] = uint8_t(std::clamp(value, 0.f, 1.f) * 255);
          }
          rgba[i * 4 + 3] = 255;
        }
      } else {
        bcdec_bc7(source, rgba, 16);
      }
    }
    // Bits [start, start + count) of a little-endian 128-bit block, count <= 32.
    uint32_t blockBits(const uint8_t *data, unsigned start, unsigned count) {
      uint64_t low, high;
      std::memcpy(&low, data, 8);
      std::memcpy(&high, data + 8, 8);
      const uint64_t value =
          start >= 64 ? high >> (start - 64) : (low >> start) | (start ? high << (64 - start) : 0);
      return uint32_t(value & ((1ull << count) - 1));
    }
    // 16-bit mask of the block's texels whose decoded alpha is not 255, without
    // decoding blocks whose encoding proves them opaque.
    uint32_t transparentTexels(uint32_t block, bool bc1Alpha, const uint8_t *data) {
      uint32_t mask = 0;
      if (block == 1) {
        // Only the three-color mode (color0 <= color1) has a transparent index.
        if (!bc1Alpha || (data[0] | data[1] << 8) > (data[2] | data[3] << 8))
          return 0;
        const uint32_t indices = uint32_t(data[4]) | uint32_t(data[5]) << 8 | uint32_t(data[6]) << 16 |
                                 uint32_t(data[7]) << 24;
        for (unsigned i = 0; i < 16; ++i)
          if (((indices >> (2 * i)) & 3) == 3)
            mask |= 1u << i;
        return mask;
      }
      if (block == 3) {
        alignas(8) uint8_t source[8];
        uint8_t alpha[16];
        std::memcpy(source, data, 8);
        bcdec_bc4(source, alpha, 4); // BC3's alpha half is a BC4 block.
        for (unsigned i = 0; i < 16; ++i)
          if (alpha[i] != 255)
            mask |= 1u << i;
        return mask;
      }
      if (block != 7)
        return 0; // BC4, BC5 and BC6H decode with alpha 255.
      const unsigned mode = data[0] ? unsigned(std::countr_zero(unsigned(data[0]))) : 8;
      if (mode < 4)
        return 0; // Modes 0-3 carry no alpha.
      // Opaque endpoints interpolate to exactly 255. Mode 4/5 rotations move
      // alpha into a color channel, so those blocks are decoded instead.
      bool opaque = false;
      if (mode == 4)
        opaque = !blockBits(data, 5, 2) && blockBits(data, 38, 6) == 63 && blockBits(data, 44, 6) == 63;
      else if (mode == 5)
        opaque = !blockBits(data, 6, 2) && blockBits(data, 50, 8) == 255 && blockBits(data, 58, 8) == 255;
      else if (mode == 6)
        opaque = blockBits(data, 49, 7) == 127 && blockBits(data, 56, 7) == 127 && blockBits(data, 63, 2) == 3;
      else if (mode == 7)
        opaque = blockBits(data, 74, 20) == 0xfffff && blockBits(data, 94, 4) == 15;
      if (opaque)
        return 0;
      uint8_t rgba[64];
      decodeBlock(7, false, data, rgba, nullptr);
      for (unsigned i = 0; i < 16; ++i)
        if (rgba[i * 4 + 3] != 255)
          mask |= 1u << i;
      return mask;
    }
    // Word-at-a-time content hash. Byte-wise FNV over a multi-megabyte chain
    // would dominate the cost of a compressed CreateTexture.
    uint64_t hashBlocks(const uint8_t *data, size_t size, uint64_t seed) {
      uint64_t lanes[4] = {seed ^ 0x9e3779b97f4a7c15ull, seed ^ 0xc2b2ae3d27d4eb4full,
                           seed ^ 0x165667b19e3779f9ull, seed ^ 0x27d4eb2f165667c5ull};
      size_t i = 0;
      for (; i + 32 <= size; i += 32)
        for (unsigned k = 0; k < 4; ++k) {
          uint64_t value;
          std::memcpy(&value, data + i + 8 * k, 8);
          lanes[k] = std::rotl(lanes[k] + value * 0xc2b2ae3d27d4eb4full, 31) * 0x9e3779b97f4a7c15ull;
        }
      uint64_t hash = hashBytes(data + i, size - i, seed ^ size);
      for (auto lane : lanes)
        hash = std::rotl(hash ^ lane, 27) * 0x9e3779b97f4a7c15ull + 0x52dce729ull;
      hash ^= hash >> 33;
      hash *= 0xff51afd7ed558ccdull;
      hash ^= hash >> 33;
      return hash ? hash : 1;
    }
  } // namespace

  bool isBlockCompressed(remixapi_Format format) {
    switch (uint32_t(format)) {
    case REMIXAPI_FORMAT_BC1_RGB_SRGB:
    case REMIXAPI_FORMAT_BC1_RGB_UNORM:
    case REMIXAPI_FORMAT_BC1_RGBA_SRGB_EXT:
    case REMIXAPI_FORMAT_BC1_RGBA_UNORM_EXT:
    case REMIXAPI_FORMAT_BC3_SRGB:
    case REMIXAPI_FORMAT_BC3_UNORM:
    case REMIXAPI_FORMAT_BC4_UNORM_EXT:
    case REMIXAPI_FORMAT_BC5_UNORM:
    case REMIXAPI_FORMAT_BC6H_UFLOAT_EXT:
    case REMIXAPI_FORMAT_BC6H_SFLOAT_EXT:
    case REMIXAPI_FORMAT_BC7_SRGB:
    case REMIXAPI_FORMAT_BC7_UNORM:
      return true;
    default:
      return false;
    }
  }

  Pixels compressTexture(const remixapi_TextureInfo &info) {
    if (info.depth > 1 || info.dataSize > SIZE_MAX)
      throw invalid;
    auto c = std::make_shared<CompressedPixels>();
    c->format = uint32_t(info.format);
    bool srgb = false;
    switch (uint32_t(info.format)) {
    case REMIXAPI_FORMAT_BC1_RGB_SRGB:
      srgb = true;
      [[fallthrough]];
    case REMIXAPI_FORMAT_BC1_RGB_UNORM:
      c->block = 1;
      break;
    case REMIXAPI_FORMAT_BC1_RGBA_SRGB_EXT:
      srgb = true;
      [[fallthrough]];
    case REMIXAPI_FORMAT_BC1_RGBA_UNORM_EXT:
      c->block = 1;
      c->bc1Alpha = true;
      break;
    case REMIXAPI_FORMAT_BC3_SRGB:
      srgb = true;
      [[fallthrough]];
    case REMIXAPI_FORMAT_BC3_UNORM:
      c->block = 3;
      break;
    case REMIXAPI_FORMAT_BC4_UNORM_EXT:
      c->block = 4;
      break;
    case REMIXAPI_FORMAT_BC5_UNORM:
      c->block = 5;
      break;
    case REMIXAPI_FORMAT_BC6H_UFLOAT_EXT:
      c->block = 6;
      break;
    case REMIXAPI_FORMAT_BC6H_SFLOAT_EXT:
      c->block = 16;
      break;
    case REMIXAPI_FORMAT_BC7_SRGB:
      srgb = true;
      [[fallthrough]];
    case REMIXAPI_FORMAT_BC7_UNORM:
      c->block = 7;
      break;
    default:
      throw unavailable;
    }
    const uint32_t width = info.width, height = info.height;
    const auto *data = static_cast<const uint8_t *>(info.data);
    const size_t size = size_t(info.dataSize);
    if (!width || !height || width > maxTextureDimension || height > maxTextureDimension || !data)
      throw invalid;
    uint32_t available = 0;
    for (auto dimension = std::max(width, height); dimension; dimension >>= 1)
      ++available;
    c->levels = std::max(1u, info.mipLevels);
    if (c->levels > available)
      throw invalid;
    // Accept exactly what decodeTexture accepts, including its decoded-size cap.
    size_t used = 0, decoded = 0;
    for (uint32_t mip = 0, w = width, h = height; mip < c->levels; ++mip) {
      const size_t bytes = levelBytes(c->block, w, h);
      decoded += size_t(w) * h * 4 * ((c->block == 6 || c->block == 16) ? 5 : 1);
      if (bytes > size - used || decoded > 512u * 1024 * 1024)
        throw invalid;
      used += bytes;
      w = std::max(1u, w / 2);
      h = std::max(1u, h / 2);
    }
    c->upload.assign(data, data + used);
    const uint64_t identity[] = {c->format, c->levels, width, height};
    c->hash = hashBlocks(c->upload.data(), c->upload.size(), hashBytes(identity, sizeof(identity)));

    const size_t stride = blockBytes(c->block);
    const uint32_t blocksWide = (width + 3) / 4, blocksHigh = (height + 3) / 4;
    if (c->block == 3 || c->block == 7 || (c->block == 1 && c->bc1Alpha)) {
      for (uint32_t by = 0; by < blocksHigh; ++by)
        for (uint32_t bx = 0; bx < blocksWide; ++bx) {
          const uint32_t mask =
              transparentTexels(c->block, c->bc1Alpha, c->upload.data() + (size_t(by) * blocksWide + bx) * stride);
          if (!mask)
            continue;
          if (c->transparent.empty())
            c->transparent.assign((size_t(width) * height + 63) / 64, 0);
          for (uint32_t i = 0; i < 16; ++i) {
            const uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
            if ((mask >> i & 1) && x < width && y < height) {
              const size_t texel = size_t(y) * width + x;
              c->transparent[texel >> 6] |= 1ull << (texel & 63);
            }
          }
        }
    }

    // Preview: the first supplied level no larger than 64x64 decodes exactly;
    // short chains point-sample their smallest level, one block per texel.
    uint32_t lw = width, lh = height;
    size_t levelOffset = 0;
    for (uint32_t mip = 0; mip + 1 < c->levels && std::max(lw, lh) > 64; ++mip) {
      levelOffset += levelBytes(c->block, lw, lh);
      lw = std::max(1u, lw / 2);
      lh = std::max(1u, lh / 2);
    }
    uint32_t step = 1;
    while (std::max(lw, lh) > 64 * step)
      step *= 2;
    const bool hdr = c->block == 6 || c->block == 16;
    c->previewWidth = (lw + step - 1) / step;
    c->previewHeight = (lh + step - 1) / step;
    c->preview.resize(size_t(c->previewWidth) * c->previewHeight * 4);
    if (hdr)
      c->hdrPreview.resize(c->preview.size());
    uint8_t rgba[64];
    float rgb[48];
    size_t cached = SIZE_MAX;
    const uint32_t levelBlocksWide = (lw + 3) / 4;
    for (uint32_t y = 0; y < c->previewHeight; ++y)
      for (uint32_t x = 0; x < c->previewWidth; ++x) {
        const uint32_t sx = std::min(lw - 1, x * step + step / 2), sy = std::min(lh - 1, y * step + step / 2);
        const size_t index = size_t(sy / 4) * levelBlocksWide + sx / 4;
        if (index != cached) {
          decodeBlock(c->block, c->bc1Alpha, c->upload.data() + levelOffset + index * stride, rgba,
                      hdr ? rgb : nullptr);
          cached = index;
        }
        const unsigned texel = (sy % 4) * 4 + sx % 4;
        const size_t out = (size_t(y) * c->previewWidth + x) * 4;
        std::memcpy(c->preview.data() + out, rgba + texel * 4, 4);
        if (hdr) {
          for (unsigned channel = 0; channel < 3; ++channel)
            c->hdrPreview[out + channel] = rgb[texel * 3 + channel];
          c->hdrPreview[out + 3] = 1;
        }
      }

    Pixels out{width, height, srgb, {}};
    out.twoChannel = c->block == 5;
    out.compressed = std::move(c);
    return out;
  }

  Pixels decodeCompressed(const Pixels &pixels) {
    const auto &c = *pixels.compressed;
    if (c.upload.empty())
      throw unavailable;
    remixapi_TextureInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    info.width = pixels.width;
    info.height = pixels.height;
    info.depth = 1;
    info.mipLevels = c.levels;
    info.format = remixapi_Format(c.format);
    info.data = c.upload.data();
    info.dataSize = c.upload.size();
    return decodeTexture(info);
  }

  Pixels loadTexture(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
      throw REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
    const auto length = file.tellg();
    if (length <= 0 || length > 512 * 1024 * 1024) {
      throw invalid;
    }
    std::vector<uint8_t> data(static_cast<size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(data.data()), length)) {
      throw invalid;
    }
    if (data.size() >= 128 && word(data, 0) == 0x20534444) {
      if (word(data, 4) != 124 || word(data, 76) != 32 || word(data, 24) > 1 ||
          (word(data, 112) & 0x20fe00)) {
        throw invalid;
      } // No volumes or cube faces.
      uint32_t width = word(data, 16), height = word(data, 12), block = 0;
      bool bgra = false, srgb = true, forceAlpha = false;
      size_t offset = 128;
      const uint32_t fourCC = word(data, 84);
      unsigned floatChannels = 0;
      bool floatHalf = false;
      if (word(data, 80) & 4) {
        if (fourCC >= 111 && fourCC <= 116) {
          floatHalf = fourCC <= 113;
          const auto n = (fourCC - 111) % 3;
          floatChannels = n == 0 ? 1 : n == 1 ? 2 : 4;
          srgb = false;
        } else if (fourCC == 0x31545844) {
          block = 1;
        } else if (fourCC == 0x33545844) {
          block = 2;
        } else if (fourCC == 0x35545844) {
          block = 3;
        } else if (fourCC == 0x31495441 || fourCC == 0x55344342) {
          block = 4;
          srgb = false;
        } else if (fourCC == 0x32495441 || fourCC == 0x55354342) {
          block = 5;
          srgb = false;
        } else if (fourCC == 0x30315844) {
          if (data.size() < 148 || word(data, 132) != 3 || word(data, 140) != 1 || (word(data, 136) & 4)) {
            throw invalid;
          }
          offset = 148;
          switch (word(data, 128)) {
          case 2:
            floatChannels = 4;
            break;
          case 6:
            floatChannels = 3;
            break;
          case 10:
            floatChannels = 4;
            floatHalf = true;
            break;
          case 16:
            floatChannels = 2;
            break;
          case 34:
            floatChannels = 2;
            floatHalf = true;
            break;
          case 41:
            floatChannels = 1;
            break;
          case 54:
            floatChannels = 1;
            floatHalf = true;
            break;
          case 28:
          case 29:
            break;
          case 87:
          case 91:
            bgra = true;
            break;
          case 71:
          case 72:
            block = 1;
            break;
          case 74:
          case 75:
            block = 2;
            break;
          case 77:
          case 78:
            block = 3;
            break;
          case 80:
            block = 4;
            srgb = false;
            break;
          case 95:
            block = 6;
            srgb = false;
            break;
          case 96:
            block = 16;
            srgb = false;
            break;
          case 83:
            block = 5;
            srgb = false;
            break;
          case 98:
          case 99:
            block = 7;
            break;
          default:
            throw unavailable;
          }
        } else {
          throw unavailable;
        }
      } else {
        if (!(word(data, 80) & 0x40) || word(data, 88) != 32 || word(data, 96) != 0xff00 ||
            (word(data, 104) != 0xff000000 && word(data, 104) != 0)) {
          throw unavailable;
        }
        const auto r = word(data, 92), b = word(data, 100);
        if (r == 0xff0000 && b == 0xff) {
          bgra = true;
        } else if (r != 0xff || b != 0xff0000) {
          throw unavailable;
        }
        forceAlpha = word(data, 104) == 0;
        // This subset uses tightly packed 32-bit rows.
        if ((word(data, 8) & 8) && word(data, 20) != width * 4ull) {
          throw unavailable;
        }
      }
      if (floatChannels)
        return decodeFloat(width, height, word(data, 28), floatChannels, floatHalf, data.data() + offset,
                           data.size() - offset);
      // Block-compressed files stay compressed, as API uploads do: decoding a
      // large mod's DDS set to RGBA multiplies its memory several times over.
      // BC2 (DXT3) has no compressed path and still decodes.
      if (block && block != 2) {
        remixapi_TextureInfo info{};
        info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
        info.width = width;
        info.height = height;
        info.depth = 1;
        info.mipLevels = std::max(1u, word(data, 28));
        info.data = data.data() + offset;
        info.dataSize = data.size() - offset;
        switch (block) {
        case 1:
          info.format = remixapi_Format(srgb ? REMIXAPI_FORMAT_BC1_RGBA_SRGB_EXT : REMIXAPI_FORMAT_BC1_RGBA_UNORM_EXT);
          break;
        case 3:
          info.format = srgb ? REMIXAPI_FORMAT_BC3_SRGB : REMIXAPI_FORMAT_BC3_UNORM;
          break;
        case 4:
          info.format = remixapi_Format(REMIXAPI_FORMAT_BC4_UNORM_EXT);
          break;
        case 5:
          info.format = REMIXAPI_FORMAT_BC5_UNORM;
          break;
        case 6:
          info.format = remixapi_Format(REMIXAPI_FORMAT_BC6H_UFLOAT_EXT);
          break;
        case 16:
          info.format = remixapi_Format(REMIXAPI_FORMAT_BC6H_SFLOAT_EXT);
          break;
        default:
          info.format = srgb ? REMIXAPI_FORMAT_BC7_SRGB : REMIXAPI_FORMAT_BC7_UNORM;
          break;
        }
        return compressTexture(info);
      }
      auto out = decode(width, height, word(data, 28), srgb, block, bgra, data.data() + offset,
                        data.size() - offset);
      if (forceAlpha) {
        for (auto &mip : out.mips) {
          for (size_t i = 3; i < mip.size(); i += 4) {
            mip[i] = 255;
          }
        }
      }
      return out;
    }
    int width = 0, height = 0, channels = 0;
    if (!stbi_info_from_memory(data.data(), int(data.size()), &width, &height, &channels) || width <= 0 ||
        height <= 0 || uint64_t(width) * height * 4 > 512u * 1024 * 1024) {
      throw invalid;
    }
    if (stbi_is_hdr_from_memory(data.data(), int(data.size()))) {
      std::unique_ptr<float, decltype(&stbi_image_free)> hdr(
          stbi_loadf_from_memory(data.data(), int(data.size()), &width, &height, &channels, 4),
          stbi_image_free);
      if (!hdr || uint64_t(width) * height * 20 > 512u * 1024 * 1024)
        throw invalid;
      Pixels out{
          uint32_t(width), uint32_t(height), false, {std::vector<uint8_t>(size_t(width) * height * 4)}};
      out.hdrMips.emplace_back(hdr.get(), hdr.get() + size_t(width) * height * 4);
      for (size_t i = 0; i < out.mips[0].size(); ++i)
        out.mips[0][i] = uint8_t(std::clamp(hdr.get()[i], 0.f, 1.f) * 255);
      return out;
    }
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
        stbi_load_from_memory(data.data(), int(data.size()), &width, &height, &channels, 4), stbi_image_free);
    if (!pixels) {
      throw invalid;
    }
    return decode(uint32_t(width), uint32_t(height), 1, true, 0, false, pixels.get(),
                  size_t(width) * height * 4);
  }

  void generateMips(Pixels &pixels) {
    if (pixels.compressed || pixels.mips.size() != 1 || (pixels.width <= 1 && pixels.height <= 1) ||
        pixels.mips[0].size() != size_t(pixels.width) * pixels.height * 4)
      return;
    const bool hdr = pixels.hdrMips.size() == 1;
    static const auto toLinear = [] {
      std::array<float, 256> table{};
      for (int i = 0; i < 256; ++i) {
        const float c = i / 255.f;
        table[i] = std::pow(c, 2.2f); // sRGB-encoded: gamma 2.2
      }
      return table;
    }();
    static const auto srgbTable = [] {
      std::array<uint8_t, 4097> table{};
      for (int i = 0; i <= 4096; ++i) {
        const float c = i / 4096.f;
        table[i] = uint8_t(std::lround(std::pow(c, 1 / 2.2f) * 255));
      }
      return table;
    }();
    auto toSrgb = [](float c) { return srgbTable[size_t(std::lround(std::clamp(c, 0.f, 1.f) * 4096))]; };
    uint32_t width = pixels.width, height = pixels.height;
    while (width > 1 || height > 1) {
      const uint32_t w = std::max(1u, width / 2), h = std::max(1u, height / 2);
      const auto &source = pixels.mips.back();
      std::vector<uint8_t> level(size_t(w) * h * 4);
      std::vector<float> hdrLevel(hdr ? level.size() : 0);
      const float *hdrSource = hdr ? pixels.hdrMips.back().data() : nullptr;
      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
          // 2x2 box (a 1-texel edge stays as is); odd sizes drop their last row/column.
          const uint32_t x0 = std::min(2 * x, width - 1), x1 = std::min(2 * x + 1, width - 1);
          const uint32_t y0 = std::min(2 * y, height - 1), y1 = std::min(2 * y + 1, height - 1);
          const size_t taps[4] = {(size_t(y0) * width + x0) * 4, (size_t(y0) * width + x1) * 4,
                                  (size_t(y1) * width + x0) * 4, (size_t(y1) * width + x1) * 4};
          const size_t out = (size_t(y) * w + x) * 4;
          for (int c = 0; c < 4; ++c) {
            float sum = 0;
            for (const size_t t : taps)
              sum += pixels.srgb && c < 3 ? toLinear[source[t + c]] : source[t + c] / 255.f;
            sum *= .25f;
            level[out + c] = pixels.srgb && c < 3 ? toSrgb(sum) : uint8_t(std::lround(std::clamp(sum, 0.f, 1.f) * 255));
            if (hdr)
              hdrLevel[out + c] =
                  .25f * (hdrSource[taps[0] + c] + hdrSource[taps[1] + c] + hdrSource[taps[2] + c] + hdrSource[taps[3] + c]);
          }
        }
      pixels.mips.push_back(std::move(level));
      if (hdr)
        pixels.hdrMips.push_back(std::move(hdrLevel));
      width = w;
      height = h;
    }
  }
} // namespace remix::scene
