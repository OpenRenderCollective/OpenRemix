#pragma once
#include "forward/bake/grid.h"
#include "forward/bake/probe_visibility.h"
#include "forward/mixed/data.h"
#include "forward/probes/data.h"
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
  // CPU planning revisions still invalidate the complete lighting cache key.
  uint64_t key(uint64_t content, const Config &config, float cutoff, const Device &device,
               const DirectPolicy &policy = {});
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
  };
  struct Snapshot {
    void takeGrid(Grid &source);
    Grid grid;
    lightmap::Layout layout; // render mappings only; no surface sampling work lists
    mixed::Plan mixed;
    std::vector<probes::Capture> captures;
    probes::Data specular;
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
  };
  // Misses, corruption, incompatible versions and unavailable files fall back
  // to tracing. Neither operation publishes resources or performs GPU work.
  // Stored aggregate ownership must also match the expected policy, independently
  // of the caller-supplied file key.
  std::optional<Snapshot> read(const std::filesystem::path &file, uint64_t key,
                               std::shared_ptr<const scene::BakeScene> source, const Config &config,
                               const Device &device, std::stop_token cancelled = {},
                               const DirectPolicy &policy = {});
  bool write(const std::filesystem::path &file, uint64_t key, const View &view,
             std::stop_token cancelled = {});
} // namespace remix::forward::bake::cache
