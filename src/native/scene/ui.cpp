#include "scene/resources.h"
#include "scene/images.h"
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

  void NativeResources::registerUiTexture(uint64_t id, uint32_t width, uint32_t height,
                                          remixapi_Format format, const void *pixels, uint64_t size) {
    if (!id || !width || !height || !pixels || uint64_t(width) * height > UINT64_MAX / 4 ||
        size != uint64_t(width) * height * 4 ||
        (format != REMIXAPI_FORMAT_R8G8B8A8_UNORM && format != REMIXAPI_FORMAT_R8G8B8A8_SRGB &&
         format != REMIXAPI_FORMAT_B8G8R8A8_UNORM && format != REMIXAPI_FORMAT_B8G8R8A8_SRGB)) {
      throw invalid;
    }
    remixapi_TextureInfo info = {};
    info.width = width;
    info.height = height;
    info.format = format;
    info.data = pixels;
    info.dataSize = size;
    auto texture = std::make_shared<Texture>();
    texture->pixels = decodeTexture(info);
    // Game UI uploads are display-color bytes, including legacy UNORM captures.
    texture->pixels.srgb = true;
    texture->updates = std::make_shared<TextureUpdates>();
    m_uiTextures[id] = std::move(texture);
  }

  void NativeResources::updateUiTextureRegion(uint64_t id, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                                              remixapi_Format format, const void *pixels, uint32_t rowPitch) {
    const bool bgra = format == REMIXAPI_FORMAT_B8G8R8A8_UNORM || format == REMIXAPI_FORMAT_B8G8R8A8_SRGB;
    if (!id || !width || !height || !pixels || uint64_t(rowPitch) < uint64_t(width) * 4 ||
        (!bgra && format != REMIXAPI_FORMAT_R8G8B8A8_UNORM && format != REMIXAPI_FORMAT_R8G8B8A8_SRGB))
      throw invalid;
    auto it = m_uiTextures.find(id);
    if (it == m_uiTextures.end() || !it->second->updates)
      throw invalid;
    auto &texture = *it->second;
    auto &p = texture.pixels;
    if (uint64_t(x) + width > p.width || uint64_t(y) + height > p.height || p.mips.size() != 1 ||
        p.mips[0].size() != size_t(p.width) * p.height * 4)
      throw invalid;
    auto &updates = *texture.updates;
    std::lock_guard lock(updates.lock);
    const auto *source = static_cast<const uint8_t *>(pixels);
    for (uint32_t row = 0; row < height; ++row) {
      const uint8_t *from = source + size_t(row) * rowPitch;
      uint8_t *to = p.mips[0].data() + (size_t(y + row) * p.width + x) * 4;
      if (!bgra) {
        std::memcpy(to, from, size_t(width) * 4);
        continue;
      }
      for (uint32_t i = 0; i < width; ++i, from += 4, to += 4) {
        to[0] = from[2];
        to[1] = from[1];
        to[2] = from[0];
        to[3] = from[3];
      }
    }
    texture.contentHash = 0;
    if (updates.dirty[2] <= updates.dirty[0] || updates.dirty[3] <= updates.dirty[1]) {
      updates.dirty[0] = x;
      updates.dirty[1] = y;
      updates.dirty[2] = x + width;
      updates.dirty[3] = y + height;
    } else {
      updates.dirty[0] = std::min(updates.dirty[0], x);
      updates.dirty[1] = std::min(updates.dirty[1], y);
      updates.dirty[2] = std::max(updates.dirty[2], x + width);
      updates.dirty[3] = std::max(updates.dirty[3], y + height);
    }
    ++updates.revision;
  }

  void NativeResources::freeUiTexture(uint64_t id) {
    if (!id) {
      throw invalid;
    }
    m_uiTextures.erase(id);
  }

  void NativeResources::submitUi(const remixapi_UIDrawList *info) {
    if (!info || !info->commandCount) {
      m_uiDraws.clear();
      return;
    }
    if (info->sType != REMIXAPI_STRUCT_TYPE_UI_DRAW_LIST || !info->pVertices || !info->pIndices ||
        !info->pCommands || !info->displayWidth || !info->displayHeight ||
        info->vertexCount > 8 * 1024 * 1024 || info->indexCount > 24 * 1024 * 1024 ||
        info->commandCount > 65536) {
      throw invalid;
    }
    auto surface = std::make_shared<Surface>();
    surface->vertices.resize(info->vertexCount);
    surface->indices.assign(info->pIndices, info->pIndices + info->indexCount);
    for (uint32_t i = 0; i < info->vertexCount; ++i) {
      const auto &src = info->pVertices[i];
      auto &dst = surface->vertices[i];
      const float values[] = {src.x, src.y, src.z, src.u, src.v};
      finite(values, 5);
      std::copy_n(values, 3, dst.position);
      std::copy_n(values + 3, 2, dst.uv);
      rgba(src.color, dst.color, true);
    }
    std::vector<Draw> pending;
    for (uint32_t i = 0; i < info->commandCount; ++i) {
      const auto &cmd = info->pCommands[i];
      const uint64_t end = uint64_t(cmd.indexOffset) + cmd.indexCount;
      constexpr uint32_t nativeFlags = 0xff1fu | REMIXAPI_UI_NATIVE_MASK_GROUP_BITS | REMIXAPI_UI_NATIVE_MASK_OUTSIDE;
      const uint32_t maskGroup = (cmd.flags & REMIXAPI_UI_NATIVE_MASK_GROUP_BITS) >> REMIXAPI_UI_NATIVE_MASK_GROUP_SHIFT;
      const uint32_t sourceBlend = (cmd.flags >> REMIXAPI_UI_NATIVE_SRC_BLEND_SHIFT) & 15;
      const uint32_t destinationBlend = (cmd.flags >> REMIXAPI_UI_NATIVE_DST_BLEND_SHIFT) & 15;
      if ((cmd.flags & ~nativeFlags) || end > info->indexCount || cmd.indexCount % 3 || sourceBlend > 14 ||
          destinationBlend > 14 || maskGroup > Mask::maxGroup ||
          (!maskGroup && (cmd.flags & REMIXAPI_UI_NATIVE_MASK_OUTSIDE)) ||
          (!(cmd.flags & REMIXAPI_UI_NATIVE_CUSTOM_BLEND) && (cmd.flags & 0xff00u))) {
        throw invalid;
      }
      for (uint64_t n = cmd.indexOffset; n < end; ++n) {
        const int64_t v = int64_t(surface->indices[n]) + cmd.vertexOffset;
        if (v < 0 || uint64_t(v) >= info->vertexCount) {
          throw invalid;
        }
      }
      if (!cmd.indexCount) {
        continue;
      }
      Draw draw;
      draw.surface = surface;
      draw.ui = true;
      draw.depth = (cmd.flags & REMIXAPI_UI_DRAW_FLAG_DEPTH_TEST) != 0;
      draw.doubleSided = true;
      draw.blend.enabled = true;
      draw.blend.alphaOpTexture = 3;
      draw.blend.alpha2 = 2;
      if (cmd.flags & REMIXAPI_UI_NATIVE_CUSTOM_BLEND) {
        draw.blend.src = draw.blend.srcAlpha = sourceBlend;
        draw.blend.dst = draw.blend.dstAlpha = destinationBlend;
      }
      draw.material = std::make_shared<Material>();
      if (cmd.textureId) {
        auto it = m_uiTextures.find(cmd.textureId);
        if (it == m_uiTextures.end()) {
          throw invalid;
        }
        draw.material->texture = it->second;
      } else {
        draw.material->texture = texturePath(nullptr);
      }
      draw.material->wrapU = (cmd.flags & REMIXAPI_UI_NATIVE_REPEAT_U) ? 1 : 0;
      draw.material->wrapV = (cmd.flags & REMIXAPI_UI_NATIVE_REPEAT_V) ? 1 : 0;
      draw.material->filter = (cmd.flags & REMIXAPI_UI_NATIVE_LINEAR) ? 1 : 0;
      draw.firstIndex = cmd.indexOffset;
      draw.indexCount = cmd.indexCount;
      draw.vertexOffset = cmd.vertexOffset;
      draw.canvasWidth = info->displayWidth;
      draw.canvasHeight = info->displayHeight;
      draw.maskGroup = uint8_t(maskGroup);
      draw.maskOutside = (cmd.flags & REMIXAPI_UI_NATIVE_MASK_OUTSIDE) != 0;
      pending.push_back(std::move(draw));
    }
    m_uiDraws = std::move(pending);
  }

  void NativeResources::overlay(const void *pixels, uint32_t width, uint32_t height, remixapi_Format format,
                                float opacity) {
    if (!std::isfinite(opacity) || opacity > 1) {
      throw invalid;
    }
    if (!pixels && !width && !height && opacity <= 0) {
      m_overlay.clear();
      return;
    }
    if (!pixels || !width || !height) {
      throw invalid;
    }
    remixapi_TextureInfo info = {};
    info.width = width;
    info.height = height;
    info.format = format;
    info.data = pixels;
    info.dataSize = uint64_t(width) * height * 4;
    if (format != REMIXAPI_FORMAT_R8G8B8A8_UNORM && format != REMIXAPI_FORMAT_R8G8B8A8_SRGB &&
        format != REMIXAPI_FORMAT_B8G8R8A8_UNORM && format != REMIXAPI_FORMAT_B8G8R8A8_SRGB) {
      throw invalid;
    }
    // Hosts resubmit the captured UI every frame, mostly unchanged. An image
    // equal to the current one keeps its texture (and, at the same opacity,
    // the whole draw), so the renderer neither decodes nor uploads it again.
    const auto *previous = m_overlay.empty() ? nullptr : &m_overlay.front();
    auto sameImage = [&](const Pixels &p, const uint8_t *bytes) {
      return p.width == width && p.height == height && p.mips.size() == 1 && p.hdrMips.empty() &&
             !p.compressed && p.srgb && std::memcmp(p.mips[0].data(), bytes, p.mips[0].size()) == 0;
    };
    std::shared_ptr<Texture> texture;
    const bool rgba = format == REMIXAPI_FORMAT_R8G8B8A8_UNORM || format == REMIXAPI_FORMAT_R8G8B8A8_SRGB;
    if (previous && rgba && sameImage(previous->material->texture->pixels, static_cast<const uint8_t *>(pixels)))
      texture = previous->material->texture; // RGBA decoding is a plain copy.
    if (!texture) {
      auto decoded = decodeTexture(info);
      decoded.srgb = true;
      if (previous && sameImage(previous->material->texture->pixels, decoded.mips[0].data()))
        texture = previous->material->texture;
      else {
        texture = std::make_shared<Texture>();
        texture->pixels = std::move(decoded);
      }
    }
    if (previous && texture == previous->material->texture &&
        previous->material->tint[3] == std::max(0.0f, opacity))
      return;
    Draw draw;
    draw.ui = true;
    draw.depth = false;
    draw.doubleSided = true;
    draw.blend.enabled = true;
    draw.canvasWidth = draw.canvasHeight = 1;
    draw.material = std::make_shared<Material>();
    draw.material->texture = std::move(texture);
    draw.material->tint[3] = std::max(0.0f, opacity);
    draw.material->wrapU = draw.material->wrapV = 0;
    draw.surface = std::make_shared<Surface>();
    draw.surface->vertices = {{{0, 0, 0}, {0, 0}, {1, 1, 1, 1}},
                              {{1, 0, 0}, {1, 0}, {1, 1, 1, 1}},
                              {{1, 1, 0}, {1, 1}, {1, 1, 1, 1}},
                              {{0, 1, 0}, {0, 1}, {1, 1, 1, 1}}};
    draw.surface->indices = {0, 1, 2, 0, 2, 3};
    m_overlay = {std::move(draw)};
  }

  void NativeResources::setTint(float r, float g, float b, float a) {
    const float values[] = {r, g, b, a};
    finite(values, 4);
    if (a > 1) {
      throw invalid;
    }
    std::copy_n(values, 4, m_tint);
    m_tint[3] = std::max(0.0f, a);
  }
} // namespace remix
