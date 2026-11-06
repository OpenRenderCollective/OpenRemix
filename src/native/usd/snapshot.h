#pragma once
#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
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
  // A mesh as composed: one surface per material, its triangles' corners in
  // order. Skinned surfaces have `influences` joints a corner.
  struct ComposedSurface {
    std::string material;
    uint32_t categoryFlags = 0;
    std::vector<float> vertices; // Position, normal and texcoord: 8 a corner.
    std::vector<uint32_t> colors, jointIndices; // Colors: none, or one a corner.
    std::vector<float> jointWeights;
    uint32_t influences = 0;
  };
  struct ComposedMesh {
    std::string id, sourcePath, skeleton;
    std::array<double, 16> transform{};
    std::vector<ComposedSurface> surfaces;
    bool skinned = false, doubleSided = false, ignoreAlpha = false, alphaBlendToCutout = false;
    Json alphaState;
  };
  // What a load has got to (mods.h modLoadText says it).
  enum class LoadStage : uint32_t { Waiting, Opening, Meshes, Materials, Cache, Textures, Finishing };
  struct Progress {
    std::atomic<LoadStage> stage{LoadStage::Waiting};
    std::atomic<uint32_t> done{0}, total{0}; // of the stage
    void enter(LoadStage next, uint32_t count) {
      done = 0;
      total = count;
      stage = next;
    }
  };
  struct ComposeScope {
    // Skinned meshes as authored, with their influences: the host's draw poses
    // them with its bones (a replacement), not a USD skeleton.
    bool hostBones = false;
    // Set: meshes go here, not into the JSON, whose per-number overhead a large
    // mod's geometry cannot afford. They compose in parallel after the rest of
    // the stage, so this is called from several threads at once.
    std::function<void(ComposedMesh &&)> meshSink;
    Progress *progress = nullptr; // Counts those meshes.
  };
  Json compose_scene(const pxr::UsdStageRefPtr &, uint64_t, const ComposeScope &scope = {});
  Json compose_lights(const pxr::UsdStageRefPtr &, uint64_t);
  Json compose_material(const pxr::UsdPrim &);
  std::string resolved_texture_path(const pxr::UsdPrim &, const char *);
  // A light's attribute `name` (without "inputs:"): as "inputs:<name>", else
  // the name USD lights had before it, when only that is authored.
  pxr::UsdAttribute light_attribute(const pxr::UsdPrim &, const char *name);
} // namespace remix::usd
