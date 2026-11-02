#include "forward/probes/standalone_cache.h"
#include "forward/cache_directory.h"
#include "forward/cache_file.h"
#include <bit>
#include <random>
#include <sstream>

namespace remix::forward::probes::standalone_cache {
uint64_t key(uint64_t content, uint64_t shaders, const CaptureConfig &config,
             uint32_t count, float tolerance, const Device &device) {
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
std::filesystem::path path(uint64_t identity) {
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
bool write(const std::filesystem::path &file, uint64_t identity,
           const Snapshot &snapshot, std::stop_token cancelled) {
  std::filesystem::path temporary;
  try {
    if (!identity || snapshot.captures.empty() ||
        retainedCharge(snapshot) > maximumBytes)
      return false;
    cache_codec::validate(snapshot.captures, snapshot.data);
    std::filesystem::create_directories(file.parent_path());
    std::random_device random;
    temporary = file;
    temporary +=
        ".tmp-" + std::to_string((uint64_t(random()) << 32) ^ random());
    if (std::filesystem::exists(temporary))
      return false;
    cache_io::Writer output(temporary, identity, cancelled);
    cache_codec::write(output, snapshot.captures, snapshot.data);
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
cache_io::Persistence<Snapshot> &persistence() {
  static cache_io::Persistence<Snapshot> service(
      maximumBytes, retainedCharge,
      [](const auto &file, uint64_t identity, const Snapshot &snapshot) {
        return write(file, identity, snapshot);
      },
      "Forward standalone reflection");
  return service;
}
} // namespace remix::forward::probes::standalone_cache
