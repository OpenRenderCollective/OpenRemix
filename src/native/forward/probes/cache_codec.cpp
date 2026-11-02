#include "forward/probes/cache_codec.h"
#include "forward/probes/priority_bands.h"
#include <cmath>

namespace remix::forward::probes::cache_codec {
  namespace {
    // Disk headers use whole 32-record blocks, independent of GPU capacity.
    // This preserves every old <=32-capture payload, and keeps larger payloads
    // stable if the GPU table grows again. The preceding count defines the size.
    constexpr uint32_t probeHeaderBlockSlots = 32;
    using ProbeHeaderBlock = std::array<probes::Record, probeHeaderBlockSlots>;
    static_assert(sizeof(ProbeHeaderBlock) == 4096 && probes::maxProbes % probeHeaderBlockSlots == 0);
    uint32_t probeHeaderBlocks(uint32_t count) {
      return std::max(1u, (count + probeHeaderBlockSlots - 1) / probeHeaderBlockSlots);
    }
    void require(bool value) {
      if (!value) throw std::runtime_error("Invalid reflection cache metadata");
    }
    template <typename Range> void finite(const Range &values) {
      for (auto value : values) require(std::isfinite(value));
    }
    void bounds(const Bounds &value) {
      finite(value.minimum);
      finite(value.maximum);
      if (value.valid)
        for (uint32_t c = 0; c < 3; ++c)
          require(value.minimum[c] < value.maximum[c] && std::isfinite(value.maximum[c] - value.minimum[c]) &&
                  std::isfinite(value.maximum[c] + value.minimum[c]));
    }
  }
  void read(cache_io::Reader &input, std::vector<Capture> &captures, Data &data, Limits limits) {
    const auto count = input.pod<uint32_t>();
    require(count <= std::min(limits.count, maxProbes));
    captures.resize(count);
    require(!count || (limits.resolution && limits.resolution <= 512 &&
                       std::has_single_bit(limits.resolution)));
    for (auto &capture : captures) {
      const auto flags = input.pod<uint32_t>();
      require(flags == 1 || flags == 3);
      capture.bounds.valid = true;
      capture.influence.valid = (flags & 2) != 0;
      capture.bounds.minimum = input.pod<Vec3>();
      capture.bounds.maximum = input.pod<Vec3>();
      capture.influence.minimum = input.pod<Vec3>();
      capture.influence.maximum = input.pod<Vec3>();
      capture.position = input.pod<Vec3>();
      capture.worldToLocal = input.pod<ProjectionSpace>();
      capture.priority = input.pod<int32_t>();
      require(validProjectionSpace(capture.worldToLocal) &&
              capture.priority >= -32768 && capture.priority <= 32767);
      const auto localOrigin = projectionPoint(capture.worldToLocal, capture.position);
      capture.resolution = input.pod<uint32_t>();
      bounds(capture.bounds);
      bounds(capture.influence);
      finite(capture.position);
      for (uint32_t c = 0; c < 3; ++c)
        require(localOrigin[c] >= capture.bounds.minimum[c] &&
                localOrigin[c] <= capture.bounds.maximum[c]);
      require(capture.resolution == limits.resolution);
      const uint64_t pixels = 6ull * capture.resolution * capture.resolution;
      capture.radiance = input.array<Vec4>(pixels);
      require(capture.radiance.size() == pixels);
      for (const auto &pixel : capture.radiance) {
        finite(pixel);
        require(pixel[0] >= 0 && pixel[0] <= 65000 && pixel[1] >= 0 && pixel[1] <= 65000 && pixel[2] >= 0 &&
                pixel[2] <= 65000 && pixel[3] >= 0);
      }
    }
    data.header.parameters = input.pod<std::array<uint32_t, 4>>();
    for (uint32_t block = 0; block < probeHeaderBlocks(count); ++block) {
      const auto stored = input.pod<ProbeHeaderBlock>();
      std::copy(stored.begin(), stored.end(), data.header.probes.begin() + block * probeHeaderBlockSlots);
    }
    const uint32_t size = count ? limits.resolution : 0;
    const uint32_t levels = uint32_t(std::bit_width(size));
    const uint32_t stride = size ? levelOffset(size, levels) : 0;
    require(data.header.parameters == std::array<uint32_t, 4>{count, size, levels, stride});
    require(limits.storageRange >= sizeof(GpuHeader) &&
            (!count || count <= imageCapacity(size, limits.imageSize, limits.imageLayers, limits.storageRange)));
    data.texels = input.array<Vec4>(uint64_t(count) * stride);
    require(data.texels.size() == uint64_t(count) * stride);
    for (uint32_t i = 0; i < count; ++i) {
      const auto &record = data.header.probes[i];
      const auto &capture = captures[i];
      const auto &influence = capture.influence.valid ? capture.influence : capture.bounds;
      require(record.worldToLocal == capture.worldToLocal && record.minimum[3] == float(capture.priority));
      finite(record.minimum);
      finite(record.maximum);
      finite(record.position);
      finite(record.influenceMinimum);
      finite(record.influenceMaximum);
      for (uint32_t c = 0; c < 3; ++c)
        require(record.minimum[c] < record.maximum[c] &&
                record.influenceMinimum[c] < record.influenceMaximum[c] &&
                record.minimum[c] == capture.bounds.minimum[c] &&
                record.maximum[c] == capture.bounds.maximum[c] &&
                record.position[c] == capture.position[c] &&
                record.influenceMinimum[c] == influence.minimum[c] &&
                record.influenceMaximum[c] == influence.maximum[c]);
      // This preference is derived from retained raw captures, so older
      // valid caches gain it without tracing or filtering their lighting again.
      data.header.probes[i].position[3] = projectionConfidence(capture);
    }
    for (const auto &texel : data.texels)
      finite(texel);
  }
  void validate(std::span<const Capture> captures, const Data &data) {
    require(captures.size() <= maxProbes &&
            data.header.parameters[0] == captures.size());
    for (size_t i = 0; i < captures.size(); ++i) {
      const auto &capture = captures[i];
      const auto &record = data.header.probes[i];
      require(validProjectionSpace(capture.worldToLocal) &&
              capture.priority >= -32768 && capture.priority <= 32767 &&
              record.worldToLocal == capture.worldToLocal && record.minimum[3] == float(capture.priority));
    }
  }
  template <typename Sink> void writePayload(Sink &output, std::span<const Capture> captures, const Data &data) {
    output.pod(uint32_t(captures.size()));
    for (const auto &capture : captures) {
      output.pod(uint32_t(capture.bounds.valid) | uint32_t(capture.influence.valid) << 1);
      output.pod(capture.bounds.minimum);
      output.pod(capture.bounds.maximum);
      output.pod(capture.influence.minimum);
      output.pod(capture.influence.maximum);
      output.pod(capture.position);
      output.pod(capture.worldToLocal);
      output.pod(capture.priority);
      output.pod(capture.resolution);
      output.array(capture.radiance);
    }
    output.pod(data.header.parameters);
    for (uint32_t block = 0; block < probeHeaderBlocks(uint32_t(captures.size())); ++block) {
      ProbeHeaderBlock stored;
      std::copy_n(data.header.probes.begin() + block * probeHeaderBlockSlots,
                  stored.size(), stored.begin());
      output.pod(stored);
    }
    output.array(data.texels);
  }
  void write(cache_io::Writer &output, std::span<const Capture> captures, const Data &data) {
    writePayload(output, captures, data);
  }
  cache_io::Section fingerprint(std::span<const Capture> captures, const Data &data, std::stop_token cancelled) {
    cache_io::Fingerprint output(cancelled);
    writePayload(output, captures, data);
    return output.digest();
  }

}
