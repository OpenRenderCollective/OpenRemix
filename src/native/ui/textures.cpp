#include "ui/tabs.h"
#include "ui/context.h"
#include "ui/widgets.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

namespace {
  // Renderer images shown as ImGui textures (ui::addTexture), by view.
  std::unordered_map<const void *, ImTextureID> s_descriptorCache;
  std::unordered_map<uint64_t, ImTextureData *> s_thumbnailCache;
  char s_searchFilter[64] = {};
  int s_filterMode = 0; // 0: All, 1: Visible, 2: Hidden
  float s_thumbSize = 80.0f;
  uint64_t s_selectedHash = 0;

  bool matchesSearch(uint64_t hash, const char *search) {
    if (!search || !*search)
      return true;
    char hexStr[32], decStr[32];
    std::snprintf(hexStr, sizeof(hexStr), "%016" PRIx64, hash);
    std::snprintf(decStr, sizeof(decStr), "%" PRIu64, hash);
    std::string needle(search);
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return std::tolower(c); });
    if (needle.rfind("0x", 0) == 0)
      needle = needle.substr(2);
    return std::string(hexStr).find(needle) != std::string::npos ||
           std::string(decStr).find(needle) != std::string::npos;
  }

  struct ThumbnailImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels; // RGBA8
  };

  ThumbnailImage extractThumbnail(const remix::scene::Texture &texture, uint32_t maxDimension = 128) {
    ThumbnailImage result;
    const auto &p = texture.pixels;

    // 1. Block compressed preview
    if (p.compressed) {
      const auto &c = *p.compressed;
      if (c.previewWidth > 0 && c.previewHeight > 0 && !c.preview.empty()) {
        result.width = c.previewWidth;
        result.height = c.previewHeight;
        result.pixels = c.preview;
      }
    }

    // 2. Decoded mips
    if (result.pixels.empty() && !p.mips.empty() && p.width > 0 && p.height > 0) {
      size_t chosenMip = 0;
      uint32_t curW = p.width, curH = p.height;
      for (size_t m = 0; m < p.mips.size(); ++m) {
        if (std::max(curW, curH) <= maxDimension) {
          chosenMip = m;
          break;
        }
        if (m + 1 < p.mips.size()) {
          curW = std::max(1u, curW / 2);
          curH = std::max(1u, curH / 2);
        }
      }

      const auto &source = p.mips[chosenMip];
      if (source.size() >= size_t(curW) * curH * 4) {
        uint32_t step = 1;
        while (std::max(curW, curH) > maxDimension * step) {
          step *= 2;
        }
        const uint32_t tw = std::max(1u, (curW + step - 1) / step);
        const uint32_t th = std::max(1u, (curH + step - 1) / step);
        result.width = tw;
        result.height = th;
        result.pixels.resize(size_t(tw) * th * 4);

        for (uint32_t y = 0; y < th; ++y) {
          for (uint32_t x = 0; x < tw; ++x) {
            const uint32_t sx = std::min(x * step, curW - 1);
            const uint32_t sy = std::min(y * step, curH - 1);
            const size_t srcIdx = (size_t(sy) * curW + sx) * 4;
            const size_t dstIdx = (size_t(y) * tw + x) * 4;
            std::memcpy(&result.pixels[dstIdx], &source[srcIdx], 4);
          }
        }
      }
    }

    // 3. HDR mips (linear float)
    if (result.pixels.empty() && !p.hdrMips.empty() && p.width > 0 && p.height > 0) {
      const auto &source = p.hdrMips[0];
      uint32_t step = 1;
      while (std::max(p.width, p.height) > maxDimension * step) {
        step *= 2;
      }
      const uint32_t tw = std::max(1u, (p.width + step - 1) / step);
      const uint32_t th = std::max(1u, (p.height + step - 1) / step);
      result.width = tw;
      result.height = th;
      result.pixels.resize(size_t(tw) * th * 4);

      for (uint32_t y = 0; y < th; ++y) {
        for (uint32_t x = 0; x < tw; ++x) {
          const uint32_t sx = std::min(x * step, p.width - 1);
          const uint32_t sy = std::min(y * step, p.height - 1);
          const size_t srcIdx = (size_t(sy) * p.width + sx) * 4;
          const size_t dstIdx = (size_t(y) * tw + x) * 4;
          for (int c = 0; c < 3; ++c) {
            result.pixels[dstIdx + c] = uint8_t(std::clamp(source[srcIdx + c], 0.0f, 1.0f) * 255.0f);
          }
          result.pixels[dstIdx + 3] = uint8_t(std::clamp(source[srcIdx + 3], 0.0f, 1.0f) * 255.0f);
        }
      }
    }

    // 4. Normal map reconstruction for two-channel textures
    if (!result.pixels.empty() && p.twoChannel) {
      for (size_t i = 0; i < result.pixels.size(); i += 4) {
        const float nx = (result.pixels[i] / 255.0f) * 2.0f - 1.0f;
        const float ny = (result.pixels[i + 1] / 255.0f) * 2.0f - 1.0f;
        const float nz = std::sqrt(std::max(0.0f, 1.0f - nx * nx - ny * ny));
        result.pixels[i + 2] = uint8_t(std::clamp((nz * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
        result.pixels[i + 3] = 255;
      }
    }

    return result;
  }

  ImTextureData *getOrCreateThumbnail(uint64_t hash, const remix::scene::Texture &texture) {
    if (!hash)
      return nullptr;
    auto it = s_thumbnailCache.find(hash);
    if (it != s_thumbnailCache.end())
      return it->second;

    auto thumb = extractThumbnail(texture);
    if (thumb.pixels.empty() || !thumb.width || !thumb.height) {
      s_thumbnailCache[hash] = nullptr;
      return nullptr;
    }

    auto *texData = new ImTextureData();
    texData->Create(ImTextureFormat_RGBA32, int(thumb.width), int(thumb.height));
    std::memcpy(texData->Pixels, thumb.pixels.data(), size_t(thumb.width) * thumb.height * 4);
    ImGui::RegisterUserTexture(texData);
    remix::ui::updateTexture(texData);

    if (texData->Status == ImTextureStatus_OK && texData->TexID != ImTextureID_Invalid) {
      s_thumbnailCache[hash] = texData;
      return texData;
    }

    ImGui::UnregisterUserTexture(texData);
    delete texData;
    s_thumbnailCache[hash] = nullptr;
    return nullptr;
  }
} // namespace

namespace remix::ui {
  void resetTextureUiCache() {
    for (auto &[hash, texData] : s_thumbnailCache) {
      if (texData) {
        texData->UnusedFrames = 100;
        texData->SetStatus(ImTextureStatus_WantDestroy);
        remix::ui::updateTexture(texData);
        ImGui::UnregisterUserTexture(texData);
        delete texData;
      }
    }
    s_thumbnailCache.clear();
    for (auto &[view, texture] : s_descriptorCache)
      removeTexture(texture);
    s_descriptorCache.clear();
  }
} // namespace remix::ui

namespace remix {
  void RemixNativeRuntime::drawTexturesTab(config::Settings &settings) {
    const auto &allTextures = textures();
    const auto &allPaths = texturePaths();

    struct TextureItem {
      uint64_t hash = 0;
      std::shared_ptr<scene::Texture> texture;
      bool hidden = false;
    };

    std::vector<TextureItem> items;
    items.reserve(allTextures.size() + allPaths.size());

    std::unordered_map<uint64_t, std::shared_ptr<scene::Texture>> combined;
    for (const auto &[keyHash, tex] : allTextures) {
      if (tex)
        combined[tex->getHash()] = tex;
    }
    for (const auto &[path, weakTex] : allPaths) {
      if (auto tex = weakTex.lock()) {
        const uint64_t h = tex->getHash();
        if (h && combined.find(h) == combined.end())
          combined[h] = std::move(tex);
      }
    }

    size_t hiddenCount = 0;
    for (const auto &[hash, tex] : combined) {
      if (!tex)
        continue;
      const bool hidden = std::find(settings.hideTextures.begin(), settings.hideTextures.end(), hash) !=
                          settings.hideTextures.end();
      if (hidden)
        ++hiddenCount;
      items.push_back({hash, tex, hidden});
    }

    std::sort(items.begin(), items.end(), [](const TextureItem &a, const TextureItem &b) {
      return a.hash < b.hash;
    });

    ImGui::Text("Textures: %zu loaded, %zu hidden", items.size(), hiddenCount);
    ImGui::SameLine();
    if (hiddenCount > 0) {
      if (ImGui::SmallButton("Unhide all")) {
        settings.hideTextures.clear();
        m_raster.syncHiddenTextures(settings.hideTextures);
      }
    }

    ImGui::Separator();

    // Search and filter row
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
    ImGui::InputTextWithHint("##TexSearch", "Filter hash (0x...)", s_searchFilter, sizeof(s_searchFilter));
    ImGui::SameLine();
    if (ImGui::Button("Clear") && s_searchFilter[0] != '\0') {
      s_searchFilter[0] = '\0';
    }

    // Filter mode buttons
    ImGui::RadioButton("All", &s_filterMode, 0);
    ImGui::SameLine();
    char visLabel[32], hidLabel[32];
    std::snprintf(visLabel, sizeof(visLabel), "Visible (%zu)", items.size() - hiddenCount);
    std::snprintf(hidLabel, sizeof(hidLabel), "Hidden (%zu)", hiddenCount);
    ImGui::RadioButton(visLabel, &s_filterMode, 1);
    ImGui::SameLine();
    ImGui::RadioButton(hidLabel, &s_filterMode, 2);

    ui::SliderFloat("Size", &s_thumbSize, 48.0f, 160.0f, "%.0f px");

    // Selected Texture Inspector Panel
    if (s_selectedHash != 0) {
      auto it = std::find_if(items.begin(), items.end(), [](const TextureItem &item) {
        return item.hash == s_selectedHash;
      });
      if (it != items.end()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.14f, 0.18f, 0.9f));
        ImGui::BeginChild("SelectedTexturePanel", ImVec2(0, 116.0f), ImGuiChildFlags_Borders);

        const void *view = uiTextureView(it->texture.get());
        ImTextureRef selRef;
        if (view) {
          auto dIt = s_descriptorCache.find(view);
          if (dIt == s_descriptorCache.end())
            dIt = s_descriptorCache.emplace(view, ui::addTexture(view)).first;
          selRef = ImTextureRef(dIt->second);
        } else {
          ImTextureData *thumb = getOrCreateThumbnail(it->hash, *it->texture);
          if (thumb && thumb->TexID != ImTextureID_Invalid) {
            selRef = thumb->GetTexRef();
          }
        }

        const float panelH = 100.0f;
        const float aspect = float(std::max(1u, it->texture->pixels.width)) /
                             float(std::max(1u, it->texture->pixels.height));
        const ImVec2 previewExtent(aspect >= 1.0f ? panelH : panelH * aspect,
                                   aspect <= 1.0f ? panelH : panelH / aspect);

        if (selRef._TexData != nullptr || selRef._TexID != ImTextureID_Invalid) {
          ImGui::Image(selRef, previewExtent, ImVec2(0, 0), ImVec2(1, 1),
                       ImVec4(1, 1, 1, 1), ImVec4(0.25f, 0.25f, 0.3f, 1.0f));
          ImGui::SameLine();
        }

        ImGui::BeginGroup();
        char hexBuffer[32];
        std::snprintf(hexBuffer, sizeof(hexBuffer), "0x%016" PRIX64, it->hash);

        ImGui::Text("Selected: %s", hexBuffer);
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy Hash")) {
          ImGui::SetClipboardText(hexBuffer);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Deselect")) {
          s_selectedHash = 0;
        }

        ImGui::Text("Resolution: %u x %u | Mips: %u | %s",
                    it->texture->pixels.width, it->texture->pixels.height,
                    it->texture->pixels.levels(),
                    it->texture->pixels.srgb ? "sRGB" : "Linear");

        const bool isHidden = it->hidden;
        if (isHidden) {
          ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.25f, 0.25f, 1.0f));
          ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.35f, 0.35f, 1.0f));
          if (ImGui::Button("Unhide Texture", ImVec2(140.0f, 26.0f))) {
            settings.hideTextures.erase(
                std::remove(settings.hideTextures.begin(), settings.hideTextures.end(), it->hash),
                settings.hideTextures.end());
            m_raster.syncHiddenTextures(settings.hideTextures);
          }
          ImGui::PopStyleColor(2);
        } else {
          ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.35f, 1.0f));
          ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
          if (ImGui::Button("Hide Texture", ImVec2(140.0f, 26.0f))) {
            if (std::find(settings.hideTextures.begin(), settings.hideTextures.end(), it->hash) ==
                settings.hideTextures.end()) {
              settings.hideTextures.push_back(it->hash);
              m_raster.syncHiddenTextures(settings.hideTextures);
            }
          }
          ImGui::PopStyleColor(2);
        }
        ImGui::EndGroup();

        ImGui::EndChild();
        ImGui::PopStyleColor();
      } else {
        s_selectedHash = 0;
      }
    }

    ImGui::Separator();

    // Filter displayed items
    std::vector<const TextureItem *> displayItems;
    displayItems.reserve(items.size());
    for (const auto &item : items) {
      if (s_filterMode == 1 && item.hidden)
        continue;
      if (s_filterMode == 2 && !item.hidden)
        continue;
      if (!matchesSearch(item.hash, s_searchFilter))
        continue;
      displayItems.push_back(&item);
    }

    // Grid of Thumbnails
    const float availWidth = ImGui::GetContentRegionAvail().x;
    const float itemSpacing = ImGui::GetStyle().ItemSpacing.x;
    const int columns = std::max(1, int((availWidth + itemSpacing) / (s_thumbSize + itemSpacing + 8.0f)));

    ImGui::BeginChild("TextureThumbnailGrid", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);

    for (size_t i = 0; i < displayItems.size(); ++i) {
      const auto &item = *displayItems[i];
      const void *view = uiTextureView(item.texture.get());

      ImTextureRef texRef;
      if (view) {
        auto it = s_descriptorCache.find(view);
        if (it == s_descriptorCache.end())
          it = s_descriptorCache.emplace(view, ui::addTexture(view)).first;
        texRef = ImTextureRef(it->second);
      } else {
        ImTextureData *thumb = getOrCreateThumbnail(item.hash, *item.texture);
        if (thumb && thumb->TexID != ImTextureID_Invalid) {
          texRef = thumb->GetTexRef();
        }
      }

      // Aspect ratio extent
      const float aspect = float(std::max(1u, item.texture->pixels.width)) /
                           float(std::max(1u, item.texture->pixels.height));
      const ImVec2 extent(aspect >= 1.0f ? s_thumbSize : s_thumbSize * aspect,
                          aspect <= 1.0f ? s_thumbSize : s_thumbSize / aspect);

      ImGui::PushID(int(i));
      ImGui::BeginGroup();

      // Border and tint colors
      const bool isSelected = (item.hash == s_selectedHash);
      if (item.hidden) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.25f, 0.25f, 1.0f));
      } else if (isSelected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.55f, 0.85f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.65f, 0.95f, 1.0f));
      } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.12f, 0.14f, 0.7f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.25f, 0.3f, 0.9f));
      }

      if (texRef._TexData != nullptr || texRef._TexID != ImTextureID_Invalid) {
        if (ImGui::ImageButton("##thumb", texRef, extent, ImVec2(0, 0), ImVec2(1, 1),
                               ImVec4(0.15f, 0.15f, 0.18f, 1.0f))) {
          s_selectedHash = item.hash;
        }
      } else {
        char hashLabel[16];
        std::snprintf(hashLabel, sizeof(hashLabel), "%08" PRIX64, item.hash & 0xFFFFFFFF);
        if (ImGui::Button(hashLabel, ImVec2(s_thumbSize, s_thumbSize))) {
          s_selectedHash = item.hash;
        }
      }

      ImGui::PopStyleColor(2);

      // Context Menu
      if (ImGui::BeginPopupContextItem()) {
        s_selectedHash = item.hash;
        char hexBuffer[32];
        std::snprintf(hexBuffer, sizeof(hexBuffer), "0x%016" PRIX64, item.hash);

        if (ImGui::MenuItem(item.hidden ? "Unhide Texture" : "Hide Texture")) {
          if (item.hidden) {
            settings.hideTextures.erase(
                std::remove(settings.hideTextures.begin(), settings.hideTextures.end(), item.hash),
                settings.hideTextures.end());
          } else {
            if (std::find(settings.hideTextures.begin(), settings.hideTextures.end(), item.hash) ==
                settings.hideTextures.end()) {
              settings.hideTextures.push_back(item.hash);
            }
          }
          m_raster.syncHiddenTextures(settings.hideTextures);
        }

        if (ImGui::MenuItem("Copy Hash (Hex)")) {
          ImGui::SetClipboardText(hexBuffer);
        }

        char decBuffer[32];
        std::snprintf(decBuffer, sizeof(decBuffer), "%" PRIu64, item.hash);
        if (ImGui::MenuItem("Copy Hash (Decimal)")) {
          ImGui::SetClipboardText(decBuffer);
        }

        ImGui::EndPopup();
      }

      // Tooltip on Hover
      if (ImGui::IsItemHovered()) {
        char hexBuffer[32];
        std::snprintf(hexBuffer, sizeof(hexBuffer), "0x%016" PRIX64, item.hash);
        ImGui::BeginTooltip();
        ImGui::Text("Hash: %s", hexBuffer);
        ImGui::Text("Size: %u x %u (%u mips)", item.texture->pixels.width, item.texture->pixels.height,
                    item.texture->pixels.levels());
        ImGui::Text("Color space: %s", item.texture->pixels.srgb ? "sRGB" : "Linear");
        if (item.hidden) {
          ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Status: HIDDEN");
        } else {
          ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Status: VISIBLE");
        }
        ImGui::TextDisabled("Right-click for options");
        ImGui::EndTooltip();
      }

      ImGui::EndGroup();
      ImGui::PopID();

      if ((int(i) + 1) < int(displayItems.size()) && ((int(i) + 1) % columns) != 0) {
        ImGui::SameLine();
      }
    }

    ImGui::EndChild();
  }
} // namespace remix
