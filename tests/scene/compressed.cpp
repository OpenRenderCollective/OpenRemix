// "Keep BC textures compressed": the compressed path must accept the same data
// as the CPU decoder and derive exactly the alpha coverage, previews, opacity
// partitions and emission that the decoded texture would produce.
#include "scene/emission.h"
#include "scene/fixtures.h"
#include "scene/images.h"
#include "scene/opacity.h"
#include <cstring>
#include <random>
#include <string>

using namespace remix::scene;
using namespace remix::test;

namespace {
  std::mt19937 rng(20260926);

  size_t blockSize(remixapi_Format format) {
    const auto f = uint32_t(format);
    return f == REMIXAPI_FORMAT_BC1_RGB_UNORM || f == REMIXAPI_FORMAT_BC1_RGB_SRGB ||
                   f == REMIXAPI_FORMAT_BC1_RGBA_UNORM_EXT || f == REMIXAPI_FORMAT_BC1_RGBA_SRGB_EXT ||
                   f == REMIXAPI_FORMAT_BC4_UNORM_EXT
               ? 8
               : 16;
  }
  std::vector<uint8_t> blocks(remixapi_Format format, uint32_t width, uint32_t height, uint32_t mips) {
    size_t size = 0;
    for (uint32_t mip = 0; mip < mips; ++mip) {
      size += size_t((width + 3) / 4) * ((height + 3) / 4) * blockSize(format);
      width = std::max(1u, width / 2);
      height = std::max(1u, height / 2);
    }
    std::vector<uint8_t> data(size);
    for (auto &value : data)
      value = uint8_t(rng());
    return data;
  }
  remixapi_TextureInfo textureInfo(remixapi_Format format, uint32_t width, uint32_t height, uint32_t mips,
                                   const std::vector<uint8_t> &data) {
    remixapi_TextureInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    info.format = format;
    info.width = width;
    info.height = height;
    info.depth = 1;
    info.mipLevels = mips;
    info.data = data.data();
    info.dataSize = data.size();
    return info;
  }
  void setBits(uint8_t *block, unsigned start, unsigned count, uint32_t value) {
    for (unsigned i = 0; i < count; ++i) {
      const unsigned bit = start + i;
      if (value >> i & 1)
        block[bit / 8] |= uint8_t(1u << bit % 8);
      else
        block[bit / 8] &= uint8_t(~(1u << bit % 8));
    }
  }

  // Compressed metadata, alpha coverage, preview and fallback decode against
  // the CPU decoder's result for the same submission.
  void compare(const remixapi_TextureInfo &info, const std::string &name) {
    auto check = [&](bool value, const char *what) { require(value, (name + ": " + what).c_str()); };
    const auto decoded = decodeTexture(info);
    const auto packed = compressTexture(info);
    check(packed.compressed && packed.mips.empty() && packed.hdrMips.empty(), "kept a decoded copy");
    check(packed.width == decoded.width && packed.height == decoded.height && packed.srgb == decoded.srgb &&
              packed.twoChannel == decoded.twoChannel,
          "metadata differs from decoder");
    check(packed.levels() == decoded.mips.size(), "mip count differs");
    const auto &c = *packed.compressed;
    size_t expected = 0;
    for (uint32_t mip = 0, w = info.width, h = info.height; mip < decoded.mips.size(); ++mip) {
      expected += size_t((w + 3) / 4) * ((h + 3) / 4) * blockSize(info.format);
      w = std::max(1u, w / 2);
      h = std::max(1u, h / 2);
    }
    check(c.upload.size() == expected, "upload payload size");
    for (uint32_t y = 0; y < decoded.height; ++y)
      for (uint32_t x = 0; x < decoded.width; ++x)
        check(c.transparentTexel(x, y, decoded.width) ==
                  (decoded.mips[0][4 * (size_t(y) * decoded.width + x) + 3] != 255),
              "alpha coverage differs from decoded alpha");
    // Block alpha ranges: the decoded in-image texels' (min, max), one step wider.
    const uint32_t blocksWide = (decoded.width + 3) / 4, blocksHigh = (decoded.height + 3) / 4;
    check(c.alphaBlocks.empty() == c.transparent.empty(), "alpha ranges without coverage bits");
    check(c.alphaBlocks.empty() || c.alphaBlocks.size() == size_t(blocksWide) * blocksHigh, "alpha range count");
    for (uint32_t by = 0; by < blocksHigh && !c.alphaBlocks.empty(); ++by)
      for (uint32_t bx = 0; bx < blocksWide; ++bx) {
        int low = 255, high = 0;
        for (uint32_t y = by * 4; y < std::min(decoded.height, by * 4 + 4); ++y)
          for (uint32_t x = bx * 4; x < std::min(decoded.width, bx * 4 + 4); ++x) {
            const int alpha = decoded.mips[0][4 * (size_t(y) * decoded.width + x) + 3];
            low = std::min(low, alpha);
            high = std::max(high, alpha);
          }
        const auto range = c.alphaBlocks[size_t(by) * blocksWide + bx];
        const bool expected = low == 255 ? range[0] == 255 && range[1] == 255
                                         : range[0] == std::max(0, low - 1) && range[1] == std::min(255, high + 1);
        check(expected, "block alpha range differs from decoded alpha");
      }
    // Preview: the first level <= 64 texels, point-sampled if the chain is short.
    uint32_t level = 0, lw = decoded.width, lh = decoded.height;
    while (level + 1 < decoded.mips.size() && std::max(lw, lh) > 64) {
      lw = std::max(1u, lw / 2);
      lh = std::max(1u, lh / 2);
      ++level;
    }
    uint32_t step = 1;
    while (std::max(lw, lh) > 64 * step)
      step *= 2;
    check(c.previewWidth == (lw + step - 1) / step && c.previewHeight == (lh + step - 1) / step &&
              std::max(c.previewWidth, c.previewHeight) <= 64,
          "preview size");
    check(c.hdrPreview.empty() == decoded.hdrMips.empty(), "HDR preview presence");
    for (uint32_t y = 0; y < c.previewHeight; ++y)
      for (uint32_t x = 0; x < c.previewWidth; ++x) {
        const size_t source =
            (size_t(std::min(lh - 1, y * step + step / 2)) * lw + std::min(lw - 1, x * step + step / 2)) * 4;
        const size_t target = (size_t(y) * c.previewWidth + x) * 4;
        check(std::memcmp(&c.preview[target], &decoded.mips[level][source], 4) == 0, "preview texel");
        if (!c.hdrPreview.empty())
          check(std::memcmp(&c.hdrPreview[target], &decoded.hdrMips[level][source], 16) == 0, "HDR preview texel");
      }
    const auto fallback = decodeCompressed(packed);
    check(fallback.mips == decoded.mips && fallback.hdrMips == decoded.hdrMips && fallback.srgb == decoded.srgb,
          "device fallback decode differs");
    // Identical submissions hash identically; any block edit changes the hash.
    check(compressTexture(info).compressed->hash == c.hash, "unstable content hash");
    auto edited = std::vector<uint8_t>(static_cast<const uint8_t *>(info.data),
                                       static_cast<const uint8_t *>(info.data) + info.dataSize);
    edited[edited.size() / 2] ^= 1;
    auto editedInfo = info;
    editedInfo.data = edited.data();
    check(compressTexture(editedInfo).compressed->hash != c.hash, "content hash ignored a block edit");
  }
} // namespace

int main() {
  const remixapi_Format formats[] = {
      REMIXAPI_FORMAT_BC1_RGB_UNORM,     REMIXAPI_FORMAT_BC1_RGB_SRGB, REMIXAPI_FORMAT_BC1_RGBA_UNORM_EXT,
      REMIXAPI_FORMAT_BC1_RGBA_SRGB_EXT, REMIXAPI_FORMAT_BC3_UNORM,    REMIXAPI_FORMAT_BC3_SRGB,
      REMIXAPI_FORMAT_BC4_UNORM_EXT,     REMIXAPI_FORMAT_BC5_UNORM,    REMIXAPI_FORMAT_BC6H_UFLOAT_EXT,
      REMIXAPI_FORMAT_BC6H_SFLOAT_EXT,   REMIXAPI_FORMAT_BC7_UNORM,    REMIXAPI_FORMAT_BC7_SRGB};
  // Odd sizes cover partial edge blocks; 256x96 exercises the preview level
  // search and, with one mip, point sampling of a short chain.
  const uint32_t sizes[][3] = {{20, 13, 5}, {256, 96, 9}, {256, 96, 1}, {1, 1, 1}, {130, 7, 3}};
  for (auto format : formats) {
    require(isBlockCompressed(format), "BC format not recognized");
    for (const auto &size : sizes) {
      const auto data = blocks(format, size[0], size[1], size[2]);
      compare(textureInfo(format, size[0], size[1], size[2], data),
              "format " + std::to_string(uint32_t(format)) + " " + std::to_string(size[0]) + "x" +
                  std::to_string(size[1]) + "/" + std::to_string(size[2]));
    }
  }
  require(!isBlockCompressed(REMIXAPI_FORMAT_R8G8B8A8_UNORM), "RGBA reported as compressed");

  // BC7 endpoint fast paths: opaque encodings in modes 4-7 skip decoding and
  // must still agree with the decoder, as must blocks whose endpoints are not.
  {
    std::vector<uint8_t> data(16 * 16 * 16);
    for (auto &value : data)
      value = uint8_t(rng());
    for (size_t i = 0; i < data.size() / 16; ++i) {
      uint8_t *block = &data[i * 16];
      const unsigned mode = 4 + i % 4;
      block[0] = uint8_t((block[0] & ~((2u << mode) - 1)) | (1u << mode));
      if (i % 8 >= 4)
        continue; // Random endpoints: decoded check.
      if (mode == 4) {
        setBits(block, 5, 2, i % 16 == 8 ? 1 : 0);
        setBits(block, 38, 12, 0xfff);
      } else if (mode == 5) {
        setBits(block, 6, 2, 0);
        setBits(block, 50, 16, 0xffff);
      } else if (mode == 6) {
        setBits(block, 49, 14, 0x3fff);
        setBits(block, 63, 2, 3);
      } else {
        setBits(block, 74, 20, 0xfffff);
        setBits(block, 94, 4, 15);
      }
    }
    compare(textureInfo(REMIXAPI_FORMAT_BC7_SRGB, 64, 64, 1, data), "BC7 endpoint fast paths");
  }

  // Validation parity with decodeTexture.
  {
    auto data = blocks(REMIXAPI_FORMAT_BC7_UNORM, 8, 8, 4);
    auto info = textureInfo(REMIXAPI_FORMAT_BC7_UNORM, 8, 8, 4, data);
    info.mipLevels = 5;
    rejected([&] { compressTexture(info); });
    info.mipLevels = 4;
    info.dataSize = data.size() - 1;
    rejected([&] { compressTexture(info); });
    info.dataSize = data.size();
    info.depth = 2;
    rejected([&] { compressTexture(info); });
    info.depth = 1;
    info.width = 16385;
    rejected([&] { compressTexture(info); });
  }

  // Opacity partitions from the coverage bitmask match the decoded alpha.
  {
    const auto data = blocks(REMIXAPI_FORMAT_BC1_RGBA_SRGB_EXT, 16, 16, 1);
    const auto info = textureInfo(REMIXAPI_FORMAT_BC1_RGBA_SRGB_EXT, 16, 16, 1, data);
    auto partition = [&](Pixels pixels, const std::array<float, 4> &uv) {
      OpacityCache cache;
      Draw draw;
      draw.doubleSided = true;
      draw.surface = std::make_shared<Surface>();
      draw.surface->vertices = {{{0, 0, 0}, {0, 0}, {1, 1, 1, 1}},
                                {{1, 0, 0}, {.3f, 0}, {1, 1, 1, 1}},
                                {{0, 1, 0}, {0, .3f}, {1, 1, 1, 1}},
                                {{1, 1, 0}, {.3f, .3f}, {1, 1, 1, 1}}};
      draw.surface->indices = {0, 1, 2, 1, 3, 2};
      draw.material = std::make_shared<Material>();
      draw.material->compare = 4;
      draw.material->reference = 1.f / 255;
      draw.material->texture = std::make_shared<Texture>();
      draw.material->texture->pixels = std::move(pixels);
      return *cache.partition(draw, uv);
    };
    for (const auto &uv :
         {std::array<float, 4>{1, 1, 0, 0}, std::array<float, 4>{1, 1, .5f, .6f}, std::array<float, 4>{3, 3, 0, 0}}) {
      const auto a = partition(decodeTexture(info), uv), b = partition(compressTexture(info), uv);
      require(a.key == b.key && a.reasons == b.reasons, "Compressed opacity partition differs");
    }
    // A texture without transparent texels proves solid without a prefix table.
    const auto opaque = blocks(REMIXAPI_FORMAT_BC7_UNORM, 8, 8, 1);
    auto opaqueData = opaque;
    for (size_t i = 0; i < opaqueData.size(); i += 16)
      opaqueData[i] = 1; // Mode 0: no alpha.
    const auto solid =
        partition(compressTexture(textureInfo(REMIXAPI_FORMAT_BC7_UNORM, 8, 8, 1, opaqueData)), {1, 1, 0, 0});
    require(solid.reasons[OpacityPartition::Solid] == solid.triangles, "Opaque compressed texture not solid");
  }

  // Micromaps from block alpha ranges: a 32x32 BC1 texture whose right half is
  // cut out proves opaque and transparent micro-triangles, never one the
  // decoded texture does not prove.
  {
    std::vector<uint8_t> data(8 * 8 * 8);
    for (uint32_t i = 0; i < 64; ++i) {
      uint8_t *block = &data[i * 8];
      const bool cut = i % 8 >= 4;
      // Four-color mode (color0 > color1) is opaque; three-color mode with
      // every index 3 is transparent black.
      block[0] = cut ? 0x00 : 0xff, block[1] = cut ? 0x00 : 0xff;
      block[2] = cut ? 0xff : 0x00, block[3] = cut ? 0xff : 0x00;
      for (unsigned b = 4; b < 8; ++b)
        block[b] = cut ? 0xff : uint8_t(rng());
    }
    const auto info = textureInfo(REMIXAPI_FORMAT_BC1_RGBA_UNORM_EXT, 32, 32, 1, data);
    auto micromap = [&](Pixels pixels) {
      OpacityCache cache;
      cache.setMicromapLevel(4);
      Draw draw;
      draw.doubleSided = true;
      draw.surface = std::make_shared<Surface>();
      draw.surface->vertices = {{{0, 0, 0}, {.05f, .05f}, {1, 1, 1, 1}},
                                {{1, 0, 0}, {.95f, .05f}, {1, 1, 1, 1}},
                                {{0, 1, 0}, {.05f, .95f}, {1, 1, 1, 1}}};
      draw.surface->indices = {0, 1, 2};
      draw.material = std::make_shared<Material>();
      draw.material->compare = 4;
      draw.material->reference = .5f;
      draw.material->wrapU = draw.material->wrapV = 0;
      draw.material->texture = std::make_shared<Texture>();
      draw.material->texture->pixels = std::move(pixels);
      return cache.partition(draw, {1, 1, 0, 0})->micromap;
    };
    const auto packed = micromap(compressTexture(info)), decoded = micromap(decodeTexture(info));
    require(packed && packed->opaque && packed->transparent,
            "Compressed cut-out texture produced no opaque and transparent micro-triangles");
    require(decoded && packed->opaque <= decoded->opaque && packed->transparent <= decoded->transparent,
            "Compressed micromap proved more than the decoded texture");
  }

  // Emission: for textures within the preview size the triangle averages are
  // identical to the decoded path.
  {
    for (auto format : {REMIXAPI_FORMAT_BC7_SRGB, REMIXAPI_FORMAT_BC6H_UFLOAT_EXT, REMIXAPI_FORMAT_BC4_UNORM_EXT}) {
      const auto data = blocks(format, 40, 24, 1);
      const auto info = textureInfo(format, 40, 24, 1, data);
      auto emission = [&](Pixels pixels) {
        Draw draw;
        draw.material = std::make_shared<Material>();
        draw.surface = std::make_shared<Surface>();
        draw.surface->vertices = {
            {{0, 0, 0}, {.1f, .2f}, {}}, {{1, 0, 0}, {.9f, .3f}, {}}, {{0, 1, 0}, {.4f, .95f}, {}}};
        draw.surface->indices = {0, 1, 2};
        std::fill_n(draw.material->emission, 3, 2.f);
        draw.material->emissionTexture = std::make_shared<Texture>();
        draw.material->emissionTexture->pixels = std::move(pixels);
        const float uv[] = {1, 1, 0, 0};
        return triangleEmission(draw, 0, uv);
      };
      require(emission(decodeTexture(info)) == emission(compressTexture(info)), "Compressed emission differs");
    }
  }

  // Scene integration: the setting selects the path for later textures, data
  // maps alias the sRGB source, identical resubmissions keep their texture.
  {
    remix::NativeResources r;
    const auto data = blocks(REMIXAPI_FORMAT_BC7_SRGB, 32, 32, 6);
    auto info = textureInfo(REMIXAPI_FORMAT_BC7_SRGB, 32, 32, 6, data);
    info.hash = 0x1234;
    remixapi_TextureHandle handle = nullptr;
    r.createTexture(info, handle);
#ifdef _WIN32
    const remixapi_Path path = L"0x1234";
#else
    const remixapi_Path path = "0x1234";
#endif
    auto draw = [&](uint64_t hash) {
      remixapi_MaterialInfoOpaqueEXT opaque = {};
      opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
      opaque.opacityConstant = 1;
      remixapi_MaterialInfo material = {};
      material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
      material.hash = hash;
      material.pNext = &opaque;
      material.albedoTexture = path;
      material.normalTexture = path;
      remixapi_MaterialHandle result;
      r.createMaterial(material, result);
      r.drawInstance(instance(quad(r, hash, result)));
      return r.draws().back().material;
    };
    auto decoded = draw(1);
    require(!decoded->texture->pixels.compressed && !decoded->texture->pixels.mips.empty(),
            "Setting OFF did not decode");
    r.setKeepCompressedTextures(true);
    require(r.keepCompressedTextures(), "Setting not stored");
    r.createTexture(info, handle);
    auto packed = draw(2);
    const auto &source = packed->texture;
    require(source->pixels.compressed && source->pixels.mips.empty() && source->pixels.srgb,
            "Setting ON kept a decoded texture");
    require(source->fingerprint() != decoded->texture->fingerprint(), "Representation change kept fingerprint");
    const auto &linear = packed->normalTexture;
    require(linear != source && linear->encodedSource == source && !linear->pixels.srgb &&
                linear->pixels.compressed == source->pixels.compressed,
            "Data map did not alias the compressed source");
    // Like decoded data maps, the fingerprint includes the transfer function.
    require(linear->fingerprint() != source->fingerprint(), "Data map fingerprint ignored its transfer function");
    auto previous = source;
    r.createTexture(info, handle);
    require(draw(3)->texture == previous, "Identical compressed resubmission replaced the texture");
    auto changed = data;
    changed[5] ^= 0x40;
    auto changedInfo = info;
    changedInfo.data = changed.data();
    r.createTexture(changedInfo, handle);
    const auto replaced = draw(4)->texture;
    require(replaced != previous && replaced->fingerprint() != previous->fingerprint(),
            "Edited compressed texture kept the old snapshot");
    // Prepared outside the API mutex, stored inside it.
    r.createTexture(info, compressTexture(info), handle);
    require(draw(5)->texture->pixels.compressed->hash == previous->pixels.compressed->hash,
            "Prepared texture not stored");
    r.setKeepCompressedTextures(false);
    r.createTexture(info, handle);
    require(!draw(6)->texture->pixels.compressed, "Setting OFF still compressed new textures");
  }
  std::puts("Compressed texture preparation, coverage, previews and aliasing passed");
}
