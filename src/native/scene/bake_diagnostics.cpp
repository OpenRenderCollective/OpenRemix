#include "scene/bake_diagnostics.h"
#include "scene/bake_scene.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <string>

namespace remix::scene {
  bool bakeDiagnosticsEnabled() {
    const auto prefix = std::getenv("REMIX_DEBUG_BAKE_INPUT_PREFIX");
    return prefix && *prefix;
  }
  void dumpBakeFingerprint(const BakeScene &scene, std::span<const BakeDrawFingerprint> draws,
                           const std::array<uint64_t, 6> &prefixes, uint64_t domeTextureHash) noexcept {
    const auto prefix = std::getenv("REMIX_DEBUG_BAKE_INPUT_PREFIX");
    if (!prefix || !*prefix)
      return;
    try {
      const auto path = std::filesystem::path(std::string(prefix) + "." +
                                             std::to_string(scene.contentHash) + ".json");
      if (std::filesystem::exists(path))
        return; // The same immutable input may be captured again in one process.
      std::ofstream out(path);
      out.exceptions(std::ios::failbit | std::ios::badbit);
      out << std::setprecision(std::numeric_limits<float>::max_digits10);
      out << "{\n  \"version\": 1,\n  \"contentHash\": \"" << scene.contentHash
          << "\",\n  \"sceneId\": \"" << scene.sceneId << "\",\n  \"metersPerUnit\": "
          << scene.sceneScale << ",\n  \"lightCount\": " << scene.lights.size()
          << ",\n  \"chartCount\": " << scene.charts.size() << ",\n  \"prefixes\": {";
      constexpr const char *names[] = {"header", "partition", "reflectionHints", "members", "sun", "dome"};
      for (size_t i = 0; i < prefixes.size(); ++i)
        out << (i ? ", " : "") << '"' << names[i] << "\": \"" << prefixes[i] << '"';
      out << "},\n  \"dome\": {\"enabled\": " << (scene.dome.enabled ? "true" : "false")
          << ", \"textureHash\": \"" << domeTextureHash << "\", \"radiance\": [";
      for (size_t i = 0; i < scene.dome.radiance.size(); ++i)
        out << (i ? ", " : "") << scene.dome.radiance[i];
      out << "], \"worldToLocal\": [";
      for (size_t i = 0; i < scene.dome.worldToLocal.size(); ++i)
        out << (i ? ", " : "") << scene.dome.worldToLocal[i];
      out << ']';
      if (scene.dome.texture) {
        const auto &pixels = scene.dome.texture->pixels;
        out << ", \"width\": " << pixels.width << ", \"height\": " << pixels.height
            << ", \"srgb\": " << (pixels.srgb ? "true" : "false");
        if (!pixels.mips.empty()) {
          auto imagePath = path;
          imagePath.replace_extension(".dome.bin");
          std::ofstream image(imagePath, std::ios::binary);
          image.exceptions(std::ios::failbit | std::ios::badbit);
          image.write(reinterpret_cast<const char *>(pixels.mips[0].data()), pixels.mips[0].size());
          image.close();
        }
      }
      out << "},\n  \"draws\": [\n";
      for (size_t i = 0; i < draws.size(); ++i) {
        const auto &draw = draws[i];
        out << (i ? ",\n" : "") << "    {\"index\": " << i << ", \"sourceHash\": \""
            << scene.draws[i].material->sourceHash << "\", \"geometry\": \"" << draw.geometry
            << "\", \"material\": \"" << draw.material << "\", \"charts\": \"" << draw.charts
            << "\", \"record\": \"" << draw.record << "\"}";
      }
      out << "\n  ]\n}\n";
      out.close();
    } catch (const std::exception &error) {
      std::fprintf(stderr, "Bake input diagnostics unavailable: %s\n", error.what());
    }
  }
} // namespace remix::scene
