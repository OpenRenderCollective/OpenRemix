#pragma once
#include "forward/cache_persistence.h"
#include "forward/probes/cache_codec.h"
#include "forward/probes/capture_config.h"
#include <optional>

namespace remix::forward::probes::standalone_cache {
struct Device {
  uint32_t vendor = 0, model = 0, driver = 0;
  cache_codec::Limits limits;
  std::array<uint8_t, 16> uuid{};
};
struct Snapshot {
  std::vector<Capture> captures;
  Data data;
};
constexpr uint64_t maximumBytes = 256ull << 20;
// Versions placement/filtering and fixed transport defaults. No diffuse
// settings, execution budget, runtime handles or GPU objects enter this key.
uint64_t key(uint64_t content, uint64_t shaders, const CaptureConfig &config,
             uint32_t count, float tolerance, const Device &device);
std::filesystem::path path(uint64_t key);
std::optional<Snapshot> read(const std::filesystem::path &file, uint64_t key,
                             cache_codec::Limits limits,
                             std::stop_token cancelled = {});
bool write(const std::filesystem::path &file, uint64_t key,
           const Snapshot &snapshot, std::stop_token cancelled = {});
uint64_t retainedCharge(const Snapshot &snapshot);
cache_io::Persistence<Snapshot> &persistence();
} // namespace remix::forward::probes::standalone_cache
