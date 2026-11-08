// Loads a mods directory as a game would, without a GPU, and reports what it
// takes: remix-usd-mods-probe <mods directory> [meters per unit] [up axis: 1 Y, 2 Z].
#include "scene/resources.h"
#include "usd/mods.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif
using namespace remix;
namespace {
  std::pair<double, double> memoryGb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
      return {double(counters.PagefileUsage) / 1e9, double(counters.PeakPagefileUsage) / 1e9};
#endif
    return {0, 0};
  }
} // namespace
int main(int argc, char **argv) {
  if (argc < 2)
    return 2;
  const double meters = argc > 2 ? std::atof(argv[2]) : .01;
  const uint32_t axis = argc > 3 ? uint32_t(std::atoi(argv[3])) : 2;
  NativeResources resources;
  if (usd::configureMods(argv[1], meters, axis, false) != REMIXAPI_ERROR_CODE_SUCCESS)
    return 1;
  const auto started = std::chrono::steady_clock::now();
  const auto seconds = [&] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  };
  std::string last;
  double worstBoundary = 0;
  for (;;) {
    const auto before = std::chrono::steady_clock::now();
    usd::modBoundary(resources);
    worstBoundary = std::max(
        worstBoundary, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count());
    const auto status = usd::modStatus();
    if (!status.loading)
      break;
    auto text = usd::modLoadText();
    if (const auto cut = text.find(" ("); cut != std::string::npos)
      text.resize(cut);
    if (text != last) {
      const auto [now, peak] = memoryGb();
      std::printf("%7.1f s  %-40s memory %.2f GB, peak %.2f GB\n", seconds(), text.c_str(), now, peak);
      std::fflush(stdout);
      last = text;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  auto status = usd::modStatus();
  const auto [now, peak] = memoryGb();
  std::printf("%7.1f s  done%s: %zu mods, geometry %.2f GB, %zu materials; memory %.2f GB, peak %.2f GB; "
              "slowest frame boundary %.1f ms\n",
              seconds(), status.fromCache ? " from its cache" : "", status.mods.size(),
              double(status.geometryBytes) / 1e9, status.materials, now, peak, worstBoundary);
  std::fflush(stdout);
  if (status.cacheWriting) {
    while (usd::modStatus().cacheWriting)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::printf("%7.1f s  cache file written\n", seconds());
  }
  for (const auto &mod : status.mods)
    std::printf("  %s: %zu meshes, %zu attached lights, %zu light overrides, %zu materials, %zu missing textures\n",
                mod.name.c_str(), mod.meshes, mod.lights, mod.lightOverrides, mod.materials, mod.missingTextures);
  if (!status.diagnostic.empty())
    std::printf("diagnostic: %s\n", status.diagnostic.c_str());
  usd::stopMods(resources);
  return status.diagnostic.empty() ? 0 : 1;
}
