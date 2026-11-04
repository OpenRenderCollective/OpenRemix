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
  Json compose_scene(const pxr::UsdStageRefPtr &, uint64_t);
  Json compose_lights(const pxr::UsdStageRefPtr &, uint64_t);
  Json compose_material(const pxr::UsdPrim &);
  std::string resolved_texture_path(const pxr::UsdPrim &, const char *);
} // namespace remix::usd
