#pragma once
#include "forward/bake/grid.h"
#include "forward/bake/irradiance_contents.h"
#include "forward/bake/diffuse_reference.h"
#include "forward/bake/probe_visibility.h"
#include "forward/mixed/data.h"
#include "forward/probes/data.h"
#include "forward/probes/capture_payload.h"
#include "forward/probes/capture_config.h"
#include <filesystem>
#include <memory>
#include <optional>

namespace remix::forward::bake::local_cache { struct Snapshot; }
namespace remix::forward::bake::cache {
  struct Device {
    uint32_t vendor = 0, model = 0, driver = 0;
    uint32_t imageSize = 0, imageLayers = 0, storageRange = 0;
    std::array<uint8_t, 16> uuid{};
  };
  // Scheduling and cache controls are excluded. Shader bytes and an explicit
  // CPU-algorithm revision participate, along with the frozen source content.
  // Explicit aggregate membership adds its estimator shaders and policy bytes;
  // an empty policy adds neither ownership bytes nor aggregate shader hashes.
  // Receiver-direct contents add a separate domain and estimator fingerprint;
  // the default combined identity and payload bytes retain their legacy form.
  // CPU planning revisions still invalidate the complete lighting cache key.
  // Fallback reflection placement is used without a closed spatial partition;
  // its coverage policy must not invalidate fitted BSP or diffuse-only bakes.
  uint64_t key(uint64_t content, const Config &config, float cutoff, const Device &device,
               const DirectPolicy &policy = {}, bool fallbackReflections = false);
  uint64_t diffuseKey(uint64_t content, const Config &config, float cutoff, const Device &device,
                       const DirectPolicy &policy = {});
  // Versioned procedural ray/projection identity; excludes diffuse atlas/grid settings.
  uint64_t reflectionKey(uint64_t content, uint64_t plan, std::span<const probes::Capture> captures,
                         const probes::CaptureConfig &config, const Device &device,
                         bool lightingComponents, const DirectPolicy &policy = {});
  std::filesystem::path path(uint64_t key);
  // One small receipt per attempted disk lookup. Separates changed captured
  // inputs from shader/settings/device policy changes across installations.
  // Diagnostic I/O failure never prevents a bake or changes cache validity.
  void recordLookup(const std::filesystem::path &file, uint64_t key, uint64_t input,
                     uint64_t policy, bool hit) noexcept;
  struct View {
    const Grid &grid;
    const lightmap::Layout &layout;
    const mixed::Plan &mixed;
    std::span<const probes::Capture> captures;
    const probes::Data &specular;
    std::span<const uint16_t> pixels;
    uint32_t surfaceSamples;
    bool directDiffuse, sun;
    std::span<const uint32_t> styles{};
    const ProbeVisibility *localProbes = nullptr;
    bool directionalAttempted = false;
    std::span<const uint32_t> directionals{}; // empty after a whole-replacement budget decline
    uint64_t directionalBudget = 0; // zero on write selects the current layout allowance
    const local_cache::Snapshot *localReflections = nullptr;
    bool localReflectionsOnly = false;
    uint64_t reflectionKey = 0; // optional independent capture index
    bool shareReflectionPayload = false; // legacy inline writes remain available
    IrradianceContents contents = IrradianceContents::Combined; // receiver-direct views are diffuse-only
  };
  struct Snapshot {
    DiffuseReference diffuseReference; // runtime persistence metadata; legacy payload stays unchanged
    void takeGrid(Grid &source);
    Grid grid;
    lightmap::Layout layout; // render mappings only; no surface sampling work lists
    mixed::Plan mixed;
    std::vector<probes::Capture> captures;
    probes::Data specular;
    // Runtime snapshots retain a completed reflection component. Inline fields
    // remain the owning destination for legacy cache decoding and CPU callers.
    std::shared_ptr<const probes::CapturePayload> reflectionOwner;
    const std::vector<probes::Capture> &reflectionCaptures() const {
      return reflectionOwner ? reflectionOwner->captures : captures;
    }
    const probes::Data &reflectionData() const { return reflectionOwner ? reflectionOwner->data : specular; }

    std::vector<uint16_t> pixels;
    std::vector<uint32_t> styles;
    std::optional<ProbeVisibility> localProbes;
    uint32_t surfaceSamples = 0;
    bool directDiffuse = false, sun = false;
    bool directionalAttempted = false;
    std::vector<uint32_t> directionals;
    uint64_t directionalBudget = 0;
    std::shared_ptr<const local_cache::Snapshot> localReflections;
    bool localReflectionsOnly = false; // Companion upgrade; never replace the base cache.
    uint64_t reflectionKey = 0;
    bool missingReflections = false; // valid diffuse payload, missing external capture blob
    IrradianceContents contents = IrradianceContents::Combined;
  };
  // Misses, corruption, incompatible versions and unavailable files fall back
  // to tracing. Neither operation publishes resources or performs GPU work.
  // Stored aggregate ownership must also match the expected policy, independently
  // of the caller-supplied file key. Contents must match the payload tag too.
  enum class ReflectionRead { Required, RecoverMissing, DiffuseOnly };
  std::optional<Snapshot> read(const std::filesystem::path &file, uint64_t key,
                               std::shared_ptr<const scene::BakeScene> source, const Config &config,
                               const Device &device, std::stop_token cancelled = {},
                               const DirectPolicy &policy = {}, ReflectionRead reflections = ReflectionRead::Required);
  std::optional<Snapshot> readDiffuse(const std::filesystem::path &directory, uint64_t content,
      std::shared_ptr<const scene::BakeScene> source, const Config &config, float cutoff,
      const Device &device, std::stop_token cancelled = {}, const DirectPolicy &policy = {},
      bool fallbackReflections = false);
  bool write(const std::filesystem::path &file, uint64_t key, const View &view,
             std::stop_token cancelled = {});
} // namespace remix::forward::bake::cache
