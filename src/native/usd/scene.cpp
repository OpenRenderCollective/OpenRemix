// Static capture composition adapted from the Godot toolkit. CPU-only OpenUSD 25.11.
#include "alpha_state.h"
#include "capture_categories.h"
#include "skinning.h"
#include "snapshot.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <atomic>
#include <map>
#include <mutex>
#include <pxr/base/gf/vec2f.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/ar/resolverContextBinder.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdSkel/bindingAPI.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdLux/blackbody.h>
#include <pxr/usd/usdLux/lightAPI.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <set>
#include <thread>

PXR_NAMESPACE_USING_DIRECTIVE
namespace remix::usd {
  namespace {
    std::string source_path(const UsdPrim &prim) {
      for (const auto &spec : prim.GetPrimStack()) {
        const auto path = spec->GetPath().GetString();
        if (path.rfind("/RootNode/meshes/", 0) == 0 || path.rfind("/RootNode/lights/", 0) == 0)
          return path;
      }
      return prim.GetPath().GetString();
    }
    Json matrix_json(const GfMatrix4d &matrix) {
      Json values = Json::array();
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
          values.push_back(matrix[r][c]);
      return values;
    }
    template <typename T> T attribute(const UsdPrim &prim, const char *name, const T &fallback) {
      T result = fallback;
      if (!prim)
        return result;
      auto attr = prim.GetAttribute(TfToken(name));
      if (attr)
        attr.Get(&result);
      return result;
    }
    int sampler_mode(const UsdPrim &shader, const char *name, int maximum) {
      // Captures author uints; MDL editors can author ints. Remix defaults to
      // linear filtering and repeat wrapping (both encoded as 1).
      if (!shader)
        return 1;
      const auto attr = shader.GetAttribute(TfToken(name));
      VtValue value;
      if (!attr || !attr.Get(&value))
        return 1;
      if (value.IsHolding<unsigned int>()) {
        const auto mode = value.Get<unsigned int>();
        return mode <= unsigned(maximum) ? int(mode) : 1;
      }
      if (value.IsHolding<int>()) {
        const auto mode = value.Get<int>();
        return mode >= 0 && mode <= maximum ? mode : 1;
      }
      return 1;
    }
    double number_attribute(const UsdPrim &prim, const char *name, double fallback, double maximum) {
      VtValue value;
      const auto attr = prim ? prim.GetAttribute(TfToken(name)) : UsdAttribute();
      if (!attr || !attr.Get(&value))
        return fallback;
      const auto converted = VtValue::Cast<double>(value);
      if (!converted.IsHolding<double>())
        return fallback;
      const auto number = converted.Get<double>();
      return std::isfinite(number) && number >= 0 && number <= maximum ? number : fallback;
    }
    Json capture_alpha(const UsdPrim &prim) {
      auto result = capture_alpha_defaults();
      for (auto it = result.begin(); it != result.end(); ++it) {
        const auto name = "primvars:_remix_metadata:" + it.key();
        if (it.value().is_boolean())
          it.value() = attribute(prim, name.c_str(), it.value().get<bool>());
        else {
          const auto key = it.key();
          const double maximum = key == "tFactor"                   ? double(UINT32_MAX)
                                 : key == "alphaTestReferenceValue" ? 255
                                 : key == "alphaTestCompareOp" || key.find("Operation") != std::string::npos
                                     ? 7
                                 : key.find("Source") != std::string::npos  ? 3
                                 : key.find("BlendOp") != std::string::npos ? 4
                                 : key == "writeMask"                       ? 15
                                                                            : 18;
          it.value() = uint32_t(number_attribute(prim, name.c_str(), it.value().get<double>(), maximum));
        }
      }
      return result;
    }
    uint32_t packed_color(const GfVec3f &color, float alpha) {
      auto byte = [](float value) {
        return uint32_t(std::lround(std::clamp(std::isfinite(value) ? value : 1.f, 0.f, 1.f) * 255.f));
      };
      // HardcodedVertex's color is VK_FORMAT_B8G8R8A8_UNORM.
      return byte(color[2]) | (byte(color[1]) << 8) | (byte(color[0]) << 16) | (byte(alpha) << 24);
    }
    std::string texture(const UsdPrim &shader, const char *name, Json &warnings) {
      if (!shader)
        return {};
      const auto attr = shader.GetAttribute(TfToken(name));
      SdfAssetPath value;
      if (!attr || !attr.Get(&value) || value.GetAssetPath().empty())
        return {};
      if (!value.GetResolvedPath().empty())
        return value.GetResolvedPath();

      // Captures made on Windows can contain backslashes even when opened on Linux.
      // Anchor the normalized identifier at the strongest authored value's layer:
      // references and sublayers may put it far from the project or capture root.
      auto normalized = value.GetAssetPath();
      std::replace(normalized.begin(), normalized.end(), '\\', '/');
      ArResolverContextBinder context(shader.GetStage()->GetPathResolverContext());
      std::string anchor;
      for (const auto &spec : attr.GetPropertyStack(UsdTimeCode::Default())) {
        if (!spec->HasInfo(SdfFieldKeys->Default))
          continue;
        anchor = spec->GetLayer()->GetIdentifier();
        auto identifier = SdfComputeAssetPathRelativeToLayer(spec->GetLayer(), normalized);
        auto resolved = ArGetResolver().Resolve(identifier);
        if (!resolved.empty())
          return resolved.GetPathString();
        // A missing strong opinion must not silently fall back to a weaker texture.
        break;
      }
      warnings.push_back("Unresolved texture " + attr.GetPath().GetString() + ": " + value.GetAssetPath() +
                         (anchor.empty() ? "" : " (authored in " + anchor + ")"));
      return {};
    }
    Json material_json(const UsdPrim &prim, Json &warnings) {
      UsdPrim shader;
      if (auto material = UsdShadeMaterial(prim); material && prim.IsA<UsdShadeMaterial>()) {
        if (auto connected = material.ComputeSurfaceSource(TfToken("mdl")))
          shader = connected.GetPrim();
      }
      if (!shader) {
        if (auto child = prim.GetChild(TfToken("Shader")))
          shader = child;
        else {
          for (auto child : prim.GetChildren())
            if (child.IsA<UsdShadeShader>() || child.GetName() == "Shader") {
              shader = child;
              break;
            }
        }
      }
      if (!shader)
        shader = prim;
      auto color = attribute(shader, "inputs:diffuse_color_constant", GfVec3f(0.7f));
      auto emission = attribute(shader, "inputs:emissive_color_constant", GfVec3f(0.0f));
      auto transmission = attribute(shader, "inputs:transmittance_color", GfVec3f(1.0f));
      auto source = attribute(shader, "info:mdl:sourceAsset", SdfAssetPath()).GetAssetPath();
      const bool translucent = source.find("Translucent") != std::string::npos;
      Json result = {{"id", prim.GetPath().GetString()},
              {"kind", translucent ? "translucent" : "opaque"},
              {"albedo", {color[0], color[1], color[2]}},
              {"roughness", attribute(shader, "inputs:reflection_roughness_constant", 0.5f)},
              {"metallic", attribute(shader, "inputs:metallic_constant", 0.0f)},
              {"opacity", attribute(shader, "inputs:opacity_constant", 1.0f)},
              {"use_legacy_alpha", attribute(shader, "inputs:use_legacy_alpha_state", true)},
              {"blend_enabled", attribute(shader, "inputs:blend_enabled", false)},
              {"blend_type", int(number_attribute(shader, "inputs:blend_type", 0, 10))},
              {"inverted_blend", attribute(shader, "inputs:inverted_blend", false)},
              {"alpha_test", int(number_attribute(shader, "inputs:alpha_test_type", 7, 7))},
              {"alpha_reference", int(number_attribute(shader, "inputs:alpha_test_reference_value", 0, 255))},
              {"emissive", {emission[0], emission[1], emission[2]}},
              {"emissive_intensity", attribute(shader, "inputs:enable_emission", false)
                                         ? attribute(shader, "inputs:emissive_intensity", 40.0f)
                                         : 0.0f},
              {"ior", attribute(shader, "inputs:ior_constant", 1.3f)},
              {"transmittance", {transmission[0], transmission[1], transmission[2]}},
              // AperturePBR_Translucent's body: a thin wall, or the distance over
              // which the transmittance colour is reached (Beer-Lambert).
              {"transmittance_distance", attribute(shader, "inputs:transmittance_measurement_distance", 1.0f)},
              {"thin_walled", attribute(shader, "inputs:thin_walled", false)},
              {"thin_wall_thickness", attribute(shader, "inputs:thin_wall_thickness", 1.0f)},
              {"use_diffuse_layer", attribute(shader, "inputs:use_diffuse_layer", false)},
              {"filter_mode", sampler_mode(shader, "inputs:filter_mode", 1)},
              {"wrap_mode_u", sampler_mode(shader, "inputs:wrap_mode_u", 3)},
              {"wrap_mode_v", sampler_mode(shader, "inputs:wrap_mode_v", 3)},
              {"albedo_texture",
               texture(shader, translucent ? "inputs:transmittance_texture" : "inputs:diffuse_texture",
                       warnings)},
              {"normal_texture", texture(shader, "inputs:normalmap_texture", warnings)},
              {"normal_encoding", attribute(shader, "inputs:encoding", 0)},
              {"roughness_texture", texture(shader, "inputs:reflectionroughness_texture", warnings)},
              {"metallic_texture", texture(shader, "inputs:metallic_texture", warnings)},
              {"emissive_texture", texture(shader, "inputs:emissive_mask_texture", warnings)}};
      // AperturePBR's thin film, with OpenRemix's iridescence attributes
      // (openremix:iridescence:*, outside the MDL inputs). OpenRemix also
      // reads the film on AperturePBR_Translucent, which has none of its own.
      if (attribute(shader, "inputs:enable_thin_film", false))
        result["iridescence"] = {
            {"thickness", attribute(shader, "inputs:thin_film_thickness_constant", 200.0f)},
            {"from_alpha", attribute(shader, "inputs:thin_film_thickness_from_albedo_alpha", false)},
            {"strength", attribute(shader, "openremix:iridescence:strength", 1.0f)},
            {"ior", attribute(shader, "openremix:iridescence:filmIor", 1.5f)},
            {"texture", texture(shader, "openremix:iridescence:thicknessTexture", warnings)},
            {"min", attribute(shader, "openremix:iridescence:thicknessMin", 100.0f)},
            {"max", attribute(shader, "openremix:iridescence:thicknessMax", 800.0f)},
            {"flow_amplitude", attribute(shader, "openremix:iridescence:flowAmplitude", 0.0f)},
            {"flow_scale", attribute(shader, "openremix:iridescence:flowScale", 1.0f)},
            {"flow_speed", attribute(shader, "openremix:iridescence:flowSpeed", 0.1f)},
            {"structural", attribute(shader, "openremix:iridescence:structural", 0.0f)}};
      // RTX Remix's subsurface inputs, with OpenRemix's weight outside them
      // (openremix:subsurface:weight). Under a profile the unauthored colour
      // leaves the albedo as it is, where RTX Remix's grey would replace it.
      const bool profile = attribute(shader, "inputs:subsurface_diffusion_profile", false);
      const float measurement = attribute(shader, "inputs:subsurface_measurement_distance", 0.0f),
                  weight = attribute(shader, "openremix:subsurface:weight", 1.0f);
      if (!translucent && (profile || measurement > 0) && weight > 0) {
        const auto vector = [](const GfVec3f &v) { return Json::array({v[0], v[1], v[2]}); };
        const float neutral = profile ? 1.f : .5f;
        result["subsurface"] = {
            {"diffusion_profile", profile},
            {"weight", weight},
            {"radius", vector(attribute(shader, "inputs:subsurface_radius", GfVec3f(.5f)))},
            {"radius_scale", attribute(shader, "inputs:subsurface_radius_scale", 1.0f)},
            {"max_sample_radius", attribute(shader, "inputs:subsurface_max_sample_radius", 16.0f)},
            {"anisotropy", attribute(shader, "inputs:subsurface_volumetric_anisotropy", 0.0f)},
            {"transmittance", vector(attribute(shader, "inputs:subsurface_transmittance_color", GfVec3f(neutral)))},
            {"measurement_distance", measurement},
            {"single_scattering_albedo", vector(attribute(shader, "inputs:subsurface_single_scattering_albedo", GfVec3f(.5f)))},
            {"transmittance_texture", texture(shader, "inputs:subsurface_transmittance_texture", warnings)},
            {"thickness_texture", texture(shader, "inputs:subsurface_thickness_texture", warnings)},
            {"single_scattering_texture", texture(shader, "inputs:subsurface_single_scattering_texture", warnings)},
            {"radius_texture", texture(shader, "inputs:subsurface_radius_texture", warnings)}};
      }
      return result;
    }
    int element_index(const TfToken &interpolation, int face, int corner, int vertex) {
      if (interpolation == UsdGeomTokens->constant)
        return 0;
      if (interpolation == UsdGeomTokens->uniform)
        return face;
      if (interpolation == UsdGeomTokens->faceVarying)
        return corner;
      return vertex;
    }
    // Project onto the dominant plane so concave polygon faces can be ear-clipped.
    std::vector<std::array<int, 3>> triangulate(const VtVec3fArray &points, const VtIntArray &indices,
                                                int start, int count) {
      std::vector<std::array<int, 3>> triangles;
      if (count < 3)
        return triangles;
      GfVec3f normal(0);
      for (int i = 0; i < count; ++i)
        normal += GfCross(points[indices[start + i]], points[indices[start + (i + 1) % count]]);
      int axis = 0;
      if (std::abs(normal[1]) > std::abs(normal[axis]))
        axis = 1;
      if (std::abs(normal[2]) > std::abs(normal[axis]))
        axis = 2;
      int x = (axis + 1) % 3, y = (axis + 2) % 3;
      auto cross = [&](int a, int b, int c) {
        const auto &pa = points[indices[start + a]];
        const auto &pb = points[indices[start + b]];
        const auto &pc = points[indices[start + c]];
        return (pb[x] - pa[x]) * (pc[y] - pa[y]) - (pb[y] - pa[y]) * (pc[x] - pa[x]);
      };
      const float sign = normal[axis] >= 0 ? 1.f : -1.f;
      std::vector<int> corners;
      for (int i = 0; i < count; ++i)
        corners.push_back(i);
      while (corners.size() > 3) {
        bool found = false;
        for (size_t i = 0; i < corners.size(); ++i) {
          int a = corners[(i + corners.size() - 1) % corners.size()], b = corners[i],
              c = corners[(i + 1) % corners.size()];
          if (cross(a, b, c) * sign <= 1e-12f)
            continue;
          bool contains = false;
          for (int p : corners) {
            if (p == a || p == b || p == c)
              continue;
            if (cross(a, b, p) * sign >= 0 && cross(b, c, p) * sign >= 0 && cross(c, a, p) * sign >= 0) {
              contains = true;
              break;
            }
          }
          if (contains)
            continue;
          triangles.push_back({start + a, start + b, start + c});
          corners.erase(corners.begin() + i);
          found = true;
          break;
        }
        // Degenerate or self-intersecting: what is left as a fan, as RTX Remix
        // triangulates every polygon. One such face must not lose the mesh.
        if (!found) {
          for (size_t i = 1; i + 2 < corners.size(); ++i)
            triangles.push_back({start + corners[0], start + corners[i], start + corners[i + 1]});
          corners.erase(corners.begin() + 1, corners.end() - 2);
        }
      }
      triangles.push_back({start + corners[0], start + corners[1], start + corners[2]});
      return triangles;
    }
    Json light_json(const UsdPrim &prim, const GfMatrix4d &world, Json &warnings) {
      const auto path = prim.GetPath().GetString();
      const auto lightValue = [&](const char *name, auto fallback) {
        const auto attr = light_attribute(prim, name);
        if (attr)
          attr.Get(&fallback);
        return fallback;
      };
      auto color = lightValue("color", GfVec3f(1));
      const auto cookie_up = attribute(prim, "inputs:remix:cookie:up", GfVec3f(0, 0, 1));
      if (lightValue("enableColorTemperature", false))
        color = GfCompMult(color, UsdLuxBlackbodyTemperatureAsRgb(lightValue("colorTemperature", 6500.f)));
      return {{"id", path},
              {"source_path", source_path(prim)},
              {"type", prim.GetTypeName().GetString()},
              {"transform", matrix_json(world)},
              {"color", {color[0], color[1], color[2]}},
              {"intensity", lightValue("intensity", 1.f) * std::pow(2.f, lightValue("exposure", 0.f))},
              {"radius", lightValue("radius", 0.5f)},
              {"width", lightValue("width", 1.f)},
              {"height", lightValue("height", 1.f)},
              {"length", lightValue("length", 1.f)},
              {"angle", lightValue("angle", 0.53f)},
              {"cone_angle", lightValue("shaping:cone:angle", 180.f)},
              {"cone_softness", lightValue("shaping:cone:softness", 0.f)},
              {"focus", lightValue("shaping:focus", 0.f)},
              {"texture", texture(prim, "inputs:texture:file", warnings)},
              // UsdLuxShapingAPI's IES profile, and OpenRemix's light cookie
              // (usd/capture_writer.cpp; remixapi_LightInfoCookieEXT).
              {"ies", texture(prim, "inputs:shaping:ies:file", warnings)},
              {"cookie", texture(prim, "inputs:remix:cookie:texture", warnings)},
              {"cookie_up", {cookie_up[0], cookie_up[1], cookie_up[2]}},
              {"cookie_fov", attribute(prim, "inputs:remix:cookie:fov", 0.f)}};
    }

  } // namespace

  UsdAttribute light_attribute(const UsdPrim &prim, const char *name) {
    const auto current = prim.GetAttribute(TfToken(std::string("inputs:") + name));
    return current.IsAuthored() ? current : prim.GetAttribute(TfToken(name));
  }
  std::string resolved_texture_path(const UsdPrim &prim, const char *name) {
    Json warnings = Json::array();
    return texture(prim, name, warnings);
  }
  Json compose_material(const UsdPrim &prim) {
    Json warnings = Json::array();
    auto result = material_json(prim, warnings);
    result["warnings"] = warnings;
    return result;
  }
  // Remix mods author replacements as `over "mesh_<hash>"` with defined
  // children (lights, meshes). The default predicate requires every ancestor
  // to be defined and would prune those subtrees; typeless overs compose as
  // plain transforms.
  Usd_PrimFlagsPredicate traversal() {
    return UsdTraverseInstanceProxies(UsdPrimIsActive && UsdPrimIsLoaded && !UsdPrimIsAbstract);
  }
  Json compose_lights(const UsdStageRefPtr &stage, uint64_t revision) {
    Json result{{"revision", revision}, {"lights", Json::array()}, {"warnings", Json::array()}};
    UsdGeomXformCache transforms(UsdTimeCode::EarliestTime());
    for (auto prim : UsdPrimRange::Stage(stage, traversal())) {
      // Mesh emitters take the geometry branch in compose_scene as well.
      if (!UsdLuxLightAPI(prim) || prim.IsA<UsdGeomMesh>() ||
          UsdGeomImageable(prim).ComputeVisibility() == UsdGeomTokens->invisible)
        continue;
      result["lights"].push_back(
          light_json(prim, transforms.GetLocalToWorldTransform(prim), result["warnings"]));
    }
    return result;
  }
  namespace {
    Json mesh_json(const ComposedMesh &mesh) {
      Json parts = Json::array();
      for (const auto &surface : mesh.surfaces) {
        Json part{{"material", surface.material},
                  {"category_flags", surface.categoryFlags},
                  {"vertices", Json::array()},
                  {"indices", Json::array()}};
        const size_t corners = surface.vertices.size() / 8;
        auto &vertices = part["vertices"];
        auto &indices = part["indices"];
        for (size_t i = 0; i < corners; ++i) {
          const float *v = surface.vertices.data() + i * 8;
          vertices.push_back({v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]});
          indices.push_back(i);
        }
        if (surface.influences) {
          part["influences"] = surface.influences;
          part["joint_indices"] = surface.jointIndices;
          part["joint_weights"] = surface.jointWeights;
        }
        if (!surface.colors.empty())
          part["colors"] = surface.colors;
        parts.push_back(std::move(part));
      }
      Json transform = Json::array();
      for (const double value : mesh.transform)
        transform.push_back(value);
      return {{"id", mesh.id},
              {"source_path", mesh.sourcePath},
              {"transform", std::move(transform)},
              {"surfaces", std::move(parts)},
              {"skinned", mesh.skinned},
              {"skeleton", mesh.skeleton},
              {"double_sided", mesh.doubleSided},
              {"alpha_state", mesh.alphaState},
              {"ignore_alpha", mesh.ignoreAlpha},
              {"alpha_blend_to_cutout", mesh.alphaBlendToCutout}};
    }
  } // namespace
  Json compose_scene(const UsdStageRefPtr &stage, uint64_t revision, const ComposeScope &scope) {
    Json result{{"revision", revision},
                {"meshes", Json::array()},
                {"materials", Json::array()},
                {"lights", Json::array()},
                {"cameras", Json::array()},
                {"warnings", Json::array()},
                {"up_axis", UsdGeomGetStageUpAxis(stage).GetString()},
                {"meters_per_unit", UsdGeomGetStageMetersPerUnit(stage)}};
    const auto time = UsdTimeCode::EarliestTime();
    UsdGeomXformCache transforms(time);
    SceneSkinning skinning(transforms);
    CaptureCategories categories(stage, result["warnings"]);
    result["category_sources"] = categories.sources();
    std::set<std::string> materials;
    // Guards the JSON below while meshes compose in parallel (ComposeScope::meshSink).
    std::mutex shared;
    const auto warn = [&](const std::string &text) {
      std::lock_guard lock(shared);
      result["warnings"].push_back(text);
    };
    const auto compose_mesh = [&](const UsdPrim &prim, const GfMatrix4d &world) {
      const auto path = prim.GetPath().GetString();
      const UsdGeomMesh mesh(prim);
      try {
        VtVec3fArray points, normals;
        VtIntArray indices, counts, holes;
        mesh.GetPointsAttr().Get(&points, time);
        mesh.GetFaceVertexIndicesAttr().Get(&indices, time);
        mesh.GetFaceVertexCountsAttr().Get(&counts, time);
        mesh.GetNormalsAttr().Get(&normals, time);
        mesh.GetHoleIndicesAttr().Get(&holes, time);
        std::string reason;
        if (!UsdGeomMesh::ValidateTopology(indices, counts, points.size(), &reason))
          throw std::runtime_error(reason);
        auto normal_interpolation = mesh.GetNormalsInterpolation();
        auto normal_primvar = UsdGeomPrimvarsAPI(mesh).GetPrimvar(TfToken("normals"));
        if (normal_primvar && normal_primvar.ComputeFlattened(&normals, time))
          normal_interpolation = normal_primvar.GetInterpolation();
        std::string skeleton;
        // A replacement's joint indices name the bones of the draw it
        // replaces, whatever its skeleton (dxvk-remix UsdMeshImporter).
        VtIntArray joints;
        VtFloatArray jointWeights;
        int influences = 0;
        bool everyVertex = true;
        if (scope.hostBones) {
          const UsdSkelBindingAPI binding(prim);
          const auto jointPrimvar = binding.GetJointIndicesPrimvar();
          const auto weightPrimvar = binding.GetJointWeightsPrimvar();
          if (jointPrimvar.HasValue() && weightPrimvar.HasValue() && jointPrimvar.ComputeFlattened(&joints, time) &&
              weightPrimvar.ComputeFlattened(&jointWeights, time)) {
            influences = jointPrimvar.GetElementSize();
            everyVertex = jointPrimvar.GetInterpolation() != UsdGeomTokens->constant;
            if (influences < 1 || joints.size() != jointWeights.size() ||
                joints.size() != size_t(influences) * (everyVertex ? points.size() : 1)) {
              warn(path + ": joint influences do not match its points; not skinned");
              influences = 0;
            }
          }
        } else {
          try {
            skeleton = skinning.deform(mesh, points, normals, normal_interpolation, indices, counts);
          } catch (const std::exception &error) {
            warn(path + ": " + error.what() + "; showing undeformed geometry");
          }
        }
        VtVec2fArray uv;
        auto st = UsdGeomPrimvarsAPI(mesh).GetPrimvar(TfToken("st"));
        if (st)
          st.ComputeFlattened(&uv, time);
        const auto color_primvar =
            UsdGeomPrimvarsAPI(mesh).FindPrimvarWithInheritance(TfToken("displayColor"));
        const auto opacity_primvar =
            UsdGeomPrimvarsAPI(mesh).FindPrimvarWithInheritance(TfToken("displayOpacity"));
        VtVec3fArray colors;
        VtFloatArray opacities;
        if (color_primvar)
          color_primvar.ComputeFlattened(&colors, time);
        if (opacity_primvar)
          opacity_primvar.ComputeFlattened(&opacities);
        auto binding = UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial();
        std::map<int, UsdShadeMaterial> face_materials;
        for (auto subset :
             UsdGeomSubset::GetGeomSubsets(mesh, UsdGeomTokens->face, TfToken("materialBind"))) {
          VtIntArray faces;
          subset.GetIndicesAttr().Get(&faces, time);
          auto bound = UsdShadeMaterialBindingAPI(subset.GetPrim()).ComputeBoundMaterial();
          for (int face : faces)
            face_materials[face] = bound;
        }
        std::map<std::string, ComposedSurface> surfaces;
        auto surface_of = [&](const UsdShadeMaterial &material) -> ComposedSurface & {
          std::string material_id = material ? material.GetPath().GetString() : "default";
          auto found = surfaces.find(material_id);
          if (found != surfaces.end())
            return found->second;
          auto &added = surfaces[material_id];
          added.material = material_id;
          std::lock_guard lock(shared);
          if (material && materials.insert(material_id).second)
            result["materials"].push_back(material_json(material.GetPrim(), result["warnings"]));
          added.categoryFlags = categories.flags(prim, material);
          return added;
        };
        const bool leftHanded =
            attribute(prim, "orientation", UsdGeomTokens->rightHanded) == UsdGeomTokens->leftHanded;
        ComposedSurface *whole = nullptr; // Every face's, without subsets.
        int corner = 0;
        for (size_t face = 0; face < counts.size(); ++face) {
          const int count = counts[face], first = corner;
          corner += count;
          if (!holes.empty() && std::find(holes.begin(), holes.end(), int(face)) != holes.end())
            continue;
          if (!whole && face_materials.empty())
            whole = &surface_of(binding);
          auto &surface = whole ? *whole : surface_of(face_materials.count(face) ? face_materials.at(face) : binding);
          for (auto triangle : triangulate(points, indices, first, count)) {
            if (leftHanded)
              std::swap(triangle[1], triangle[2]);
            auto geometric = GfCross(points[indices[triangle[1]]] - points[indices[triangle[0]]],
                                     points[indices[triangle[2]]] - points[indices[triangle[0]]])
                                 .GetNormalized();
            for (int c : triangle) {
              int vertex = indices[c];
              auto point = points[vertex];
              auto normal = geometric;
              auto n = element_index(normal_interpolation, face, c, vertex);
              if (n >= 0 && size_t(n) < normals.size())
                normal = normals[n];
              GfVec2f texcoord(0);
              int u = st ? element_index(st.GetInterpolation(), face, c, vertex) : -1;
              if (u >= 0 && size_t(u) < uv.size())
                texcoord = uv[u];
              surface.vertices.insert(surface.vertices.end(), {point[0], point[1], point[2], normal[0], normal[1],
                                                               normal[2], texcoord[0], texcoord[1]});
              if (influences) {
                surface.influences = uint32_t(influences);
                const size_t first = everyVertex ? size_t(vertex) * influences : 0;
                for (int k = 0; k < influences; ++k) {
                  surface.jointIndices.push_back(uint32_t(std::max(joints[first + k], 0)));
                  surface.jointWeights.push_back(jointWeights[first + k]);
                }
              }
              if (!colors.empty() || !opacities.empty()) {
                auto color = GfVec3f(1);
                float opacity = 1;
                const int ci =
                    color_primvar ? element_index(color_primvar.GetInterpolation(), face, c, vertex) : -1;
                const int ai = opacity_primvar
                                   ? element_index(opacity_primvar.GetInterpolation(), face, c, vertex)
                                   : -1;
                if (ci >= 0 && size_t(ci) < colors.size())
                  color = colors[ci];
                if (ai >= 0 && size_t(ai) < opacities.size())
                  opacity = opacities[ai];
                surface.colors.push_back(packed_color(color, opacity));
              }
            }
          }
        }
        ComposedMesh composed;
        composed.id = path;
        composed.sourcePath = source_path(prim);
        composed.skeleton = skeleton;
        for (int row = 0; row < 4; ++row)
          for (int col = 0; col < 4; ++col)
            composed.transform[row * 4 + col] = world[row][col];
        for (auto &entry : surfaces)
          composed.surfaces.push_back(std::move(entry.second));
        composed.skinned = !skeleton.empty() || influences > 0;
        composed.doubleSided = attribute(prim, "doubleSided", false);
        composed.alphaState = capture_alpha(prim);
        composed.ignoreAlpha = attribute(prim, "remix_category:ignore_alpha_channel", false);
        composed.alphaBlendToCutout = attribute(prim, "remix_category:alpha_blend_to_cutout", false);
        if (scope.meshSink)
          scope.meshSink(std::move(composed));
        else
          result["meshes"].push_back(mesh_json(composed));
      } catch (const std::exception &error) {
        warn(path + ": " + error.what());
      }
    };
    std::vector<std::pair<UsdPrim, GfMatrix4d>> deferred;
    for (auto prim : UsdPrimRange::Stage(stage, traversal())) {
      auto imageable = UsdGeomImageable(prim);
      if (imageable && imageable.ComputeVisibility() == UsdGeomTokens->invisible)
        continue;
      const auto path = prim.GetPath().GetString();
      const auto world = transforms.GetLocalToWorldTransform(prim);
      if (UsdGeomMesh(prim)) {
        if (scope.meshSink)
          deferred.emplace_back(prim, world);
        else
          compose_mesh(prim, world);
      } else if (auto light = UsdLuxLightAPI(prim)) {
        result["lights"].push_back(light_json(prim, world, result["warnings"]));
      } else if (auto camera = UsdGeomCamera(prim)) {
        result["cameras"].push_back(
            {{"id", path},
             {"transform", matrix_json(world)},
             {"focal_length", attribute(prim, "focalLength", 50.f)},
             {"horizontal_aperture", attribute(prim, "horizontalAperture", 20.955f)}});
      }
    }
    if (!deferred.empty()) {
      if (scope.progress)
        scope.progress->enter(LoadStage::Meshes, uint32_t(deferred.size()));
      std::atomic<size_t> next{0};
      const auto work = [&] {
        for (size_t i; (i = next++) < deferred.size();) {
          try {
            compose_mesh(deferred[i].first, deferred[i].second);
          } catch (...) {
            // The sink's to report. Exceptions stay on their thread.
          }
          if (scope.progress)
            ++scope.progress->done;
        }
      };
      const unsigned threads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
      std::vector<std::thread> pool;
      for (unsigned t = 1; t < threads && t < deferred.size(); ++t)
        pool.emplace_back(work);
      work();
      for (auto &thread : pool)
        thread.join();
    }
    result["skeletons"] = skinning.skeletons();
    return result;
  }
} // namespace remix::usd
