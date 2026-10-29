#include "usd/loader.h"
#include "scene/images.h"
#include "platform/utf8_path.h"
#include <charconv>
#include <algorithm>
#include <set>
#include <thread>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <functional>
#include <mutex>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/tf/errorMark.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdUtils/dependencies.h>

namespace remix::usd {
  using namespace pxr;
  uint64_t capturedHash(const std::string &path) {
    for (const char *prefix : {"mesh_", "inst_", "mat_", "light_"}) {
      const auto pos = path.rfind(prefix);
      if (pos == std::string::npos)
        continue;
      const auto begin = pos + std::strlen(prefix);
      uint64_t value = 0;
      if (path.size() < begin + 16)
        continue;
      const auto result = std::from_chars(path.data() + begin, path.data() + begin + 16, value, 16);
      if (result.ec == std::errc{} && result.ptr == path.data() + begin + 16)
        return value;
    }
    return 0;
  }
  std::vector<std::shared_ptr<scene::Texture>> decodeTextures(const std::vector<std::string> &paths) {
    // Half the cores leave room for the host still rendering meanwhile.
    std::vector<std::shared_ptr<scene::Texture>> decoded(paths.size());
    std::atomic<size_t> next{0};
    auto work = [&] {
      for (size_t i; (i = next++) < paths.size();) {
        try {
          auto texture = std::make_shared<scene::Texture>();
          texture->pixels = scene::loadTexture(utf8Path(paths[i]));
          texture->fingerprint();
          decoded[i] = std::move(texture);
        } catch (...) {
          // Left null for the caller. Exceptions stay on their thread.
        }
      }
    };
    const unsigned threads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads && t < paths.size(); ++t)
      pool.emplace_back(work);
    work();
    for (auto &thread : pool)
      thread.join();
    return decoded;
  }

  Snapshot load(const Options &options) {
    static std::mutex parserMutex;
    std::lock_guard lock(parserMutex);
    Snapshot result;
    try {
      if (options.path.empty() || !std::isfinite(options.meters) || options.meters < 0 || options.axis > 2)
        throw std::runtime_error("Invalid USD load options");
      TfErrorMark errors;
      // Private layers keep reload parsing independent of an active scene and never
      // modify the capture on disk. Anchor every asset to its authoring layer before
      // replacing layer references with private identifiers, including Windows paths.
      std::map<std::string, SdfLayerRefPtr> layers;
      std::function<SdfLayerRefPtr(std::filesystem::path)> openLayer;
      openLayer = [&](std::filesystem::path path) -> SdfLayerRefPtr {
        path = std::filesystem::absolute(path).lexically_normal();
        const auto key = path.string();
        if (auto found = layers.find(key); found != layers.end())
          return found->second;
        result.dependencies.insert(key);
        // A layer another layer names but which is not on disk is left out, as
        // USD itself composes around an unresolved reference: Remix mods
        // commonly reference material templates (SubUSDs/AperturePBR_*.usda)
        // they do not ship, and author everything that matters over them. It
        // stays a dependency, so the file appearing later reloads the mod. The
        // root layer, and a layer that exists but does not parse, are errors.
        std::error_code missing;
        if (!layers.empty() && !std::filesystem::exists(path, missing)) {
          // The Remix material templates (AperturePBR_Opacity.usda, ..._Translucent.usda)
          // are what make a mod's material overs materials at all: the over itself only
          // carries the inputs. A mod that references one without shipping it gets the
          // template's structure from here (the material, its shader and the MDL the
          // loader tells opaque from translucent by).
          const auto stem = path.stem().string();
          if (!stem.starts_with("AperturePBR_"))
            return nullptr;
          auto layer = SdfLayer::CreateAnonymous(stem + ".usda");
          const std::string text = "#usda 1.0\n"
                                   "def Scope \"Looks\"\n{\n"
                                   "    def Material \"mat_" + stem + "\"\n    {\n"
                                   "        token outputs:mdl:surface.connect = </Looks/mat_" + stem + "/Shader.outputs:out>\n"
                                   "        def Shader \"Shader\"\n        {\n"
                                   "            uniform token info:implementationSource = \"sourceAsset\"\n"
                                   "            uniform asset info:mdl:sourceAsset = @" + stem + ".mdl@\n"
                                   "            uniform token info:mdl:sourceAsset:subIdentifier = \"" + stem + "\"\n"
                                   "            token outputs:out\n"
                                   "        }\n    }\n}\n";
          if (!layer || !layer->ImportFromString(text))
            return nullptr;
          layers.emplace(key, layer);
          return layer;
        }
        auto layer = SdfLayer::OpenAsAnonymous(key);
        if (!layer)
          throw std::runtime_error("Could not open USD layer: " + key);
        layers.emplace(key, layer);
        UsdUtilsModifyAssetPaths(layer, [&](const std::string &authored) {
          if (authored.empty())
            return authored;
          auto normalized = authored;
          std::replace(normalized.begin(), normalized.end(), '\\', '/');
          auto asset = std::filesystem::path(normalized);
          if (asset.is_relative())
            asset = path.parent_path() / asset;
          asset = asset.lexically_normal();
          auto ext = asset.extension().string();
          std::transform(ext.begin(), ext.end(), ext.begin(),
                         [](unsigned char c) { return std::tolower(c); });
          if (ext == ".usd" || ext == ".usda" || ext == ".usdc") {
            // An empty path removes the reference, sublayer or payload.
            const auto opened = openLayer(asset);
            return opened ? opened->GetIdentifier() : std::string();
          }
          // Asset dependencies include absent paths so a corrected file is watched.
          result.dependencies.insert(asset.string());
          return asset.string();
        });
        return layer;
      };
      auto stage = UsdStage::Open(openLayer(options.path));
      if (!stage)
        throw std::runtime_error("Could not compose USD stage: " + options.path);
      result.scene = compose_scene(stage, 1);
      for (const auto &layer : stage->GetUsedLayers())
        if (!layer->GetRealPath().empty())
          result.dependencies.insert(layer->GetRealPath());
      if (!errors.IsClean()) {
        std::string message = "USD composition failed";
        for (auto it = errors.GetBegin(); it != errors.GetEnd(); ++it)
          message += "\n" + it->GetCommentary();
        errors.Clear();
        throw std::runtime_error(message);
      }
      for (const auto &warning : result.scene["warnings"]) {
        if (warning.get<std::string>().starts_with("Unresolved texture "))
          throw std::runtime_error(warning.get<std::string>());
      }
      result.scene["replacements"] = {
          {"meshes", Json::array()}, {"lights", Json::array()}, {"materials", Json::array()}};
      for (const auto &[group, path] :
           std::vector<std::pair<std::string, std::string>>{{"meshes", "/RootNode/meshes"},
                                                            {"lights", "/RootNode/lights"},
                                                            {"materials", "/RootNode/Looks"}}) {
        auto root = stage->GetPrimAtPath(SdfPath(path));
        if (!root)
          continue;
        for (auto prim : root.GetAllChildren()) {
          auto hash = capturedHash(prim.GetPath().GetString());
          if (!hash)
            continue;
          bool preserve = false;
          if (group == "meshes") {
            int flag = 0;
            auto attr = prim.GetAttribute(TfToken("preserveOriginalDrawCall"));
            if (attr && attr.Get(&flag))
              preserve = flag != 0;
            else {
              preserve = !prim.IsA<UsdGeomMesh>() && !prim.GetChild(TfToken("mesh"));
              for (auto spec : prim.GetPrimStack())
                if (spec->HasReferences()) {
                  auto refs = spec->GetReferenceList();
                  if (refs.IsExplicit() || !refs.GetDeletedItems().empty())
                    preserve = false;
                }
            }
          }
          if (!prim.IsActive())
            preserve = false;
          result.scene["replacements"][group].push_back(
              {{"hash", hash}, {"path", prim.GetPath().GetString()}, {"preserve", preserve}});
          if (group == "lights") {
            auto &entry = result.scene["replacements"][group].back();
            entry["hidden"] = !prim.IsActive();
            entry["type"] = prim.GetTypeName().GetString();
            auto &overrides = entry["overrides"];
            overrides = Json::object();
            for (const char *name :
                 {"intensity", "exposure", "radius", "width", "height", "length", "angle", "colorTemperature",
                  "shaping:cone:angle", "shaping:cone:softness", "shaping:focus"}) {
              auto attr = prim.GetAttribute(TfToken(std::string("inputs:") + name));
              float value;
              if (attr && attr.HasAuthoredValueOpinion() && attr.Get(&value))
                overrides[name] = value;
            }
            auto color = prim.GetAttribute(TfToken("inputs:color"));
            GfVec3f rgb;
            if (color && color.HasAuthoredValueOpinion() && color.Get(&rgb))
              overrides["color"] = {rgb[0], rgb[1], rgb[2]};
            auto temperature = prim.GetAttribute(TfToken("inputs:enableColorTemperature"));
            bool enabled;
            if (temperature && temperature.HasAuthoredValueOpinion() && temperature.Get(&enabled))
              overrides["enableColorTemperature"] = enabled;
            auto texture = prim.GetAttribute(TfToken("inputs:texture:file"));
            if (texture && texture.HasAuthoredValueOpinion())
              overrides["texture"] = resolved_texture_path(prim, "inputs:texture:file");
            TfToken visible;
            if (prim.GetAttribute(TfToken("visibility")).Get(&visible) && visible == TfToken("invisible"))
              entry["hidden"] = true;
            if (prim.GetAttribute(TfToken("xformOpOrder")).HasAuthoredValueOpinion()) {
              auto matrix = UsdGeomXformCache().GetLocalToWorldTransform(prim);
              overrides["transform"] = Json::array();
              for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 4; ++col)
                  overrides["transform"].push_back(matrix[row][col]);
            }
          }
          if (group == "meshes" && prim.IsActive()) {
            if (auto bound = UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial()) {
              auto material = compose_material(bound.GetPrim());
              result.scene["replacements"][group].back()["material"] = material["id"];
              bool found = false;
              for (const auto &existing : result.scene["materials"])
                found |= existing["id"] == material["id"];
              if (!found)
                result.scene["materials"].push_back(std::move(material));
            }
          }
          if (group == "materials" && prim.IsActive()) {
            auto material = compose_material(prim);
            material["hash"] = hash;
            bool found = false;
            for (const auto &existing : result.scene["materials"])
              found |= existing["id"] == material["id"];
            if (!found)
              result.scene["materials"].push_back(std::move(material));
          }
        }
      }
      const double sourceMeters = UsdGeomGetStageMetersPerUnit(stage);
      if (!std::isfinite(sourceMeters) || sourceMeters <= 0)
        throw std::runtime_error("Invalid stage metersPerUnit");
      const auto sourceAxis = UsdGeomGetStageUpAxis(stage);
      const auto axis = options.axis == 1   ? UsdGeomTokens->y
                        : options.axis == 2 ? UsdGeomTokens->z
                                            : sourceAxis;
      GfMatrix4d conversion(1);
      if (axis != sourceAxis)
        conversion.SetRotate(GfRotation(GfVec3d(1, 0, 0), axis == UsdGeomTokens->y ? -90 : 90));
      GfMatrix4d scale(1);
      scale.SetScale(sourceMeters / (options.meters > 0 ? options.meters : sourceMeters));
      conversion *= scale;
      for (const char *group : {"meshes", "lights", "cameras"})
        for (auto &item : result.scene[group]) {
          GfMatrix4d matrix(1);
          for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
              matrix[row][col] = item["transform"][row * 4 + col].get<double>();
          matrix *= conversion;
          for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
              item["transform"][row * 4 + col] = matrix[row][col];
          item["hash"] = capturedHash(item.value("source_path", item["id"].get<std::string>()));
        }
      for (auto &entry : result.scene["replacements"]["lights"]) {
        auto &overrides = entry["overrides"];
        if (overrides.contains("transform")) {
          GfMatrix4d matrix(1);
          for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
              matrix[r][c] = overrides["transform"][r * 4 + c].get<double>();
          matrix *= conversion;
          for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
              overrides["transform"][r * 4 + c] = matrix[r][c];
        } else {
          // Authored dimensions of a property-only override are in mod units.
          for (const char *key : {"radius", "width", "height", "length"})
            if (overrides.contains(key))
              overrides[key] = overrides[key].get<double>() * sourceMeters /
                               (options.meters > 0 ? options.meters : sourceMeters);
        }
      }
      for (auto &material : result.scene["materials"]) {
        material["hash"] = capturedHash(material["id"]);
        for (auto it = material.begin(); it != material.end(); ++it)
          if (it.key().ends_with("_texture") && it.value().is_string() &&
              !it.value().get<std::string>().empty())
            result.dependencies.insert(it.value());
      }
      for (const auto &light : result.scene["lights"])
        for (const char *key : {"texture", "ies", "cookie"})
          if (!light.value(key, std::string()).empty())
            result.dependencies.insert(light[key]);
      // Unbound material replacements are composed after the visible scene.
      // Their asset errors must reject the complete set too.
      for (const auto &material : result.scene["materials"])
        for (const auto &warning : material.value("warnings", Json::array()))
          result.scene["warnings"].push_back(warning);
      for (const auto &warning : result.scene["warnings"])
        if (warning.get<std::string>().starts_with("Unresolved texture "))
          throw std::runtime_error(warning.get<std::string>());
      if (options.preloadTextures) {
        // Textures the snapshot's geometry and lights draw with. Materials
        // referenced only as replacements load when first drawn (mods.cpp):
        // large mods reference far more than any one map shows.
        std::set<std::string> used{"default"};
        for (const auto &mesh : result.scene["meshes"])
          for (const auto &surface : mesh.value("surfaces", Json::array()))
            used.insert(surface.value("material", std::string("default")));
        for (const auto &entry : result.scene["replacements"]["meshes"])
          if (entry.contains("material"))
            used.insert(entry["material"].get<std::string>());
        std::vector<std::string> paths;
        std::set<std::string> seen;
        auto want = [&](const std::string &path) {
          if (!path.empty() && !result.textures.contains(path) && seen.insert(path).second)
            paths.push_back(path);
        };
        for (const auto &material : result.scene["materials"])
          if (used.contains(material.value("id", std::string()))) {
            for (const char *key : {"albedo_texture", "normal_texture", "roughness_texture", "metallic_texture",
                                    "emissive_texture"})
              want(material.value(key, std::string()));
            if (const auto film = material.find("iridescence"); film != material.end())
              want(film->value("texture", std::string()));
          }
        for (const auto &light : result.scene["lights"])
          for (const char *key : {"texture", "cookie"})
            want(light.value(key, std::string()));
        for (const auto &light : result.scene["replacements"]["lights"])
          want(light["overrides"].value("texture", std::string()));
        auto decoded = decodeTextures(paths);
        for (size_t i = 0; i < paths.size(); ++i) {
          if (!decoded[i])
            throw std::runtime_error("Could not decode texture: " + paths[i]);
          result.textures.emplace(paths[i], std::move(decoded[i]));
        }
      }
      result.scene["meters_per_unit"] = options.meters > 0 ? options.meters : sourceMeters;
      result.scene["up_axis"] = axis.GetString();
      return result;
    } catch (const std::exception &e) {
      throw LoadError(e.what(), std::move(result.dependencies));
    }
  }
} // namespace remix::usd
