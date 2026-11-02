#pragma once
#include "forward/cache_io.h"
#include "forward/cache_file.h"
#include <bit>
#include <cmath>
#include <random>
#include <sstream>

namespace remix::forward::bake::cache {
  // A bounded hint to an existing validated container, never an asset copy.
  struct DiffuseReference {
    uint64_t diffuseKey = 0, containerKey = 0;
    uint32_t reflectionProbes = 0, reflectionResolution = 0, toleranceBits = 0;
    bool operator==(const DiffuseReference &) const = default;
  };
  inline std::filesystem::path diffuseReferencePath(const std::filesystem::path &directory, uint64_t key) {
    std::ostringstream name;
    name << std::hex << key << ".diffuse-index";
    return directory / name.str();
  }
  inline std::optional<DiffuseReference> readDiffuseReference(
      const std::filesystem::path &directory, uint64_t key, std::stop_token cancelled = {}) {
    try {
      const auto file = diffuseReferencePath(directory, key);
      if (!key || std::filesystem::file_size(file) != sizeof(cache_io::Header) + 24) return {};
      cache_io::Reader input(file, key, cancelled);
      if (input.pod<uint32_t>() != 1) return {};
      DiffuseReference reference{key, input.pod<uint64_t>(), input.pod<uint32_t>(),
          input.pod<uint32_t>(), input.pod<uint32_t>()};
      input.finish();
      const float tolerance = std::bit_cast<float>(reference.toleranceBits);
      if (!reference.containerKey || reference.reflectionProbes > 64 ||
          (reference.reflectionProbes && (!reference.reflectionResolution || reference.reflectionResolution > 512 ||
                                         !std::has_single_bit(reference.reflectionResolution))) ||
          !std::isfinite(tolerance) || tolerance < 0) return {};
      return reference;
    } catch (const std::exception &) { return {}; }
  }
  inline bool writeDiffuseReference(const std::filesystem::path &directory, DiffuseReference reference,
                                    std::stop_token cancelled = {}) {
    std::filesystem::path temporary;
    try {
      if (!reference.diffuseKey || !reference.containerKey) return false;
      if (readDiffuseReference(directory, reference.diffuseKey, cancelled) == reference) return true;
      cache_io::check(cancelled);
      std::filesystem::create_directories(directory);
      const auto file = diffuseReferencePath(directory, reference.diffuseKey);
      std::random_device random;
      temporary = file;
      temporary += ".tmp-" + std::to_string((uint64_t(random()) << 32) ^ random());
      if (std::filesystem::exists(temporary)) return false;
      cache_io::Writer output(temporary, reference.diffuseKey, cancelled);
      output.pod(uint32_t(1));
      output.pod(reference.containerKey);
      output.pod(reference.reflectionProbes);
      output.pod(reference.reflectionResolution);
      output.pod(reference.toleranceBits);
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
}
