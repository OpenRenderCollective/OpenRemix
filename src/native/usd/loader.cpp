#include "usd/loader.h"
#include "scene/images.h"
#include "scene/types.h"
#include "platform/utf8_path.h"
#include <charconv>
#include <algorithm>
#include <set>
#include <thread>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
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
  namespace {
    // The parts of a material (scene::ModParts) its shader authors in this
    // stage: of a mod's `over` of a captured material, what the mod changes.
    uint32_t authoredParts(const UsdPrim &material) {
      // Found as scene.cpp's material_json finds it: an override's shader
      // is an untyped `over` too.
      UsdPrim shader;
      if (material.IsA<UsdShadeMaterial>())
        if (auto connected = UsdShadeMaterial(material).ComputeSurfaceSource(TfToken("mdl")))
          shader = connected.GetPrim();
      if (!shader)
        shader = material.GetChild(TfToken("Shader"));
      for (auto child : material.GetAllChildren())
        if (!shader && child.IsA<UsdShadeShader>())
          shader = child;
      if (!shader)
        shader = material;
      using namespace scene::ModParts;
      static const std::vector<std::pair<uint32_t, std::vector<const char *>>> parts{
          {color, {"inputs:diffuse_color_constant", "inputs:diffuse_texture", "inputs:opacity_constant"}},
          {roughness, {"inputs:reflection_roughness_constant", "inputs:reflectionroughness_texture"}},
          {metallic, {"inputs:metallic_constant", "inputs:metallic_texture"}},
          {normal, {"inputs:normalmap_texture", "inputs:encoding"}},
          {emission,
           {"inputs:enable_emission", "inputs:emissive_intensity", "inputs:emissive_color_constant",
            "inputs:emissive_mask_texture"}},
          {transmission,
           {"info:mdl:sourceAsset", "inputs:ior_constant", "inputs:transmittance_color", "inputs:transmittance_texture",
            "inputs:transmittance_measurement_distance", "inputs:thin_walled", "inputs:thin_wall_thickness",
            "inputs:use_diffuse_layer"}},
          {film,
           {"inputs:enable_thin_film", "inputs:thin_film_thickness_constant",
            "inputs:thin_film_thickness_from_albedo_alpha"}},
          {subsurface,
           {"inputs:subsurface_diffusion_profile", "inputs:subsurface_radius", "inputs:subsurface_radius_scale",
            "inputs:subsurface_max_sample_radius", "inputs:subsurface_volumetric_anisotropy",
            "inputs:subsurface_transmittance_color", "inputs:subsurface_measurement_distance",
            "inputs:subsurface_single_scattering_albedo", "inputs:subsurface_transmittance_texture",
            "inputs:subsurface_thickness_texture", "inputs:subsurface_single_scattering_texture",
            "inputs:subsurface_radius_texture"}},
          {alpha,
           {"inputs:use_legacy_alpha_state", "inputs:blend_enabled", "inputs:blend_type", "inputs:inverted_blend",
            "inputs:alpha_test_type", "inputs:alpha_test_reference_value"}},
          {sampling, {"inputs:filter_mode", "inputs:wrap_mode_u", "inputs:wrap_mode_v"}}};
      uint32_t result = 0;
      for (const auto &[bit, names] : parts)
        for (const char *name : names)
          if (auto attr = shader.GetAttribute(TfToken(name)); attr && attr.HasAuthoredValue())
            result |= bit;
      // OpenRemix's own film and subsurface attributes (openremix:*).
      for (const auto &attr : shader.GetAttributes()) {
        const auto name = attr.GetName().GetString();
        if (!attr.HasAuthoredValue())
          continue;
        if (name.rfind("openremix:iridescence:", 0) == 0)
          result |= film;
        else if (name.rfind("openremix:subsurface:", 0) == 0)
          result |= subsurface;
      }
      return result;
    }
  } // namespace
  std::vector<std::shared_ptr<scene::Texture>> decodeTextures(const std::vector<std::string> &paths, uint32_t maxEdge,
                                                              std::atomic<uint32_t> *progress) {
    // Half the cores leave room for the host still rendering meanwhile.
    std::vector<std::shared_ptr<scene::Texture>> decoded(paths.size());
    std::atomic<size_t> next{0};
    auto work = [&] {
      for (size_t i; (i = next++) < paths.size();) {
        try {
          auto texture = std::make_shared<scene::Texture>();
          texture->pixels =
              maxEdge ? scene::loadTexturePreview(utf8Path(paths[i]), maxEdge) : scene::loadTexture(utf8Path(paths[i]));
          texture->fingerprint();
          decoded[i] = std::move(texture);
        } catch (...) {
          // Left null for the caller. Exceptions stay on their thread.
        }
        if (progress)
          ++*progress;
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

  namespace {
    // Composed meshes, lights and cameras in host coordinates, with their
    // capture hashes.
    void toHost(Json &scene, const GfMatrix4d &conversion) {
      for (const char *group : {"meshes", "lights", "cameras"})
        for (auto &item : scene[group]) {
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
    }
  } // namespace
  std::vector<std::string> materialTextureFiles(const Json &material) {
    std::vector<std::string> paths;
    for (const char *key :
         {"albedo_texture", "normal_texture", "roughness_texture", "metallic_texture", "emissive_texture"})
      if (auto path = material.value(key, std::string()); !path.empty())
        paths.push_back(path);
    if (const auto film = material.find("iridescence"); film != material.end())
      if (auto path = film->value("texture", std::string()); !path.empty())
        paths.push_back(path);
    if (const auto sss = material.find("subsurface"); sss != material.end())
      for (const char *key : subsurfaceTextureKeys)
        if (auto path = sss->value(key, std::string()); !path.empty())
          paths.push_back(path);
    return paths;
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
      // A mod is hundreds of layers, and a layer can name a hundred others: they
      // open on several threads as soon as what names them has, rather than one
      // by one as its paths are rewritten.
      struct Layer {
        std::once_flag once;
        SdfLayerRefPtr layer;
      };
      std::mutex mutex; // Everything here but what a Layer's once guards.
      std::condition_variable wake;
      std::map<std::string, std::shared_ptr<Layer>> layers;
      std::vector<std::pair<SdfLayerRefPtr, std::filesystem::path>> rewrites;
      std::vector<std::filesystem::path> opens;
      size_t rewriting = 0;
      const auto assetOf = [](const std::filesystem::path &parent, const std::string &authored) {
        auto normalized = authored;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        auto asset = std::filesystem::path(normalized);
        if (asset.is_relative())
          asset = parent.parent_path() / asset;
        return asset.lexically_normal();
      };
      std::optional<std::string> failure;
      const auto makeLayer = [&](const std::filesystem::path &path, const std::string &key,
                                 bool root) -> SdfLayerRefPtr {
        // A layer another layer names but which is not on disk is left out, as
        // USD itself composes around an unresolved reference: Remix mods
        // commonly reference material templates (SubUSDs/AperturePBR_*.usda)
        // they do not ship, and author everything that matters over them. It
        // stays a dependency, so the file appearing later reloads the mod. The
        // root layer, and a layer that exists but does not parse, are errors.
        std::error_code missing;
        if (!root && !std::filesystem::exists(path, missing)) {
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
                                   "    def Material \"mat_" +
                                   stem +
                                   "\"\n    {\n"
                                   "        token outputs:mdl:surface.connect = </Looks/mat_" +
                                   stem +
                                   "/Shader.outputs:out>\n"
                                   "        def Shader \"Shader\"\n        {\n"
                                   "            uniform token info:implementationSource = \"sourceAsset\"\n"
                                   "            uniform asset info:mdl:sourceAsset = @" +
                                   stem +
                                   ".mdl@\n"
                                   "            uniform token info:mdl:sourceAsset:subIdentifier = \"" +
                                   stem +
                                   "\"\n"
                                   "            token outputs:out\n"
                                   "        }\n    }\n}\n";
          if (!layer || !layer->ImportFromString(text))
            return nullptr;
          return layer;
        }
        auto layer = SdfLayer::OpenAsAnonymous(key);
        if (!layer)
          throw std::runtime_error("Could not open USD layer: " + key);
        const auto children = layer->GetCompositionAssetDependencies();
        {
          std::lock_guard lock(mutex);
          rewrites.emplace_back(layer, path);
          for (const auto &child : children)
            if (!child.empty())
              opens.push_back(assetOf(path, child));
        }
        wake.notify_all();
        return layer;
      };
      const auto openLayer = [&](std::filesystem::path path, bool root = false) -> SdfLayerRefPtr {
        path = std::filesystem::absolute(path).lexically_normal();
        const auto key = path.string();
        std::shared_ptr<Layer> entry;
        {
          std::lock_guard lock(mutex);
          result.dependencies.insert(key);
          auto &slot = layers[key];
          if (!slot)
            slot = std::make_shared<Layer>();
          entry = slot;
        }
        std::call_once(entry->once, [&] { entry->layer = makeLayer(path, key, root); });
        return entry->layer;
      };
      const auto rewrite = [&](const SdfLayerRefPtr &layer, const std::filesystem::path &path) {
        UsdUtilsModifyAssetPaths(layer, [&](const std::string &authored) {
          if (authored.empty())
            return authored;
          const auto asset = assetOf(path, authored);
          auto ext = asset.extension().string();
          std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
          if (ext == ".usd" || ext == ".usda" || ext == ".usdc") {
            // An empty path removes the reference, sublayer or payload.
            const auto opened = openLayer(asset);
            return opened ? opened->GetIdentifier() : std::string();
          }
          // Asset dependencies include absent paths so a corrected file is watched.
          {
            std::lock_guard lock(mutex);
            result.dependencies.insert(asset.string());
          }
          return asset.string();
        });
      };
      if (options.progress)
        options.progress->enter(LoadStage::Opening, 0);
      const auto rootLayer = openLayer(options.path, true);
      {
        const auto work = [&] {
          std::unique_lock lock(mutex);
          for (;;) {
            wake.wait(lock, [&] { return failure || !opens.empty() || !rewrites.empty() || !rewriting; });
            if (failure || (opens.empty() && rewrites.empty() && !rewriting))
              break;
            if (!opens.empty()) {
              auto path = std::move(opens.back());
              opens.pop_back();
              ++rewriting;
              lock.unlock();
              std::optional<std::string> error;
              try {
                auto ext = path.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
                if (ext == ".usd" || ext == ".usda" || ext == ".usdc")
                  openLayer(path);
              } catch (const std::exception &e) {
                error = e.what();
              } catch (...) {
                error = "Could not read USD layer: " + path.string();
              }
              lock.lock();
              --rewriting;
              if (error && !failure)
                failure = std::move(error);
              wake.notify_all();
              continue;
            }
            auto [layer, path] = std::move(rewrites.back());
            rewrites.pop_back();
            ++rewriting;
            lock.unlock();
            std::optional<std::string> error;
            try {
              rewrite(layer, path);
            } catch (const std::exception &e) {
              error = e.what();
            } catch (...) {
              error = "Could not read USD layer: " + path.string();
            }
            lock.lock();
            --rewriting;
            if (error && !failure)
              failure = std::move(error);
            wake.notify_all();
          }
          wake.notify_all();
        };
        const unsigned threads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < threads; ++t)
          pool.emplace_back(work);
        work();
        for (auto &thread : pool)
          thread.join();
      }
      if (failure)
        throw std::runtime_error(*failure);
      auto stage = UsdStage::Open(rootLayer);
      if (!stage)
        throw std::runtime_error("Could not compose USD stage: " + options.path);
      const double sourceMeters = UsdGeomGetStageMetersPerUnit(stage);
      if (!std::isfinite(sourceMeters) || sourceMeters <= 0)
        throw std::runtime_error("Invalid stage metersPerUnit");
      const auto sourceAxis = UsdGeomGetStageUpAxis(stage);
      const auto axis = options.axis == 1 ? UsdGeomTokens->y : options.axis == 2 ? UsdGeomTokens->z : sourceAxis;
      GfMatrix4d conversion(1);
      if (axis != sourceAxis)
        conversion.SetRotate(GfRotation(GfVec3d(1, 0, 0), axis == UsdGeomTokens->y ? -90 : 90));
      GfMatrix4d scale(1);
      scale.SetScale(sourceMeters / (options.meters > 0 ? options.meters : sourceMeters));
      conversion *= scale;
      ComposeScope scope;
      if (options.meshSink) {
        scope.hostBones = true;
        scope.progress = options.progress;
        scope.meshSink = [&](ComposedMesh &&mesh) {
          GfMatrix4d matrix(1);
          for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
              matrix[row][col] = mesh.transform[row * 4 + col];
          matrix *= conversion;
          for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
              mesh.transform[row * 4 + col] = matrix[row][col];
          options.meshSink(std::move(mesh));
        };
      }
      result.scene = compose_scene(stage, 1, scope);
      // A mod's light scale (mods.h ModInfo::authoredLightScale).
      const auto custom = stage->GetRootLayer()->GetCustomLayerData();
      if (const auto found = custom.find(lightScaleKey); found != custom.end()) {
        if (found->second.IsHolding<double>())
          result.scene["light_scale"] = found->second.UncheckedGet<double>();
        else if (found->second.IsHolding<float>())
          result.scene["light_scale"] = double(found->second.UncheckedGet<float>());
      }
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
      if (options.strictAssets)
        for (const auto &warning : result.scene["warnings"]) {
          if (warning.get<std::string>().starts_with("Unresolved texture "))
            throw std::runtime_error(warning.get<std::string>());
        }
      result.scene["replacements"] = {
          {"meshes", Json::array()}, {"lights", Json::array()}, {"materials", Json::array()}};
      if (options.progress) {
        const auto root = stage->GetPrimAtPath(SdfPath("/RootNode/Looks"));
        uint32_t count = 0;
        if (root)
          for (auto child : root.GetAllChildren())
            (void)child, ++count;
        options.progress->enter(LoadStage::Materials, count);
      }
      for (const auto &[group, path] : std::vector<std::pair<std::string, std::string>>{
               {"meshes", "/RootNode/meshes"}, {"lights", "/RootNode/lights"}, {"materials", "/RootNode/Looks"}}) {
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
              auto attr = light_attribute(prim, name);
              float value;
              if (attr && attr.HasAuthoredValueOpinion() && attr.Get(&value))
                overrides[name] = value;
            }
            auto color = light_attribute(prim, "color");
            GfVec3f rgb;
            if (color && color.HasAuthoredValueOpinion() && color.Get(&rgb))
              overrides["color"] = {rgb[0], rgb[1], rgb[2]};
            auto temperature = light_attribute(prim, "enableColorTemperature");
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
          if (group == "materials" && options.progress)
            ++options.progress->done;
          if (group == "materials" && prim.IsActive()) {
            auto material = compose_material(prim);
            material["hash"] = hash;
            // An `over` of a captured material (no definition of its own)
            // changes only the parts it authors (scene::ModParts).
            if (prim.GetSpecifier() == SdfSpecifierOver)
              result.scene["replacements"][group].back()["authored"] = authoredParts(prim);
            bool found = false;
            for (const auto &existing : result.scene["materials"])
              found |= existing["id"] == material["id"];
            if (!found)
              result.scene["materials"].push_back(std::move(material));
          }
        }
      }
      toHost(result.scene, conversion);
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
              overrides[key] =
                  overrides[key].get<double>() * sourceMeters / (options.meters > 0 ? options.meters : sourceMeters);
        }
      }
      for (auto &material : result.scene["materials"]) {
        material["hash"] = capturedHash(material["id"]);
        for (auto it = material.begin(); it != material.end(); ++it)
          if (it.key().ends_with("_texture") && it.value().is_string() && !it.value().get<std::string>().empty())
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
      if (options.strictAssets)
        for (const auto &warning : result.scene["warnings"])
          if (warning.get<std::string>().starts_with("Unresolved texture "))
            throw std::runtime_error(warning.get<std::string>());
      if (options.preloadTextures) {
        std::vector<std::string> paths;
        std::set<std::string> seen;
        auto want = [&](const std::string &path) {
          if (!path.empty() && !result.textures.contains(path) && seen.insert(path).second)
            paths.push_back(path);
        };
        for (const auto &material : result.scene["materials"])
          for (const auto &path : materialTextureFiles(material))
            want(path);
        for (const auto &light : result.scene["lights"])
          for (const char *key : {"texture", "cookie"})
            want(light.value(key, std::string()));
        for (const auto &light : result.scene["replacements"]["lights"])
          want(light["overrides"].value("texture", std::string()));
        auto decoded = decodeTextures(paths);
        for (size_t i = 0; i < paths.size(); ++i) {
          if (decoded[i]) {
            result.textures.emplace(paths[i], std::move(decoded[i]));
            continue;
          }
          if (options.strictAssets)
            throw std::runtime_error("Could not decode texture: " + paths[i]);
          // Left out wherever it is used.
          result.scene["warnings"].push_back("Undecodable texture " + paths[i]);
          auto drop = [&](Json &object, const char *key) {
            if (object.is_object() && object.value(key, std::string()) == paths[i])
              object[key] = "";
          };
          for (auto &material : result.scene["materials"]) {
            for (auto it = material.begin(); it != material.end(); ++it)
              if (it.key().ends_with("_texture") && it.value().is_string() && it.value() == paths[i])
                it.value() = "";
            if (material.contains("iridescence"))
              drop(material["iridescence"], "texture");
            if (material.contains("subsurface"))
              for (const char *key : subsurfaceTextureKeys)
                drop(material["subsurface"], key);
          }
          for (auto &light : result.scene["lights"]) {
            drop(light, "texture");
            drop(light, "cookie");
          }
          for (auto &light : result.scene["replacements"]["lights"])
            drop(light["overrides"], "texture");
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
