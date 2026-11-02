#pragma once
#include "forward/cache_file.h"
#include "forward/probes/capture_cache.h"
#include "forward/probes/capture_payload.h"
#include <optional>
#include <random>
#include <sstream>

namespace remix::forward::probes::cache_index {
  struct Location {
    uint64_t containerKey = 0;
    cache_io::Section section;
  };
  using Snapshot = CapturePayload;
  inline std::filesystem::path path(const std::filesystem::path &directory, uint64_t key,
                                     const char *suffix = ".reflection-index") {
    std::ostringstream name;
    name << std::hex << key << suffix;
    return directory / name.str();
  }
  // Publish only after the immutable container has been atomically replaced.
  // The index stores no paths or cubemap data: a section location or blob key.
  template <typename Payload>
  inline bool writeRecord(const std::filesystem::path &directory, uint64_t key, Payload payload,
                          std::stop_token cancelled) {
    std::filesystem::path temporary;
    try {
      if (!key) return false;
      const auto file = path(directory, key);
      std::filesystem::create_directories(directory);
      std::random_device random;
      temporary = file;
      temporary += ".tmp-" + std::to_string((uint64_t(random()) << 32) ^ random());
      if (std::filesystem::exists(temporary)) return false;
      cache_io::Writer out(temporary, key, cancelled);
      payload(out);
      out.finish();
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
  inline bool write(const std::filesystem::path &directory, uint64_t key, Location location,
                    std::stop_token cancelled = {}) {
    if (!location.containerKey || !location.section.bytes) return false;
    return writeRecord(directory, key, [&](auto &out) {
      out.pod(uint32_t(1)); out.pod(location.containerKey); out.pod(location.section);
    }, cancelled);
  }
  inline bool writeShared(const std::filesystem::path &directory, uint64_t key, uint64_t blob,
                          std::stop_token cancelled = {}) {
    if (!blob) return false;
    return writeRecord(directory, key, [&](auto &out) {
      out.pod(uint32_t(2)); out.pod(blob);
    }, cancelled);
  }
  // A corrupt/missing index, container or reflection section is an ordinary miss.
  // Other payload sections need not be read, allocated or valid.
  inline std::optional<Snapshot> read(const std::filesystem::path &directory, uint64_t key,
                                     cache_codec::Limits limits, std::stop_token cancelled = {}) {
    try {
      Location location;
      uint64_t blob = 0;
      {
        cache_io::Reader index(path(directory, key), key, cancelled);
        const auto version = index.pod<uint32_t>();
        if (version == 2) {
          blob = index.pod<uint64_t>();
          if (!blob) return {};
        } else {
          if (version != 1) return {};
          location.containerKey = index.pod<uint64_t>();
          location.section = index.pod<cache_io::Section>();
        }
        index.finish();
      } // Do not hold the index open while reading a potentially large image.

      if (blob) return capture_cache::readShared(directory, blob, limits, cancelled);
      if (!location.containerKey || !location.section.bytes) return {};
      cache_io::Reader input(path(directory, location.containerKey, ".bake"),
          location.containerKey, cancelled, location.section);
      Snapshot result;
      cache_codec::read(input, result.captures, result.data, limits);
      input.finish();
      return result;
    } catch (const std::exception &) { return {}; }
  }
}
