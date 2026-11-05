#pragma once
#include "usd/snapshot.h"
#include <array>
#include <atomic>
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
    // Mods: every mesh goes here in host units and as authored
    // (ComposeScope::hostBones), not into the scene's JSON. Called from several
    // threads at once.
    std::function<void(ComposedMesh &&)> meshSink;
    // Mods: what the load has got to.
    Progress *progress = nullptr;
  };
  // The root layer's customLayerData key of a mod's light scale (a double).
  inline constexpr const char *lightScaleKey = "openremix:lightScale";
  struct Snapshot {
    Json scene;
    std::set<std::string> dependencies;
    std::map<std::string, std::shared_ptr<scene::Texture>> textures;
  };
  struct LoadError : std::runtime_error {
    std::set<std::string> dependencies;
    LoadError(const std::string &message, std::set<std::string> paths)
        : std::runtime_error(message), dependencies(std::move(paths)) {}
  };
  Snapshot load(const Options &options);
  // The texture files a composed material reads (empty entries omitted).
  std::vector<std::string> materialTextureFiles(const Json &material);
  // Decodes texture files in parallel; null entries could not be decoded.
  // maxEdge > 0: scene::loadTexturePreview's reduced textures. progress, if
  // given, counts the files done.
  std::vector<std::shared_ptr<scene::Texture>> decodeTextures(const std::vector<std::string> &paths,
                                                              uint32_t maxEdge = 0,
                                                              std::atomic<uint32_t> *progress = nullptr);
  uint64_t capturedHash(const std::string &path);
} // namespace remix::usd
