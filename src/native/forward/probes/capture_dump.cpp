#include "forward/probes/capture_dump.h"
#include "forward/probes/selection.h"
#include "forward/probes/receivers.h"
#include "forward/probes/receiver_pairs.h"
#include "forward/probes/receiver_audit.h"
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
  std::unique_ptr<ReceiverAudit> prepareReceiverAudit(const scene::BakeScene &source,
      std::span<const Capture> captures, std::stop_token cancelled) noexcept {
    const char *path = std::getenv("REMIX_DEBUG_PROBE_RECEIVER_VISIBILITY");
    if (!path || !*path) return {};
    try {
      if (std::filesystem::exists(path)) throw std::runtime_error("Receiver audit report already exists");
      const auto receivers = sampleReceivers(source, 8192, cancelled);
      return std::make_unique<ReceiverAudit>(
          receiverPairs(receivers, captures, .002f / source.sceneScale, 32768, 4, cancelled),
          .001f / source.sceneScale);
    } catch (const std::exception &error) {
      std::fprintf(stderr, "Receiver visibility audit unavailable: %s\n", error.what());
      return {};
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
