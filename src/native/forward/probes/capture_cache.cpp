#include "forward/probes/capture_cache.h"
#include "forward/probes/cache_index.h"
#include "forward/cache_directory.h"
#include "forward/cache_file.h"
#include <bit>
#include <random>
#include <sstream>

namespace remix::forward::probes::capture_cache {
uint64_t standaloneKey(uint64_t content, uint64_t shaders,
                       const CaptureConfig &config, uint32_t count,
                       float tolerance, const Device &device) {
  if (!content || !shaders || !count)
    return 0;
  // Increment when geometric seed planning, CPU projection/filtering or fixed
  // transport defaults change. Layout comes from this standalone planner.
  const uint32_t fields[] = {0x52454653u,
                             1,
                             count,
                             config.reflectionResolution,
                             config.textureResolution,
                             config.samples,
                             config.bounces,
                             std::bit_cast<uint32_t>(tolerance),
                             device.vendor,
                             device.model,
                             device.driver,
                             device.limits.imageSize,
                             device.limits.imageLayers,
                             device.limits.storageRange};
  auto hash = scene::fastHash(fields, sizeof(fields), content ^ shaders);
  return scene::fastHash(device.uuid.data(), device.uuid.size(), hash) | 1;
}
uint64_t plannedKey(uint64_t content, uint64_t shaders, uint64_t plan,
                    std::span<const Capture> captures, const CaptureConfig &config,
                    const Device &device, bool lightingComponents,
                    std::span<const std::byte> directOwnership) {
  if (!plan || captures.empty()) return 0;
  // Preserve the existing planned-capture wire identity exactly.
  const uint32_t fields[] = {2, config.samples, config.bounces, config.textureResolution,
      uint32_t(lightingComponents), device.vendor, device.model, device.driver,
      device.limits.imageSize, device.limits.imageLayers, device.limits.storageRange};
  auto hash = scene::fastHash(fields, sizeof(fields), content ^ shaders);
  hash = scene::fastHash(device.uuid.data(), device.uuid.size(), hash);
  hash = scene::fastHash(&plan, sizeof(plan), hash);
  hash = scene::fastHash(directOwnership.data(), directOwnership.size_bytes(), hash);
  for (const auto &capture : captures) {
    const auto record = projectionInputRecord(capture);
    hash = scene::fastHash(&record, sizeof(record), hash);
  }
  return hash | 1;
}
std::filesystem::path standalonePath(uint64_t identity) {
  std::ostringstream name;
  name << std::hex << identity << ".reflection";
  return cache_io::directory() / "reflections" / name.str();
}
uint64_t retainedCharge(const Snapshot &snapshot) {
  uint64_t bytes = sizeof(Snapshot) +
                   snapshot.captures.capacity() * sizeof(Capture) +
                   snapshot.data.texels.capacity() * sizeof(Vec4);
  for (const auto &capture : snapshot.captures)
    bytes += capture.radiance.capacity() * sizeof(Vec4);
  return bytes;
}
std::optional<Snapshot> read(const std::filesystem::path &file,
                             uint64_t identity, cache_codec::Limits limits,
                             std::stop_token cancelled) {
  try {
    if (!identity || std::filesystem::file_size(file) > maximumBytes)
      return {};
    cache_io::Reader input(file, identity, cancelled);
    Snapshot result;
    cache_codec::read(input, result.captures, result.data, limits);
    input.finish();
    if (result.captures.empty())
      return {};
    return result;
  } catch (const std::exception &) {
    return {};
  }
}
static bool writePayload(const std::filesystem::path &file, uint64_t identity,
                         std::span<const Capture> captures, const Data &data,
                         std::stop_token cancelled) {
  std::filesystem::path temporary;
  try {
    if (!identity || captures.empty())
      return false;
    cache_codec::validate(captures, data);
    std::filesystem::create_directories(file.parent_path());
    std::random_device random;
    temporary = file;
    temporary +=
        ".tmp-" + std::to_string((uint64_t(random()) << 32) ^ random());
    if (std::filesystem::exists(temporary))
      return false;
    cache_io::Writer output(temporary, identity, cancelled);
    cache_codec::write(output, captures, data);
    output.finish();
    cache_io::check(cancelled);
    cache_io::replaceFile(temporary, file);
    return true;
  } catch (const std::exception &) {
    if (!temporary.empty()) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
    }
    return false;
  }
}
bool write(const std::filesystem::path &file, uint64_t identity,
           const Snapshot &snapshot, std::stop_token cancelled) {
  if (retainedCharge(snapshot) > maximumBytes)
    return false;
  return writePayload(file, identity, snapshot.captures, snapshot.data,
                      cancelled);
}
namespace {
uint64_t contentKey(cache_io::Section fingerprint) {
  const uint64_t fields[] = {0x524546424c4f4201ull, fingerprint.bytes,
                             fingerprint.checksum};
  return scene::fastHash(fields, sizeof(fields), 0) | 1;
}
} // namespace
std::filesystem::path sharedPath(const std::filesystem::path &directory,
                                 uint64_t identity) {
  std::ostringstream name;
  name << std::hex << identity << ".reflection";
  return directory / "reflections" / "shared" / name.str();
}
uint64_t storeShared(const std::filesystem::path &directory,
                     std::span<const Capture> captures, const Data &data,
                     std::stop_token cancelled) {
  try {
    if (captures.empty())
      return 0;
    cache_codec::validate(captures, data);
    const auto expected = cache_codec::fingerprint(captures, data, cancelled);
    if (expected.bytes > maximumBytes - sizeof(cache_io::Header))
      return 0;
    const auto identity = contentKey(expected);
    const auto file = sharedPath(directory, identity);
    try {
      // Reader verifies the entire checksum without allocating decoded arrays.
      if (std::filesystem::file_size(file) > maximumBytes)
        throw std::length_error("Shared reflection object exceeds file budget");
      cache_io::Reader existing(file, identity, cancelled);
      const auto actual = existing.verifiedFingerprint();
      // A valid colliding object must never be replaced by different content.
      return actual.bytes == expected.bytes &&
                     actual.checksum == expected.checksum
                 ? identity
                 : 0;
    } catch (const std::exception &) {
      cache_io::check(cancelled);
    }
    return writePayload(file, identity, captures, data, cancelled) ? identity
                                                                   : 0;
  } catch (const std::exception &) {
    return 0;
  }
}
std::optional<Snapshot> readShared(const std::filesystem::path &directory,
                                   uint64_t identity,
                                   cache_codec::Limits limits,
                                   std::stop_token cancelled) {
  try {
    const auto file = sharedPath(directory, identity);
    if (!identity || std::filesystem::file_size(file) > maximumBytes)
      return {};
    cache_io::Reader input(file, identity, cancelled);
    if (contentKey(input.verifiedFingerprint()) != identity)
      return {};
    Snapshot result;
    cache_codec::read(input, result.captures, result.data, limits);
    input.finish();
    if (result.captures.empty())
      return {};
    return result;
  } catch (const std::exception &) {
    return {};
  }
}
cache_io::Persistence<Snapshot> &sharedPersistence() {
  static cache_io::Persistence<Snapshot> service(maximumBytes, retainedCharge,
      [](const auto &directory, uint64_t identity, const Snapshot &snapshot) {
        const auto blob = storeShared(directory, snapshot.captures, snapshot.data);
        return blob && cache_index::writeShared(directory, identity, blob);
      }, "Forward reflection update");
  return service;
}
cache_io::Persistence<Snapshot> &persistence() {
  static cache_io::Persistence<Snapshot> service(
      maximumBytes, retainedCharge,
      [](const auto &file, uint64_t identity, const Snapshot &snapshot) {
        return write(file, identity, snapshot);
      },
      "Forward standalone reflection");
  return service;
}
} // namespace remix::forward::probes::capture_cache
