// Static capture composition adapted from the Godot toolkit. CPU-only OpenUSD 25.11.
#include "capture_categories.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <pxr/usd/usdShade/shader.h>
#include <remix/remix_c.h>
#include <sstream>

PXR_NAMESPACE_USING_DIRECTIVE
namespace remix::usd {
  namespace {
    struct Category {
      const char *attribute;
      const char *option;
      uint32_t bit;
    };
    // Explicit public API bits: the runtime's internal enum has a different order.
    constexpr Category categories[] = {
        {"world_ui", "rtx.worldSpaceUiTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_UI},
        {"world_matte", "rtx.worldSpaceUiBackgroundTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_MATTE},
        {"sky", "rtx.skyBoxTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_SKY},
        {"ignore", "rtx.ignoreTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE},
        {"ignore_lights", "rtx.ignoreLights", REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_LIGHTS},
        {"ignore_anti_culling", "rtx.antiCulling.antiCullingTextures",
         REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_ANTI_CULLING},
        {"ignore_motion_blur", "rtx.postfx.motionBlurMaskOutTextures",
         REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_MOTION_BLUR},
        {"ignore_opacity_micromap", "rtx.opacityMicromapIgnoreTextures",
         REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_OPACITY_MICROMAP},
        {"hidden", "rtx.hideInstanceTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN},
        {"particle", "rtx.particleTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE},
        {"beam", "rtx.beamTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_BEAM},
        {"decal_Static", "rtx.decalTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC},
        {"decal_dynamic", "rtx.dynamicDecalTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC},
        {"decal_single_offset", "rtx.singleOffsetDecalTextures",
         REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_SINGLE_OFFSET},
        {"decal_no_offset", "rtx.nonOffsetDecalTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_NO_OFFSET},
        {"alpha_blend_to_cutout", "rtx.cutoutTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_ALPHA_BLEND_TO_CUTOUT},
        {"terrain", "rtx.terrainTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_TERRAIN},
        {"animated_water", "rtx.animatedWaterTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_ANIMATED_WATER},
        {"third_person_player_model", "rtx.playerModelTextures",
         REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_MODEL},
        {"third_person_player_body", "rtx.playerModelBodyTextures",
         REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_BODY},
        {"ignore_baked_lighting", "rtx.ignoreBakedLightingTextures",
         REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_BAKED_LIGHTING},
        {"ignore_alpha_channel", "rtx.ignoreAlphaOnTextures",
         REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_ALPHA_CHANNEL},
        {"ignore_transparency_layer", "rtx.ignoreTransparencyLayerTextures",
         REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_TRANSPARENCY_LAYER},
        {"particle_emitter", "rtx.particleEmitterTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE_EMITTER},
        {"smooth_normals", "rtx.smoothNormalsTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_SMOOTH_NORMALS},
        {"hair_cards", "rtx.hairCardTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_HAIR_CARDS},
        {"view_model", "", REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL},
        {"legacy_emissive", "rtx.legacyEmissiveTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_LEGACY_EMISSIVE},
        {"first_person_player_shadow", "", REMIXAPI_INSTANCE_CATEGORY_BIT_FIRST_PERSON_PLAYER_SHADOW},
    };
    std::string trim(std::string value) {
      const auto first = value.find_first_not_of(" \t\r\n");
      if (first == std::string::npos)
        return {};
      return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    }
    std::optional<uint64_t> hash_value(std::string value) {
      value = trim(value);
      if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0)
        value.erase(0, 2);
      if (value.empty() || value.size() > 16)
        return {};
      uint64_t result = 0;
      auto parsed = std::from_chars(value.data(), value.data() + value.size(), result, 16);
      if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size())
        return {};
      return result;
    }
    std::optional<uint64_t> asset_hash(const std::string &asset) {
      auto normalized = asset;
      std::replace(normalized.begin(), normalized.end(), '\\', '/');
      const auto stem = fs::path(normalized).stem().string();
      // Captured textures use <16-digit texture hash>.dds, not a material hash.
      if (stem.size() != 16 && !(stem.size() == 18 && stem.substr(0, 2) == "0x"))
        return {};
      return hash_value(stem);
    }
    using Options = std::map<std::string, std::map<uint64_t, bool>>;
    bool recognized(const std::string &key) {
      if (key.empty())
        return false;
      if (key == "rtx.skyBoxGeometries")
        return true;
      for (const auto &category : categories)
        if (key == category.option)
          return true;
      return false;
    }
    void parse_line(std::string line, Options &options, Json &warnings, const std::string &source) {
      line = line.substr(0, line.find_first_of("#;"));
      const auto equal = line.find('=');
      if (equal == std::string::npos)
        return;
      const auto key = trim(line.substr(0, equal));
      if (!recognized(key))
        return;
      auto value = trim(line.substr(equal + 1));
      if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        value = value.substr(1, value.size() - 2);
      std::map<uint64_t, bool> hashes;
      std::istringstream input(value);
      bool invalid = false;
      for (std::string part; std::getline(input, part, ',');) {
        part = trim(part);
        if (part.empty())
          continue;
        const bool negative = part.front() == '-';
        if (negative || part.front() == '+')
          part.erase(0, 1);
        if (auto hash = hash_value(part)) {
          // Like Remix's HashSetLayer, an exclusion wins if both signs occur.
          if (negative || !hashes.count(*hash))
            hashes[*hash] = !negative;
        } else
          invalid = true;
      }
      // Reject malformed assignments as a whole, keeping lower-priority opinions
      // instead of applying a partially parsed category list.
      if (invalid)
        warnings.push_back("Invalid category hash list in " + source + ": " + key);
      else
        options[key] = std::move(hashes);
    }
    fs::path find_config(fs::path capture) {
      std::error_code error;
      capture = fs::weakly_canonical(capture, error);
      if (error)
        return {};
      for (auto directory = capture.parent_path(); !directory.empty();) {
        const auto candidate = directory / "rtx.conf";
        if (fs::is_regular_file(candidate, error))
          return candidate;
        const auto parent = directory.parent_path();
        if (parent == directory)
          break;
        directory = parent;
      }
      return {};
    }
  } // namespace

  const char *categoryAttribute(uint32_t bit) {
    for (const auto &category : categories)
      if (category.bit == bit)
        return category.attribute;
    return nullptr;
  }

  CaptureCategories::CaptureCategories(const UsdStageRefPtr &stage, Json &warnings) {
    Options options;
    auto apply = [&](const Options &layer) {
      auto merge = [](auto &table, uint32_t bit, const auto &hashes) {
        for (const auto &[hash, enabled] : hashes) {
          auto &opinion = table[hash];
          opinion.mask |= bit;
          opinion.values = (opinion.values & ~bit) | (enabled ? bit : 0);
        }
      };
      for (const auto &category : categories)
        if (auto entry = layer.find(category.option); entry != layer.end())
          merge(textures_, category.bit, entry->second);
      if (auto sky = layer.find("rtx.skyBoxGeometries"); sky != layer.end())
        merge(geometries_, REMIXAPI_INSTANCE_CATEGORY_BIT_SKY, sky->second);
    };
    const auto settings = stage->GetPrimAtPath(SdfPath("/remix_settings"));
    if (settings) {
      auto attr = settings.GetAttribute(TfToken("remix:remix_config"));
      // An authored block/empty namespaced value also suppresses the legacy data.
      bool authored = false;
      if (attr)
        for (const auto &spec : attr.GetPropertyStack())
          authored |= spec->HasDefaultValue();
      if (!authored)
        attr = settings.GetAttribute(TfToken("remix_config"));
      VtStringArray lines;
      if (attr && attr.Get(&lines)) {
        for (const auto &line : lines)
          parse_line(line, options, warnings, "capture");
        if (!options.empty())
          sources_.push_back("Embedded capture settings");
      }
    }
    apply(options);
    std::set<fs::path> files;
    std::string executable;
    for (const auto &layer : stage->GetLayerStack(false)) {
      const auto data = layer->GetCustomLayerData();
      auto kind = data.find("lightspeed_layer_type");
      if (kind == data.end() || !kind->second.IsHolding<std::string>() ||
          kind->second.Get<std::string>() != "capture")
        continue;
      auto name = data.find("lightspeed_exe_name");
      if (name != data.end() && name->second.IsHolding<std::string>())
        executable = name->second.Get<std::string>();
      if (auto capture = UsdStage::Open(layer))
        for (const auto &dependency : capture->GetUsedLayers())
          capture_layers_.insert(dependency->GetIdentifier());
      const auto file = find_config(layer->GetRealPath());
      if (!file.empty())
        files.insert(file);
    }
    for (const auto &path : files) {
      std::ifstream input(path);
      if (!input) {
        warnings.push_back("Could not read category configuration: " + path.string());
        continue;
      }
      options.clear();
      bool active = true;
      for (std::string line; std::getline(input, line);) {
        line = trim(line);
        if (line.rfind("\xef\xbb\xbf", 0) == 0)
          line.erase(0, 3);
        if (!line.empty() && line.front() == '[') {
          const auto end = line.find(']');
          active = end != std::string::npos && !executable.empty() && line.substr(1, end - 1) == executable;
        } else if (active)
          parse_line(line, options, warnings, path.string());
      }
      apply(options);
      sources_.push_back(path.generic_string());
    }
  }

  std::optional<uint64_t> CaptureCategories::texture_hash(const UsdShadeMaterial &material) {
    if (!material)
      return {};
    const auto id = material.GetPath().GetString();
    if (auto found = material_hashes_.find(id); found != material_hashes_.end())
      return found->second;
    auto shader = material.ComputeSurfaceSource(TfToken("mdl")).GetPrim();
    if (!shader)
      for (const auto &child : material.GetPrim().GetChildren())
        if (child.IsA<UsdShadeShader>()) {
          shader = child;
          break;
        }
    std::optional<uint64_t> result;
    if (shader)
      for (const char *name : {"inputs:diffuse_texture", "inputs:transmittance_texture"}) {
        const auto attr = shader.GetAttribute(TfToken(name));
        if (!attr)
          continue;
        // Preserve the captured texture identity when the material was replaced.
        for (const auto &spec : attr.GetPropertyStack()) {
          const auto value = spec->GetDefaultValue();
          if (capture_layers_.count(spec->GetLayer()->GetIdentifier()) && value.IsHolding<SdfAssetPath>()) {
            result = asset_hash(value.Get<SdfAssetPath>().GetAssetPath());
            if (result)
              break;
          }
        }
        if (!result) {
          SdfAssetPath value;
          if (attr.Get(&value))
            result = asset_hash(value.GetAssetPath());
        }
        if (result)
          break;
      }
    material_hashes_[id] = result;
    return result;
  }

  uint32_t CaptureCategories::flags(const UsdPrim &mesh, const UsdShadeMaterial &material) {
    uint32_t baseline = 0, overrides = 0, override_mask = 0;
    std::vector<UsdPrim> ancestors;
    for (auto prim = mesh; prim && !prim.IsPseudoRoot(); prim = prim.GetParent())
      ancestors.push_back(prim);
    std::optional<uint64_t> geometry;
    for (auto i = ancestors.rbegin(); i != ancestors.rend(); ++i) {
      const auto name = i->GetName().GetString();
      if ((name.rfind("mesh_", 0) == 0 || name.rfind("inst_", 0) == 0) && name.size() >= 21)
        geometry = hash_value(name.substr(5, 16));
      for (const auto &category : categories) {
        const auto attr = i->GetAttribute(TfToken(std::string("remix_category:") + category.attribute));
        if (!attr)
          continue;
        for (const auto &spec : attr.GetPropertyStack()) {
          if (!spec->HasDefaultValue())
            continue;
          const auto value = spec->GetDefaultValue();
          const bool enabled = value.IsHolding<bool>() && value.Get<bool>();
          const bool captured = capture_layers_.count(spec->GetLayer()->GetIdentifier());
          auto &bits = captured ? baseline : overrides;
          bits = (bits & ~category.bit) | (enabled ? category.bit : 0);
          if (!captured)
            override_mask |= category.bit;
          break;
        }
      }
    }
    uint32_t mask = 0, matched = 0;
    if (auto texture = texture_hash(material)) {
      if (auto found = textures_.find(*texture); found != textures_.end()) {
        mask |= found->second.mask;
        matched |= found->second.values;
      }
    }
    if (geometry) {
      if (auto found = geometries_.find(*geometry); found != geometries_.end()) {
        mask |= found->second.mask;
        matched |= found->second.values;
      }
    }
    const uint32_t flags = (baseline & ~mask) | matched;
    return (flags & ~override_mask) | overrides;
  }
} // namespace remix::usd
