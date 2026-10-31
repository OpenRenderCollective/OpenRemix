// USD capture writer: the Remix capture layout that loader.cpp and the RTX
// Remix Toolkit read. CPU-only OpenUSD 25.11.
#include "usd/capture_categories.h"
#include "usd/capture_scene.h"
#include "scene/images.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/relationshipSpec.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/tokens.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>

PXR_NAMESPACE_USING_DIRECTIVE
namespace remix::usd {
  namespace {
    std::string hex(uint64_t value) {
      char text[17];
      std::snprintf(text, sizeof text, "%016llX", static_cast<unsigned long long>(value));
      return text;
    }
    std::string utf8(const fs::path &path) {
      auto text = path.u8string();
      return {text.begin(), text.end()};
    }

    // A file appears complete or not at all: written beside its target with
    // the same extension (USD picks the format by it), then renamed.
    fs::path partial(const fs::path &target) {
      return target.parent_path() / (target.stem().string() + ".partial" + target.extension().string());
    }
    void commit(const fs::path &temporary, const fs::path &target) {
      std::error_code error;
      fs::rename(temporary, target, error);
      if (error) {
        fs::remove(temporary, error);
        throw std::runtime_error("Could not write " + utf8(target));
      }
    }
    void writeFile(const fs::path &target, const std::vector<uint8_t> &bytes) {
      const auto temporary = partial(target);
      {
        std::ofstream out(temporary, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
        if (!out)
          throw std::runtime_error("Could not write " + utf8(target));
      }
      commit(temporary, target);
    }
    void exportLayer(const SdfLayerHandle &layer, const fs::path &target) {
      const auto temporary = partial(target);
      if (!layer->Export(utf8(temporary)))
        throw std::runtime_error("Could not write " + utf8(target));
      commit(temporary, target);
    }

    // scene::Matrix (column vectors, column-major) and GfMatrix4d (row
    // vectors, row-major) store the same 16 numbers in the same order.
    GfMatrix4d matrix(const scene::Matrix &m) {
      GfMatrix4d result;
      for (int i = 0; i < 16; ++i)
        result.data()[i] = m[i];
      return result;
    }
    GfMatrix4d matrix(const remixapi_Transform &t) {
      GfMatrix4d result(1);
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
          result[c][r] = t.matrix[r][c];
      return result;
    }

    void stageMetrics(const UsdStageRefPtr &stage, const CaptureScene &scene) {
      UsdGeomSetStageMetersPerUnit(stage, scene.metersPerUnit);
      UsdGeomSetStageUpAxis(stage, scene.upAxis == 2 ? UsdGeomTokens->z : UsdGeomTokens->y);
    }

    // A mesh's surfaces are prims of their own, each bound to its material:
    // RTX Remix binds one material per mesh prim and ignores face subsets.
    std::string surfaceName(size_t surface) {
      return surface ? "surface_" + std::to_string(surface) : std::string("mesh");
    }

    // Whole repeats moved out of a surface's texture coordinates: a repeating
    // (or, by an even count, mirrored) map samples the same texels, and
    // coordinates far from 0 (hosts that bake tiling and offset into them)
    // lose precision in tools.
    float wrapShift(float sum, size_t count, uint8_t mode) {
      if ((mode != 1 && mode != 2) || !count)
        return 0;
      const float mean = sum / float(count);
      return mode == 2 ? 2 * std::floor(mean / 2) : std::floor(mean);
    }

    // meshes/mesh_<hash>.usd: the mesh in object space, a Mesh per surface.
    // Bindings live in the capture layer.
    void writeMesh(const CaptureScene &scene, uint64_t hash, const CaptureScene::Mesh &mesh, const fs::path &file) {
      auto stage = UsdStage::CreateInMemory(file.filename().string());
      stageMetrics(stage, scene);
      const auto name = "mesh_" + hex(hash);
      auto root = UsdGeomXform::Define(stage, SdfPath("/" + name));
      stage->SetDefaultPrim(root.GetPrim());
      for (size_t s = 0; s < mesh.surfaces.size(); ++s) {
        const auto &surface = mesh.surfaces[s];
        auto geometry = UsdGeomMesh::Define(stage, SdfPath("/" + name + "/" + surfaceName(s)));
        uint8_t wrapU = 1, wrapV = 1;
        if (const auto found = scene.materials.find(mesh.materials[s]); found != scene.materials.end())
          wrapU = found->second.wrapU, wrapV = found->second.wrapV;
        float sumU = 0, sumV = 0;
        for (const auto &v : surface.vertices)
          sumU += v.uv[0], sumV += v.uv[1];
        const float shiftU = wrapShift(sumU, surface.vertices.size(), wrapU),
                    shiftV = wrapShift(sumV, surface.vertices.size(), wrapV);
        VtVec3fArray points, normals, colors;
        VtVec2fArray st;
        VtFloatArray opacities;
        VtIntArray counts, indices;
        bool normalsValid = true, colored = false;
        for (const auto &v : surface.vertices) {
          points.push_back(GfVec3f(v.position[0], v.position[1], v.position[2]));
          normals.push_back(GfVec3f(v.normal[0], v.normal[1], v.normal[2]));
          normalsValid &= v.normal[0] != 0 || v.normal[1] != 0 || v.normal[2] != 0;
          // Captures store V flipped from runtime texture coordinates.
          st.push_back(GfVec2f(v.uv[0] - shiftU, 1.f - (v.uv[1] - shiftV)));
          colors.push_back(GfVec3f(v.color[0], v.color[1], v.color[2]));
          opacities.push_back(v.color[3]);
          colored |= v.color[0] != 1 || v.color[1] != 1 || v.color[2] != 1 || v.color[3] != 1;
        }
        for (size_t i = 0; i + 2 < surface.indices.size(); i += 3) {
          counts.push_back(3);
          // The Remix API's fronts are clockwise, USD's counter-clockwise.
          indices.push_back(int(surface.indices[i]));
          indices.push_back(int(surface.indices[i + 2]));
          indices.push_back(int(surface.indices[i + 1]));
        }
        geometry.CreateSubdivisionSchemeAttr(VtValue(UsdGeomTokens->none));
        geometry.CreatePointsAttr(VtValue(points));
        geometry.CreateFaceVertexCountsAttr(VtValue(counts));
        geometry.CreateFaceVertexIndicesAttr(VtValue(indices));
        if (normalsValid) {
          geometry.CreateNormalsAttr(VtValue(normals));
          geometry.SetNormalsInterpolation(UsdGeomTokens->vertex);
        }
        UsdGeomPrimvarsAPI(geometry)
            .CreatePrimvar(TfToken("st"), SdfValueTypeNames->TexCoord2fArray, UsdGeomTokens->vertex)
            .Set(st);
        if (colored) {
          geometry.CreateDisplayColorPrimvar(UsdGeomTokens->vertex).Set(colors);
          geometry.CreateDisplayOpacityPrimvar(UsdGeomTokens->vertex).Set(opacities);
        }
      }
      exportLayer(stage->GetRootLayer(), file);
    }

    // A normal map as RTX Remix samples it: unsigned octahedral in RG (the
    // runtime decodes every normal map that way; the Toolkit's ingest converts
    // to it). Named per source encoding, which decides the decode.
    std::string normalName(uint64_t texture, uint32_t encoding) {
      return hex(texture) + "_" + std::to_string(encoding) + "_OTH_Normal.dds";
    }
    // Level 0 as RGBA8: decoded BC blocks, else the preview, the 8-bit level
    // or the HDR level clamped.
    std::vector<uint8_t> rgba(const scene::Pixels &p, uint32_t &width, uint32_t &height) {
      width = p.width, height = p.height;
      if (p.compressed && !p.compressed->upload.empty())
        return scene::decodeCompressed(p).mips.at(0);
      if (p.compressed) {
        width = p.compressed->previewWidth, height = p.compressed->previewHeight;
        return p.compressed->preview;
      }
      if (!p.mips.empty())
        return p.mips[0];
      std::vector<uint8_t> out;
      if (!p.hdrMips.empty())
        for (float v : p.hdrMips[0])
          out.push_back(uint8_t(std::lround(std::clamp(v, 0.f, 1.f) * 255)));
      return out;
    }
    // Each texel decoded as the path tracer's materialLocalNormal does for the
    // encoding (0 auto, 1 octahedral, 2 tangent DX, 3 tangent GL, 4 $ssbump,
    // 5 water), then encoded as Remix's unsigned octahedral.
    std::vector<uint8_t> octahedral(const scene::Pixels &p, uint32_t encoding, uint32_t &width, uint32_t &height) {
      auto texels = rgba(p, width, height);
      if (texels.size() != size_t(width) * height * 4)
        return {};
      const bool twoChannel = p.twoChannel || (p.compressed && p.compressed->block == 5);
      for (size_t i = 0; i < texels.size(); i += 4) {
        float v[3] = {texels[i] / 255.f, texels[i + 1] / 255.f, texels[i + 2] / 255.f};
        if (encoding == 4) {
          // Source's $ssbump: weights of its three bump basis directions.
          const float w[3] = {v[0], v[1], v[2]};
          v[0] = .81649658f * w[0] - .40824829f * w[1] - .40824829f * w[2];
          v[1] = .70710678f * w[1] - .70710678f * w[2];
          v[2] = .57735027f * (w[0] + w[1] + w[2]);
        } else
          for (float &c : v)
            c = c * 2 - 1;
        if (twoChannel)
          v[2] = std::sqrt(std::max(0.f, 1 - v[0] * v[0] - v[1] * v[1]));
        if (encoding == 1) {
          const float x = (v[0] + v[1]) * .5f, y = (v[0] - v[1]) * .5f;
          v[0] = x, v[1] = y, v[2] = 1 - std::abs(x) - std::abs(y);
        }
        if (encoding == 3)
          v[1] = -v[1];
        v[2] = std::max(v[2], .001f);
        if (v[0] * v[0] + v[1] * v[1] + v[2] * v[2] < 1e-8f)
          v[0] = v[1] = 0, v[2] = 1;
        const float sum = std::abs(v[0]) + std::abs(v[1]) + v[2];
        const float x = v[0] / sum, y = v[1] / sum;
        texels[i] = uint8_t(std::lround(std::clamp((x + y) * .5f + .5f, 0.f, 1.f) * 255));
        texels[i + 1] = uint8_t(std::lround(std::clamp((x - y) * .5f + .5f, 0.f, 1.f) * 255));
        texels[i + 2] = 0;
        texels[i + 3] = 255;
      }
      return texels;
    }

    // materials/mat_<hash>.usda: an AperturePBR material as Remix captures
    // author it, with the inputs scene.cpp reads.
    void writeMaterial(uint64_t hash, const CaptureScene::Material &m, const fs::path &file,
                       const std::set<uint64_t> &textures, const std::set<std::string> &normals) {
      auto stage = UsdStage::CreateInMemory(file.filename().string());
      const auto name = "mat_" + hex(hash);
      auto material = UsdShadeMaterial::Define(stage, SdfPath("/" + name));
      stage->SetDefaultPrim(material.GetPrim());
      auto shader = UsdShadeShader::Define(stage, SdfPath("/" + name + "/Shader"));
      const std::string mdl = m.translucent ? "AperturePBR_Translucent" : "AperturePBR_Opacity";
      shader.SetSourceAsset(SdfAssetPath(mdl + ".mdl"), TfToken("mdl"));
      shader.SetSourceAssetSubIdentifier(TfToken(mdl), TfToken("mdl"));
      material.CreateSurfaceOutput(TfToken("mdl")).ConnectToSource(shader.CreateOutput(TfToken("out"), SdfValueTypeNames->Token));
      auto input = [&](const char *name, const SdfValueTypeName &type, const VtValue &value) {
        shader.CreateInput(TfToken(name), type).Set(value);
      };
      auto map = [&](const char *name, uint64_t texture) {
        if (!texture || !textures.count(texture))
          return;
        auto attribute = shader.CreateInput(TfToken(name), SdfValueTypeNames->Asset).GetAttr();
        attribute.Set(SdfAssetPath("../textures/" + hex(texture) + ".dds"));
        attribute.SetColorSpace(TfToken("auto"));
      };
      const GfVec3f tint(m.tint[0], m.tint[1], m.tint[2]);
      if (m.translucent) {
        input("transmittance_color", SdfValueTypeNames->Color3f, VtValue(tint));
        map("transmittance_texture", m.albedo);
        input("ior_constant", SdfValueTypeNames->Float, VtValue(m.refractiveIndex));
        // A body keeps its measurement distance; without one the material is
        // the tinted sheet of a thin wall (scene/materials.cpp).
        if (m.transmittanceDistance > 0)
          input("transmittance_measurement_distance", SdfValueTypeNames->Float, VtValue(m.transmittanceDistance));
        else
          input("thin_walled", SdfValueTypeNames->Bool, VtValue(true));
      } else {
        input("diffuse_color_constant", SdfValueTypeNames->Color3f, VtValue(tint));
        input("opacity_constant", SdfValueTypeNames->Float, VtValue(m.tint[3]));
        map("diffuse_texture", m.albedo);
        input("reflection_roughness_constant", SdfValueTypeNames->Float, VtValue(m.roughness));
        input("metallic_constant", SdfValueTypeNames->Float, VtValue(m.metallic));
        map("reflectionroughness_texture", m.roughnessMap);
        map("metallic_texture", m.metallicMap);
        // The film: AperturePBR's inputs, and OpenRemix's beyond them as
        // attributes outside the MDL inputs (usd/scene.cpp material_json).
        if (m.film.strength > 0) {
          input("enable_thin_film", SdfValueTypeNames->Bool, VtValue(true));
          input("thin_film_thickness_constant", SdfValueTypeNames->Float, VtValue(m.film.thickness));
          input("thin_film_thickness_from_albedo_alpha", SdfValueTypeNames->Bool, VtValue(m.film.fromAlpha));
          auto extra = [&](const char *name, float value) {
            shader.GetPrim()
                .CreateAttribute(TfToken(std::string("openremix:iridescence:") + name), SdfValueTypeNames->Float,
                                 true)
                .Set(value);
          };
          extra("strength", m.film.strength);
          extra("filmIor", m.film.ior);
          extra("thicknessMin", m.film.min);
          extra("thicknessMax", m.film.max);
          extra("flowAmplitude", m.film.flowAmplitude);
          extra("flowScale", m.film.flowScale);
          extra("flowSpeed", m.film.flowSpeed);
          extra("structural", m.film.structural);
          if (m.filmMap && textures.count(m.filmMap))
            shader.GetPrim()
                .CreateAttribute(TfToken("openremix:iridescence:thicknessTexture"), SdfValueTypeNames->Asset, true)
                .Set(SdfAssetPath("../textures/" + hex(m.filmMap) + ".dds"));
        }
      }
      if (m.normal && normals.count(normalName(m.normal, m.normalEncoding))) {
        input("normalmap_texture", SdfValueTypeNames->Asset,
              VtValue(SdfAssetPath("../textures/" + normalName(m.normal, m.normalEncoding))));
        input("encoding", SdfValueTypeNames->Int, VtValue(0)); // octahedral
      }
      const float intensity = std::max({m.emission[0], m.emission[1], m.emission[2]});
      input("enable_emission", SdfValueTypeNames->Bool, VtValue(intensity > 0 || m.emissive != 0));
      if (intensity > 0) {
        input("emissive_color_constant", SdfValueTypeNames->Color3f,
              VtValue(GfVec3f(m.emission[0], m.emission[1], m.emission[2]) / intensity));
        input("emissive_intensity", SdfValueTypeNames->Float, VtValue(intensity));
      }
      map("emissive_mask_texture", m.emissive);
      input("use_legacy_alpha_state", SdfValueTypeNames->Bool, VtValue(m.drawAlpha));
      input("alpha_test_type", SdfValueTypeNames->Int, VtValue(int(m.compare)));
      input("alpha_test_reference_value", SdfValueTypeNames->Int,
            VtValue(int(std::lround(std::clamp(m.reference, 0.f, 1.f) * 255))));
      // Sampler state, twice: as shader inputs (what the Toolkit shows and
      // mods override), and as the uniform un-prefixed attributes RTX Remix's
      // own capture exporter writes (game_exporter.cpp), which its capture
      // rendering reads; without them captured textures render clamped.
      const std::pair<const char *, unsigned> sampler[] = {
          {"filter_mode", m.filter}, {"wrap_mode_u", m.wrapU}, {"wrap_mode_v", m.wrapV}};
      for (const auto &[name, value] : sampler) {
        input(name, SdfValueTypeNames->UInt, VtValue(value));
        shader.GetPrim()
            .CreateAttribute(TfToken(name), SdfValueTypeNames->UInt, false, SdfVariabilityUniform)
            .Set(value);
      }
      shader.GetPrim()
          .CreateAttribute(TfToken("enable_opacity"), SdfValueTypeNames->Bool, false, SdfVariabilityUniform)
          .Set(m.tint[3] < 1 || m.compare != 7 || m.drawAlpha);
      exportLayer(stage->GetRootLayer(), file);
    }

    // Sdf authoring for the capture layer: its references are composed only
    // when the capture is opened, so no stage resolves them here.
    SdfPrimSpecHandle define(const SdfLayerHandle &layer, const std::string &path, const char *type) {
      auto spec = SdfCreatePrimInLayer(layer, SdfPath(path));
      spec->SetSpecifier(SdfSpecifierDef);
      if (type)
        spec->SetTypeName(type);
      return spec;
    }
    SdfPrimSpecHandle over(const SdfPrimSpecHandle &parent, const std::string &name) {
      for (const auto &child : parent->GetNameChildren())
        if (child->GetName() == name)
          return child;
      return SdfPrimSpec::New(parent, name, SdfSpecifierOver);
    }
    void set(const SdfPrimSpecHandle &prim, const std::string &name, const SdfValueTypeName &type, const VtValue &value,
             SdfVariability variability = SdfVariabilityVarying) {
      auto attribute = prim->GetAttributeAtPath(prim->GetPath().AppendProperty(TfToken(name)));
      if (!attribute)
        attribute = SdfAttributeSpec::New(prim, name, type, variability);
      attribute->SetDefaultValue(value);
    }
    void applySchema(const SdfPrimSpecHandle &prim, const char *schema) {
      SdfTokenListOp schemas;
      if (prim->HasInfo(UsdTokens->apiSchemas))
        schemas = prim->GetInfo(UsdTokens->apiSchemas).Get<SdfTokenListOp>();
      auto prepended = schemas.GetPrependedItems();
      if (std::find(prepended.begin(), prepended.end(), TfToken(schema)) == prepended.end())
        prepended.push_back(TfToken(schema));
      schemas.SetPrependedItems(prepended);
      prim->SetInfo(UsdTokens->apiSchemas, VtValue(schemas));
    }
    void bind(const SdfPrimSpecHandle &prim, uint64_t material) {
      applySchema(prim, "MaterialBindingAPI");
      auto relationship = SdfRelationshipSpec::New(prim, "material:binding", false);
      relationship->GetTargetPathList().Prepend(SdfPath("/RootNode/Looks/mat_" + hex(material)));
    }
    void transform(const SdfPrimSpecHandle &prim, const GfMatrix4d &value) {
      set(prim, "xformOp:transform", SdfValueTypeNames->Matrix4d, VtValue(value));
      set(prim, "xformOpOrder", SdfValueTypeNames->TokenArray, VtValue(VtTokenArray{TfToken("xformOp:transform")}),
          SdfVariabilityUniform);
    }
    // Binds each surface prim of a mesh to its material.
    void bindSurfaces(const SdfPrimSpecHandle &mesh, const std::vector<uint64_t> &materials) {
      for (size_t s = 0; s < materials.size(); ++s)
        if (materials[s])
          bind(over(mesh, surfaceName(s)), materials[s]);
    }

    // A skinned surface's posed points, as the runtime deforms them.
    VtVec3fArray posed(const scene::Surface &surface, const std::vector<scene::Matrix> &bones) {
      VtVec3fArray points;
      for (size_t v = 0; v < surface.vertices.size(); ++v) {
        const auto &p = surface.vertices[v].position;
        GfVec3f out(0);
        for (uint32_t b = 0; b < surface.bonesPerVertex; ++b) {
          const auto weight = v * surface.bonesPerVertex + b;
          if (weight >= surface.bones.size() || surface.bones[weight] >= bones.size())
            continue;
          const auto &m = bones[surface.bones[weight]];
          for (int c = 0; c < 3; ++c)
            out[c] += surface.weights[weight] * (m[c] * p[0] + m[4 + c] * p[1] + m[8 + c] * p[2] + m[12 + c]);
        }
        points.push_back(out);
      }
      return points;
    }

    void writeInstance(const SdfPrimSpecHandle &prim, const CaptureScene::Instance &instance,
                       const CaptureScene::Mesh &mesh) {
      prim->GetReferenceList().Prepend(SdfReference(std::string(), SdfPath("/RootNode/meshes/mesh_" + hex(instance.mesh))));
      set(prim, "visibility", SdfValueTypeNames->Token, VtValue(UsdGeomTokens->inherited));
      transform(prim, matrix(instance.model));
      for (uint32_t bit = 1; bit; bit <<= 1)
        if (instance.categoryFlags & bit)
          if (const char *name = categoryAttribute(bit))
            set(prim, std::string("remix_category:") + name, SdfValueTypeNames->Bool, VtValue(true));
      const auto &b = instance.blend;
      for (size_t s = 0; s < mesh.surfaces.size(); ++s) {
        auto geometry = over(prim, surfaceName(s));
        set(geometry, "doubleSided", SdfValueTypeNames->Bool, VtValue(instance.doubleSided), SdfVariabilityUniform);
        // The draw's alpha and blend state, as capture_alpha reads it back.
        auto meta = [&](const char *key, const VtValue &value) {
          set(geometry, std::string("primvars:_remix_metadata:") + key,
              value.IsHolding<bool>() ? SdfValueTypeNames->Bool : SdfValueTypeNames->UInt, value);
        };
        meta("alphaTestEnabled", VtValue(instance.alphaTest));
        meta("alphaTestReferenceValue",
             VtValue(unsigned(std::lround(std::clamp(instance.alphaReference, 0.f, 1.f) * 255))));
        meta("alphaTestCompareOp", VtValue(unsigned(instance.alphaCompare)));
        meta("alphaBlendEnabled", VtValue(b.enabled));
        meta("srcColorBlendFactor", VtValue(unsigned(b.src)));
        meta("dstColorBlendFactor", VtValue(unsigned(b.dst)));
        meta("colorBlendOp", VtValue(unsigned(b.op)));
        meta("srcAlphaBlendFactor", VtValue(unsigned(b.srcAlpha)));
        meta("dstAlphaBlendFactor", VtValue(unsigned(b.dstAlpha)));
        meta("alphaBlendOp", VtValue(unsigned(b.alphaOp)));
        meta("writeMask", VtValue(unsigned(b.mask)));
        meta("textureColorArg1Source", VtValue(unsigned(b.color1)));
        meta("textureColorArg2Source", VtValue(unsigned(b.color2)));
        meta("textureColorOperation", VtValue(unsigned(b.colorOp)));
        meta("textureAlphaArg1Source", VtValue(unsigned(b.alpha1)));
        meta("textureAlphaArg2Source", VtValue(unsigned(b.alpha2)));
        meta("textureAlphaOperation", VtValue(unsigned(b.alphaOpTexture)));
        meta("tFactor", VtValue(unsigned(b.factor)));
        if (instance.materialOverride)
          bind(geometry, instance.materialOverride);
        if (!instance.bones.empty() && mesh.surfaces[s].bonesPerVertex)
          set(geometry, "points", SdfValueTypeNames->Point3fArray, VtValue(posed(mesh.surfaces[s], instance.bones)));
      }
    }

    // Lights keep the parameters they were created with (LightSource).
    // Spheres and distant lights are placed by their direction alone: USD
    // lights shine (and are shaped) along their local -Z.
    void writeLight(const SdfLayerHandle &layer, const fs::path &directory, uint64_t hash,
                    const CaptureScene::Light &captured, const std::set<uint64_t> &textures) {
      const auto &light = captured.source;
      const char *type = nullptr;
      switch (light.type) {
      case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT: type = "SphereLight"; break;
      case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_RECT_EXT: type = "RectLight"; break;
      case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISK_EXT: type = "DiskLight"; break;
      case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT: type = "CylinderLight"; break;
      case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT: type = "DistantLight"; break;
      case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT: type = "DomeLight"; break;
      default: return;
      }
      auto prim = define(layer, "/RootNode/lights/light_" + hex(hash), type);
      GfMatrix4d placement = matrix(light.transform);
      const bool aimed = light.type == REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT ||
                         light.type == REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
      if (aimed) {
        const GfVec3d z = -GfVec3d(light.direction.x, light.direction.y, light.direction.z).GetNormalized();
        const GfVec3d x = GfCross(std::abs(z[2]) < .9 ? GfVec3d(0, 0, 1) : GfVec3d(0, 1, 0), z).GetNormalized();
        const GfVec3d y = GfCross(z, x);
        const auto position = placement.ExtractTranslation();
        placement.SetRow(0, GfVec4d(x[0], x[1], x[2], 0));
        placement.SetRow(1, GfVec4d(y[0], y[1], y[2], 0));
        placement.SetRow(2, GfVec4d(z[0], z[1], z[2], 0));
        placement.SetRow(3, GfVec4d(position[0], position[1], position[2], 1));
      }
      transform(prim, placement);
      auto value = [&](const char *name, float number) {
        set(prim, std::string("inputs:") + name, SdfValueTypeNames->Float, VtValue(number));
      };
      set(prim, "inputs:color", SdfValueTypeNames->Color3f, VtValue(GfVec3f(light.color.x, light.color.y, light.color.z)));
      value("intensity", light.intensity);
      value("exposure", light.exposure);
      if (light.temperatureEnabled) {
        set(prim, "inputs:enableColorTemperature", SdfValueTypeNames->Bool, VtValue(true));
        value("colorTemperature", light.temperature);
      }
      constexpr float degrees = float(180 / 3.141592653589793);
      switch (light.type) {
      case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT:
      case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISK_EXT:
        value("radius", light.radius);
        break;
      case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_RECT_EXT:
        value("width", light.width);
        value("height", light.height);
        break;
      case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT:
        value("radius", light.radius);
        value("length", light.length);
        break;
      case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT:
        value("angle", light.angle * degrees);
        break;
      default:
        if (!light.texture.empty()) {
          set(prim, "inputs:texture:file", SdfValueTypeNames->Asset, VtValue(SdfAssetPath(utf8(light.texture))));
          set(prim, "inputs:texture:format", SdfValueTypeNames->Token, VtValue(TfToken("latlong")));
        }
        break;
      }
      const bool shaped = light.cone < 3.14159f || light.focus > 0;
      if (light.type != REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT &&
          light.type != REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT && (shaped || !light.ies.empty())) {
        applySchema(prim, "ShapingAPI");
        if (shaped) {
          value("shaping:cone:angle", light.cone * degrees);
          value("shaping:cone:softness", light.softness);
          value("shaping:focus", light.focus);
        }
        // The profile's text as given, beside the capture (UsdLuxShapingAPI's file).
        if (!light.ies.empty()) {
          const auto name = "light_" + hex(hash) + ".ies";
          writeFile(directory / "lights" / name, std::vector<uint8_t>(light.ies.begin(), light.ies.end()));
          set(prim, "inputs:shaping:ies:file", SdfValueTypeNames->Asset, VtValue(SdfAssetPath("./lights/" + name)));
        }
      }
      // A light cookie has no UsdLux counterpart: OpenRemix's own inputs
      // (scene.cpp reads them back), its texture beside the others.
      if (captured.cookie && textures.count(captured.cookie)) {
        set(prim, "inputs:remix:cookie:texture", SdfValueTypeNames->Asset,
            VtValue(SdfAssetPath("./textures/" + hex(captured.cookie) + ".dds")));
        set(prim, "inputs:remix:cookie:up", SdfValueTypeNames->Float3,
            VtValue(GfVec3f(light.cookieUp.x, light.cookieUp.y, light.cookieUp.z)));
        value("remix:cookie:fov", light.cookieFov);
      }
    }

    void writeCamera(const SdfLayerHandle &layer, const CaptureScene::Camera &camera) {
      if (!camera.valid)
        return;
      auto prim = define(layer, "/RootNode/Camera", "Camera");
      GfMatrix4d toWorld;
      std::copy_n(camera.toWorld, 16, toWorld.data());
      transform(prim, toWorld);
      // submission.cpp derives the vertical field of view from these.
      const float aperture = 20.955f;
      const float focal = aperture / (2 * camera.aspect * std::tan(camera.fovY / 2));
      set(prim, "projection", SdfValueTypeNames->Token, VtValue(TfToken("perspective")));
      set(prim, "focalLength", SdfValueTypeNames->Float, VtValue(focal));
      set(prim, "horizontalAperture", SdfValueTypeNames->Float, VtValue(aperture));
      set(prim, "verticalAperture", SdfValueTypeNames->Float, VtValue(aperture / camera.aspect));
      set(prim, "clippingRange", SdfValueTypeNames->Float2, VtValue(GfVec2f(camera.nearPlane, camera.farPlane)));
    }
  } // namespace

  fs::path writeCapture(const CaptureScene &scene, const fs::path &directory) {
    std::error_code error;
    for (const char *folder : {"meshes", "materials", "textures", "lights"}) {
      fs::create_directories(directory / folder, error);
      if (error)
        throw std::runtime_error("Could not create " + utf8(directory / folder) + ": " + error.message());
    }
    // Textures are named by their hash: an existing file of the same size is
    // the same texture. A different size is a preview-level stand-in from an
    // earlier capture (replaced) or the full texture (kept over a stand-in).
    std::set<uint64_t> textures;
    for (const auto &[hash, pixels] : scene.textures) {
      const auto file = directory / "textures" / (hex(hash) + ".dds");
      const auto bytes = scene::encodeDds(pixels);
      if (bytes.empty())
        continue;
      // A stand-in never replaces a file already there.
      const bool standIn = pixels.compressed && pixels.compressed->upload.empty();
      const auto size = fs::file_size(file, error);
      if (!error && (size == bytes.size() || standIn)) {
        textures.insert(hash);
        continue;
      }
      writeFile(file, bytes);
      textures.insert(hash);
    }
    std::set<std::string> normals;
    for (const auto &[hash, material] : scene.materials) {
      const auto source = scene.textures.find(material.normal);
      if (source == scene.textures.end())
        continue;
      const auto name = normalName(material.normal, material.normalEncoding);
      if (!normals.insert(name).second)
        continue;
      scene::Pixels converted;
      converted.srgb = false;
      auto texels = octahedral(source->second, material.normalEncoding, converted.width, converted.height);
      if (texels.empty()) {
        normals.erase(name);
        continue;
      }
      converted.mips.push_back(std::move(texels));
      writeFile(directory / "textures" / name, scene::encodeDds(converted));
    }
    for (const auto &[hash, material] : scene.materials)
      writeMaterial(hash, material, directory / "materials" / ("mat_" + hex(hash) + ".usda"), textures, normals);
    for (const auto &[hash, mesh] : scene.meshes)
      writeMesh(scene, hash, mesh, directory / "meshes" / ("mesh_" + hex(hash) + ".usd"));

    // A bare layer: on a stage, each edit would recompose the references
    // against an anonymous identifier they cannot resolve from.
    const auto layer = SdfLayer::CreateAnonymous(scene.name + ".usda");
    layer->SetField(SdfPath::AbsoluteRootPath(), UsdGeomTokens->metersPerUnit, VtValue(scene.metersPerUnit));
    layer->SetField(SdfPath::AbsoluteRootPath(), UsdGeomTokens->upAxis,
                    VtValue(scene.upAxis == 2 ? UsdGeomTokens->z : UsdGeomTokens->y));
    layer->SetDefaultPrim(TfToken("RootNode"));
    VtDictionary data;
    data["lightspeed_layer_type"] = VtValue(std::string("capture"));
    data["lightspeed_exe_name"] = VtValue(scene.executable);
    data["lightspeed_game_name"] = VtValue(utf8(utf8Path(scene.executable).stem()));
    layer->SetCustomLayerData(data);
    define(layer, "/RootNode", "Xform");
    define(layer, "/RootNode/Looks", "Scope");
    define(layer, "/RootNode/meshes", "Scope");
    define(layer, "/RootNode/instances", "Scope");
    define(layer, "/RootNode/lights", "Scope");
    for (const auto &[hash, material] : scene.materials) {
      auto prim = define(layer, "/RootNode/Looks/mat_" + hex(hash), "Material");
      prim->GetReferenceList().Prepend(SdfReference("./materials/mat_" + hex(hash) + ".usda"));
    }
    for (const auto &[hash, mesh] : scene.meshes) {
      auto prim = define(layer, "/RootNode/meshes/mesh_" + hex(hash), "Xform");
      prim->GetReferenceList().Prepend(SdfReference("./meshes/mesh_" + hex(hash) + ".usd"));
      // The definitions are invisible; their instances draw them.
      set(prim, "visibility", SdfValueTypeNames->Token, VtValue(UsdGeomTokens->invisible));
      bindSurfaces(prim, mesh.materials);
    }
    std::map<uint64_t, size_t> counts;
    for (const auto &instance : scene.instances) {
      const auto &mesh = scene.meshes.at(instance.mesh);
      const auto name = "inst_" + hex(instance.mesh) + "_" + std::to_string(counts[instance.mesh]++);
      writeInstance(define(layer, "/RootNode/instances/" + name, "Xform"), instance, mesh);
    }
    for (const auto &[hash, light] : scene.lights)
      writeLight(layer, directory, hash, light, textures);
    writeCamera(layer, scene.camera);
    const auto file = directory / (scene.name + ".usda");
    exportLayer(layer, file);
    return file;
  }
} // namespace remix::usd
