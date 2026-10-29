// The per-instance material (remixapi_InstanceInfoMaterialEXT) and dynamic
// textures.
#include "scene/fixtures.h"
#include "scene/emission.h"
#include "scene/opacity.h"
#include "scene/temporal.h"
#include <chrono>
#include <thread>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>

using namespace remix::test;
namespace scene = remix::scene;

namespace {
  std::shared_ptr<scene::Material> drawn(remix::NativeResources &r, remixapi_MeshHandle mesh) {
    auto info = instance(mesh);
    r.drawInstance(info);
    return r.draws().back().material;
  }

  void materialOverride() {
    remix::NativeResources r;
    auto own = material(r, 1, 1, 0, 0), other = material(r, 2, 0, 1, 0);
    auto mesh = quad(r, 3, own);
    const auto ownMaterial = drawn(r, mesh);
    remixapi_InstanceInfoMaterialEXT override{};
    override.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MATERIAL_EXT;
    override.material = other;
    remixapi_Transform locals[2] = {};
    for (auto &local : locals)
      local.matrix[0][0] = local.matrix[1][1] = local.matrix[2][2] = 1;
    remixapi_InstanceInfoGpuInstancingEXT instancing{};
    instancing.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_GPU_INSTANCING_EXT;
    instancing.pNext = &override;
    instancing.instanceTransforms_values = locals;
    instancing.instanceTransforms_count = 2;
    auto info = instance(mesh);
    info.pNext = &instancing;
    r.finishFrame();
    r.drawInstance(info);
    require(r.draws().size() == 2, "Instanced override draw count");
    const auto otherMaterial = r.draws()[0].material;
    require(otherMaterial != ownMaterial && r.draws()[1].material == otherMaterial,
            "DrawInstance ignored the material override");
    // Same chain through a retained instance: the same material.
    const uint64_t retained = r.createRetained(info);
    r.finishFrame();
    r.drawRetained(nullptr);
    require(r.draws().size() == 2 && r.draws()[0].material == otherMaterial &&
                r.draws()[1].material == otherMaterial,
            "Retained and immediate overrides disagree");
    r.destroyRetained(&retained, 1);
    // A null handle keeps the mesh's own material.
    override.material = nullptr;
    r.finishFrame();
    r.drawInstance(info);
    require(r.draws()[0].material == ownMaterial, "Null override replaced the material");
    // A dead handle keeps the mesh's own material, without an error.
    override.material = other;
    r.destroyMaterial(other);
    r.finishFrame();
    r.drawInstance(info);
    require(r.draws().size() == 2 && r.draws()[0].material == ownMaterial,
            "Dead override not ignored");
    // A handle that was never a material is ignored the same way.
    override.material = reinterpret_cast<remixapi_MaterialHandle>(uintptr_t(0x1234));
    r.finishFrame();
    r.drawInstance(info);
    require(r.draws()[0].material == ownMaterial, "Unknown override not ignored");
    // Hidden instances draw nothing with or without the override.
    info.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN;
    r.finishFrame();
    r.drawInstance(info);
    require(r.draws().empty(), "Hidden override drawn");
  }
  // remixapi_TextureInfoDynamicEXT / remixapi_UpdateTextureEXT: the texture
  // and what was made from it stay; only the pixels change.
  void dynamicTexture() {
    remix::NativeResources r;
    const uint8_t red[4][4] = {{0, 0, 255, 255}, {0, 0, 255, 255}, {0, 0, 255, 255}, {0, 0, 255, 255}}; // B8G8R8A8
    remixapi_TextureInfoDynamicEXT dynamic{};
    dynamic.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_DYNAMIC_EXT;
    dynamic.dynamic = 1;
    remixapi_TextureInfoGenerateMipsEXT mips{};
    mips.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_GENERATE_MIPS_EXT;
    mips.generate = 1;
    remixapi_TextureInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    info.pNext = &dynamic;
    info.hash = 0x51;
    info.format = REMIXAPI_FORMAT_B8G8R8A8_SRGB;
    info.width = info.height = 2;
    info.depth = info.mipLevels = 1;
    info.data = red;
    info.dataSize = sizeof(red);
    remixapi_TextureHandle handle{}, plain{};
    // Generated mips, made again from each update (animated maps).
    dynamic.pNext = &mips;
    info.hash = 0x55;
    remixapi_TextureHandle mipped{};
    r.createTexture(info, mipped);
    {
      remixapi_MaterialInfoOpaqueEXT o{};
      o.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
      o.albedoConstant = {1, 1, 1};
      o.opacityConstant = 1;
      o.alphaTestType = 7;
      remixapi_MaterialInfo m{};
      m.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
      m.pNext = &o;
      m.hash = 0x56;
#ifdef _WIN32
      m.albedoTexture = L"0x55";
#else
      m.albedoTexture = "0x55";
#endif
      remixapi_MaterialHandle mh{};
      r.createMaterial(m, mh);
      const auto t = drawn(r, quad(r, 0x57, mh))->texture;
      require(t->dynamic && t->pixels.mips.size() == 2 && t->pixels.mips[1][0] == 255, "Dynamic texture with mips");
      const uint8_t green[16] = {0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255};
      r.updateTexture(mipped, 0, 0, 2, 2, REMIXAPI_FORMAT_R8G8B8A8_UNORM, green, 8);
      std::lock_guard lock(t->updates->lock);
      require(t->pixels.mips.size() == 2 && t->pixels.mips[1][0] == 0 && t->pixels.mips[1][1] == 255 &&
                  t->updates->dirty[0] == 0 && t->updates->dirty[2] == 2 && t->updates->dirty[3] == 2,
              "Mips made again from an update");
    }
    info.hash = 0x51;
    // One uncompressed level only.
    dynamic.pNext = nullptr;
    info.format = REMIXAPI_FORMAT_BC1_RGB_SRGB;
    rejected([&] { r.createTexture(info, handle); });
    info.format = REMIXAPI_FORMAT_B8G8R8A8_SRGB;
    r.createTexture(info, handle);
    info.pNext = nullptr;
    info.hash = 0x52;
    r.createTexture(info, plain);

    // An unlit material of it (self-illuminated, emitting), alpha-tested.
    remixapi_MaterialInfoSelfIlluminationEXT self{};
    self.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_SELF_ILLUMINATION_EXT;
    self.maskScale = 1;
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.pNext = &self;
    opaque.albedoConstant = {1, 1, 1};
    opaque.opacityConstant = 1;
    opaque.alphaTestType = 4; // greater
    opaque.alphaReferenceValue = 128;
    remixapi_MaterialInfo materialInfo{};
    materialInfo.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    materialInfo.pNext = &opaque;
    materialInfo.hash = 0x53;
#ifdef _WIN32
    materialInfo.albedoTexture = L"0x51";
#else
    materialInfo.albedoTexture = "0x51";
#endif
    materialInfo.emissiveIntensity = 1;
    materialInfo.emissiveColorConstant = {1, 1, 1};
    remixapi_MaterialHandle material{};
    r.createMaterial(materialInfo, material);
    const auto mesh = quad(r, 0x54, material);
    auto info2 = instance(mesh);
    info2.doubleSided = 1;
    r.drawInstance(info2);
    const scene::Draw draw = r.draws().back();
    const auto texture = draw.material->texture;
    require(texture->dynamic && texture->updates && texture->pixels.mips.size() == 1 &&
                texture->pixels.mips[0][0] == 255 && texture->pixels.mips[0][2] == 0,
            "Dynamic texture creation");
    const uint64_t fingerprint = texture->fingerprint(), signature = scene::TemporalHistory::materialSignature(draw);

    // Estimates of the first image.
    scene::SurfaceEmissionCache emission;
    const std::array<float, 4> uv{1, 1, 0, 0};
    const auto first = emission.reduce(draw, uv);
    require(first->size() == 2 && (*first)[0].radiance[0] > .9f && (*first)[0].radiance[1] == 0,
            "Dynamic texture emission of the first image");
    scene::OpacityCache opacity;
    const auto partition = opacity.partition(draw, uv);
    require(partition->reasons[scene::OpacityPartition::Solid] == 0 &&
                partition->reasons[scene::OpacityPartition::TextureCutout] == 2,
            "A dynamic texture's opaque texels were proven opaque");

    // The same pixels: no update. Other handles, regions and formats: rejected.
    r.updateTexture(handle, 0, 0, 2, 2, REMIXAPI_FORMAT_B8G8R8A8_SRGB, red, 8);
    require(texture->updates->revision == 0, "An unchanged image counted as an update");
    rejected([&] { r.updateTexture(plain, 0, 0, 2, 2, REMIXAPI_FORMAT_B8G8R8A8_SRGB, red, 8); });
    rejected([&] { r.updateTexture(handle, 1, 0, 2, 2, REMIXAPI_FORMAT_B8G8R8A8_SRGB, red, 8); });
    rejected([&] { r.updateTexture(handle, 0, 0, 2, 2, REMIXAPI_FORMAT_BC1_RGB_SRGB, red, 8); });
    rejected([&] { r.updateTexture(handle, 0, 0, 2, 2, REMIXAPI_FORMAT_B8G8R8A8_SRGB, red, 4); });

    // The bottom row turns green (R8G8B8A8 this time, with a row pitch).
    const uint8_t rows[2][12] = {{255, 0, 0, 255, 255, 0, 0, 255, 9, 9, 9, 9}, {0, 255, 0, 255, 0, 255, 0, 255, 9, 9, 9, 9}};
    r.updateTexture(handle, 0, 0, 2, 2, REMIXAPI_FORMAT_R8G8B8A8_UNORM, rows, 12);
    {
      std::lock_guard lock(texture->updates->lock);
      const auto &u = *texture->updates;
      require(u.revision == 1 && u.dirty[0] == 0 && u.dirty[1] == 1 && u.dirty[2] == 2 && u.dirty[3] == 2,
              "Changed region of an update");
      const uint8_t expected[16] = {255, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255};
      require(std::memcmp(texture->pixels.mips[0].data(), expected, 16) == 0, "Updated pixels");
    }
    // Nothing else changed: the same texture and material objects, the same
    // fingerprint and material signature.
    r.finishFrame();
    r.drawInstance(info2);
    require(r.draws().back().material == draw.material && r.draws().back().material->texture == texture &&
                texture->fingerprint() == fingerprint &&
                scene::TemporalHistory::materialSignature(r.draws().back()) == signature && texture->pixels.srgb,
            "An update changed the texture's identity");
    // The emissive estimate is the first image's until it is old enough, for
    // this surface and for a new one of the same material.
    require(emission.reduce(draw, uv) == first, "Emission was estimated again at once");
    scene::Draw other = draw;
    other.surface = std::make_shared<scene::Surface>(*draw.surface);
    const auto fresh = emission.reduce(other, uv);
    require(fresh->size() == 2 && (*fresh)[0].radiance == (*first)[0].radiance && (*fresh)[1].radiance == (*first)[1].radiance,
            "A new surface did not keep the material's estimate");
    std::this_thread::sleep_for(std::chrono::milliseconds(int(REMIXAPI_DYNAMIC_TEXTURE_EMISSION_SECONDS_EXT * 1000) + 50));
    const auto second = emission.reduce(draw, uv);
    float sum[3] = {};
    for (const auto &triangle : *second)
      for (int c = 0; c < 3; ++c)
        sum[c] += triangle.radiance[c];
    require(second != first && sum[0] > .5f && sum[1] > .5f && sum[2] == 0, "Emission did not follow the new image");
    require(emission.reduce(draw, uv) == second, "Emission of an unchanged image was estimated again");
    // A direct estimate reads the current pixels.
    r.updateTexture(handle, 0, 0, 2, 2, REMIXAPI_FORMAT_B8G8R8A8_SRGB, red, 8);
    const float transform[4] = {1, 1, 0, 0};
    const auto direct = scene::triangleEmission(draw, 0, transform);
    require(direct[0] > .9f && direct[1] == 0, "Direct emission estimate of a dynamic texture");
    // CreateTexture under the hash is a new texture; the material keeps its own.
    info.pNext = &dynamic;
    info.hash = 0x51;
    r.createTexture(info, handle);
    r.finishFrame();
    r.drawInstance(info2);
    require(r.draws().back().material->texture == texture, "CreateTexture replaced a material's dynamic texture");
  }

  // remixapi_MaterialPbrRemapEXT and the SSBump normal encoding.
  void pbrRemap() {
    remix::NativeResources r;
    const uint8_t texel[4] = {0, 0, 255, 128};
    remixapi_TextureInfo texture{};
    texture.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    texture.hash = 0x61;
    texture.format = REMIXAPI_FORMAT_B8G8R8A8_SRGB;
    texture.width = texture.height = texture.depth = texture.mipLevels = 1;
    texture.data = texel;
    texture.dataSize = sizeof(texel);
    remixapi_TextureHandle textureHandle;
    r.createTexture(texture, textureHandle);
    remixapi_MaterialNormalEncodingEXT encoding{};
    encoding.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_NORMAL_ENCODING_EXT;
    encoding.encoding = 4;
    remixapi_MaterialPbrRemapEXT remap{};
    remap.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_REMAP_EXT;
    remap.pNext = &encoding;
    remap.roughness = {3, 1, 1, 2, 1, 149};
    remap.metallic = {0, 0, 0, 1, 0, 1};
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.pNext = &remap;
    opaque.albedoConstant = {1, 1, 1};
    opaque.opacityConstant = 1;
    opaque.alphaTestType = 7;
#ifdef _WIN32
    opaque.roughnessTexture = L"0x61";
#else
    opaque.roughnessTexture = "0x61";
#endif
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = 0x60;
    remixapi_MaterialHandle handle;
    r.createMaterial(info, handle);
    const auto made = drawn(r, quad(r, 0x62, handle));
    const auto &rough = made->remap[0];
    require(made->normalEncoding == 4 && rough.channel == 3 && rough.invert && rough.curve == 1 &&
                rough.exponent == 2 && rough.bias == 1 && rough.scale == 149 && made->remap[1].identity() &&
                made->pbr,
            "PBR remap fields");
    remap.roughness.channel = 4;
    rejected([&] { r.createMaterial(info, handle); });
    remap.roughness.channel = 0;
    remap.roughness.exponent = 16;
    rejected([&] { r.createMaterial(info, handle); });
    remap.roughness.exponent = 1;
    encoding.encoding = 6;
    rejected([&] { r.createMaterial(info, handle); });

    // Water (encoding 5): scroll rates; no roughness or metallic map beside.
    remixapi_MaterialNormalScrollEXT scroll{};
    scroll.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_NORMAL_SCROLL_EXT;
    scroll.pNext = &encoding;
    scroll.layers = 3;
    scroll.base[0] = .05f;
    scroll.layer2[1] = 2.6e-4f;
    opaque.pNext = &scroll;
    encoding.encoding = 5;
    rejected([&] { r.createMaterial(info, handle); }); // a roughness map
    opaque.roughnessTexture = nullptr;
#ifdef _WIN32
    info.normalTexture = L"0x61";
#else
    info.normalTexture = "0x61";
#endif
    info.hash = 0x63;
    r.createMaterial(info, handle);
    const auto water = drawn(r, quad(r, 0x64, handle));
    require(water->normalEncoding == 5 && water->scrollLayers == 3 && water->scroll[0] == .05f &&
                water->scroll[5] == 2.6e-4f,
            "Water scroll fields");
    scroll.layers = 2;
    rejected([&] { r.createMaterial(info, handle); });
    scroll.layers = 1;
    encoding.encoding = 2;
    rejected([&] { r.createMaterial(info, handle); }); // scroll without encoding 5

    // A translucent body (no thin wall, a measurement distance): PBR, its
    // refractive index and distance kept; a thin wall stays a tinted sheet.
    remixapi_MaterialInfoTranslucentEXT body{};
    body.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
    body.refractiveIndex = 1.333f;
    body.transmittanceColor = {.1f, .4f, .5f};
    body.transmittanceMeasurementDistance = 300;
    remixapi_MaterialInfo bodyInfo{};
    bodyInfo.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    bodyInfo.pNext = &body;
    bodyInfo.hash = 0x65;
    r.createMaterial(bodyInfo, handle);
    const auto water2 = drawn(r, quad(r, 0x66, handle));
    require(water2->transmission && water2->translucent && water2->pbr && water2->refractiveIndex == 1.333f &&
                water2->transmittanceDistance == 300 && water2->tint[1] == .4f,
            "Translucent body fields");
    body.thinWallThickness_hasvalue = 1;
    bodyInfo.hash = 0x67;
    r.createMaterial(bodyInfo, handle);
    const auto sheet = drawn(r, quad(r, 0x68, handle));
    require(sheet->transmission && !sheet->pbr && sheet->transmittanceDistance == 0, "Thin translucent sheet");

    // Caustics: translucent materials with a normal map, water and glass alike.
    require(!water2->caustics && !sheet->caustics, "Caustics without a normal map");
#ifdef _WIN32
    bodyInfo.normalTexture = L"0x61";
#else
    bodyInfo.normalTexture = "0x61";
#endif
    body.thinWallThickness_hasvalue = 0;
    bodyInfo.hash = 0x69;
    r.createMaterial(bodyInfo, handle);
    require(drawn(r, quad(r, 0x6a, handle))->caustics, "Water with a normal map has caustics");
    body.thinWallThickness_hasvalue = 1;
    bodyInfo.hash = 0x6b;
    r.createMaterial(bodyInfo, handle);
    require(drawn(r, quad(r, 0x6c, handle))->caustics, "Glass with a normal map has caustics");
    remixapi_MaterialInfoOpaqueEXT plain{};
    plain.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    plain.opacityConstant = 1;
    remixapi_MaterialInfo plainInfo{};
    plainInfo.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    plainInfo.pNext = &plain;
    plainInfo.normalTexture = bodyInfo.normalTexture;
    plainInfo.hash = 0x6d;
    r.createMaterial(plainInfo, handle);
    require(!drawn(r, quad(r, 0x6e, handle))->caustics, "Caustics on an opaque material");
  }

  // A thin film: the opaque info's legacy thickness fields, then
  // remixapi_MaterialIridescenceEXT, validated and part of the material's identity.
  void iridescence() {
    remix::NativeResources r;
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {1, 1, 1};
    opaque.opacityConstant = 1;
    opaque.alphaTestType = 7;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = 0x70;
    remixapi_MaterialHandle handle;
    r.createMaterial(info, handle);
    require(drawn(r, quad(r, 0x71, handle))->iridescence.strength == 0, "No film by default");
    opaque.thinFilmThickness_hasvalue = 1;
    opaque.thinFilmThickness_value = 350;
    r.createMaterial(info, handle);
    auto legacy = drawn(r, quad(r, 0x72, handle));
    require(legacy->iridescence.strength == 1 && legacy->iridescence.thickness == 350 &&
                legacy->iridescence.ior == 1.5f && legacy->pbr,
            "Legacy thin film");
    opaque.alphaIsThinFilmThickness = 1;
    opaque.opacityConstant = .5f;
    r.createMaterial(info, handle);
    auto alpha = drawn(r, quad(r, 0x73, handle));
    require(alpha->iridescence.fromAlpha && alpha->tint[3] == 1 && !alpha->translucent && alpha->compare == 7,
            "Thickness from albedo alpha makes the material opaque");
    opaque.alphaIsThinFilmThickness = 0;
    opaque.opacityConstant = 1;

    remixapi_MaterialIridescenceEXT film{};
    film.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_IRIDESCENCE_EXT;
    film.pNext = &opaque;
    film.strength = .75f;
    film.filmRefractiveIndex = 2.4f;
    film.thickness = 600;
    film.thicknessMin = 200;
    film.thicknessMax = 1200;
    film.flowAmplitude = 300;
    film.flowScale = 2;
    film.flowSpeed = -.5f;
    film.structural = .5f;
    info.pNext = &film;
    r.createMaterial(info, handle);
    const auto made = drawn(r, quad(r, 0x74, handle));
    const auto &f = made->iridescence;
    require(f.strength == .75f && f.ior == 2.4f && f.thickness == 600 && f.min == 200 && f.max == 1200 &&
                f.flowAmplitude == 300 && f.flowScale == 2 && f.flowSpeed == -.5f && f.structural == .5f &&
                !f.thicknessTexture,
            "Iridescence fields");
    film.structural = .25f;
    r.createMaterial(info, handle);
    require(drawn(r, quad(r, 0x75, handle))->iridescence.structural == .25f, "A changed film replaces the material");
    auto reject = [&](float &field, float bad) {
      const float good = field;
      field = bad;
      rejected([&] { r.createMaterial(info, handle); });
      field = good;
    };
    reject(film.strength, 1.5f);
    reject(film.filmRefractiveIndex, .9f);
    reject(film.thickness, 2500);
    reject(film.thicknessMin, 1300); // above the maximum
    reject(film.flowScale, 0);
    reject(film.structural, -1);
    reject(film.flowSpeed, std::numeric_limits<float>::quiet_NaN());
    // The film alone makes a plain material PBR: a film is a reflection.
    const float roughness = opaque.roughnessConstant, metallic = opaque.metallicConstant;
    opaque.roughnessConstant = opaque.metallicConstant = 0;
    r.createMaterial(info, handle);
    require(drawn(r, quad(r, 0x76, handle))->pbr, "A film alone selects PBR shading");
    opaque.roughnessConstant = roughness;
    opaque.metallicConstant = metallic;
    // Glass and water take a film too: a smooth dielectric under it.
    remixapi_MaterialInfoTranslucentEXT glass{};
    glass.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
    glass.refractiveIndex = 1.5f;
    glass.transmittanceColor = {1, 1, 1};
    glass.thinWallThickness_hasvalue = 1;
    film.pNext = &glass;
    r.createMaterial(info, handle);
    const auto filmedGlass = drawn(r, quad(r, 0x77, handle));
    require(filmedGlass->transmission && filmedGlass->pbr && filmedGlass->iridescence.strength == .75f &&
                filmedGlass->roughness == .04f && filmedGlass->metallic == 0,
            "A film on a translucent material");
    // Portals take none.
    remixapi_MaterialInfoPortalEXT portal{};
    portal.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_PORTAL_EXT;
    film.pNext = &portal;
    rejected([&] { r.createMaterial(info, handle); });
  }
  // A mod's partial override (an `over` of a captured material: XashRT's PBR
  // materials given a film in Remix Studio) changes only the parts it
  // authors; the game's values stay for the rest.
  void partialMod() {
    remix::NativeResources r;
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {1, 1, 1};
    opaque.opacityConstant = 1;
    opaque.alphaTestType = 7;
    opaque.roughnessConstant = 0;
    opaque.metallicConstant = 1;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = 0x90;
    remixapi_MaterialHandle game, glass, mod;
    r.createMaterial(info, game);
    // The mod's material: its film, and defaults for what it does not author.
    remixapi_MaterialInfoOpaqueEXT plain = opaque;
    plain.roughnessConstant = .5f;
    plain.metallicConstant = 0;
    remixapi_MaterialIridescenceEXT film{};
    film.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_IRIDESCENCE_EXT;
    film.pNext = &plain;
    film.strength = 1;
    film.filmRefractiveIndex = 2.4f;
    film.thickness = 450;
    film.thicknessMin = 100;
    film.thicknessMax = 800;
    film.flowScale = 1;
    remixapi_MaterialInfo modInfo = info;
    modInfo.pNext = &film;
    modInfo.hash = 0x91;
    r.createMaterial(modInfo, mod);
    r.addModMaterial(0x90, mod, scene::ModParts::film);
    auto merged = drawn(r, quad(r, 0x92, game));
    require(merged->roughness == 0 && merged->metallic == 1 && merged->pbr && merged->iridescence.strength == 1 &&
                merged->iridescence.thickness == 450,
            "A partial override keeps the game's PBR values");
    // A full replacement takes the mod's own.
    r.addModMaterial(0x90, mod);
    r.finishFrame();
    auto replaced = drawn(r, quad(r, 0x93, game));
    require(replaced->roughness == .5f && replaced->metallic == 0 && replaced->iridescence.strength == 1,
            "A full replacement takes the mod's values");
    // The game's glass stays glass under a film-only override.
    remixapi_MaterialInfoTranslucentEXT body{};
    body.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
    body.refractiveIndex = 1.33f;
    body.transmittanceColor = {.8f, .9f, 1};
    body.transmittanceMeasurementDistance = 2;
    remixapi_MaterialInfo glassInfo = info;
    glassInfo.pNext = &body;
    glassInfo.hash = 0x94;
    r.createMaterial(glassInfo, glass);
    r.addModMaterial(0x94, mod, scene::ModParts::film);
    r.finishFrame();
    auto water = drawn(r, quad(r, 0x95, glass));
    require(water->transmission && water->transmittanceDistance == 2 && water->refractiveIndex == 1.33f &&
                water->tint[2] == 1 && water->iridescence.strength == 1 && water->roughness == .04f,
            "A partial override keeps the game's glass");
  }

  remixapi_Path name(const char *text) {
#ifdef _WIN32
    static std::wstring names[8];
    static int next = 0;
    auto &out = names[next++ % 8];
    out.assign(text, text + std::strlen(text));
    return out.c_str();
#else
    return text;
#endif
  }
  void rgbaTexture(remix::NativeResources &r, uint64_t hash, remixapi_Format format) {
    const uint8_t texels[16] = {200, 100, 50, 255, 10, 20, 30, 40, 1, 2, 3, 4, 5, 6, 7, 8};
    remixapi_TextureInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    info.hash = hash;
    info.format = format;
    info.width = info.height = 2;
    info.depth = info.mipLevels = 1;
    info.data = texels;
    info.dataSize = sizeof(texels);
    remixapi_TextureHandle handle{};
    r.createTexture(info, handle);
  }
  // remixapi_MaterialGradientMapEXT: validated, the albedo read as stored,
  // the material opaque.
  void gradientMap() {
    remix::NativeResources r;
    rgbaTexture(r, 0x91, REMIXAPI_FORMAT_R8G8B8A8_SRGB);
    rgbaTexture(r, 0x92, REMIXAPI_FORMAT_R8G8B8A8_SRGB);
    rgbaTexture(r, 0x93, REMIXAPI_FORMAT_R8G8B8A8_SRGB);
    remixapi_MaterialGradientMapEXT g{};
    g.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_GRADIENT_MAP_EXT;
    g.gradientTexture = name("0x92");
    g.detailTexture = name("0x93");
    g.skinRow = 3;
    g.clothRow = 17;
    g.spriteOffset[0] = .5f;
    g.detailU[0] = 4;
    g.detailV[1] = 4;
    g.bloodColor = {.15f, 0, 0};
    g.bloodRange[0] = .1f;
    g.bloodRange[1] = .9f;
    remixapi_MaterialInfoOpaqueEXT o{};
    o.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    o.pNext = &g;
    o.albedoConstant = {1, 1, 1};
    o.opacityConstant = 1;
    o.alphaTestType = 4;
    o.alphaReferenceValue = 128;
    remixapi_MaterialInfo m{};
    m.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    m.pNext = &o;
    m.hash = 0x94;
    m.albedoTexture = name("0x91");
    remixapi_MaterialHandle handle{};
    r.createMaterial(m, handle);
    const auto material = drawn(r, quad(r, 0x95, handle));
    require(material->gradient && material->gradient->gradient && material->gradient->detail,
            "Gradient map: parsed");
    const auto &map = *material->gradient;
    require(!material->texture->pixels.srgb && material->texture->hash == 0x91,
            "Gradient map: the albedo is not read as stored");
    require(map.gradient->pixels.srgb && !map.detail->pixels.srgb, "Gradient map: texture encodings");
    require(material->compare == 7 && !material->translucent && !material->drawAlpha,
            "Gradient map: the material is not opaque");
    require(map.gradientSampler->texture == map.gradient && map.gradientSampler->filter == 1 &&
                map.gradientSampler->wrapU == 0 && map.gradientSampler->wrapV == 0,
            "Gradient map: gradient sampler");
    require(map.detailSampler->texture == map.detail && map.detailSampler->wrapU == 1,
            "Gradient map: detail sampler");
    require(map.skinRow == 3 && map.clothRow == 17 && map.spriteOffset[0] == .5f && map.detailU[0] == 4 &&
                map.detailV[1] == 4 && map.bloodColor[0] == .15f && map.bloodRange[1] == .9f,
            "Gradient map: parameters");
    require((scene::materialDecode(*material).flags & scene::MaterialDecode::gradientMap) != 0,
            "Gradient map: decode flag");
    // The same description keeps the material; a new parameter replaces it.
    r.createMaterial(m, handle);
    require(drawn(r, quad(r, 0x95, handle)) == material, "Gradient map: an equal material was replaced");
    g.bloodRange[0] = .2f;
    r.createMaterial(m, handle);
    require(drawn(r, quad(r, 0x95, handle)) != material, "Gradient map: a change kept the material");
    // Without a detail texture: none.
    g.detailTexture = nullptr;
    r.createMaterial(m, handle);
    require(!drawn(r, quad(r, 0x95, handle))->gradient->detail, "Gradient map: empty detail");
    // Invalid descriptions.
    auto invalid = [&](auto &&change) {
      auto saved = g;
      auto savedOpaque = o;
      change();
      rejected([&] { r.createMaterial(m, handle); });
      g = saved;
      o = savedOpaque;
      o.pNext = &g;
    };
    invalid([&] { g.gradientTexture = nullptr; });
    invalid([&] { g.gradientTexture = name("0x99"); });
    invalid([&] { g.bloodRange[0] = g.bloodRange[1]; });
    invalid([&] { g.bloodColor.y = 1.5f; });
    invalid([&] { g.skinRow = std::numeric_limits<float>::quiet_NaN(); });
    invalid([&] { g.detailU[1] = 1e6f; });
    invalid([&] { o.opacityConstant = .5f; });
    invalid([&] { o.thinFilmThickness_hasvalue = 1; });
    remixapi_MaterialInfoTranslucentEXT t{};
    t.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
    t.pNext = &g;
    m.pNext = &t;
    rejected([&] { r.createMaterial(m, handle); });
    m.pNext = &o;
    r.createMaterial(m, handle);
  }
  // remixapi_InstanceInfoWoundCutEXT: validated, carried by every draw of
  // the instance with its resolved texture, and never static.
  void woundCut() {
    remix::NativeResources r;
    rgbaTexture(r, 0xa1, REMIXAPI_FORMAT_R8G8B8A8_SRGB);
    auto mesh = quad(r, 0xa2, material(r, 0xa3, 1, 1, 1));
    remixapi_InstanceInfoWoundCutEXT w{};
    w.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_WOUND_CUT_EXT;
    w.cutTexture = name("0xa1");
    for (auto &e : w.ellipsoids) {
      e.center = {1e6f, 1e6f, 1e6f};
      e.axisX = {.1f, 0, 0};
      e.axisY = {0, .1f, 0};
      e.axisZ = {0, 0, .1f};
    }
    w.ellipsoids[1].center = {0, 0, 1};
    w.mappingScale = 7;
    w.secondBlend = .5f;
    for (int i = 0; i < 4; ++i) {
      w.projectionScale[i] = .5f;
      w.projectionBias[i] = .5f;
      w.cutScaleBias[0][i] = w.cutScaleBias[1][i] = i < 2 ? .5f : 0.f;
    }
    w.cutScaleBias[1][2] = .5f;
    w.threshold = .95f;
    remixapi_InstanceIdentityEXT identity{};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.pNext = &w;
    identity.instanceId = 7;
    identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
    identity.rasterVisible = 1;
    auto info = instance(mesh);
    info.pNext = &identity;
    r.drawInstance(info);
    require(r.draws().size() == 1 && r.draws()[0].wound, "Wound cut: not carried by the draw");
    const auto &draw = r.draws()[0];
    const auto &wound = *draw.wound;
    require(draw.classification == REMIXAPI_INSTANCE_DYNAMIC_EXT, "Wound cut: a wounded instance stayed static");
    require(wound.texture && wound.texture->hash == 0xa1 && !wound.texture->pixels.srgb,
            "Wound cut: the cut texture is not read as stored");
    require(wound.sampler && wound.sampler->texture == wound.texture && wound.sampler->filter == 1 &&
                wound.sampler->wrapU == 3 && wound.sampler->wrapV == 3,
            "Wound cut: sampler");
    require(wound.ellipsoids[1][0][2] == 1 && wound.ellipsoids[0][1][0] == .1f && wound.mappingScale == 7 &&
                wound.secondBlend == .5f && wound.threshold == .95f && wound.cutScaleBias[1][2] == .5f &&
                wound.projectionBias[3] == .5f,
            "Wound cut: parameters");
    // Every frame finds the same sampler material (backends keep descriptors by it).
    const auto sampler = wound.sampler;
    r.finishFrame();
    r.drawInstance(info);
    require(r.draws()[0].wound->sampler == sampler, "Wound cut: a new sampler material every frame");
    // Retained: the same, and it does not settle into the static set.
    const uint64_t retained = r.createRetained(info);
    r.finishFrame();
    r.drawRetained(nullptr);
    require(r.draws().size() == 1 && r.draws()[0].wound && r.draws()[0].wound->sampler == sampler &&
                r.draws()[0].classification == REMIXAPI_INSTANCE_DYNAMIC_EXT,
            "Wound cut: retained instance");
    r.destroyRetained(&retained, 1);
    // Invalid descriptions.
    auto invalid = [&](auto &&change) {
      auto saved = w;
      change();
      r.finishFrame();
      rejected([&] { r.drawInstance(info); });
      w = saved;
    };
    invalid([&] { w.mappingScale = 0; });
    invalid([&] { w.mappingScale = -1; });
    invalid([&] { w.cutTexture = nullptr; });
    invalid([&] { w.cutTexture = name(""); });
    invalid([&] { w.cutTexture = name("0xa9"); });
    invalid([&] { w.threshold = std::numeric_limits<float>::infinity(); });
    invalid([&] { w.ellipsoids[1].axisY.z = std::numeric_limits<float>::quiet_NaN(); });
    invalid([&] { w.cutScaleBias[1][3] = std::numeric_limits<float>::quiet_NaN(); });
    // Not a scene instance: a wireframe overlay ignores it.
    remixapi_InstanceInfoWireframeEXT wire{};
    wire.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_WIREFRAME_EXT;
    wire.pNext = &w;
    info.pNext = &wire;
    r.finishFrame();
    r.drawInstance(info);
    require(r.draws().size() == 1 && !r.draws()[0].wound, "Wound cut: a wireframe overlay was cut");
  }

  // Subsurface scattering (remixapi_MaterialSubsurfaceEXT), and RTX Remix's
  // diffusion profile read into it.
  void subsurface() {
    remix::NativeResources r;
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {1, 1, 1};
    opaque.opacityConstant = 1;
    opaque.alphaTestType = 7;
    remixapi_MaterialSubsurfaceEXT sss{};
    sss.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_SUBSURFACE_EXT;
    sss.pNext = &opaque;
    sss.weight = .8f;
    sss.radius = {1, .2f, .1f};
    sss.scale = .05f;
    sss.anisotropy = .5f;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &sss;
    info.hash = 0xa0;
    remixapi_MaterialHandle handle;
    r.createMaterial(info, handle);
    auto made = drawn(r, quad(r, 0xa1, handle));
    require(made->subsurface.weight == .8f && made->subsurface.radius[1] == .2f && made->subsurface.scale == .05f &&
                made->subsurface.anisotropy == .5f && made->pbr,
            "Subsurface fields, PBR shading");
    sss.weight = .4f;
    r.createMaterial(info, handle);
    require(drawn(r, quad(r, 0xa2, handle))->subsurface.weight == .4f, "A changed subsurface replaces the material");
    auto reject = [&](float &field, float bad) {
      const float good = field;
      field = bad;
      rejected([&] { r.createMaterial(info, handle); });
      field = good;
    };
    reject(sss.weight, 1.5f);
    reject(sss.radius.x, -1);
    reject(sss.scale, 0);
    reject(sss.anisotropy, .95f);
    reject(sss.weight, std::numeric_limits<float>::quiet_NaN());
    // RTX Remix's diffusion profile: weight 1, its scale in millimetres.
    remixapi_MaterialInfoOpaqueSubsurfaceEXT remix{};
    remix.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_SUBSURFACE_EXT;
    remix.pNext = &opaque;
    remix.subsurfaceDiffusionProfile = 1;
    remix.subsurfaceRadius = {.5f, .25f, .125f};
    remix.subsurfaceRadiusScale = 20;
    info.pNext = &remix;
    info.hash = 0xa3;
    r.createMaterial(info, handle);
    made = drawn(r, quad(r, 0xa4, handle));
    require(made->subsurface.weight == 1 && made->subsurface.radius[0] == .5f &&
                std::abs(made->subsurface.scale - .02f) < 1e-7f,
            "RTX Remix's diffusion profile");
    remix.subsurfaceDiffusionProfile = 0;
    r.createMaterial(info, handle);
    require(drawn(r, quad(r, 0xa5, handle))->subsurface.weight == 0, "No diffusion profile, no subsurface");
  }
} // namespace

  // A dynamic texture with generated mips: an update makes each level again
  // only where it reads the changed texels, and the region stays level 0's.
  void scopedMipUpdate() {
    remix::NativeResources r;
    remixapi_TextureInfoGenerateMipsEXT mips{};
    mips.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_GENERATE_MIPS_EXT;
    mips.generate = 1;
    remixapi_TextureInfoDynamicEXT dynamic{};
    dynamic.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_DYNAMIC_EXT;
    dynamic.pNext = &mips;
    dynamic.dynamic = 1;
    std::vector<uint8_t> black(4 * 4 * 4, 0);
    for (size_t i = 3; i < black.size(); i += 4)
      black[i] = 255;
    remixapi_TextureInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    info.pNext = &dynamic;
    info.hash = 0x81;
    info.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
    info.width = info.height = 4;
    info.depth = info.mipLevels = 1;
    info.data = black.data();
    info.dataSize = black.size();
    remixapi_TextureHandle handle{};
    r.createTexture(info, handle);
    remixapi_MaterialInfoOpaqueEXT o{};
    o.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    o.albedoConstant = {1, 1, 1};
    o.opacityConstant = 1;
    o.alphaTestType = 7;
    remixapi_MaterialInfo m{};
    m.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    m.pNext = &o;
    m.hash = 0x82;
#ifdef _WIN32
    m.albedoTexture = L"0x81";
#else
    m.albedoTexture = "0x81";
#endif
    remixapi_MaterialHandle mh{};
    r.createMaterial(m, mh);
    const auto t = drawn(r, quad(r, 0x83, mh))->texture;
    require(t->pixels.mips.size() == 3, "Scoped mips: chain");
    const uint8_t white[4] = {255, 255, 255, 255};
    r.updateTexture(handle, 0, 0, 1, 1, REMIXAPI_FORMAT_R8G8B8A8_UNORM, white, 4);
    std::lock_guard lock(t->updates->lock);
    const auto &p = t->pixels;
    const auto &u = *t->updates;
    require(u.dirty[0] == 0 && u.dirty[1] == 0 && u.dirty[2] == 1 && u.dirty[3] == 1, "Scoped mips: level 0 region");
    require(p.mips[1][0] == 64 && p.mips[1][4] == 0 && p.mips[1][12] == 0, "Scoped mips: level 1 texels");
    require(p.mips[2][0] == 16, "Scoped mips: level 2 texel");
  }

  // A float dynamic texture (an HDR sky): RGBA32F texels updated from halves.
  void floatDynamicTexture() {
    remix::NativeResources r;
    remixapi_TextureInfoDynamicEXT dynamic{};
    dynamic.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_DYNAMIC_EXT;
    dynamic.dynamic = 1;
    const float bright[8] = {4, 0.5f, 0, 1, 4, 0.5f, 0, 1};
    remixapi_TextureInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    info.pNext = &dynamic;
    info.hash = 0x71;
    info.format = REMIXAPI_FORMAT_R32G32B32A32_SFLOAT_EXT;
    info.width = 2;
    info.height = 1;
    info.depth = info.mipLevels = 1;
    info.data = bright;
    info.dataSize = sizeof(bright);
    remixapi_TextureHandle handle{};
    r.createTexture(info, handle);
    remixapi_MaterialInfoOpaqueEXT o{};
    o.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    o.albedoConstant = {1, 1, 1};
    o.opacityConstant = 1;
    o.alphaTestType = 7;
    remixapi_MaterialInfo m{};
    m.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    m.pNext = &o;
    m.hash = 0x72;
#ifdef _WIN32
    m.albedoTexture = L"0x71";
#else
    m.albedoTexture = "0x71";
#endif
    remixapi_MaterialHandle mh{};
    r.createMaterial(m, mh);
    const auto t = drawn(r, quad(r, 0x73, mh))->texture;
    require(t->dynamic && t->pixels.hdrMips.size() == 1 && t->pixels.hdrMips[0][0] == 4 &&
                t->pixels.mips[0][0] == 255 && t->pixels.mips[0][1] == 127,
            "Float dynamic texture");
    // The second texel turns to (0.25, 2, 0, 1) in halves.
    const uint16_t halves[4] = {0x3400, 0x4000, 0x0000, 0x3c00};
    r.updateTexture(handle, 1, 0, 1, 1, REMIXAPI_FORMAT_R16G16B16A16_SFLOAT_EXT, halves, 8);
    {
      std::lock_guard lock(t->updates->lock);
      const auto &u = *t->updates;
      require(u.revision == 1 && u.dirty[0] == 1 && u.dirty[2] == 2 && t->pixels.hdrMips[0][4] == .25f &&
                  t->pixels.hdrMips[0][5] == 2 && t->pixels.hdrMips[0][0] == 4 && t->pixels.mips[0][5] == 255,
              "Float update");
    }
    // An 8-bit update of a float texture, and a float one of an 8-bit texture: rejected.
    const uint8_t texel[4] = {1, 2, 3, 4};
    rejected([&] { r.updateTexture(handle, 0, 0, 1, 1, REMIXAPI_FORMAT_R8G8B8A8_UNORM, texel, 4); });
    info.hash = 0x74;
    info.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
    info.data = texel;
    info.dataSize = sizeof(texel);
    info.width = 1;
    remixapi_TextureHandle plain{};
    r.createTexture(info, plain);
    rejected([&] { r.updateTexture(plain, 0, 0, 1, 1, REMIXAPI_FORMAT_R16G16B16A16_SFLOAT_EXT, halves, 8); });
  }

int main() {
  materialOverride();
  dynamicTexture();
  floatDynamicTexture();
  scopedMipUpdate();
  pbrRemap();
  iridescence();
  partialMod();
  gradientMap();
  woundCut();
  subsurface();
  std::puts("Instance material and dynamic textures passed");
  return 0;
}
