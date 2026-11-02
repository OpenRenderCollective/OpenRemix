#include "scene/images.h"
#include "scene/resources.h"
#include "scene/validation.h"
#include <remix/remix_native_ext.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace remix {
  using namespace scene;
  using namespace scene::detail;

  namespace scene {
    uint64_t Texture::fingerprint() const {
      if (!contentHash) {
        uint64_t hash = 1469598103934665603ull;
        auto append = [&](const void *data, size_t size) { hash = hashBytes(data, size, hash); };
        append(&pixels.width, 4);
        append(&pixels.height, 4);
        append(&pixels.srgb, sizeof(bool));
        append(&pixels.twoChannel, sizeof(bool));
        for (const auto &mip : pixels.mips)
          append(mip.data(), mip.size());
        for (const auto &mip : pixels.hdrMips)
          append(mip.data(), mip.size() * sizeof(float));
        if (pixels.compressed)
          append(&pixels.compressed->hash, sizeof(pixels.compressed->hash));
        contentHash = hash ? hash : 1;
      }
      return contentHash;
    }
  } // namespace scene

  std::shared_ptr<Texture> NativeResources::texturePath(remixapi_Path path) {
    if (!m_white) {
      auto white = std::make_shared<Texture>();
      white->pixels = {1, 1, true, {{255, 255, 255, 255}}};
      white->hash = white->fingerprint();
      m_white = std::move(white);
    }
    if (!path || !*path) {
      return m_white;
    }
    const std::filesystem::path name(path);
    const auto text = name.u8string();
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
      uint64_t hash = 0;
      if (text.size() > 18) {
        throw invalid;
      }
      for (size_t i = 2; i < text.size(); ++i) {
        const auto c = text[i];
        unsigned digit = c >= '0' && c <= '9'   ? c - '0'
                         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                : 16;
        if (digit > 15) {
          throw invalid;
        }
        hash = hash * 16 + digit;
      }
      auto it = m_textures.find(hash);
      if (it == m_textures.end()) {
        throw invalid;
      }
      return it->second;
    }
    const auto normalized = std::filesystem::absolute(name).lexically_normal();
    if (auto it = m_paths.find(normalized); it != m_paths.end()) {
      if (auto existing = it->second.lock()) {
        return existing;
      }
    }
    auto texture = std::make_shared<Texture>();
    texture->pixels = loadTexture(normalized);
    generateMips(texture->pixels);
    texture->hash = texture->fingerprint();
    m_paths[normalized] = texture;
    return texture;
  }

  std::shared_ptr<Texture> NativeResources::dataTexturePath(remixapi_Path path) {
    if (!path || !*path)
      return {};
    auto source = texturePath(path);
    if (!source->pixels.srgb)
      return source;
    if (auto linear = source->linear.lock())
      return linear;
    auto linear = std::make_shared<Texture>();
    linear->pixels = source->pixels;
    linear->pixels.srgb = false; // Data maps store values, not sRGB colors.
    linear->encodedSource = source;
    linear->hash = source->hash;
    source->linear = linear;
    return linear;
  }

  namespace {
    // remixapi_TextureInfoDynamicEXT, validated. withMips: generated mips were
    // asked for (remixapi_TextureInfoGenerateMipsEXT), made again from every
    // update (small animated maps; a large render target leaves them out).
    bool dynamicTexture(const remixapi_TextureInfo &info, bool &withMips) {
      const auto *dynamic = static_cast<const remixapi_TextureInfoDynamicEXT *>(
          extension(info.pNext, REMIXAPI_STRUCT_TYPE_TEXTURE_DYNAMIC_EXT));
      if (!dynamic || !dynamic->dynamic)
        return false;
      const auto *mips = static_cast<const remixapi_TextureInfoGenerateMipsEXT *>(
          extension(info.pNext, REMIXAPI_STRUCT_TYPE_TEXTURE_GENERATE_MIPS_EXT));
      withMips = mips && mips->generate;
      if (dynamic->dynamic > 1 || info.mipLevels != 1 || (withMips && isFloatFormat(info.format)) ||
          (info.format != REMIXAPI_FORMAT_R8G8B8A8_UNORM && info.format != REMIXAPI_FORMAT_R8G8B8A8_SRGB &&
           info.format != REMIXAPI_FORMAT_B8G8R8A8_UNORM && info.format != REMIXAPI_FORMAT_B8G8R8A8_SRGB &&
           !isFloatFormat(info.format)))
        throw invalid;
      return true;
    }
  } // namespace

  void NativeResources::createTexture(const remixapi_TextureInfo &info, remixapi_TextureHandle &handle) {
    if (!info.hash) {
      throw invalid;
    }
    bool withMips = false;
    if (dynamicTexture(info, withMips)) {
      auto pixels = decodeTexture(info);
      // Float ones keep their texels in hdrMips[0] (mips[0] is its 8-bit preview).
      if (pixels.mips.size() != 1 || pixels.hdrMips.size() != (isFloatFormat(info.format) ? 1u : 0u) ||
          pixels.compressed || pixels.mips[0].size() != size_t(pixels.width) * pixels.height * 4)
        throw invalid;
      if (withMips)
        generateMips(pixels);
      auto texture = std::make_shared<Texture>();
      texture->pixels = std::move(pixels);
      texture->dynamic = true;
      texture->updates = std::make_shared<TextureUpdates>();
      // Identity, never contents (fingerprint() returns it as it is).
      static std::atomic<uint64_t> serial{0};
      const uint64_t identity[] = {0x64796e616d696374ull, serial.fetch_add(1) + 1};
      texture->contentHash = hashBytes(identity, sizeof(identity)) | 1;
      texture->hash = info.hash;
      m_textures[info.hash] = std::move(texture);
      textureCounters.texturesCreated.fetch_add(1, std::memory_order_relaxed);
      handle = reinterpret_cast<remixapi_TextureHandle>(uintptr_t(info.hash));
      return;
    }
    const auto *mips = static_cast<const remixapi_TextureInfoGenerateMipsEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_TEXTURE_GENERATE_MIPS_EXT));
    const bool generate = mips && mips->generate && info.mipLevels <= 1;
    // A requested chain is built from decoded texels, so such BC data is not kept compressed.
    if (!generate && keepCompressedTextures() && isBlockCompressed(info.format)) {
      storeTexture(info.hash, compressTexture(info), handle);
      return;
    }
    auto pixels = decodeTexture(info);
    if (generate)
      generateMips(pixels);
    storeTexture(info.hash, std::move(pixels), handle);
  }

  void NativeResources::createTexture(const remixapi_TextureInfo &info, scene::Pixels &&pixels,
                                      remixapi_TextureHandle &handle) {
    if (!info.hash) {
      throw invalid;
    }
    storeTexture(info.hash, std::move(pixels), handle);
  }

  void NativeResources::storeTexture(uint64_t hash, scene::Pixels &&pixels, remixapi_TextureHandle &handle) {
    auto texture = std::make_shared<Texture>();
    texture->pixels = std::move(pixels);
    texture->hash = hash;
    // Identical compressed resubmissions keep their uploaded image. The block
    // hash stands in for the released payload.
    auto sameCompressed = [](const Pixels &a, const Pixels &b) {
      if (!a.compressed || !b.compressed)
        return !a.compressed && !b.compressed;
      return a.compressed->format == b.compressed->format && a.compressed->levels == b.compressed->levels &&
             a.compressed->hash == b.compressed->hash;
    };
    // (A dynamic texture's pixels change under its lock: it is always replaced.)
    auto previous = m_textures.find(hash);
    if (previous == m_textures.end() || previous->second->dynamic ||
        previous->second->pixels.width != texture->pixels.width ||
        previous->second->pixels.height != texture->pixels.height ||
        previous->second->pixels.srgb != texture->pixels.srgb ||
        previous->second->pixels.twoChannel != texture->pixels.twoChannel ||
        previous->second->pixels.mips != texture->pixels.mips ||
        previous->second->pixels.hdrMips != texture->pixels.hdrMips ||
        !sameCompressed(previous->second->pixels, texture->pixels)) {
      m_textures[hash] = std::move(texture);
      textureCounters.texturesCreated.fetch_add(1, std::memory_order_relaxed);
    } else {
      previous->second->hash = hash;
    }
    handle = reinterpret_cast<remixapi_TextureHandle>(uintptr_t(hash));
  }

  void NativeResources::updateTexture(remixapi_TextureHandle handle, uint32_t x, uint32_t y, uint32_t width,
                                      uint32_t height, remixapi_Format format, const void *pixels,
                                      uint32_t rowPitch) {
    textureCounters.updateCalls.fetch_add(1, std::memory_order_relaxed);
    CounterTicks time{textureCounters.updateTicks};
    const bool bgra = format == REMIXAPI_FORMAT_B8G8R8A8_UNORM || format == REMIXAPI_FORMAT_B8G8R8A8_SRGB;
    const bool half = format == REMIXAPI_FORMAT_R16G16B16A16_SFLOAT_EXT;
    const bool hdr = isFloatFormat(format);
    // Source and stored bytes a texel (a float texture stores RGBA32F).
    const uint32_t sourceTexel = half ? 8 : hdr ? 16 : 4, texel = hdr ? 16 : 4;
    if (!width || !height || !pixels || uint64_t(rowPitch) < uint64_t(width) * sourceTexel ||
        (!bgra && !hdr && format != REMIXAPI_FORMAT_R8G8B8A8_UNORM && format != REMIXAPI_FORMAT_R8G8B8A8_SRGB))
      throw invalid;
    const auto it = m_textures.find(key(handle));
    if (it == m_textures.end() || !it->second->dynamic || it->second->pixels.hdrMips.empty() != !hdr)
      throw invalid;
    auto &texture = *it->second;
    auto &p = texture.pixels;
    if (uint64_t(x) + width > p.width || uint64_t(y) + height > p.height)
      throw invalid;
    // Rows that differ, and the columns they differ in. Only this thread
    // writes the texels, so they are compared without the lock; the lock
    // (shared with the upload and the emission estimates) covers the copy.
    const size_t bytes = size_t(width) * texel;
    const auto *source = static_cast<const uint8_t *>(pixels);
    uint8_t *const texels = hdr ? reinterpret_cast<uint8_t *>(p.hdrMips[0].data()) : p.mips[0].data();
    std::vector<uint8_t> converted;
    if (bgra || hdr)
      converted.resize(bytes);
    auto row = [&](uint32_t r) {
      const uint8_t *from = source + size_t(r) * rowPitch;
      if (hdr) {
        auto *to = reinterpret_cast<float *>(converted.data());
        for (size_t i = 0; i < size_t(width) * 4; ++i) {
          float value;
          if (half) {
            uint16_t bits;
            std::memcpy(&bits, from + i * 2, 2);
            value = halfToFloat(bits);
          } else {
            std::memcpy(&value, from + i * 4, 4);
            if (!std::isfinite(value))
              value = 0;
          }
          to[i] = value;
        }
        return static_cast<const uint8_t *>(converted.data());
      }
      if (!bgra)
        return from;
      for (size_t i = 0; i < bytes; i += 4) {
        converted[i] = from[i + 2];
        converted[i + 1] = from[i + 1];
        converted[i + 2] = from[i];
        converted[i + 3] = from[i + 3];
      }
      return static_cast<const uint8_t *>(converted.data());
    };
    auto stored = [&](uint32_t r) { return texels + (size_t(y + r) * p.width + x) * texel; };
    uint32_t first = 0, last = height;
    while (first < height && std::memcmp(stored(first), row(first), bytes) == 0)
      ++first;
    if (first == height)
      return;
    while (last > first + 1 && std::memcmp(stored(last - 1), row(last - 1), bytes) == 0)
      --last;
    uint32_t x0 = width, x1 = 0;
    auto &updates = *texture.updates;
    {
      std::lock_guard lock(updates.lock);
      for (uint32_t r = first; r < last; ++r) {
        const uint8_t *from = row(r);
        uint8_t *to = stored(r);
        size_t begin = 0, end = bytes;
        while (begin < end && to[begin] == from[begin])
          ++begin;
        if (begin == end)
          continue;
        while (to[end - 1] == from[end - 1])
          --end;
        std::memcpy(to + begin, from + begin, end - begin);
        const uint32_t c0 = uint32_t(begin / texel), c1 = uint32_t((end + texel - 1) / texel);
        x0 = std::min(x0, c0);
        x1 = std::max(x1, c1);
        if (hdr) { // the 8-bit preview the CPU consumers read
          const float *values = p.hdrMips[0].data() + (size_t(y + r) * p.width + x) * 4;
          uint8_t *preview = p.mips[0].data() + (size_t(y + r) * p.width + x) * 4;
          for (size_t i = size_t(c0) * 4; i < size_t(c1) * 4; ++i)
            preview[i] = uint8_t(std::clamp(values[i], 0.f, 1.f) * 255);
        }
      }
      // Generated mips are made again from the new level 0: all of it is new.
      const bool mipped = p.mips.size() > 1;
      if (mipped) {
        p.mips.resize(1);
        generateMips(p);
      }
      const uint32_t region[4] = {mipped ? 0 : x + x0, mipped ? 0 : y + first, mipped ? p.width : x + x1,
                                  mipped ? p.height : y + last};
      if (updates.dirty[2] <= updates.dirty[0] || updates.dirty[3] <= updates.dirty[1]) {
        std::copy_n(region, 4, updates.dirty);
      } else {
        updates.dirty[0] = std::min(updates.dirty[0], region[0]);
        updates.dirty[1] = std::min(updates.dirty[1], region[1]);
        updates.dirty[2] = std::max(updates.dirty[2], region[2]);
        updates.dirty[3] = std::max(updates.dirty[3], region[3]);
      }
      ++updates.revision;
    }
    textureCounters.updates.fetch_add(1, std::memory_order_relaxed);
    textureCounters.updateBytes.fetch_add(uint64_t(last - first) * (x1 - x0) * texel, std::memory_order_relaxed);
  }

  void NativeResources::destroyTexture(remixapi_TextureHandle handle) {
    textureCounters.texturesDestroyed.fetch_add(m_textures.erase(key(handle)), std::memory_order_relaxed);
  }
} // namespace remix
