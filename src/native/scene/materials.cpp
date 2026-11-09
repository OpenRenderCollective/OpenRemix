#include "scene/images.h"
#include "scene/bake_material.h"
#include "scene/portals.h"
#include "scene/resources.h"
#include "scene/validation.h"
#include <remix/remix_native_ext.h>
#include <remix/remix_bake_ext.h>
#include <remix/remix_reflection_ext.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace remix::scene {
  MaterialDecode materialDecode(const Material &m) {
    MaterialDecode d;
    if (m.selfIllumination) {
      d.flags |= MaterialDecode::selfIllumination | uint32_t(m.selfIllumination - 1)
                                                        << MaterialDecode::selfIlluminationMaskShift;
      d.selfIlluminationScale = m.selfIlluminationScale;
    }
    if (m.metalColored) {
      d.flags |= MaterialDecode::metalColor;
      for (unsigned i = 0; i < 3; ++i)
        d.pbrWord |= uint32_t(std::lround(std::clamp(m.metalColor[i], 0.f, 1.f) * 255)) << (8 * i);
    }
    if (m.pbrBlur > 0) {
      d.flags |= MaterialDecode::pbrBlur;
      d.pbrWord |= uint32_t(std::lround(std::clamp(m.pbrBlur, 0.f, 15.9375f) * 16)) << 24;
    }
    if (m.gradient)
      d.flags |= MaterialDecode::gradientMap;
    if (m.portalRim)
      d.flags |= MaterialDecode::portalRim;
    if (m.solidEnergy)
      d.flags |= MaterialDecode::solidEnergy;
    if (m.blendModulation)
      d.flags |= MaterialDecode::blendModulation;
    if (m.fogBody && (m.refractTint[0] != 1 || m.refractTint[1] != 1 || m.refractTint[2] != 1))
      d.flags |= MaterialDecode::refractTint;
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
    const auto *portal = static_cast<const remixapi_MaterialInfoPortalEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_PORTAL_EXT));
    // A portal stands alone (RTX Remix's portal material) or extends an
    // opaque material, the look of its surface where it does not open.
    if (unsigned(opaque != nullptr) + unsigned(translucent != nullptr) + unsigned(portal && !opaque) != 1) {
      throw invalid;
    }
    // Indices past remixapi_GetRayPortalCapabilities' count open nothing.
    if (portal && portal->rayPortalIndex < scene::rayPortalSlots)
      material->rayPortal = uint8_t(portal->rayPortalIndex + 1);
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
      if (const auto *metal = static_cast<const remixapi_MaterialMetalColorEXT *>(
              extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_METAL_COLOR_EXT))) {
        finite(&metal->color.x, 3);
        const float color[3] = {metal->color.x, metal->color.y, metal->color.z};
        for (unsigned i = 0; i < 3; ++i) {
          if (color[i] < 0 || color[i] > 1)
            throw invalid;
          material->metalColor[i] = color[i];
        }
        material->metalColored = true;
      }
      if (const auto *blur = static_cast<const remixapi_MaterialPbrBlurEXT *>(
              extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_BLUR_EXT))) {
        finite(&blur->roughnessMinimumLod, 1);
        if (blur->roughnessMinimumLod < 0 || blur->roughnessMinimumLod > 15)
          throw invalid;
        material->pbrBlur = blur->roughnessMinimumLod;
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
      if (const auto *screen = static_cast<const remixapi_MaterialInfoScreenCameraEXT *>(
              extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_SCREEN_CAMERA_EXT))) {
        if (screen->slot >= REMIXAPI_SCREEN_CAMERA_SLOTS_EXT)
          throw invalid;
        material->screenCamera = uint8_t(screen->slot + 1);
      }
      // RTX Remix's thin film: a film of index 1.5 at a constant thickness, or
      // at albedo alpha times 1500 nm, the material then fully opaque.
      auto &film = material->iridescence;
      if (opaque->thinFilmThickness_hasvalue) {
        finite(&opaque->thinFilmThickness_value, 1);
        film.strength = 1;
        film.thickness = std::clamp(opaque->thinFilmThickness_value, 0.f, 2000.f);
      }
      if (opaque->alphaIsThinFilmThickness) {
        film.strength = 1;
        film.fromAlpha = true;
        material->tint[3] = 1;
        material->compare = 7;
        material->drawAlpha = material->translucent = false;
      }
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
      // wall retains its thickness for finite optical transport; primary thin
      // refraction is routed separately from containing-body rendering.
      finite(&translucent->refractiveIndex, 1);
      finite(&translucent->transmittanceMeasurementDistance, 1);
      if (translucent->thinWallThickness_hasvalue) {
        finite(&translucent->thinWallThickness_value, 1);
        if (translucent->thinWallThickness_value < 0)
          throw invalid;
        material->thinWallThickness = translucent->thinWallThickness_value;
      }
      // Zero-thickness legacy sheets retain their tinted compatibility path.
      // A positive measured thickness supplies finite sheet absorption.
      if ((!translucent->thinWallThickness_hasvalue || material->thinWallThickness > 0) &&
          translucent->transmittanceMeasurementDistance > 0) {
        material->refractiveIndex = translucent->refractiveIndex >= 1 ? translucent->refractiveIndex : 1.5f;
        material->transmittanceDistance = translucent->transmittanceMeasurementDistance;
        material->roughness = .04f; // a smooth surface, shaded as PBR for its reflection
        material->metallic = 0;
      }
      if (const auto *fog = static_cast<const remixapi_MaterialTranslucentFogEXT *>(
              extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_TRANSLUCENT_FOG_EXT))) {
        const float tint[3] = {fog->refractTint.x, fog->refractTint.y, fog->refractTint.z};
        finite(&fog->fogEnd, 1);
        finite(tint, 3);
        if (material->transmittanceDistance <= 0 || !(fog->fogEnd > 0) ||
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_IRIDESCENCE_EXT) ||
            std::any_of(tint, tint + 3, [](float v) { return v < 0 || v > 1; }))
          throw invalid;
        material->transmittanceDistance = fog->fogEnd;
        material->fogBody = true;
        std::copy_n(tint, 3, material->refractTint);
      }
    }
    if (const auto *scroll = static_cast<const remixapi_MaterialTransmittanceScrollEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_TRANSMITTANCE_SCROLL_EXT))) {
      finite(scroll->rate, 2);
      if (!validSmoothBoundary(*material) || material->thinWallThickness <= 0)
        throw invalid;
      std::copy_n(scroll->rate, 2, material->transmittanceScroll);
    }
    if (const auto *average = static_cast<const remixapi_MaterialBakeTintAverageEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_BAKE_TINT_AVERAGE_EXT))) {
      if ((average->axes & ~3u) || !validSmoothBoundary(*material) || material->thinWallThickness <= 0)
        throw invalid;
      material->bakeTintAxes = average->axes;
    }
    if (const auto *boundary = static_cast<const remixapi_MaterialBakeBoundaryEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_BAKE_BOUNDARY_EXT))) {
      if (boundary->smoothBoundary > 1 || (boundary->smoothBoundary && !validSmoothBoundary(*material)))
        throw invalid;
      material->bakeSmoothBoundary = boundary->smoothBoundary != 0;
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
    const auto *gradient = static_cast<const remixapi_MaterialGradientMapEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_GRADIENT_MAP_EXT));
    const auto *rim = static_cast<const remixapi_MaterialPortalRimEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_PORTAL_RIM_EXT));
    // A gradient map reads the albedo texture as stored: its alpha is
    // luminance. So does a portal rim: it is its noise.
    material->texture = (gradient || rim) && texture && *texture ? dataTexturePath(texture) : texturePath(texture);
    if (rim) {
      // The rim's parameters take the film's fields (frame_tables.h
      // portalRimFields), its colour and coverage the material's own.
      if (!opaque || gradient || portal || opaque->thinFilmThickness_hasvalue || opaque->alphaIsThinFilmThickness ||
          extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_IRIDESCENCE_EXT) || !rim->colorTexture ||
          !*rim->colorTexture || !std::isfinite(rim->colorScale) || rim->colorScale < 0)
        throw invalid;
      auto out = std::make_shared<PortalRim>();
      out->ramp = texturePath(rim->colorTexture);
      out->rampSampler = samplerMaterial(out->ramp, 1, 0);
      out->colorScale = rim->colorScale;
      material->portalRim = std::move(out);
    }
    if (const auto *energy = static_cast<const remixapi_MaterialSolidEnergyEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_SOLID_ENERGY_EXT))) {
      // Its details and rates take the film's fields (frame_tables.h
      // solidEnergyFields): half floats.
      const float values[] = {energy->intensity,        energy->baseScroll[0],    energy->baseScroll[1],
                              energy->detail1Scroll[0], energy->detail1Scroll[1], energy->detail2Scroll[0],
                              energy->detail2Scroll[1], energy->detail1Sway[0],   energy->detail1Sway[1]};
      if (!opaque || gradient || rim || portal || opaque->thinFilmThickness_hasvalue ||
          opaque->alphaIsThinFilmThickness || extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_IRIDESCENCE_EXT) ||
          !energy->detail1Texture || !*energy->detail1Texture || !energy->detail2Texture || !*energy->detail2Texture)
        throw invalid;
      finite(values, 9);
      for (const float v : values)
        if (std::fabs(v) > 65504)
          throw invalid;
      if (energy->intensity < 0 || energy->detail1Sway[1] < 0)
        throw invalid;
      auto out = std::make_shared<SolidEnergy>();
      out->detail1 = texturePath(energy->detail1Texture);
      out->detail2 = texturePath(energy->detail2Texture);
      out->detail1Sampler = samplerMaterial(out->detail1, 1, 1);
      out->detail2Sampler = samplerMaterial(out->detail2, 1, 1);
      out->intensity = energy->intensity;
      std::copy_n(energy->baseScroll, 2, out->baseScroll);
      std::copy_n(energy->detail1Scroll, 2, out->detail1Scroll);
      std::copy_n(energy->detail2Scroll, 2, out->detail2Scroll);
      std::copy_n(energy->detail1Sway, 2, out->detail1Sway);
      material->solidEnergy = std::move(out);
    }
    if (const auto *modulation = static_cast<const remixapi_MaterialBlendModulationEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_BLEND_MODULATION_EXT))) {
      // The mask and its transform take the film's fields (frame_tables.h
      // blendModulationFields).
      if (!opaque || gradient || rim || portal || material->solidEnergy || opaque->thinFilmThickness_hasvalue ||
          opaque->alphaIsThinFilmThickness || extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_IRIDESCENCE_EXT) ||
          !modulation->maskTexture || !*modulation->maskTexture)
        throw invalid;
      finite(&modulation->transform[0][0], 6);
      auto out = std::make_shared<BlendModulation>();
      out->mask = dataTexturePath(modulation->maskTexture);
      out->sampler = samplerMaterial(out->mask, 1, 1);
      std::copy_n(&modulation->transform[0][0], 6, &out->transform[0][0]);
      material->blendModulation = std::move(out);
    }
    if (gradient) {
      // B.a is luminance, not coverage: an opaque material without alpha test
      // or a film (the film's fields hold the gradient's parameters), whose
      // opacityConstant alone gives its opacity.
      if (!opaque || opaque->thinFilmThickness_hasvalue || opaque->alphaIsThinFilmThickness ||
          extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_IRIDESCENCE_EXT) || !gradient->gradientTexture ||
          !*gradient->gradientTexture)
        throw invalid;
      const float values[] = {gradient->skinRow,         gradient->clothRow,     gradient->spriteOffset[0],
                              gradient->spriteOffset[1], gradient->detailU[0],   gradient->detailU[1],
                              gradient->detailV[0],      gradient->detailV[1],   gradient->bloodRange[0],
                              gradient->bloodRange[1],   gradient->bloodColor.x, gradient->bloodColor.y,
                              gradient->bloodColor.z,    gradient->burnStrength};
      finite(values, 14);
      // Packed as half floats (frame_tables.h gradientFields), the colour as unorm8;
      // the colour and the burn strength 0 to 1.
      for (unsigned i = 0; i < 10; ++i)
        if (std::fabs(values[i]) > 65504)
          throw invalid;
      for (unsigned i = 10; i < 14; ++i)
        if (values[i] < 0 || values[i] > 1)
          throw invalid;
      if (!(gradient->bloodRange[0] < gradient->bloodRange[1]))
        throw invalid;
      auto out = std::make_shared<GradientMap>();
      out->gradient = texturePath(gradient->gradientTexture);
      out->gradientSampler = samplerMaterial(out->gradient, 1, 0);
      if (gradient->detailTexture && *gradient->detailTexture) {
        out->detail = dataTexturePath(gradient->detailTexture);
        out->detailSampler = samplerMaterial(out->detail, material->filter, 1);
      }
      // The burn texture is read as an albedo texture is (sRGB-decoded where
      // it is sRGB-encoded), repeating.
      if (gradient->burnTexture && *gradient->burnTexture) {
        out->burn = texturePath(gradient->burnTexture);
        out->burnSampler = samplerMaterial(out->burn, material->filter, 1);
        out->burnStrength = gradient->burnStrength;
      }
      out->skinRow = gradient->skinRow;
      out->clothRow = gradient->clothRow;
      std::copy_n(gradient->spriteOffset, 2, out->spriteOffset);
      std::copy_n(gradient->detailU, 2, out->detailU);
      std::copy_n(gradient->detailV, 2, out->detailV);
      std::copy_n(values + 10, 3, out->bloodColor);
      std::copy_n(gradient->bloodRange, 2, out->bloodRange);
      material->gradient = std::move(out);
      // Below opacity 1 it blends as any opaque material does (translucent).
      material->compare = 7;
      material->drawAlpha = false;
    }
    if (material->texture && !material->sourceHash) {
      material->sourceHash = material->texture->getHash();
    }
    material->normalTexture = dataTexturePath(info.normalTexture);
    // Light through a translucent surface is focused by its waves or bumps.
    material->caustics = material->translucent && material->normalTexture;
    // Water shares the packed remap fields (frame_tables.h materialFields).
    if (material->normalEncoding == 5 && (material->roughnessTexture || material->metallicTexture))
      throw invalid;
    const auto *pbr =
        static_cast<const remixapi_MaterialInfoPbrEXT *>(extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_EXT));
    if (pbr && (!opaque || pbr->enabled > 1))
      throw invalid;
    if (const auto *film = static_cast<const remixapi_MaterialIridescenceEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_IRIDESCENCE_EXT))) {
      finite(&film->strength, 9);
      if (portal || film->strength < 0 || film->strength > 1 || film->filmRefractiveIndex < 1 ||
          film->filmRefractiveIndex > 3 || film->thickness < 0 || film->thickness > 2000 || film->thicknessMin < 0 ||
          film->thicknessMax > 2000 || film->thicknessMin > film->thicknessMax || film->flowAmplitude < 0 ||
          film->flowAmplitude > 2000 || !(film->flowScale > 0) || film->structural < 0 || film->structural > 1)
        throw invalid;
      auto &out = material->iridescence;
      out.strength = film->strength;
      out.ior = film->filmRefractiveIndex;
      out.thickness = film->thickness;
      out.min = film->thicknessMin;
      out.max = film->thicknessMax;
      out.flowAmplitude = film->flowAmplitude;
      out.flowScale = film->flowScale;
      out.flowSpeed = film->flowSpeed;
      out.structural = film->structural;
      out.thicknessTexture = dataTexturePath(film->thicknessTexture);
      // On glass or water, a smooth dielectric whose film colours the
      // reflection and, by what it reflects, the light passing through.
      if (translucent && out.strength > 0) {
        material->roughness = .04f;
        material->metallic = 0;
      }
    }
    const auto *sssWeight = static_cast<const remixapi_MaterialSubsurfaceWeightEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_SUBSURFACE_WEIGHT_EXT));
    if (sssWeight && (!opaque || !std::isfinite(sssWeight->weight) || sssWeight->weight < 0 || sssWeight->weight > 1))
      throw invalid;
    if (const auto *sss = static_cast<const remixapi_MaterialInfoOpaqueSubsurfaceEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_SUBSURFACE_EXT));
        sss && opaque) {
      finite(&sss->subsurfaceTransmittanceColor.x, 3);
      finite(&sss->subsurfaceSingleScatteringAlbedo.x, 3);
      finite(&sss->subsurfaceRadius.x, 3);
      const float scalars[] = {sss->subsurfaceMeasurementDistance, sss->subsurfaceVolumetricAnisotropy,
                               sss->subsurfaceRadiusScale, sss->subsurfaceMaxSampleRadius};
      finite(scalars, 4);
      auto &out = material->subsurface;
      out.diffusionProfile = sss->subsurfaceDiffusionProfile != 0;
      out.measurementDistance = out.diffusionProfile ? 0 : std::max(sss->subsurfaceMeasurementDistance, 0.f);
      out.anisotropy = std::clamp(sss->subsurfaceVolumetricAnisotropy, -.9f, .9f);
      const remixapi_Float3D &color = sss->subsurfaceTransmittanceColor,
                             &single = sss->subsurfaceSingleScatteringAlbedo, &radius = sss->subsurfaceRadius;
      out.transmittance[0] = std::clamp(color.x, 0.f, 1.f);
      out.transmittance[1] = std::clamp(color.y, 0.f, 1.f);
      out.transmittance[2] = std::clamp(color.z, 0.f, 1.f);
      out.singleScatteringAlbedo[0] = std::clamp(single.x, 0.f, 1.f);
      out.singleScatteringAlbedo[1] = std::clamp(single.y, 0.f, 1.f);
      out.singleScatteringAlbedo[2] = std::clamp(single.z, 0.f, 1.f);
      out.radius[0] = std::max(radius.x, 0.f);
      out.radius[1] = std::max(radius.y, 0.f);
      out.radius[2] = std::max(radius.z, 0.f);
      out.radiusScale = std::max(sss->subsurfaceRadiusScale, 1e-5f);
      // RTX Remix stores this in a unorm: at most a metre. Zero there silently
      // turns the profile off; here, like an unauthored one, it is no limit.
      out.maxSampleRadius = sss->subsurfaceMaxSampleRadius > 0 ? std::min(sss->subsurfaceMaxSampleRadius, 1.f) : 1;
      out.transmittanceTexture = texturePath(sss->subsurfaceTransmittanceTexture);
      out.thicknessTexture = dataTexturePath(sss->subsurfaceThicknessTexture);
      out.singleScatteringAlbedoTexture = texturePath(sss->subsurfaceSingleScatteringAlbedoTexture);
      out.radiusTexture = dataTexturePath(sss->subsurfaceRadiusTexture);
      // Its transmittance map is the surface's colour under a profile: as the
      // albedo map it reads no differently, tints and vertex colours included
      // (RTX Remix's rtx.subsurface.transmittanceFollowsAlbedo).
      if (out.transmittanceTexture == material->texture)
        out.transmittanceTexture.reset();
      if (out.diffusionProfile) {
        out.thicknessTexture.reset();
        out.singleScatteringAlbedoTexture.reset();
        // A zero-initialized colour is no colour.
        if (!(color.x > 0 || color.y > 0 || color.z > 0) && !out.transmittanceTexture)
          std::fill_n(out.transmittance, 3, 1.f);
      } else {
        out.radiusTexture.reset();
      }
      const bool scatters = out.radius[0] > 0 || out.radius[1] > 0 || out.radius[2] > 0 || out.radiusTexture;
      if ((out.diffusionProfile && scatters) || out.measurementDistance > 0)
        out.weight = sssWeight ? sssWeight->weight : 1;
    }
    material->pbr = material->transmittanceDistance > 0 ||
                    (opaque && (pbr ? pbr->enabled != 0
                                    : material->roughness > 0 || material->metallic > 0 || material->roughnessTexture ||
                                          material->metallicTexture || material->normalTexture)) ||
                    material->iridescence.strength > 0 || // a film is a reflection
                    material->subsurface.weight > 0;
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
    if (const auto *alternates = static_cast<const remixapi_MaterialReplacementKeysEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_REPLACEMENT_KEYS_EXT))) {
      if (alternates->count > 32 || (alternates->count && !alternates->keys))
        throw invalid;
      for (uint32_t i = 0; i < alternates->count; ++i) {
        const auto hash = alternates->keys[i];
        if (hash && hash != info.hash && hash != material->sourceHash &&
            std::find(material->replacementKeys.begin(), material->replacementKeys.end(), hash) ==
                material->replacementKeys.end())
          material->replacementKeys.push_back(hash);
      }
    }
    scene::ReflectionBinding reflection;
    if (const auto *binding = static_cast<const remixapi_MaterialReflectionBindingEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_REFLECTION_BINDING_EXT))) {
      if (bool(binding->sceneId) != bool(binding->volumeId) || material->translucent || material->transmission)
        throw invalid;
      reflection = {binding->sceneId, binding->volumeId};
    }
    auto equal = [&](const Material &a, const Material &b) {
      return a.sourceHash == b.sourceHash && a.replacementKeys == b.replacementKeys && a.texture == b.texture && a.emissionTexture == b.emissionTexture &&
             a.suppressEmissionLight == b.suppressEmissionLight && a.selfIllumination == b.selfIllumination &&
             a.selfIlluminationScale == b.selfIlluminationScale && a.roughnessTexture == b.roughnessTexture &&
             a.metallicTexture == b.metallicTexture && a.normalTexture == b.normalTexture &&
             a.normalEncoding == b.normalEncoding && a.roughness == b.roughness && a.metallic == b.metallic &&
             a.remap[0] == b.remap[0] && a.remap[1] == b.remap[1] && a.scrollLayers == b.scrollLayers &&
             std::equal(a.scroll, a.scroll + 6, b.scroll) && a.refractiveIndex == b.refractiveIndex &&
             a.transmittanceDistance == b.transmittanceDistance && a.fogBody == b.fogBody &&
             std::equal(a.refractTint, a.refractTint + 3, b.refractTint) &&
             a.bakeSmoothBoundary == b.bakeSmoothBoundary &&
             a.thinWallThickness == b.thinWallThickness &&
             std::equal(a.transmittanceScroll, a.transmittanceScroll + 2, b.transmittanceScroll) &&
             a.bakeTintAxes == b.bakeTintAxes && a.iridescence == b.iridescence &&
             a.subsurface == b.subsurface && a.metalColored == b.metalColored &&
             std::equal(a.metalColor, a.metalColor + 3, b.metalColor) && a.pbrBlur == b.pbrBlur &&
             (a.gradient == b.gradient || (a.gradient && b.gradient && *a.gradient == *b.gradient)) &&
             (a.portalRim == b.portalRim || (a.portalRim && b.portalRim && *a.portalRim == *b.portalRim)) &&
             (a.solidEnergy == b.solidEnergy || (a.solidEnergy && b.solidEnergy && *a.solidEnergy == *b.solidEnergy)) &&
             (a.blendModulation == b.blendModulation ||
              (a.blendModulation && b.blendModulation && *a.blendModulation == *b.blendModulation)) &&
             a.pbr == b.pbr && std::equal(a.emission, a.emission + 3, b.emission) &&
             std::equal(a.tint, a.tint + 4, b.tint) && a.compare == b.compare && a.reference == b.reference &&
             a.drawAlpha == b.drawAlpha && a.translucent == b.translucent && a.transmission == b.transmission &&
             a.caustics == b.caustics && a.screenCamera == b.screenCamera && a.rayPortal == b.rayPortal &&
             a.filter == b.filter && a.wrapU == b.wrapU && a.wrapV == b.wrapV && a.rows == b.rows &&
             a.columns == b.columns && a.fps == b.fps;
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
    setMaterialReflection(info.hash, reflection);
    handle = reinterpret_cast<remixapi_MaterialHandle>(uintptr_t(info.hash));
  }

  void NativeResources::destroyMaterial(remixapi_MaterialHandle handle) {
    setMaterialReflection(key(handle), {});
    if (auto it = m_materials.find(key(handle)); it != m_materials.end()) {
      it->second->retired = true;
      ++m_retireGeneration;
      m_materials.erase(it);
      textureCounters.materialsDestroyed.fetch_add(1, std::memory_order_relaxed);
    }
  }
} // namespace remix
