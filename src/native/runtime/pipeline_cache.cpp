#include "runtime/pipeline_cache.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace remix::pipeline {
namespace {
constexpr uint64_t maxBytes = maxCacheBytes;
uint64_t checksum(std::span<const uint8_t> data) {
  uint64_t h = 14695981039346656037ull;
  for (auto b : data)
    h = (h ^ b) * 1099511628211ull;
  return h;
}
void put(std::vector<uint8_t> &out, uint64_t value, size_t bytes) {
  for (size_t i = 0; i < bytes; ++i)
    out.push_back(uint8_t(value >> (8 * i)));
}
uint64_t get(std::span<const uint8_t> in, size_t offset, size_t bytes) {
  uint64_t value = 0;
  for (size_t i = 0; i < bytes; ++i)
    value |= uint64_t(in[offset + i]) << (8 * i);
  return value;
}
std::vector<uint8_t> header(const CacheIdentity &id,
                            std::span<const uint8_t> data) {
  std::vector<uint8_t> out{'O', 'R', 'V', 'K', 'C', 'A', 'C', 'H'};
  put(out, 1, 4); // envelope format, explicitly little endian
  put(out, id.vendor, 4);
  put(out, id.device, 4);
  put(out, id.driver, 4);
  out.insert(out.end(), id.uuid.begin(), id.uuid.end());
  put(out, data.size(), 8);
  put(out, checksum(data), 8);
  return out;
}
} // namespace
std::vector<uint8_t> readCache(const std::filesystem::path &file,
                               const CacheIdentity &id) noexcept {
  try {
    std::ifstream stream(file, std::ios::binary | std::ios::ate);
    if (!stream)
      return {};
    const auto size = stream.tellg();
    if (size < 56 + 32 || uint64_t(size) > maxBytes + 56)
      return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char *>(bytes.data()), size))
      return {};
    std::span<const uint8_t> data(bytes.data() + 56, bytes.size() - 56);
    const auto expected = header(id, data);
    if (!std::equal(expected.begin(), expected.end(), bytes.begin()))
      return {};
    // Validate Vulkan's own version-one header before giving it to a driver.
    if (get(data, 0, 4) != 32 || get(data, 4, 4) != 1 ||
        get(data, 8, 4) != id.vendor || get(data, 12, 4) != id.device ||
        !std::equal(id.uuid.begin(), id.uuid.end(), data.begin() + 16))
      return {};
    return {data.begin(), data.end()};
  } catch (...) {
    return {};
  }
}
bool writeCache(const std::filesystem::path &file, const CacheIdentity &id,
                std::span<const uint8_t> data) noexcept {
  std::filesystem::path temporary;
  try {
    if (data.empty() || data.size() > maxBytes)
      return false;
    std::filesystem::create_directories(file.parent_path());
    // Each writer gets its own temporary file; replacement is one atomic
    // operation.
    temporary = file;
    static std::atomic<uint64_t> sequence{0};
#ifdef _WIN32
    const auto process = GetCurrentProcessId();
#else
    const auto process = getpid();
#endif
    temporary +=
        "." + std::to_string(process) + "." + std::to_string(++sequence) + "." +
        std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
        ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    const auto prefix = header(id, data);
    stream.write(reinterpret_cast<const char *>(prefix.data()), prefix.size());
    stream.write(reinterpret_cast<const char *>(data.data()), data.size());
    stream.flush();
    if (!stream)
      throw std::runtime_error("Pipeline cache write failed");
    stream.close();
    if (!stream)
      throw std::runtime_error("Pipeline cache close failed");
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), file.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      throw std::runtime_error("Pipeline cache replacement failed");
#else
    std::filesystem::rename(temporary, file);
#endif
    return true;
  } catch (...) {
    std::error_code ignored;
    if (!temporary.empty())
      std::filesystem::remove(temporary, ignored);
    return false;
  }
}
void removeStaleCaches(const std::filesystem::path &file) noexcept {
  try {
    const auto name = file.stem().string();
    const auto device = name.substr(0, name.rfind('-') + 1);
    if (device.empty())
      return;
    std::error_code error;
    for (const auto &entry :
         std::filesystem::directory_iterator(file.parent_path(), error)) {
      const auto path = entry.path();
      const auto stem = path.stem().string();
      if (path.extension() == ".bin" && path != file &&
          stem.starts_with(device) &&
          stem.find_first_not_of("0123456789", device.size()) ==
              std::string::npos)
        std::filesystem::remove(path, error);
    }
  } catch (...) {
  }
}
} // namespace remix::pipeline
