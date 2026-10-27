#include "scene/images.h"
#include "scene/resources.h"
#include "scene/validation.h"
#include <remix/remix_native_ext.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace remix::scene {
  MaterialDecode materialDecode(const Material &m) {
    MaterialDecode d;
    if (m.selfIllumination) {
      d.flags |= MaterialDecode::selfIllumination |
                 uint32_t(m.selfIllumination - 1) << MaterialDecode::selfIlluminationMaskShift;
      d.selfIlluminationScale = m.selfIlluminationScale;
    }
    return d;
  }
} // namespace remix::scene

namespace remix {
  using namespace scene;
  using namespace scene::detail;

  void NativeResources::createMaterial(const remixapi_MaterialInfo &info, remixapi_MaterialHandle &handle) {
    if (!info.hash) {
      throw invalid;
    }
    auto material = std::make_shared<Material>();
    if (auto *normal = static_cast<const remixapi_MaterialNormalEncodingEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_NORMAL_ENCODING_EXT))) {
      if (normal->encoding > 5)
        throw invalid;
      material->normalEncoding = normal->encoding;
    }
    if (const auto *scroll = static_cast<const remixapi_MaterialNormalScrollEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_NORMAL_SCROLL_EXT))) {
      finite(scroll->base, 2);
      finite(scroll->layer1, 2);
      finite(scroll->layer2, 2);
      if (material->normalEncoding != 5 || (scroll->layers != 1 && scroll->layers != 3))
        throw invalid;
      material->scrollLayers = scroll->layers;
      const float *rates[] = {scroll->base, scroll->layer1, scroll->layer2};
      for (unsigned i = 0; i < 3; ++i)
        std::copy_n(rates[i], 2, material->scroll + 2 * i);
    }
    const auto *opaque = static_cast<const remixapi_MaterialInfoOpaqueEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT));
    const auto *translucent = static_cast<const remixapi_MaterialInfoTranslucentEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT));
    const auto *portal = extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_PORTAL_EXT);
    if (unsigned(opaque != nullptr) + unsigned(translucent != nullptr) + unsigned(portal != nullptr) != 1) {
      throw invalid;
    }
    remixapi_Path texture = info.albedoTexture;
    if (opaque) {
      finite(&opaque->roughnessConstant, 1);
      finite(&opaque->metallicConstant, 1);
      if (opaque->roughnessConstant < 0 || opaque->roughnessConstant > 1 || opaque->metallicConstant < 0 ||
          opaque->metallicConstant > 1)
        throw invalid;
      material->roughness = opaque->roughnessConstant;
      material->metallic = opaque->metallicConstant;
      material->roughnessTexture = dataTexturePath(opaque->roughnessTexture);
      material->metallicTexture = dataTexturePath(opaque->metallicTexture);
      if (const auto *remap = static_cast<const remixapi_MaterialPbrRemapEXT *>(
              extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_REMAP_EXT))) {
        const remixapi_MaterialPbrChannelEXT *maps[] = {&remap->roughness, &remap->metallic};
        for (unsigned i = 0; i < 2; ++i) {
          const auto &in = *maps[i];
          finite(&in.exponent, 3);
          if (in.channel > 3 || in.invert > 1 || in.curve > 1 || in.exponent < 0 || in.exponent > 15)
            throw invalid;
          auto &out = material->remap[i];
          out.channel = uint8_t(in.channel);
          out.curve = uint8_t(in.curve);
          out.invert = in.invert != 0;
          out.exponent = in.exponent;
          out.bias = in.bias;
          out.scale = in.scale;
        }
      }
      material->tint[0] = opaque->albedoConstant.x;
      material->tint[1] = opaque->albedoConstant.y;
      material->tint[2] = opaque->albedoConstant.z;
      material->tint[3] = opaque->opacityConstant;
      if (opaque->alphaTestType < 0 || opaque->alphaTestType > 7) {
        throw invalid;
      }
      material->compare = uint32_t(opaque->alphaTestType);
      material->reference = float(opaque->alphaReferenceValue) / 255;
      material->drawAlpha = opaque->useDrawCallAlphaState != 0;
      material->translucent = opaque->opacityConstant < 1;
    } else if (translucent) {
      material->tint[0] = translucent->transmittanceColor.x;
      material->tint[1] = translucent->transmittanceColor.y;
      material->tint[2] = translucent->transmittanceColor.z;
      // Deliberate raster approximation: texture alpha and transmission tint, no refraction.
      if ((!texture || !*texture) && translucent->transmittanceTexture) {
        texture = translucent->transmittanceTexture;
      }
      material->translucent = true;
      material->transmission = true;
      // Without a thin wall, a body (water): Fresnel reflection by the
      // refractive index, and what passes is the colour to the power of the
      // distance behind over the measurement distance (Beer-Lambert). A thin
      // wall keeps the tinted-sheet approximation.
      finite(&translucent->refractiveIndex, 1);
      finite(&translucent->transmittanceMeasurementDistance, 1);
      if (!translucent->thinWallThickness_hasvalue && translucent->transmittanceMeasurementDistance > 0) {
        material->refractiveIndex = translucent->refractiveIndex >= 1 ? translucent->refractiveIndex : 1.5f;
        material->transmittanceDistance = translucent->transmittanceMeasurementDistance;
        material->roughness = .04f; // a smooth surface, shaded as PBR for its reflection
        material->metallic = 0;
      }
    }
    finite(material->tint, 4);
    if (material->tint[3] < 0 || material->tint[3] > 1) {
      throw invalid;
    }
    // Wrap modes as RTX Remix's MDL samplers: 0 clamp, 1 repeat, 2 mirrored
    // repeat, 3 clip (transparent black outside 0..1).
    if (info.filterMode > 1 || info.wrapModeU > 3 || info.wrapModeV > 3) {
      throw unavailable;
    }
    material->filter = info.filterMode;
    material->wrapU = info.wrapModeU;
    material->wrapV = info.wrapModeV;
    material->rows = std::max(uint8_t(1), info.spriteSheetRow);
    material->columns = std::max(uint8_t(1), info.spriteSheetCol);
    material->fps = info.spriteSheetFps;
    material->texture = texturePath(texture);
    if (material->texture && !material->sourceHash) {
      material->sourceHash = material->texture->getHash();
    }
    material->normalTexture = dataTexturePath(info.normalTexture);
    // Light through a translucent surface is focused by its waves or bumps.
    material->caustics = material->translucent && material->normalTexture;
    // Water shares the packed remap fields (frame_tables.h materialFields).
    if (material->normalEncoding == 5 && (material->roughnessTexture || material->metallicTexture))
      throw invalid;
    const auto *pbr = static_cast<const remixapi_MaterialInfoPbrEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_EXT));
    if (pbr && (!opaque || pbr->enabled > 1))
      throw invalid;
    material->pbr =
        material->transmittanceDistance > 0 ||
        (opaque && (pbr ? pbr->enabled != 0
                       : material->roughness > 0 || material->metallic > 0 || material->roughnessTexture ||
                             material->metallicTexture || material->normalTexture));
    finite(&info.emissiveColorConstant.x, 3);
    finite(&info.emissiveIntensity, 1);
    if (info.emissiveIntensity < 0 || info.emissiveColorConstant.x < 0 || info.emissiveColorConstant.y < 0 ||
        info.emissiveColorConstant.z < 0)
      throw invalid;
    for (unsigned c = 0; c < 3; ++c)
      material->emission[c] = (&info.emissiveColorConstant.x)[c] * info.emissiveIntensity;
    finite(material->emission, 3);
    if (const auto *emission = static_cast<const remixapi_MaterialInfoEmissionEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_EMISSION_EXT))) {
      if (emission->suppressLightGeneration > 1)
        throw invalid;
      material->suppressEmissionLight = emission->suppressLightGeneration != 0;
    }
    const auto *self = static_cast<const remixapi_MaterialInfoSelfIlluminationEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_SELF_ILLUMINATION_EXT));
    const bool emissionMap = info.emissiveTexture && *info.emissiveTexture;
    if (self) {
      if (!opaque || self->mask > REMIXAPI_SELF_ILLUMINATION_MASK_EMISSIVE_ALPHA_EXT ||
          !std::isfinite(self->maskScale) || !(self->maskScale > 0) ||
          (self->mask == REMIXAPI_SELF_ILLUMINATION_MASK_EMISSIVE_ALPHA_EXT && !emissionMap))
        throw invalid;
      material->selfIllumination = uint8_t(self->mask + 1);
      material->selfIlluminationScale = self->maskScale;
    }
    // A self-illumination mask holds values, not colours.
    if (emissionMap)
      material->emissionTexture = self ? dataTexturePath(info.emissiveTexture) : texturePath(info.emissiveTexture);
    auto previous = m_materials.find(info.hash);
    // A render thread may be filling the previous material's cache: it is
    // only carried over while frames render here (a CPU cache; the replacement
    // computes the same values on demand).
    if (!m_concurrentConsumer && previous != m_materials.end() &&
        previous->second->emissionTexture == material->emissionTexture &&
        previous->second->selfIllumination == material->selfIllumination &&
        previous->second->selfIlluminationScale == material->selfIlluminationScale &&
        (!material->selfIllumination || (previous->second->texture == material->texture &&
                                         std::equal(material->tint, material->tint + 3, previous->second->tint))) &&
        std::equal(material->emission, material->emission + 3, previous->second->emission) &&
        material->wrapU == previous->second->wrapU && material->wrapV == previous->second->wrapV)
      material->emissionCache = previous->second->emissionCache;
    // remixapi_MaterialInfoReplacementHashEXT: the key USD mods replace it by.
    if (const auto *replacement = static_cast<const remixapi_MaterialInfoReplacementHashEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_REPLACEMENT_HASH_EXT)))
      material->sourceHash = replacement->materialHash;
    auto equal = [&](const Material &a, const Material &b) {
      return a.sourceHash == b.sourceHash && a.texture == b.texture && a.emissionTexture == b.emissionTexture &&
             a.suppressEmissionLight == b.suppressEmissionLight && a.selfIllumination == b.selfIllumination &&
             a.selfIlluminationScale == b.selfIlluminationScale && a.roughnessTexture == b.roughnessTexture &&
             a.metallicTexture == b.metallicTexture && a.normalTexture == b.normalTexture &&
             a.normalEncoding == b.normalEncoding && a.roughness == b.roughness && a.metallic == b.metallic &&
             a.remap[0] == b.remap[0] && a.remap[1] == b.remap[1] && a.scrollLayers == b.scrollLayers &&
             std::equal(a.scroll, a.scroll + 6, b.scroll) && a.refractiveIndex == b.refractiveIndex &&
             a.transmittanceDistance == b.transmittanceDistance &&
             a.pbr == b.pbr && std::equal(a.emission, a.emission + 3, b.emission) &&
             std::equal(a.tint, a.tint + 4, b.tint) && a.compare == b.compare && a.reference == b.reference &&
             a.drawAlpha == b.drawAlpha && a.translucent == b.translucent &&
             a.transmission == b.transmission && a.caustics == b.caustics && a.filter == b.filter && a.wrapU == b.wrapU &&
             a.wrapV == b.wrapV && a.rows == b.rows && a.columns == b.columns && a.fps == b.fps;
    };
    if (previous == m_materials.end() || !equal(*previous->second, *material)) {
      if (previous != m_materials.end()) {
        previous->second->retired = true;
        ++m_retireGeneration;
      }
      m_materials[info.hash] = std::move(material);
      ++m_resourceGeneration;
      textureCounters.materialsCreated.fetch_add(1, std::memory_order_relaxed);
    }
    handle = reinterpret_cast<remixapi_MaterialHandle>(uintptr_t(info.hash));
  }

  void NativeResources::destroyMaterial(remixapi_MaterialHandle handle) {
    if (auto it = m_materials.find(key(handle)); it != m_materials.end()) {
      it->second->retired = true;
      ++m_retireGeneration;
      m_materials.erase(it);
      textureCounters.materialsDestroyed.fetch_add(1, std::memory_order_relaxed);
    }
  }
} // namespace remix
