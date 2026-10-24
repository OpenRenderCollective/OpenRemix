#include "forward/bake/local_reflection_cache.h"
#include "forward/bake/cache_io.h"
#include "forward/bake/cache_file.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <set>

namespace remix::forward::bake::local_cache {
  namespace {
    namespace io = cache::detail;
    constexpr uint32_t version = 1, maximumAttempts = 32, maximumObservations = 32768;
    constexpr uint64_t maximumBytes = 160ull << 20;
    void require(bool value) {
      if (!value) throw std::invalid_argument("Invalid local reflection cache");
    }
    template <class T> void finite(const T &values) {
      for (float value : values) require(std::isfinite(value));
    }
    bool fraction(float x) { return std::isfinite(x) && x >= 0 && x <= 1.00001f; }
    void validateBounds(const Bounds &b, bool required = true) {
      finite(b.minimum); finite(b.maximum);
      require(!required || b.valid);
      if (b.valid) for (uint32_t c = 0; c < 3; ++c)
        require((required ? b.minimum[c] < b.maximum[c] : b.minimum[c] <= b.maximum[c]) &&
                std::isfinite(b.maximum[c] - b.minimum[c]));
    }
    void putBounds(io::Writer &out, const Bounds &b) {
      out.pod(uint32_t(b.valid)); out.pod(b.minimum); out.pod(b.maximum);
    }
    Bounds getBounds(io::Reader &in, bool required = true) {
      Bounds b;
      const auto valid = in.pod<uint32_t>(); require(valid <= 1);
      b.valid = valid != 0; b.minimum = in.pod<Vec3>(); b.maximum = in.pod<Vec3>();
      validateBounds(b, required); return b;
    }
    std::array<uint64_t, 2> geometryIdentity(const scene::BakeScene &source, std::stop_token cancelled) {
      uint64_t receivers = 1469598103934665603ull, hints = receivers;
      for (const auto &sample : probes::sampleReceivers(source, 8192, cancelled)) {
        receivers = scene::hashBytes(sample.position.data(), sizeof(sample.position), receivers);
        receivers = scene::hashBytes(sample.normal.data(), sizeof(sample.normal), receivers);
        receivers = scene::hashBytes(&sample.areaMetresSquared, sizeof(sample.areaMetresSquared), receivers);
        const uint32_t owner[]{sample.draw, sample.chart, sample.firstIndex};
        receivers = scene::hashBytes(owner, sizeof(owner), receivers);
      }
      for (const auto &hint : source.reflectionHints) {
        io::check(cancelled); hints = scene::hashBytes(hint.data(), sizeof(hint), hints);
      }
      return {receivers, hints};
    }
    void validateGeometry(const probes::FittedRegion &g) {
      validateBounds(g.bounds); finite(g.site.position);
      require(g.geometryEvaluated && std::isfinite(g.site.clearance) && g.site.clearance >= 0 &&
              g.site.enclosedAxes <= 3 && fraction(g.quality) && fraction(g.missFraction) && fraction(g.backfaceFraction));
      for (const auto &f : g.faces) {
        for (float x : {f.solidAngleFraction, f.supportFraction, f.nearFraction, f.farFraction,
                         f.missFraction, f.backfaceFraction}) require(fraction(x));
        require(std::isfinite(f.meanRelativeResidual) && std::isfinite(f.meanAbsoluteRelativeResidual) &&
                f.meanAbsoluteRelativeResidual >= 0);
        validateBounds(f.hitBounds, false);
        if (f.plane) require(std::isfinite(f.plane->lower) && std::isfinite(f.plane->median) &&
            std::isfinite(f.plane->upper) && f.plane->lower <= f.plane->median && f.plane->median <= f.plane->upper &&
            fraction(f.plane->sampleFraction) && f.plane->hits <= 384);
      }
    }
    void putGeometry(io::Writer &out, const probes::FittedRegion &g) {
      putBounds(out, g.bounds); out.pod(g.site.position); out.pod(g.site.clearance); out.pod(g.site.enclosedAxes);
      out.pod(std::array{g.quality, g.missFraction, g.backfaceFraction}); out.pod(uint32_t(g.geometryEvaluated));
      for (const auto &f : g.faces) {
        out.pod(std::array{f.solidAngleFraction, f.supportFraction, f.nearFraction, f.farFraction,
                           f.missFraction, f.backfaceFraction, f.meanRelativeResidual, f.meanAbsoluteRelativeResidual});
        putBounds(out, f.hitBounds); out.pod(uint32_t(bool(f.plane)));
        if (f.plane) {
          out.pod(std::array{f.plane->lower, f.plane->median, f.plane->upper, f.plane->sampleFraction});
          out.pod(f.plane->hits);
        }
      }
    }
    probes::FittedRegion getGeometry(io::Reader &in) {
      probes::FittedRegion g;
      g.bounds = getBounds(in); g.site.position = in.pod<Vec3>(); g.site.clearance = in.pod<float>();
      g.site.enclosedAxes = in.pod<uint32_t>();
      const auto signal = in.pod<std::array<float,3>>();
      g.quality = signal[0]; g.missFraction = signal[1]; g.backfaceFraction = signal[2];
      require(in.pod<uint32_t>() == 1); g.geometryEvaluated = true;
      finite(g.site.position);
      require(std::isfinite(g.site.clearance) && g.site.clearance >= 0 && g.site.enclosedAxes <= 3);
      for (float value : signal) require(fraction(value));
      for (auto &f : g.faces) {
        const auto values = in.pod<std::array<float,8>>(); finite(values);
        for (uint32_t c = 0; c < 6; ++c) require(fraction(values[c]));
        require(values[7] >= 0);
        f.solidAngleFraction = values[0]; f.supportFraction = values[1]; f.nearFraction = values[2];
        f.farFraction = values[3]; f.missFraction = values[4]; f.backfaceFraction = values[5];
        f.meanRelativeResidual = values[6]; f.meanAbsoluteRelativeResidual = values[7];
        f.hitBounds = getBounds(in, false);
        const auto present = in.pod<uint32_t>(); require(present <= 1);
        if (present) {
          const auto p = in.pod<std::array<float,4>>(); finite(p);
          const auto hits = in.pod<uint32_t>();
          require(p[0] <= p[1] && p[1] <= p[2] && fraction(p[3]) && hits <= 384);
          f.plane = probes::ProjectionPlaneAudit{p[0],p[1],p[2],p[3],hits};
        }
      }
      return g;
    }
    void validate(const Snapshot &s, std::stop_token cancelled) {
      require(s.source && s.source->ready && s.evidence &&
              probes::matchesCoverageContext(s.evidence->sourceContext, *s.source, {}));
      require(std::isfinite(s.detailTolerance) && s.detailTolerance >= 0);
      const auto &fits = s.evidence->fits;
      require(fits.attempts.size() <= maximumAttempts && s.evidence->observations.size() <= maximumObservations &&
              std::isfinite(fits.detailToleranceMetres) && fits.detailToleranceMetres >= 0);
      uint32_t candidates = 0;
      for (const auto &a : fits.attempts) {
        require(a.variants.size() <= probes::ReceiverSiteFit::maximumVariants);
        candidates += uint32_t(a.variants.size());
        for (const auto &v : a.variants) {
          validateBounds(v.projection); validateBounds(v.influence);
          require(v.geometry && v.geometry->geometryEvaluated);
          validateGeometry(*v.geometry);
        }
      }
      const auto receivers = probes::sampleReceivers(*s.source, 8192, cancelled);
      std::set<std::array<uint32_t,2>> observations;
      for (const auto &o : s.evidence->observations) {
        require(o.receiver < receivers.size() && o.capture < fits.attempts.size() &&
                (o.visibility == probes::ReceiverVisibility::Unknown || o.visibility == probes::ReceiverVisibility::Visible ||
                 o.visibility == probes::ReceiverVisibility::Blocked) && observations.insert({o.receiver,o.capture}).second);
      }
      const auto &data = s.image.data;
      const auto &[count, size, levels, stride] = data.header.parameters;
      require(count && count <= probes::LocalCaptureWork::maximumCaptures && size >= 8 && size <= 512 &&
              std::has_single_bit(size) && levels == std::bit_width(size) && stride == probes::levelOffset(size, levels) &&
              data.texels.size() == size_t(count) * stride && s.image.selection.layers.size() == count);
      require(std::memcmp(&s.image.selection.header, &data.header, sizeof(probes::Header)) == 0);
      std::set<uint32_t> layers, selected;
      for (const auto &mapping : s.image.selection.layers) {
        require(mapping.candidate < candidates && mapping.layer < count && layers.insert(mapping.layer).second &&
                selected.insert(mapping.candidate).second);
        const auto &r = data.header.probes[mapping.layer];
        uint32_t ordinal = mapping.candidate;
        const probes::ReceiverProjection *variant = nullptr;
        for (const auto &attempt : fits.attempts) {
          if (ordinal < attempt.variants.size()) { variant = &attempt.variants[ordinal]; break; }
          ordinal -= uint32_t(attempt.variants.size());
        }
        require(variant && r.worldToLocal == probes::worldSpace);
        for (uint32_t c = 0; c < 3; ++c)
          require(r.minimum[c] == variant->projection.minimum[c] && r.maximum[c] == variant->projection.maximum[c] &&
                  r.influenceMinimum[c] == variant->influence.minimum[c] &&
                  r.influenceMaximum[c] == variant->influence.maximum[c] && r.position[c] == variant->geometry->site.position[c]);
        probes::Capture capture;
        capture.resolution = size; capture.worldToLocal = r.worldToLocal;
        capture.bounds.valid = capture.influence.valid = true;
        for (uint32_t c = 0; c < 3; ++c) {
          capture.bounds.minimum[c] = r.minimum[c]; capture.bounds.maximum[c] = r.maximum[c];
          capture.influence.minimum[c] = r.influenceMinimum[c]; capture.influence.maximum[c] = r.influenceMaximum[c];
          capture.position[c] = r.position[c];
        }
        probes::validateCapture(capture);
        for (float x : std::bit_cast<std::array<float,32>>(r)) require(std::isfinite(x));
        require(r.minimum[3] == 0 && fraction(r.position[3]) && r.maximum[3] >= 0 &&
                r.influenceMinimum[3] >= 0 && r.influenceMaximum[3] >= 0);
      }
      for (const auto &pixel : data.texels) {
        io::check(cancelled); finite(pixel);
        require(pixel[0] >= 0 && pixel[0] <= 65000 && pixel[1] >= 0 && pixel[1] <= 65000 &&
                pixel[2] >= 0 && pixel[2] <= 65000 && pixel[3] >= 0);
      }
      // Also enforce completed geometry, finite planner errors and candidate ownership.
      (void)probes::planReceiverCoverage(*s.source, {}, *s.evidence, cancelled);
    }
  }
  std::filesystem::path path(std::filesystem::path file) { file += ".local-reflections-v1"; return file; }
  uint64_t retainedBytes(const Snapshot &s) {
    uint64_t bytes = sizeof(Snapshot) + s.image.data.texels.capacity() * sizeof(Vec4) +
        s.image.selection.layers.capacity() * sizeof(probes::CoverageReadyLayer) +
        s.image.selection.context.baseline.volumes.capacity() * sizeof(probes::CoverageVolume);
    if (s.evidence) {
      bytes += sizeof(*s.evidence) + s.evidence->observations.capacity() * sizeof(probes::ReceiverCoverageObservation) +
          s.evidence->fits.attempts.capacity() * sizeof(probes::ReceiverSiteFit);
      for (const auto &attempt : s.evidence->fits.attempts)
        bytes += attempt.variants.capacity() * sizeof(probes::ReceiverProjection);
    }
    return bytes;
  }
  bool write(const std::filesystem::path &file, uint64_t key, const Snapshot &s, std::stop_token cancelled) {
    std::filesystem::path temporary;
    bool owned = false;
    try {
      validate(s, cancelled);
      std::filesystem::create_directories(file.parent_path());
      std::random_device random;
      temporary = file; temporary += ".tmp-" + std::to_string((uint64_t(random()) << 32) ^ random());
      if (std::filesystem::exists(temporary)) throw std::runtime_error("Local reflection cache temporary exists");
      io::Writer out(temporary, key, cancelled); owned = true;
      out.pod(version); out.pod(std::bit_cast<uint32_t>(s.source->sceneScale)); out.pod(s.source->contentHash);
      out.pod(geometryIdentity(*s.source, cancelled)); out.pod(s.detailTolerance);
      const auto &fits = s.evidence->fits;
      out.pod(uint32_t(fits.available)); out.pod(fits.supported); out.pod(fits.partitionRefused); out.pod(fits.duplicates);
      out.pod(fits.detailToleranceMetres); out.pod(uint32_t(fits.attempts.size()));
      for (const auto &attempt : fits.attempts) {
        out.pod(attempt.site); out.pod(uint32_t(attempt.variants.size()));
        for (const auto &v : attempt.variants) {
          out.pod(uint32_t(v.coversReceiver) | uint32_t(v.detailTolerant) << 1);
          putBounds(out, v.projection); putBounds(out, v.influence); putGeometry(out, *v.geometry);
        }
      }
      static_assert(sizeof(probes::ReceiverCoverageObservation) == 12 && sizeof(probes::CoverageReadyLayer) == 8);
      out.array(s.evidence->observations); out.pod(s.image.data.header); out.array(s.image.data.texels);
      out.array(s.image.selection.layers); out.finish();
      require(std::filesystem::file_size(temporary) <= maximumBytes);
      io::check(cancelled); cache::detail::replaceFile(temporary, file); return true;
    } catch (const std::exception &error) {
      if (owned) { std::error_code ignored; std::filesystem::remove(temporary, ignored); }
      if (!cancelled.stop_requested()) std::fprintf(stderr, "Local reflection cache write unavailable: %s\n", error.what());
      return false;
    }
  }
  std::unique_ptr<Snapshot> read(const std::filesystem::path &file, uint64_t key,
      std::shared_ptr<const scene::BakeScene> source, const Config &config,
      const probes::CoverageBaselineSet &baseline, std::stop_token cancelled) {
    try {
      require(source && source->ready && config.reflectionResolution >= 8 && config.reflectionResolution <= 512 &&
              std::has_single_bit(config.reflectionResolution) && std::filesystem::file_size(file) <= maximumBytes);
      io::Reader in(file, key, cancelled);
      require(in.pod<uint32_t>() == version && in.pod<uint32_t>() == std::bit_cast<uint32_t>(source->sceneScale) &&
              in.pod<uint64_t>() == source->contentHash && in.pod<std::array<uint64_t,2>>() == geometryIdentity(*source, cancelled));
      auto result = std::make_unique<Snapshot>(); result->source = source;
      result->detailTolerance = in.pod<float>();
      require(std::bit_cast<uint32_t>(result->detailTolerance) == std::bit_cast<uint32_t>(config.reflectionDetailTolerance));
      auto evidence = std::make_shared<probes::ReceiverCoverageEvidence>();
      evidence->sourceContext = probes::coveragePlanContext(*source, {});
      auto &fits = evidence->fits;
      const auto available = in.pod<uint32_t>(); require(available <= 1); fits.available = available != 0;
      fits.supported = in.pod<uint32_t>(); fits.partitionRefused = in.pod<uint32_t>(); fits.duplicates = in.pod<uint32_t>();
      fits.detailToleranceMetres = in.pod<float>();
      const auto attempts = in.pod<uint32_t>(); require(attempts <= maximumAttempts);
      for (uint32_t a = 0; a < attempts; ++a) {
        probes::ReceiverSiteFit fit; fit.site = in.pod<uint32_t>();
        const auto variants = in.pod<uint32_t>(); require(variants <= probes::ReceiverSiteFit::maximumVariants);
        for (uint32_t v = 0; v < variants; ++v) {
          probes::ReceiverProjection projection;
          const auto flags = in.pod<uint32_t>(); require(flags <= 3);
          projection.coversReceiver = (flags & 1) != 0; projection.detailTolerant = (flags & 2) != 0;
          projection.projection = getBounds(in); projection.influence = getBounds(in); projection.geometry = getGeometry(in);
          fit.variants.push_back(std::move(projection));
        }
        fits.attempts.push_back(std::move(fit));
      }
      evidence->observations = in.array<probes::ReceiverCoverageObservation>(maximumObservations);
      result->evidence = std::move(evidence);
      auto &image = result->image;
      image.data.header = in.pod<probes::Header>();
      require(image.data.header.parameters[1] == config.reflectionResolution);
      image.data.texels = in.array<Vec4>(4ull * probes::levelOffset(config.reflectionResolution, std::bit_width(config.reflectionResolution)));
      image.selection.header = image.data.header;
      image.selection.context = probes::coveragePlanContext(*source, baseline);
      image.selection.layers = in.array<probes::CoverageReadyLayer>(4);
      in.finish(); validate(*result, cancelled); return result;
    } catch (const std::exception &error) {
      if (!cancelled.stop_requested()) std::fprintf(stderr, "Local reflection cache miss: %s\n", error.what());
      return {};
    }
  }
}
