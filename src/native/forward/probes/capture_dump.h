#pragma once
#include "forward/probes/data.h"
#include <stop_token>

namespace remix::scene { struct BakeScene; }

namespace remix::forward::probes {
  struct FittedRegion;
  // Small, independent candidate report for hardware-fit experiments. Only
  // writes when REMIX_DEBUG_PROBE_CANDIDATES names a fresh JSON output file.
  void dumpCandidates(const scene::BakeScene &source, std::span<const FittedRegion> regions,
                      std::span<const Capture> captures, std::stop_token cancelled = {}) noexcept;
  // Optional developer aid. No CPU capture copy is retained after publication.
  // Writes only when REMIX_DEBUG_BAKE_CAPTURE_PREFIX names a fresh output prefix.
  // Failures are reported to stderr and never invalidate the completed bake.
  void dumpCaptures(uint64_t sceneHash, std::span<const Capture> captures) noexcept;
  // Dumps a named CPU block once, after a capture dump and .frame.request exist.
  // Suffixes are internal constants; raw files are tied to the producing build.
  void dumpFrameData(const char *suffix, const void *data, size_t bytes) noexcept;
  void dumpPublishedData(const char *suffix, const void *data, size_t bytes) noexcept;
  // Immutable transport diagnostics can precede reflection capture/publication.
  // Same fresh-prefix and once-per-suffix rules; never requires .capture.bin.
  void dumpTransportData(const char *suffix, const void *data, size_t bytes) noexcept;
} // namespace remix::forward::probes
