#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace remix::scene {
  struct BakeScene;
  struct BakeDrawFingerprint {
    uint64_t geometry, material, charts, record;
  };
  bool bakeDiagnosticsEnabled();
  // Optional CPU-only diagnostics. Does not participate in content identity or
  // retain mutable resources. A fresh prefix is required for each process.
  void dumpBakeFingerprint(const BakeScene &scene, std::span<const BakeDrawFingerprint> draws,
                           const std::array<uint64_t, 6> &prefixes, uint64_t domeTextureHash) noexcept;
} // namespace remix::scene
