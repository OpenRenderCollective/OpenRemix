#pragma once
#include "forward/bake/config.h"
#include "forward/probes/local_capture_work.h"
#include <filesystem>
#include <memory>

namespace remix::forward::bake::local_cache {
  // CPU-only persistence payload; never retains an image, transport or fence.
  struct Snapshot {
    std::shared_ptr<const scene::BakeScene> source;
    std::shared_ptr<const probes::ReceiverCoverageEvidence> evidence;
    probes::LocalCaptureImage image;
    float detailTolerance = 0;
  };
  std::filesystem::path path(std::filesystem::path lightingCache);
  uint64_t retainedBytes(const Snapshot &snapshot);
  bool write(const std::filesystem::path &file, uint64_t key, const Snapshot &snapshot,
             std::stop_token cancelled = {});
  // A miss affects only local reflection improvements. Runtime generation IDs
  // are rebound after content, receiver ordering and policy checks succeed.
  std::unique_ptr<Snapshot> read(const std::filesystem::path &file, uint64_t key,
      std::shared_ptr<const scene::BakeScene> source, const Config &config,
      const probes::CoverageBaselineSet &baseline, std::stop_token cancelled = {});
}
