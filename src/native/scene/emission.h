#pragma once
#include "scene/types.h"
#include <chrono>
#include <remix/remix_native_ext.h>
#include <map>
#include <tuple>

namespace remix::scene {
// Cached, deterministic area quadrature in UV space. No emission texture reads
// occur in lighting rays. Texture snapshots own the cache's lifetime.
std::array<float, 3> triangleEmission(const Draw &draw, uint32_t triangle, const float *uvTransform);
SphereLight triangleEmitter(const Draw &draw, uint32_t triangle, const std::array<float, 3> &radiance);
// Excludes texture contents and sprite frame: only animation may be smoothed.
uint64_t emissionHistoryKey(const Draw &draw, uint32_t triangle);
// Reduce immutable geometry once per emission/UV snapshot. Keep only luminous
// triangles: a shared terrain atlas usually leaves most of the mesh black.
// World transforms, skinning, identities and sidedness are applied per frame.
// Emissive estimates that read a dynamic texture
// (remixapi_TextureInfoDynamicEXT) follow its pixels at a low rate: once they
// changed, a material's estimates are made again when they are this old, or
// older when making them was expensive (under 1% of the time between two).
// Until then every surface of the material keeps the previous image's values.
inline constexpr double dynamicEmissionSeconds = REMIXAPI_DYNAMIC_TEXTURE_EMISSION_SECONDS_EXT;
class SurfaceEmissionCache {
public:
  struct Triangle {
    uint32_t index = 0;
    std::array<float, 3> radiance{};
    uint64_t layout = 0;
  };
  using Reduction = std::vector<Triangle>;
  std::shared_ptr<const Reduction> reduce(const Draw &draw, const std::array<float, 4> &uv);
  // The draw's emitters, triangleEmitter of each triangle of its reduction
  // (in its order): kept while the draw's identity, surface, transform,
  // sidedness and reduction stay the same (a prop at rest is not transformed
  // again every frame). Skinned draws are made each time. A list once
  // returned is never changed (frames hold it).
  std::shared_ptr<const std::vector<SphereLight>> emitters(const Draw &draw,
                                                           const std::shared_ptr<const Reduction> &reduction);
  // Also drops the emitters no draw asked for since the last prune.
  void prune();
  size_t emitterHint = 0; // prepareTraceFrame's emitters last time

private:
  struct Emitters {
    std::shared_ptr<const Reduction> reduction;
    std::weak_ptr<Surface> surface;
    Matrix model{};
    uint64_t instanceId = 0, used = 0;
    uint32_t instanceIndex = 0, surfaceIndex = 0;
    bool doubleSided = false;
    std::shared_ptr<std::vector<SphereLight>> lights;
  };
  std::unordered_map<uint64_t, Emitters> emitterEntries;
  uint64_t emitterFrame = 1;
  using Key = std::tuple<std::array<float, 3>, std::array<float, 4>,
                         uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, std::array<float, 4>>;
  struct Entry {
    std::weak_ptr<Texture> texture;
    bool textured = false;
    std::shared_ptr<const Reduction> reduction;
    std::weak_ptr<Texture> albedo; // self-illuminated materials only
    bool colored = false;
    // Material::emissionSerial the reduction was made from (dynamic textures).
    uint64_t serial = 0;
  };
  struct SurfaceEntries {
    std::weak_ptr<Surface> owner;
    std::map<Key, Entry> entries;
  };
  std::unordered_map<const Surface *, SurfaceEntries> surfaces;
};
// Every triangle of a reduction as an emitter (triangleEmitter), in its order,
// with the surface's group and their spatial ranks: a Morton order of the
// triangles' centers in object space, so a rigid prop keeps them as it moves.
void triangleEmitters(const Draw &draw, const SurfaceEmissionCache::Reduction &reduction,
                      std::vector<SphereLight> &out);
class EmissionHistory {
public:
  void begin(uint64_t world, double seconds, uint32_t mode); // 0 raw, 1 smooth, 2 diagnostic freeze
  void apply(SphereLight &light, uint64_t layout);
  void end();
  size_t size() const { return entries.size(); }
  double rawChange = 0, filteredChange = 0;
private:
  struct Entry {
    uint64_t layout = 0, seen = 0;
    std::array<float, 3> raw{}, filtered{};
  };
  std::unordered_map<uint64_t, Entry> entries;
  uint64_t world = 0, sequence = 0;
  uint32_t mode = 0;
  double time = 0, alpha = 1;
};
}
