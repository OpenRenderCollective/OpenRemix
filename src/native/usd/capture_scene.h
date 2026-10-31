#pragma once
#include "scene/resources.h"
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace remix::usd {
  // One recorded frame (usd/capture.cpp): plain copies that the writer reads
  // off the API thread. Keys are the host's handle hashes (texture hashes for
  // textures), the names Remix captures and mods use.
  struct CaptureScene {
    struct Material {
      bool translucent = false;
      float tint[4] = {1, 1, 1, 1}; // albedo (transmittance) and opacity
      float roughness = .5f, metallic = 0;
      float emission[3] = {};
      float refractiveIndex = 1.5f, transmittanceDistance = 0;
      uint32_t compare = 7, normalEncoding = 0;
      float reference = 0; // 0..1
      bool drawAlpha = false;
      uint8_t filter = 1, wrapU = 1, wrapV = 1;
      // Texture hashes in `textures` (0: none).
      uint64_t albedo = 0, normal = 0, roughnessMap = 0, metallicMap = 0, emissive = 0;
      // A thin film (strength 0: none) and its thickness map's hash.
      scene::Material::Iridescence film;
      uint64_t filmMap = 0;
    };
    struct Mesh {
      std::vector<scene::Surface> surfaces;
      std::vector<uint64_t> materials; // per surface (0: none)
    };
    struct Instance {
      uint64_t mesh = 0;
      scene::Matrix model = scene::identity();
      uint64_t materialOverride = 0; // replaces every surface's material
      uint32_t categoryFlags = 0;
      bool doubleSided = false;
      scene::Blend blend;
      bool alphaTest = false;
      uint32_t alphaCompare = 7;
      float alphaReference = 0; // 0..1
      std::vector<scene::Matrix> bones; // this frame's pose (empty: not skinned)
    };
    struct Light {
      NativeResources::LightSource source;
      uint64_t cookie = 0; // its cookie's texture hash in `textures` (0: none)
    };
    struct Camera {
      bool valid = false;
      // USD camera to world (looking down -Z, +Y up), row-vector layout as
      // GfMatrix4d stores it.
      double toWorld[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
      float fovY = 1, aspect = 16.f / 9, nearPlane = .1f, farPlane = 1e5f;
    };
    std::string name; // capture_<timestamp>
    std::string executable;
    double metersPerUnit = .01;
    uint32_t upAxis = 1; // 1 Y, 2 Z
    std::map<uint64_t, Mesh> meshes;
    std::map<uint64_t, Material> materials;
    std::map<uint64_t, scene::Pixels> textures;
    std::vector<Instance> instances;
    std::map<uint64_t, Light> lights;
    Camera camera;
    std::vector<std::string> warnings;
  };
  // Writes a capture under `directory`: <name>.usda with meshes/, materials/
  // and textures/ beside it. Files are written to temporaries and renamed, so
  // a failure leaves no partial file. Returns the stage's path; throws
  // std::runtime_error.
  std::filesystem::path writeCapture(const CaptureScene &scene, const std::filesystem::path &directory);
} // namespace remix::usd
