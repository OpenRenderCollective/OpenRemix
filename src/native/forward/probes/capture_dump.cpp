#include "forward/probes/capture_dump.h"
#include "forward/probes/selection.h"
#include "forward/probes/receivers.h"
#include "forward/probes/receiver_pairs.h"
#include "forward/probes/receiver_audit.h"
#include "forward/probes/receiver_sites.h"
#include "forward/probes/receiver_site_checks.h"
#include "forward/probes/receiver_fits.h"
#include "forward/probes/refinement.h"
#include "forward/probes/receiver_fit_pairs.h"
#include "scene/bake_scene.h"
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>

namespace remix::forward::probes {
  std::unique_ptr<ReceiverSiteAudit> prepareReceiverSites(const scene::BakeScene &source,
      const ReceiverAudit &audit, std::stop_token cancelled) noexcept {
    const char *prefix = std::getenv("REMIX_DEBUG_PROBE_RECEIVER_VISIBILITY");
    if (!prefix || !*prefix) return {};
    try {
      if (std::filesystem::exists(std::string(prefix) + ".sites.json"))
        throw std::runtime_error("Receiver site report already exists");
      const auto receivers = sampleReceivers(source, 8192, cancelled);
      return std::make_unique<ReceiverSiteAudit>(receiverSites(source, receivers, audit, 512, cancelled));
    } catch (const std::exception &error) {
      std::fprintf(stderr, "Receiver site audit unavailable: %s\n", error.what());
      return {};
    }
  }
  std::unique_ptr<ReceiverSiteChecks> prepareReceiverSiteChecks(const scene::BakeScene &source,
      const ReceiverSiteAudit &audit, std::stop_token cancelled) noexcept {
    try {
      const auto receivers = sampleReceivers(source, 8192, cancelled);
      return std::make_unique<ReceiverSiteChecks>(audit, receivers, source.sceneScale);
    } catch (const std::exception &error) {
      std::fprintf(stderr, "Receiver site checks unavailable: %s\n", error.what());
      return {};
    }
  }
  std::unique_ptr<ReceiverFitAudit> prepareReceiverFits(const scene::BakeScene &source,
      const ReceiverSiteAudit &audit, const ReceiverSiteChecks &checks, std::stop_token cancelled) noexcept {
    try {
      const auto receivers = sampleReceivers(source, 8192, cancelled);
      return std::make_unique<ReceiverFitAudit>(fitReceiverSites(source, receivers, audit, checks, 32, cancelled),
                                               audit.sites(), cancelled);
    } catch (const std::exception &error) {
      std::fprintf(stderr, "Receiver projection assessment unavailable: %s\n", error.what());
      return {};
    }
  }
  std::unique_ptr<ReceiverAudit> prepareReceiverFitCoverage(const scene::BakeScene &source,
      std::span<const Capture> baseline, const ReceiverFitAudit &fits, std::stop_token cancelled) noexcept {
    try {
      const auto receivers = sampleReceivers(source, 8192, cancelled);
      return std::make_unique<ReceiverAudit>(receiverFitPairs(receivers, baseline, fits.finish(),
          .002f / source.sceneScale, 32768, cancelled), .001f / source.sceneScale);
    } catch (const std::exception &error) {
      std::fprintf(stderr, "Local receiver coverage unavailable: %s\n", error.what());
      return {};
    }
  }
  void dumpReceiverSites(const scene::BakeScene &source, const ReceiverSiteAudit &audit,
      const ReceiverSiteChecks *checks, const ReceiverFitAudit *assessment, const ReceiverAudit *coverage, std::stop_token cancelled) noexcept {
    const char *prefix = std::getenv("REMIX_DEBUG_PROBE_RECEIVER_VISIBILITY");
    if (!prefix || !*prefix) return;
    try {
      const auto path = std::string(prefix) + ".sites.json";
      if (!audit.complete() || (checks && !checks->complete()) || (coverage && !coverage->complete()) ||
          std::filesystem::exists(path))
        throw std::runtime_error("Receiver sites incomplete or report exists");
      const auto fits = assessment ? assessment->finish() : ReceiverFits{};
      std::ofstream output(path);
      output.exceptions(std::ios::failbit | std::ios::badbit);
      output.precision(9);
      output << "{\"version\":1,\"sceneHash\":\"" << source.contentHash << "\",\"sceneScale\":"
             << source.sceneScale << ",\"offsetMetres\":0.75,\"samplesPerSite\":" << ReceiverSiteAudit::samples
             << ",\"receiverConnectionTested\":" << (checks && checks->complete() ? "true" : "false")
             << ",\"checkRays\":" << (checks ? checks->rays().size() : 0)
             << ",\"axisClearanceMetres\":0.1,\"sites\":[\n";
      auto visibility = [](ReceiverVisibility value) {
        return value == ReceiverVisibility::Visible ? "visible" :
               value == ReceiverVisibility::Blocked ? "blocked" : "unknown";
      };
      for (size_t i = 0; i < audit.sites().size(); ++i) {
        if (cancelled.stop_requested()) throw std::runtime_error("Receiver site report superseded");
        const auto &site = audit.sites()[i];
        output << (i ? ",\n" : "") << "{\"receiver\":" << site.receiver << ",\"position\":["
               << site.point.position[0] << ',' << site.point.position[1] << ',' << site.point.position[2]
               << "],\"partitionDisagreement\":" << (site.partitionDisagreement ? "true" : "false")
               << ",\"sitePartitionCluster\":" << (source.partition ? source.partition->cluster(site.point.position.data()) : -1)
               << ",\"backfaceFraction\":";
        if (std::isfinite(audit.backface(i))) output << audit.backface(i);
        else output << "null";
        output << ",\"classification\":\"" << (!audit.known(i) ? "unknown" :
            audit.sampledFree(i) ? "sampled-free" : "sampled-inside") << "\",\"connection\":\""
               << visibility(checks ? checks->connection(i) : ReceiverVisibility::Unknown)
               << "\",\"axisClearance\":\"" << visibility(checks ? checks->clearance(i) : ReceiverVisibility::Unknown)
               << "\",\"supported\":" << (checks && checks->supported(i) ? "true" : "false")
               << ",\"connectionBlock\":";
        const auto block = checks ? checks->connectionBlock(i) : std::nullopt;
        if (block) output << '[' << (*block)[0] << ',' << (*block)[1] << ',' << (*block)[2] << ']';
        else output << "null";
        output << '}';
      }
      output << "\n],\"projectionFits\":{\"geometryAudited\":" << (assessment ? "true" : "false")
             << ",\"geometryRays\":" << (assessment ? assessment->rays().size() : 0)
             << ",\"available\":"
             << (fits.available ? "true" : "false") << ",\"supported\":" << fits.supported
             << ",\"partitionRefused\":" << fits.partitionRefused << ",\"duplicates\":" << fits.duplicates
             << ",\"attempts\":[";
      auto vector = [&](Vec3 value) { output << '[' << value[0] << ',' << value[1] << ',' << value[2] << ']'; };
      for (size_t i = 0; i < fits.attempts.size(); ++i) {
        const auto &fit = fits.attempts[i];
        output << (i ? "," : "") << "{\"site\":" << fit.site << ",\"variants\":[";
        for (size_t j = 0; j < fit.variants.size(); ++j) {
          const auto &variant = fit.variants[j];
          output << (j ? "," : "") << "{\"minimum\":"; vector(variant.projection.minimum);
          output << ",\"maximum\":"; vector(variant.projection.maximum);
          output << ",\"influenceMinimum\":"; vector(variant.influence.minimum);
          output << ",\"influenceMaximum\":"; vector(variant.influence.maximum);
          output << ",\"coversReceiver\":" << (variant.coversReceiver ? "true" : "false") << ",\"geometry\":";
          if (variant.geometry) {
            const auto &g = *variant.geometry;
            output << "{\"quality\":" << g.quality << ",\"missFraction\":" << g.missFraction
                   << ",\"backfaceFraction\":" << g.backfaceFraction << ",\"faces\":[";
            for (uint32_t face = 0; face < 6; ++face) {
              const auto &a = g.faces[face];
              output << (face ? "," : "") << "{\"solidAngleFraction\":" << a.solidAngleFraction << ",\"supportFraction\":" << a.supportFraction
                     << ",\"nearFraction\":" << a.nearFraction << ",\"farFraction\":" << a.farFraction
                     << ",\"missFraction\":" << a.missFraction << ",\"backfaceFraction\":" << a.backfaceFraction
                     << ",\"meanRelativeResidual\":" << a.meanRelativeResidual << ",\"meanAbsoluteRelativeResidual\":" << a.meanAbsoluteRelativeResidual << ",\"hitMinimum\":";
              if (a.hitBounds.valid) vector(a.hitBounds.minimum);
              else output << "null";
              output << ",\"hitMaximum\":";
              if (a.hitBounds.valid) vector(a.hitBounds.maximum);
              else output << "null";
              output << ",\"plane\":";
              if (a.plane) {
                output << "{\"lower\":" << a.plane->lower << ",\"median\":" << a.plane->median
                       << ",\"upper\":" << a.plane->upper << ",\"sampleFraction\":" << a.plane->sampleFraction
                       << ",\"hits\":" << a.plane->hits << '}';
              } else output << "null";
              output << '}';
            }
            output << "]}";
          } else output << "null";
          output << ",\"refinement\":";
          const auto refinement = variant.geometry ? refineProjection(*variant.geometry, .001f / source.sceneScale)
                                                    : std::nullopt;
          if (refinement) {
            auto region = *variant.geometry;
            region.bounds = refinement->bounds;
            const auto reassessed = assessment->reassess({region});
            output << "{\"reassessed\":true,\"faces\":" << refinement->faces << ",\"minimum\":";
            vector(refinement->bounds.minimum);
            output << ",\"maximum\":"; vector(refinement->bounds.maximum);
            output << ",\"geometry\":{\"quality\":" << reassessed[0].quality << ",\"faces\":[";
            for (uint32_t face = 0; face < 6; ++face) {
              const auto &g = reassessed[0].faces[face];
              output << (face ? "," : "") << "{\"solidAngleFraction\":" << g.solidAngleFraction
                     << ",\"nearFraction\":" << g.nearFraction << ",\"farFraction\":" << g.farFraction
                     << ",\"missFraction\":" << g.missFraction
                     << ",\"meanRelativeResidual\":" << g.meanRelativeResidual << ",\"meanAbsoluteRelativeResidual\":" << g.meanAbsoluteRelativeResidual << '}';
            }
            output << "]}}";
          } else output << "null";
          output << '}';
        }
        output << "]}";
      }
      output << "]},\"localCoverage\":{\"tested\":" << (coverage ? "true" : "false")
             << ",\"baseline\":\"automatic-before-authored-composition\",\"pairs\":[";
      if (coverage) for (size_t i = 0; i < coverage->pairs().size(); ++i) {
        if (cancelled.stop_requested()) throw std::runtime_error("Local coverage report superseded");
        const auto &pair = coverage->pairs()[i];
        output << (i ? "," : "") << "{\"receiver\":" << pair.receiver << ",\"attempt\":" << pair.capture
               << ",\"segmentDistance\":" << pair.ray.distance << ",\"hitDistance\":";
        if (std::isfinite(coverage->distance(i))) output << coverage->distance(i);
        else output << "null";
        output << ",\"visibility\":\"" << visibility(coverage->visibility()[i]) << "\"}";
      }
      output << "]}}\n";
    } catch (const std::exception &error) {
      std::fprintf(stderr, "Receiver site report unavailable: %s\n", error.what());
    }
  }
  std::unique_ptr<ReceiverAudit> prepareReceiverAudit(const scene::BakeScene &source,
      std::span<const Capture> captures, std::stop_token cancelled) noexcept {
    const char *path = std::getenv("REMIX_DEBUG_PROBE_RECEIVER_VISIBILITY");
    if (!path || !*path) return {};
    try {
      if (std::filesystem::exists(path) || std::filesystem::exists(std::string(path) + ".sites.json"))
        throw std::runtime_error("Receiver audit report already exists");
      const auto receivers = sampleReceivers(source, 8192, cancelled);
      return std::make_unique<ReceiverAudit>(
          receiverPairs(receivers, captures, .002f / source.sceneScale, 32768, 4, cancelled),
          .001f / source.sceneScale);
    } catch (const std::exception &error) {
      std::fprintf(stderr, "Receiver visibility audit unavailable: %s\n", error.what());
      return {};
    }
  }
  bool retryReceiverAudit(const scene::BakeScene &source, std::span<const Capture> captures,
      ReceiverAudit &audit, std::stop_token cancelled) noexcept {
    try {
      if (!audit.complete() || audit.pairs().size() == 32768) return false;
      const auto receivers = sampleReceivers(source, 8192, cancelled);
      auto pairs = receiverRetryPairs(receivers, captures, audit.pairs(), audit.visibility(),
          .002f / source.sceneScale, uint32_t(32768 - audit.pairs().size()), cancelled);
      if (pairs.empty()) return false;
      audit.append(std::move(pairs));
      return true;
    } catch (const std::exception &error) {
      std::fprintf(stderr, "Receiver visibility retry unavailable: %s\n", error.what());
      return false;
    }
  }
  void dumpReceiverAudit(const scene::BakeScene &source, std::span<const Capture> captures,
      const ReceiverAudit &audit, std::stop_token cancelled) noexcept {
    const char *path = std::getenv("REMIX_DEBUG_PROBE_RECEIVER_VISIBILITY");
    if (!path || !*path) return;
    try {
      if (!audit.complete() || std::filesystem::exists(path))
        throw std::runtime_error("Receiver audit is incomplete or its report already exists");
      const auto receivers = sampleReceivers(source, 8192, cancelled);
      std::ofstream output(path);
      output.exceptions(std::ios::failbit | std::ios::badbit);
      output.precision(9);
      auto vector = [&](Vec3 value) { output << '[' << value[0] << ',' << value[1] << ',' << value[2] << ']'; };
      output << "{\"version\":1,\"sceneHash\":\"" << source.contentHash
             << "\",\"sceneScale\":" << source.sceneScale << ",\"pairs\":[\n";
      for (size_t i = 0; i < audit.pairs().size(); ++i) {
        if (cancelled.stop_requested()) throw std::runtime_error("Receiver visibility report superseded");
        const auto &pair = audit.pairs()[i];
        const auto &receiver = receivers.at(pair.receiver);
        const auto state = audit.visibility(i);
        output << (i ? ",\n" : "") << "{\"receiver\":" << pair.receiver << ",\"capture\":" << pair.capture
               << ",\"candidateRank\":" << pair.rank
               << ",\"position\":";
        vector(receiver.position);
        output << ",\"target\":";
        vector(captures[pair.capture].position);
        output << ",\"segmentDistance\":" << pair.ray.distance << ",\"hitDistance\":";
        if (std::isfinite(audit.distance(i))) output << audit.distance(i);
        else output << "null";
        output << ",\"visibility\":\"" << (state == ReceiverVisibility::Visible ? "visible" :
            state == ReceiverVisibility::Blocked ? "blocked" : "unknown") << "\"}";
      }
      output << "\n]}\n";
    } catch (const std::exception &error) {
      std::fprintf(stderr, "Receiver visibility report unavailable: %s\n", error.what());
    }
  }
  void dumpCandidates(const scene::BakeScene &source, std::span<const FittedRegion> regions,
                      std::span<const Capture> captures, std::stop_token cancelled) noexcept {
    const char *path = std::getenv("REMIX_DEBUG_PROBE_CANDIDATES");
    if (!path || !*path)
      return;
    try {
      if (std::filesystem::exists(path))
        throw std::runtime_error("Candidate report exists; choose a fresh path");
      const auto receivers = sampleReceivers(source, 8192, cancelled);
      // Report all bounded proposals; this plans rays but never submits them.
      const auto pairs = receiverPairs(receivers, captures, .002f / source.sceneScale, 32768, 4, cancelled);
      std::ofstream output(path);
      output.exceptions(std::ios::failbit | std::ios::badbit);
      output.precision(9);
      auto vector = [&](Vec3 value) {
        output << '[' << value[0] << ',' << value[1] << ',' << value[2] << ']';
      };
      output << "{\"version\":2,\"sceneHash\":\"" << source.contentHash
             << "\",\"sceneScale\":" << source.sceneScale << ",\"candidates\":[\n";
      for (size_t i = 0; i < regions.size(); ++i) {
        const auto &region = regions[i];
        const bool selected = std::any_of(captures.begin(), captures.end(), [&](const auto &capture) {
          return capture.position == region.site.position &&
                 capture.bounds.minimum == region.bounds.minimum &&
                 capture.bounds.maximum == region.bounds.maximum;
        });
        output << (i ? ",\n" : "") << "{\"position\":";
        vector(region.site.position);
        output << ",\"minimum\":";
        vector(region.bounds.minimum);
        output << ",\"maximum\":";
        vector(region.bounds.maximum);
        output << ",\"evaluated\":" << (region.geometryEvaluated ? "true" : "false")
               << ",\"agreement\":" << region.quality << ",\"missFraction\":" << region.missFraction
               << ",\"backfaceFraction\":" << region.backfaceFraction
               << ",\"selected\":" << (selected ? "true" : "false") << ",\"faces\":[";
        for (size_t face = 0; face < region.faces.size(); ++face) {
          const auto &audit = region.faces[face];
          output << (face ? "," : "") << "{\"solidAngleFraction\":" << audit.solidAngleFraction
                 << ",\"supportFraction\":" << audit.supportFraction
                 << ",\"nearFraction\":" << audit.nearFraction
                 << ",\"farFraction\":" << audit.farFraction
                 << ",\"missFraction\":" << audit.missFraction
                 << ",\"backfaceFraction\":" << audit.backfaceFraction
                 << ",\"meanRelativeResidual\":" << audit.meanRelativeResidual
                 << ",\"meanAbsoluteRelativeResidual\":" << audit.meanAbsoluteRelativeResidual
                 << ",\"hitMinimum\":";
          if (audit.hitBounds.valid) vector(audit.hitBounds.minimum);
          else output << "null";
          output << ",\"hitMaximum\":";
          if (audit.hitBounds.valid) vector(audit.hitBounds.maximum);
          else output << "null";
          output << '}';
        }
        output << "]}";
      }
      output << "\n],\"receivers\":[\n";
      for (size_t i = 0; i < receivers.size(); ++i) {
        if (cancelled.stop_requested())
          throw std::runtime_error("Reflection receiver report superseded");
        const auto &receiver = receivers[i];
        output << (i ? ",\n" : "") << "{\"position\":";
        vector(receiver.position);
        output << ",\"normal\":";
        vector(receiver.normal);
        output << ",\"areaMetresSquared\":" << receiver.areaMetresSquared
               << ",\"draw\":" << receiver.draw << ",\"chart\":" << receiver.chart
               << ",\"firstIndex\":" << receiver.firstIndex
               << ",\"terrain\":" << (source.draws.at(receiver.draw).terrain ? "true" : "false") << '}';
      }
      output << "\n],\"visibilityCaptureOrigins\":[";
      for (size_t i = 0; i < captures.size(); ++i) {
        if (i) output << ',';
        vector(captures[i].position);
      }
      output << "],\"visibilityPairs\":[\n";
      for (size_t i = 0; i < pairs.size(); ++i) {
        if (cancelled.stop_requested()) throw std::runtime_error("Receiver pair report superseded");
        const auto &pair = pairs[i];
        output << (i ? ",\n" : "") << "{\"receiver\":" << pair.receiver
               << ",\"capture\":" << pair.capture << ",\"origin\":";
        vector({pair.ray.point.position[0], pair.ray.point.position[1], pair.ray.point.position[2]});
        output << ",\"direction\":";
        vector({pair.ray.point.normal[0], pair.ray.point.normal[1], pair.ray.point.normal[2]});
        output << ",\"distance\":" << pair.ray.distance << '}';
      }
      output << "\n]}\n";
    } catch (const std::exception &error) {
      std::fprintf(stderr, "Reflection candidate report unavailable: %s\n", error.what());
    }
  }
  namespace {
    void dumpData(const char *suffix, const void *data, size_t bytes, bool frameRequest,
                  bool requireCapture = true) noexcept {
      const char *prefix = std::getenv("REMIX_DEBUG_BAKE_CAPTURE_PREFIX");
      if ((!prefix || !*prefix) && !frameRequest) {
        // Inspect probe fields without dumping the much larger surface atlas.
        const std::string_view name(suffix);
        if (name == ".irradiance-published.bin" || name == ".styled-irradiance.bin" ||
            name == ".local-probe-visibility.bin" || name == ".receiver-support.bin" ||
            name == ".receiver-halo.bin" || name == ".receiver-grid-header.bin" ||
            name == ".receiver-grid-lookup.bin") {
          prefix = std::getenv("REMIX_DEBUG_BAKE_GRID_PREFIX");
          requireCapture = false;
        }
      }
      if (!prefix || !*prefix)
        return;
      try {
        static thread_local std::unordered_set<std::string> written;
        if (written.contains(suffix))
          return;
        if (requireCapture && !std::filesystem::exists(std::string(prefix) + ".capture.bin"))
          return;
        if (frameRequest && !std::filesystem::exists(std::string(prefix) + ".frame.request"))
          return;
        written.insert(suffix);
        const auto path = std::filesystem::path(std::string(prefix) + suffix);
        if (std::filesystem::exists(path))
          throw std::runtime_error("Frame data dump already exists; choose a fresh prefix");
        std::ofstream output(path, std::ios::binary);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output.write(static_cast<const char *>(data), std::streamsize(bytes));
        output.close();
      } catch (const std::exception &error) {
        std::fprintf(stderr, "Frame data dump unavailable: %s\n", error.what());
      }
    }
  } // namespace
  void dumpFrameData(const char *suffix, const void *data, size_t bytes) noexcept {
    dumpData(suffix, data, bytes, true);
  }
  void dumpPublishedData(const char *suffix, const void *data, size_t bytes) noexcept {
    dumpData(suffix, data, bytes, false);
  }
  void dumpTransportData(const char *suffix, const void *data, size_t bytes) noexcept {
    dumpData(suffix, data, bytes, false, false);
  }
  void dumpCaptures(uint64_t sceneHash, std::span<const Capture> captures) noexcept {
    const char *prefix = std::getenv("REMIX_DEBUG_BAKE_CAPTURE_PREFIX");
    if (!prefix || !*prefix)
      return;
    try {
      static_assert(std::endian::native == std::endian::little);
      const auto path = std::filesystem::path(std::string(prefix) + ".capture.bin");
      if (std::filesystem::exists(path))
        throw std::runtime_error("Capture dump already exists; choose a fresh prefix");
      std::ofstream output(path, std::ios::binary);
      output.exceptions(std::ios::failbit | std::ios::badbit);
      auto write = [&](const void *data, size_t bytes) {
        output.write(static_cast<const char *>(data), std::streamsize(bytes));
      };
      // Private little-endian diagnostic format: magic, scene hash, count,
      // followed by resolution, XYZ origin and six RGBA32F faces per probe.
      // RGB stores radiance; A stores first-hit distance. No struct padding.
      write("RMXCUB01", 8);
      write(&sceneHash, sizeof(sceneHash));
      const uint32_t count = uint32_t(captures.size());
      write(&count, sizeof(count));
      for (const auto &capture : captures) {
        if (capture.radiance.size() != 6ull * capture.resolution * capture.resolution)
          throw std::runtime_error("Incomplete cubemap capture dump");
        write(&capture.resolution, sizeof(capture.resolution));
        write(capture.position.data(), sizeof(capture.position));
        write(capture.radiance.data(), capture.radiance.size() * sizeof(Vec4));
      }
      output.close();
      std::fprintf(stderr, "Wrote raw cubemap captures: %s\n", path.string().c_str());
    } catch (const std::exception &error) {
      std::fprintf(stderr, "Raw cubemap capture dump unavailable: %s\n", error.what());
    }
  }
} // namespace remix::forward::probes
