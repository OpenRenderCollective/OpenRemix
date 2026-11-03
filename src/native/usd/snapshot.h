#pragma once
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <pxr/usd/usd/stage.h>
#include <string>
namespace remix::usd {
  using Json = nlohmann::json;
  namespace fs = std::filesystem;
  // A material JSON subsurface's textures (scene.cpp material_json).
  inline constexpr const char *subsurfaceTextureKeys[] = {"transmittance_texture", "thickness_texture",
                                                          "single_scattering_texture", "radius_texture"};
  // What compose_scene composes: the whole stage, or the subtree at `root`;
  // meshes under `deferMeshesUnder` left out (composed later by root).
  struct ComposeScope {
    std::string root;
    std::string deferMeshesUnder;
  };
  Json compose_scene(const pxr::UsdStageRefPtr &, uint64_t, const ComposeScope &scope = {});
  Json compose_lights(const pxr::UsdStageRefPtr &, uint64_t);
  Json compose_material(const pxr::UsdPrim &);
  std::string resolved_texture_path(const pxr::UsdPrim &, const char *);
  // A light's attribute `name` (without "inputs:"): as "inputs:<name>", else
  // the name USD lights had before it, when only that is authored.
  pxr::UsdAttribute light_attribute(const pxr::UsdPrim &, const char *name);
} // namespace remix::usd
