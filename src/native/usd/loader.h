#pragma once
#include "usd/snapshot.h"
#include <map>
#include <memory>
#include <remix/remix_usd_ext.h>
#include <set>

namespace remix::scene {
  struct Texture;
}

namespace remix::usd {
  struct Options {
    std::string path;
    double meters = 0;
    uint32_t axis = 0;
    bool preloadTextures = false;
    // False (mods): a texture that is not on disk or does not decode is left
    // out of its material with a warning, as RTX Remix leaves it, instead of
    // failing the whole load.
    bool strictAssets = true;
    // Mods: the meshes of replacements (/RootNode/meshes/mesh_<hash>/...) are
    // not composed; the stage stays (Snapshot::deferred) for composing one
    // replacement's when it is first drawn (composeReplacement). A large mod's
    // geometry composed at once takes tens of gigabytes.
    bool deferReplacementMeshes = false;
  };
  struct DeferredStage;
  // The root layer's customLayerData key of a mod's light scale (a double).
  inline constexpr const char *lightScaleKey = "openremix:lightScale";
  struct Snapshot {
    Json scene;
    std::set<std::string> dependencies;
    std::map<std::string, std::shared_ptr<scene::Texture>> textures;
    std::shared_ptr<DeferredStage> deferred; // Options::deferReplacementMeshes
  };
  struct LoadError : std::runtime_error {
    std::set<std::string> dependencies;
    LoadError(const std::string &message, std::set<std::string> paths)
        : std::runtime_error(message), dependencies(std::move(paths)) {}
  };
  Snapshot load(const Options &options);
  // The meshes of the replacement at `root` in host units, with the materials
  // they use: {"meshes", "materials", "warnings"}. Any thread; one call at a
  // time per stage.
  Json composeReplacement(DeferredStage &stage, const std::string &root);
  // The texture files a composed material reads (empty entries omitted).
  std::vector<std::string> materialTextureFiles(const Json &material);
  // Decodes texture files in parallel; null entries could not be decoded.
  std::vector<std::shared_ptr<scene::Texture>> decodeTextures(const std::vector<std::string> &paths);
  uint64_t capturedHash(const std::string &path);
} // namespace remix::usd
