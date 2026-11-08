#pragma once
#include "usd/submission.h"
#include <atomic>
#include <filesystem>
#include <optional>

namespace remix::usd {
  // What a mod's load makes of its USD, kept on disk so that the next start need
  // not parse it: <config directory>/cache/usd/<hash of the mod's path>.bin. A file
  // is used only while the runtime, the units and every file the mod depends on
  // are as they were. OPENREMIX_USD_CACHE: "0" turns it off; "always" also keeps
  // what loads quickly and every reload.
  struct CachedMod {
    Snapshot snapshot;
    PreparedMeshes meshes;
  };
  // Where a mod's file goes; empty when turned off.
  std::filesystem::path modCacheFile(const std::string &modPath);
  // The cached mod, or nothing if there is none it can use. Progress counts surfaces.
  std::optional<CachedMod> readModCache(const std::filesystem::path &file, double meters, uint32_t axis,
                                        Progress &progress);
  // What writeModCache needs, taken before the load's JSON and meshes move on.
  struct ModCacheWrite {
    std::filesystem::path file;
    std::vector<uint8_t> tables;
    std::vector<std::shared_ptr<scene::Surface>> surfaces;
    uint64_t bytes = 0;
  };
  // Nothing when a file it depends on changed since `loadStarted`: what was loaded
  // may not be what is on disk.
  std::optional<ModCacheWrite> prepareModCacheWrite(const std::filesystem::path &file, const Snapshot &snapshot,
                                                    const PreparedMeshes &meshes, double meters, uint32_t axis,
                                                    std::filesystem::file_time_type loadStarted);
  // Writes the file whole or not at all. Any thread. Leaves it out if the disk is
  // nearly full or `cancel` is set.
  void writeModCache(ModCacheWrite write, const std::atomic<bool> &cancel);
} // namespace remix::usd
