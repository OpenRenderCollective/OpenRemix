#pragma once
#include "forward/probes/data.h"
#include <memory>
#include <stop_token>

namespace remix::scene { struct BakeScene; }

namespace remix::forward::probes {
  struct FittedRegion;
  class ReceiverAudit;
  class ReceiverSiteAudit;
  class ReceiverSiteChecks;
  class ReceiverFitAudit;
  std::unique_ptr<ReceiverAudit> prepareReceiverFitCoverage(const scene::BakeScene &source,
      std::span<const Capture> baseline, const ReceiverFitAudit &fits, std::stop_token cancelled = {}) noexcept;
  std::unique_ptr<ReceiverFitAudit> prepareReceiverFits(const scene::BakeScene &source,
      const ReceiverSiteAudit &audit, const ReceiverSiteChecks &checks, float detailToleranceMetres,
      std::stop_token cancelled = {}) noexcept;
  std::unique_ptr<ReceiverSiteChecks> prepareReceiverSiteChecks(const scene::BakeScene &source,
      const ReceiverSiteAudit &audit, std::stop_token cancelled = {}) noexcept;
  std::unique_ptr<ReceiverSiteAudit> prepareReceiverSites(const scene::BakeScene &source,
      const ReceiverAudit &audit, std::stop_token cancelled = {}) noexcept;
  void dumpReceiverSites(const scene::BakeScene &source, std::span<const Capture> baseline, const ReceiverSiteAudit &audit,
      const ReceiverSiteChecks *checks, const ReceiverFitAudit *fits, const ReceiverAudit *coverage, std::stop_token cancelled = {}) noexcept;
  // Bounded preparation for opt-in local captures or an explicit diagnostic.
  // Reports are optional and existing report files never block retained evidence.
  // With neither request there is no sampling, pair storage or GPU traversal.
  std::unique_ptr<ReceiverAudit> prepareReceiverAudit(const scene::BakeScene &source,
      std::span<const Capture> captures, std::stop_token cancelled = {}) noexcept;
  bool retryReceiverAudit(const scene::BakeScene &source, std::span<const Capture> captures,
      ReceiverAudit &audit, std::stop_token cancelled = {}) noexcept;
  void dumpReceiverAudit(const scene::BakeScene &source, std::span<const Capture> captures,
      const ReceiverAudit &audit, std::stop_token cancelled = {}) noexcept;
  // Small, independent candidate report for hardware-fit experiments. Only
  // writes when REMIX_DEBUG_PROBE_CANDIDATES names a fresh JSON output file.
  void dumpCandidates(const scene::BakeScene &source, std::span<const FittedRegion> regions,
                      std::span<const Capture> captures, std::stop_token cancelled = {}) noexcept;
  // Optional developer aid. No CPU capture copy is retained after publication.
  // Writes only when REMIX_DEBUG_BAKE_CAPTURE_PREFIX names a fresh output prefix.
  // Failures are reported to stderr and never invalidate the completed bake.
  void dumpCaptures(uint64_t sceneHash, std::span<const Capture> captures) noexcept;
  // Optional local-only serialized Data dump; no whole-map raw capture copy.
  void dumpLocalImage(const Data &data) noexcept;
  // Dumps a named CPU block once, after a capture dump and .frame.request exist.
  // Suffixes are internal constants; raw files are tied to the producing build.
  void dumpFrameData(const char *suffix, const void *data, size_t bytes) noexcept;
  void dumpPublishedData(const char *suffix, const void *data, size_t bytes) noexcept;
  // Immutable transport diagnostics can precede reflection capture/publication.
  // Same fresh-prefix and once-per-suffix rules; never requires .capture.bin.
  void dumpTransportData(const char *suffix, const void *data, size_t bytes) noexcept;
} // namespace remix::forward::probes
