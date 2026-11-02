#pragma once
#include "forward/cache_persistence.h"
#include "forward/probes/cache_codec.h"
#include "forward/probes/capture_config.h"
#include "forward/probes/capture_payload.h"
#include <optional>

namespace remix::forward::probes::capture_cache {
struct Device {
  uint32_t vendor = 0, model = 0, driver = 0;
  cache_codec::Limits limits;
  std::array<uint8_t, 16> uuid{};
};
using Snapshot = CapturePayload;
constexpr uint64_t maximumBytes = 256ull << 20;
// Versions placement/filtering and fixed transport defaults. No diffuse
// settings, execution budget, runtime handles or GPU objects enter this key.
uint64_t standaloneKey(uint64_t content, uint64_t shaders,
                       const CaptureConfig &config, uint32_t count,
                       float tolerance, const Device &device);
// Provenance for already-selected captures. Ownership bytes describe transport
// light policy; this module does not interpret a diffuse provider's policy enum.
struct Planned {
  std::filesystem::path directory;
  uint64_t content = 0, shaders = 0;
  Device device;
  bool lightingComponents = false;
  std::span<const std::byte> directOwnership;
};
uint64_t plannedKey(uint64_t content, uint64_t shaders, uint64_t plan,
                    std::span<const Capture> captures, const CaptureConfig &config,
                    const Device &device, bool lightingComponents,
                    std::span<const std::byte> directOwnership = {});
std::filesystem::path standalonePath(uint64_t key);
std::optional<Snapshot> read(const std::filesystem::path &file, uint64_t key,
                             cache_codec::Limits limits,
                             std::stop_token cancelled = {});
bool write(const std::filesystem::path &file, uint64_t key,
           const Snapshot &snapshot, std::stop_token cancelled = {});
// Immutable content-addressed blobs can be shared by different diffuse layouts.
// Zero is a normal persistence failure; callers may retain an inline payload.
std::filesystem::path sharedPath(const std::filesystem::path &directory,
                                 uint64_t identity);
uint64_t storeShared(const std::filesystem::path &directory,
                     std::span<const Capture> captures, const Data &data,
                     std::stop_token cancelled = {});
std::optional<Snapshot> readShared(const std::filesystem::path &directory,
                                   uint64_t identity,
                                   cache_codec::Limits limits,
                                   std::stop_token cancelled = {});
uint64_t retainedCharge(const Snapshot &snapshot);
cache_io::Persistence<Snapshot> &persistence();
} // namespace remix::forward::probes::capture_cache
