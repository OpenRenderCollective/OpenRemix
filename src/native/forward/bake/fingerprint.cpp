#include "forward/bake/fingerprint.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <locale>
#include <mutex>

namespace remix::forward::bake {
  namespace {
    template <class T> uint64_t hash(std::span<const T> values) {
      return scene::fastHash(values.data(), values.size_bytes());
    }
    struct FloatDigest {
      std::array<float, 4096> block;
      size_t used = 0;
      uint64_t result = 0;
      void append(float value) {
        block[used++] = value;
        if (used == block.size()) {
          result = scene::fastHash(block.data(), used * sizeof(float), result);
          used = 0;
        }
      }
      uint64_t finish() const {
        return used ? scene::fastHash(block.data(), used * sizeof(float), result) : result;
      }
    };
  }
  void dumpPhaseFingerprint(const char *phase, uint64_t source, const Grid &grid, const lightmap::Layout &layout,
                            std::span<const Result> surfaces, std::span<const lightmap::Sun> sun,
                            std::span<const probes::Capture> captures) noexcept {
    const char *name = std::getenv("REMIX_DEBUG_BAKE_PHASE_HASHES");
    if (!name || !*name)
      return;
    try {
      FloatDigest surfaceLighting, surfaceAlpha, captureLighting;
      for (const auto &surface : surfaces)
        for (const auto &coefficient : surface.irradiance) {
          for (uint32_t c = 0; c < 3; ++c) surfaceLighting.append(coefficient[c]);
          surfaceAlpha.append(coefficient[3]);
        }
      uint64_t radiance = 0;
      uint64_t sites = 0;
      for (const auto &capture : captures) {
        radiance = scene::fastHash(capture.radiance.data(), capture.radiance.size() * sizeof(Vec4), radiance);
        sites = scene::fastHash(capture.position.data(), sizeof(capture.position), sites);
        sites = scene::fastHash(&capture.resolution, sizeof(capture.resolution), sites);
        for (const auto &pixel : capture.radiance)
          for (uint32_t c = 0; c < 3; ++c) captureLighting.append(pixel[c]);
      }
      const std::filesystem::path path(name);
      static std::mutex outputMutex;
      std::lock_guard lock(outputMutex);
      if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path());
      std::ofstream out(path, std::ios::app);
      out.exceptions(std::ios::failbit | std::ios::badbit);
      out.imbue(std::locale::classic());
      out << "{\"version\":1,\"phase\":\"" << phase << "\",\"source\":\"" << source
          << "\",\"gridCount\":" << grid.probes.size()
          << ",\"surfaceCount\":" << surfaces.size() << ",\"sunCount\":" << sun.size()
          << ",\"captureCount\":" << captures.size() << ",\"grid\":\"" << hash(std::span(grid.probes))
          << "\",\"gridPoints\":\"" << hash(std::span(grid.points))
          << "\",\"points\":\"" << hash(std::span(layout.points))
          << "\",\"pointCharts\":\"" << hash(std::span(layout.pointCharts))
          << "\",\"owners\":\"" << hash(std::span(layout.owners))
          << "\",\"addresses\":\"" << hash(std::span(layout.addresses))
          << "\",\"surfaces\":\"" << hash(surfaces) << "\",\"surfaceLighting\":\"" << surfaceLighting.finish()
          << "\",\"surfaceAlpha\":\"" << surfaceAlpha.finish()
          << "\",\"sun\":\"" << hash(sun) << "\",\"captures\":\"" << radiance
          << "\",\"captureSites\":\"" << sites << "\",\"captureLighting\":\"" << captureLighting.finish() << "\"}\n";
    } catch (const std::exception &error) {
      std::fprintf(stderr, "Bake phase fingerprint unavailable: %s\n", error.what());
    }
  }
}
