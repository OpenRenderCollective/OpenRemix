#pragma once
#include "forward/cache_file.h"
#include "forward/probes/cache_codec.h"
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
  // The index stores no paths or cubemap data, just a bounded section location.
  inline bool write(const std::filesystem::path &directory, uint64_t key, Location location,
                    std::stop_token cancelled = {}) {
    std::filesystem::path temporary;
    try {
      if (!key || !location.containerKey || !location.section.bytes) return false;
      const auto file = path(directory, key);
      std::filesystem::create_directories(directory);
      std::random_device random;
      temporary = file;
      temporary += ".tmp-" + std::to_string((uint64_t(random()) << 32) ^ random());
      if (std::filesystem::exists(temporary)) return false;
      cache_io::Writer out(temporary, key, cancelled);
      out.pod(uint32_t(1));
      out.pod(location.containerKey);
      out.pod(location.section);
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
  // A corrupt/missing index, container or reflection section is an ordinary miss.
  // Other payload sections need not be read, allocated or valid.
  inline std::optional<Snapshot> read(const std::filesystem::path &directory, uint64_t key,
                                     cache_codec::Limits limits, std::stop_token cancelled = {}) {
    try {
      Location location;
      {
        cache_io::Reader index(path(directory, key), key, cancelled);
        if (index.pod<uint32_t>() != 1) return {};
        location.containerKey = index.pod<uint64_t>();
        location.section = index.pod<cache_io::Section>();
        index.finish();
      } // Do not hold the index open while reading a potentially large image.

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
