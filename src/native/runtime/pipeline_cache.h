#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace remix::pipeline {
// Vulkan pipeline caches only grow, so a cache over this size is discarded
// and rebuilt from the pipelines the next run compiles.
constexpr uint64_t maxCacheBytes = 256ull * 1024 * 1024;
struct CacheIdentity {
  uint32_t vendor{}, device{}, driver{};
  std::array<uint8_t, 16> uuid{};
};
std::vector<uint8_t> readCache(const std::filesystem::path &file,
                               const CacheIdentity &identity) noexcept;
bool writeCache(const std::filesystem::path &file,
                const CacheIdentity &identity,
                std::span<const uint8_t> data) noexcept;
// Removes the cache files beside `file` that older drivers of the same device
// wrote ("vendor-device-driver.bin").
void removeStaleCaches(const std::filesystem::path &file) noexcept;
} // namespace remix::pipeline
