#include "forward/bake/cache.h"
#include "forward/bake/sparse_sh.h"
#include "forward/bake/cache_io.h"
#include "forward/bake/shaders/embedded.h"
#include "forward/lightmap/denoise.h"
#include "forward/mixed/directional_coverage.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace remix::forward::bake::cache {
  void Snapshot::takeGrid(Grid &source) {
    grid = std::move(source);
    // Move every persistent field together, then discard only known transient
    // tracing arrays. New serialized fields cannot silently be left in the job.
    std::vector<Point>{}.swap(grid.points);
    std::vector<Grid::Origin>{}.swap(grid.origins);
  }
  void recordLookup(const std::filesystem::path &file, uint64_t identity, uint64_t input,
                     uint64_t policy, bool hit) noexcept {
    try {
      static std::mutex mutex;
      std::lock_guard lock(mutex);
      std::error_code error;
      const bool exists = std::filesystem::is_regular_file(file, error);
      std::filesystem::create_directories(file.parent_path());
      std::ofstream out(file.parent_path() / "cache-events.jsonl", std::ios::app);
      out.imbue(std::locale::classic());
      const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
      out << "{\"unixMs\":" << now << ",\"key\":\"" << identity << "\",\"input\":\"" << input
          << "\",\"policy\":\"" << policy << "\",\"outcome\":\""
          << (hit ? "hit" : exists ? "rejected" : "missing") << "\"}\n";
    } catch (...) {} // Profiling must not make an otherwise valid bake fail.
  }
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
    // Bump whenever incompatible planning, transport or wire semantics change.
    constexpr uint32_t algorithmRevision = 28; // three-face cube-corner filtering in capture mip preparation
    void require(bool value) {
      if (!value)
        throw std::runtime_error("Invalid bake cache metadata");
    }
    void probeOffsets(const GridHeader &header, const Result &probe, float spacing = 0) {
      require(probe.irradiance[7][3] == 0 || probe.irradiance[7][3] == 1);
      require(probe.irradiance[8][3] >= 0 && probe.irradiance[8][3] <= 1);
      if (header.settings[2] > .5f)
        for (uint32_t c = 0; c < 3; ++c)
          require(std::isfinite(probe.irradiance[c][3]) &&
                  std::abs(probe.irradiance[c][3]) <= (spacing > 0 ? spacing : header.originSpacing[3]) * .4501f &&
                  ((header.settings[2] != 2 && header.settings[2] != 4) || probe.irradiance[c][3] == 0));
    }
    template <typename Range> void finite(const Range &values) {
      for (auto value : values)
        require(std::isfinite(value));
    }
    void bounds(const Bounds &value) {
      finite(value.minimum);
      finite(value.maximum);
      if (value.valid)
        for (uint32_t c = 0; c < 3; ++c)
          require(value.minimum[c] < value.maximum[c] && std::isfinite(value.maximum[c] - value.minimum[c]) &&
                  std::isfinite(value.maximum[c] + value.minimum[c]));
    }
    void channels(std::span<const UInt4> values, size_t charts, size_t lights) {
      require(values.size() == charts);
      for (const auto &value : values)
        for (auto light : value)
          require(light == mixed::none || light < lights);
    }
    void readGrid(detail::Reader &input, Snapshot &result, const Config &config, const Device &device) {
      auto &grid = result.grid;
      grid.header = input.pod<GridHeader>();
      finite(grid.header.originSpacing);
      finite(grid.header.settings);
      require(grid.header.settings[3] == 0);
      const uint64_t overhead = sizeof(GridHeader) + (config.directionalProbeDepth ? 16 : 0);
      require(grid.header.originSpacing[3] > 0 && device.storageRange >= overhead);
      const auto addressing = grid.header.settings[2];
      require(addressing == 0 || addressing == 2 || addressing == 4 ||
              ((addressing == 1 || addressing == 3 || addressing == 5) && config.relocateProbes));
      const auto maximum =
          std::min(uint64_t(config.maxProbes), (device.storageRange - overhead) /
              (sizeof(Result) + (config.directionalProbeDepth ? sizeof(ProbeDepth::Record) : 0)));
      uint64_t count = 1;
      for (uint32_t c = 0; c < 3; ++c) {
        const auto dimension = grid.header.dimensions[c];
        require(dimension >= 2 && dimension <= (indexed(grid.header) ? maximumGridNodes : maximum) / count);
        count *= dimension;
      }
      const uint64_t nodes = count;
      count = grid.header.dimensions[3];
      require(count > 0 && count <= maximum &&
              (adaptive(grid.header) || (indexed(grid.header) ? count <= nodes : count == nodes)));
      grid.probes = input.array<Result>(count);
      require(grid.probes.size() == count);
      for (const auto &probe : grid.probes) {
        for (const auto &coefficient : probe.irradiance)
          finite(coefficient);
        for (const auto &moment : probe.moments)
          finite(moment);
        finite(probe.statistics);
        probeOffsets(grid.header, probe);
      }
      grid.tasks = input.array<uint32_t>(count);
      std::vector<bool> used(size_t(count), false);
      for (auto task : grid.tasks) {
        require(task < count && !used[task]);
        used[task] = true;
      }
      if (indexed(grid.header)) {
        grid.lookup = input.array<uint32_t>(adaptive(grid.header) ? maximumDirectoryWords : nodes);
        require(gridBytes(grid) <= device.storageRange);
        grid.origins = directoryOrigins(grid);
        for (uint32_t i = 0; i < grid.probes.size(); ++i)
          probeOffsets(grid.header, grid.probes[i], grid.origins[i].relocationSpacing);
      }
      grid.depth.maximumDistance = input.pod<float>();
      grid.depth.records = input.array<ProbeDepth::Record>(count);
      require(config.directionalProbeDepth != grid.depth.empty() && gridBytes(grid) <= device.storageRange);
      grid.depth.validate(uint32_t(count));
    }
    void localProbeOwners(const Grid &grid, const scene::BakeScene &source,
                            const ProbeVisibility &visibility) {
      require(visibility.bytes() <= ProbeVisibility::maximumBytes);
      require(visibility.direct().empty() || visibility.direct().size() == visibility.entries().size());
      for (const auto &value : visibility.direct())
        for (float coefficient : value) require(std::isfinite(coefficient));
      ProbeVisibility::validate(uint32_t(grid.probes.size()), uint32_t(source.lights.size()),
          visibility.offsets(), visibility.entries(), ProbeVisibility::maximumBytes - visibility.direct().size_bytes());
      const auto offsets = visibility.offsets();
      for (uint32_t probe = 0; probe < grid.probes.size(); ++probe)
        for (uint32_t i = offsets[probe]; i < offsets[probe + 1]; ++i) {
          require(grid.probes[probe].statistics[3] > .5f);
          require(!source.lights[visibility.entries()[i].light].dynamic);
        }
    }
    void readLayout(detail::Reader &input, Snapshot &result, const scene::BakeScene &source,
                    const Config &config, const Device &device) {
      auto &layout = result.layout;
      layout.pageSize = input.pod<uint32_t>();
      layout.pages = input.pod<uint32_t>();
      layout.luxelMetres = input.pod<float>();
      layout.seamSamples = input.pod<uint32_t>();
      layout.curvedSeamSamples = input.pod<uint32_t>();
      require(std::isfinite(layout.luxelMetres) && layout.luxelMetres >= 0 && layout.pageSize >= 8 &&
              layout.pageSize <= config.lightmapPageSizeForBudget() && layout.pageSize <= device.imageSize &&
              std::has_single_bit(layout.pageSize) &&
              layout.pages <= std::min(256u, device.imageLayers / lightmap::atlasLayers));
      require(uint64_t(layout.pageSize) * layout.pageSize * layout.pages <= config.lightmapTexels);
      layout.charts = input.array<lightmap::Allocation>(source.charts.size());
      require(layout.charts.size() == source.charts.size() && (layout.charts.empty() == (layout.pages == 0)));
      for (const auto &chart : layout.charts)
        require(chart.page < layout.pages && chart.width >= 2 && chart.height >= 2 &&
                chart.width <= layout.pageSize && chart.height <= layout.pageSize &&
                chart.x <= layout.pageSize - chart.width && chart.y <= layout.pageSize - chart.height);
      const auto mappings = input.pod<uint64_t>();
      require(mappings == 0 || mappings == source.charts.size());
      layout.mappings.resize(size_t(mappings));
      for (size_t i = 0; i < mappings; ++i) {
        auto &mapping = layout.mappings[i];
        mapping.width = input.pod<uint32_t>();
        mapping.height = input.pod<uint32_t>();
        require(mapping.width == layout.charts[i].width && mapping.height == layout.charts[i].height);
        mapping.texcoords = input.array<std::array<float, 2>>(source.charts[i].texcoords.size());
        require(mapping.texcoords.size() == source.charts[i].texcoords.size());
        for (const auto &uv : mapping.texcoords) {
          finite(uv);
          require(uv[0] >= -.01f && uv[1] >= -.01f && uv[0] <= mapping.width - 1 + .01f &&
                  uv[1] <= mapping.height - 1 + .01f);
        }
      }
    }
    void readMixed(detail::Reader &input, Snapshot &result,
                   const std::shared_ptr<const scene::BakeScene> &source, bool hasPolicy) {
      auto &plan = result.mixed;
      plan.lights.source = source;
      plan.charts = input.array<UInt4>(source->charts.size());
      plan.directCharts = input.array<UInt4>(source->charts.size());
      channels(plan.charts, source->charts.size(), source->lights.size());
      channels(plan.directCharts, source->charts.size(), source->lights.size());
      if (hasPolicy) {
        plan.lights.directLighting.lights = input.array<DirectMode>(source->lights.size());
        require(plan.lights.directLighting.lights.size() == source->lights.size());
        validateDirectPolicy(*source, plan.lights.directLighting);
        for (const auto *owners : {&plan.charts, &plan.directCharts})
          for (const auto &chart : *owners)
            for (const auto light : chart)
              require(light == mixed::none || plan.lights.retainsVisibility(light));
      }
      auto identities = input.array<uint32_t>(source->lights.size());
      std::vector<bool> used(source->lights.size());
      for (auto identity : identities) {
        require(identity < source->lights.size() && !used[identity]);
        used[identity] = true;
        plan.lights.identities.emplace(source->lights[identity].id, identity);
      }
      for (uint32_t light = 0; light < used.size(); ++light)
        require(!plan.lights.bakedDiffuse(light) || used[light]);
      plan.overflow = input.pod<uint32_t>();
      const uint64_t count = source->charts.size() * ((source->lights.size() + 15) / 16);
      const auto words = input.array<uint32_t>(count);
      plan.occlusion =
          mixed::Occlusion(uint32_t(source->charts.size()), uint32_t(source->lights.size()), words);
      const auto entries = input.array<mixed::Masks::Entry>(uint64_t(source->charts.size()) * source->lights.size());
      const auto bytes = input.array<uint8_t>(mixed::Masks::maximumBytes);
      plan.masks = mixed::Masks::restore(entries, bytes, result.layout, uint32_t(source->lights.size()));
    }
    void readCaptures(detail::Reader &input, Snapshot &result, const Config &config, const Device &device) {
      const auto count = input.pod<uint32_t>();
      require(count <= std::min(config.reflectionProbes, probes::maxProbes));
      result.captures.resize(count);
      require(!count || (config.reflectionResolution && config.reflectionResolution <= 512 &&
                         std::has_single_bit(config.reflectionResolution)));
      for (auto &capture : result.captures) {
        const auto flags = input.pod<uint32_t>();
        require(flags == 1 || flags == 3);
        capture.bounds.valid = true;
        capture.influence.valid = (flags & 2) != 0;
        capture.bounds.minimum = input.pod<Vec3>();
        capture.bounds.maximum = input.pod<Vec3>();
        capture.influence.minimum = input.pod<Vec3>();
        capture.influence.maximum = input.pod<Vec3>();
        capture.position = input.pod<Vec3>();
        capture.worldToLocal = input.pod<probes::ProjectionSpace>();
        capture.priority = input.pod<int32_t>();
        require(probes::validProjectionSpace(capture.worldToLocal) &&
                capture.priority >= -32768 && capture.priority <= 32767);
        const auto localOrigin = probes::projectionPoint(capture.worldToLocal, capture.position);
        capture.resolution = input.pod<uint32_t>();
        bounds(capture.bounds);
        bounds(capture.influence);
        finite(capture.position);
        for (uint32_t c = 0; c < 3; ++c)
          require(localOrigin[c] >= capture.bounds.minimum[c] &&
                  localOrigin[c] <= capture.bounds.maximum[c]);
        require(capture.resolution == config.reflectionResolution);
        const uint64_t pixels = 6ull * capture.resolution * capture.resolution;
        capture.radiance = input.array<Vec4>(pixels);
        require(capture.radiance.size() == pixels);
        for (const auto &pixel : capture.radiance) {
          finite(pixel);
          require(pixel[0] >= 0 && pixel[0] <= 65000 && pixel[1] >= 0 && pixel[1] <= 65000 && pixel[2] >= 0 &&
                  pixel[2] <= 65000 && pixel[3] >= 0);
        }
      }
      auto &data = result.specular;
      data.header.parameters = input.pod<UInt4>();
      for (uint32_t block = 0; block < probeHeaderBlocks(count); ++block) {
        const auto stored = input.pod<ProbeHeaderBlock>();
        std::copy(stored.begin(), stored.end(), data.header.probes.begin() + block * probeHeaderBlockSlots);
      }
      const uint32_t size = count ? config.reflectionResolution : 0;
      const uint32_t levels = uint32_t(std::bit_width(size));
      const uint32_t stride = size ? probes::levelOffset(size, levels) : 0;
      require(data.header.parameters == UInt4{count, size, levels, stride});
      require(device.storageRange >= sizeof(probes::Header) &&
              uint64_t(count) * stride <= (device.storageRange - sizeof(probes::Header)) / sizeof(Vec4));
      data.texels = input.array<Vec4>(uint64_t(count) * stride);
      require(data.texels.size() == uint64_t(count) * stride);
      for (uint32_t i = 0; i < count; ++i) {
        const auto &record = data.header.probes[i];
        const auto &capture = result.captures[i];
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
        data.header.probes[i].position[3] = probes::projectionConfidence(capture);
      }
      for (const auto &texel : data.texels)
        finite(texel);
    }
    void replace(const std::filesystem::path &temporary, const std::filesystem::path &destination) {
#ifdef _WIN32
      if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Bake cache rename failed");
#else
      std::filesystem::rename(temporary, destination);
#endif
    }
  } // namespace
  uint64_t key(uint64_t content, const Config &c, float cutoff, const Device &d, const DirectPolicy &policy) {
    static const uint64_t shaders = [] {
      auto hash = scene::fastHash(atlasShader, sizeof(atlasShader));
      hash = scene::fastHash(transportShader, sizeof(transportShader), hash);
      hash = scene::fastHash(parallelShader, sizeof(parallelShader), hash);
      return scene::fastHash(visibilityShader, sizeof(visibilityShader), hash);
    }();
    const uint32_t fields[] = {algorithmRevision,
                               uint32_t(c.directionalProbeDepth),
                               uint32_t(sizeof(GridHeader)),
                               uint32_t(sizeof(Result)),
                               lightmap::atlasLayers,
                               uint32_t(c.directDiffuse),
                               c.antialiasSun ? (c.samples >= 16 ? 2u : 1u)
                                              : 0u, // retain low-budget cache identity
                               uint32_t(c.lightmapFitBudget),
                               std::bit_cast<uint32_t>(c.spacing),
                               c.maxProbes,
                               c.samples,
                               c.bounces,
                               c.textureResolution,
                               c.reflectionProbes,
                               c.reflectionResolution,
                               std::bit_cast<uint32_t>(c.reflectionDetailTolerance),
                               c.lightmapPageSize,
                               c.lightmapTexels,
                               std::bit_cast<uint32_t>(c.lightmapLuxelSize),
                               std::bit_cast<uint32_t>(cutoff),
                               d.vendor,
                               d.model,
                               d.driver,
                               d.imageSize,
                               d.imageLayers,
                               d.storageRange};
    const auto hash = scene::fastHash(fields, sizeof(fields), content ^ shaders);
    auto raw = scene::fastHash(d.uuid.data(), d.uuid.size(), hash);
    if (c.reflectionProbes) {
      // Balanced interior seeds prevent diagonal-room slivers. Old captures
      // retain their old proxy boxes, so reflection bakes need a new identity.
      // Diffuse-only bakes keep their existing cache keys.
      const uint32_t reflectionFit[] = {0x5245474eu, 1};
      raw = scene::fastHash(reflectionFit, sizeof(reflectionFit), raw);
    }
    if (c.styledIndirect) {
      const uint32_t styleCodec[] = {0x5354594cu, sparse::version};
      raw = scene::fastHash(styleCodec, sizeof(styleCodec), raw);
    }
    if (c.aggregateDiffuse) {
      const uint32_t aggregate[] = {0x41474752u, 2}; // attributed directional probe corrections
      raw = scene::fastHash(aggregate, sizeof(aggregate), raw);
    }
    if (c.fullyBakedLights) {
      const uint32_t fullyBaked[] = {0x46554c4cu, 2}; // omit fully baked probe visibility
      raw = scene::fastHash(fullyBaked, sizeof(fullyBaked), raw);
    }
    if (c.bakedGeometryLights) {
      const uint32_t geometry[] = {0x47454f4du, 2};
      raw = scene::fastHash(geometry, sizeof(geometry), raw);
    }
    if (c.relocateProbes) {
      const uint32_t relocation[] = {0x524c4f43u, 1};
      raw = scene::fastHash(relocation, sizeof(relocation), raw);
    }
    if (!policy.lights.empty()) {
      if (std::any_of(policy.lights.begin(), policy.lights.end(), [](DirectMode mode) {
            return aggregatesDiffuse(mode) && retainsVisibility(mode);
          })) {
        const uint32_t probeDirectVersion = 2; // Whole correction volumes use the actual storage allowance.
        raw = scene::fastHash(&probeDirectVersion, sizeof(probeDirectVersion), raw);
      }
      const uint32_t policyVersion = 1;
      raw = scene::fastHash(&policyVersion, sizeof(policyVersion), raw);
      raw = scene::fastHash(policy.lights.data(), policy.lights.size() * sizeof(DirectMode), raw);
      raw = scene::fastHash(aggregateShader, sizeof(aggregateShader), raw);
      raw = scene::fastHash(aggregateParallelShader, sizeof(aggregateParallelShader), raw);
    }
    // Grouped planning also chooses complete controllable volumes atomically.
    // Older aggregate caches may strand a group behind one deferred member.
    if (c.groupedStyledDirect) {
      const uint32_t groupedProbeVersion[] = {0x47525044u, 2};
      raw = scene::fastHash(groupedProbeVersion, sizeof(groupedProbeVersion), raw);
    }
    if (c.styledIndirect) {
      const uint32_t styles[] = {sparse::magic, sparse::version, c.styleStorageMiB};
      raw = scene::fastHash(styles, sizeof(styles), raw);
    }
    if (!c.denoiseLightmaps)
      return raw; // preserve existing raw cache entries when this optional stage is disabled
    const uint32_t filter[] = {0x4f49444eu, 1, lightmap::denoiserVersion()};
    return scene::fastHash(filter, sizeof(filter), raw);
  }
  std::filesystem::path path(uint64_t identity) {
    const char *override = std::getenv("REMIX_BAKE_CACHE_DIR");
    const char *config = std::getenv("REMIX_CONFIG_DIR");
    const auto directory = override && *override
                               ? std::filesystem::u8path(override)
                               : (config && *config ? std::filesystem::u8path(config)
                                                    : std::filesystem::current_path() / "openremix") /
                                     "cache" / "forwardplus";
    std::ostringstream name;
    name << std::hex << identity << ".bake";
    return directory / name.str();
  }
  std::optional<Snapshot> read(const std::filesystem::path &file, uint64_t identity,
                               std::shared_ptr<const scene::BakeScene> source, const Config &config,
                               const Device &device, std::stop_token cancelled, const DirectPolicy &policy) {
    try {
      std::error_code error;
      if (!std::filesystem::is_regular_file(file, error))
        return {};
      detail::Reader input(file, identity, cancelled);
      require(source && source->ready);
      Snapshot result;
      const auto flags = input.pod<uint32_t>();
      require(flags <= 255 && (!(flags & 128) || (flags & 64)) &&
              (!(flags & 64) || (flags & 1)) && (!(flags & 32) || (flags & 16)) &&
              (!(flags & 8) || config.styledIndirect) && (!(flags & 1) || config.directDiffuse) &&
              (!(flags & 2) || source->sun.isDistant));
      result.directDiffuse = (flags & 1) != 0;
      result.sun = (flags & 2) != 0;
      result.surfaceSamples = input.pod<uint32_t>();
      readGrid(input, result, config, device);
      readLayout(input, result, *source, config, device);
      readMixed(input, result, source, (flags & 4) != 0);
      require(result.mixed.lights.directLighting.lights == policy.lights);
      const auto &masks = result.mixed.masks;
      if (!masks.entries().empty()) {
        const uint64_t words = 2 + result.mixed.occlusion.words().size() +
                               source->charts.size() * 2 + masks.entries().size() * 4 + masks.bytes().size() / 4;
        require(words <= uint64_t(device.imageSize) * device.imageSize);
        for (const auto &entry : masks.entries()) {
          require(result.mixed.lights.retainsVisibility(entry.light));
          require(result.mixed.occlusion.state(entry.chart, entry.light) == mixed::Visibility::Unknown);
          const auto &owners = result.mixed.charts[entry.chart];
          require(std::find(owners.begin(), owners.end(), entry.light) == owners.end());
          const auto [first, last] = result.mixed.lights.identities.equal_range(source->lights[entry.light].id);
          require(std::any_of(first, last, [&](const auto &id) { return id.second == entry.light; }));
        }
      }
      readCaptures(input, result, config, device);
      require(result.surfaceSamples || std::any_of(result.grid.probes.begin(), result.grid.probes.end(),
                                                   [](const auto &p) { return p.statistics[3] > .5f; }));
      const auto &layout = result.layout;
      const uint64_t count =
          uint64_t(layout.pageSize) * layout.pageSize * layout.pages * lightmap::atlasLayers * 4;
      result.pixels = input.array<uint16_t>(count);
      require(result.pixels.size() == count);
      for (uint16_t value : result.pixels)
        require((value & 0x7c00u) != 0x7c00u);
      const auto ids = styleIds(*source);
      require(bool(flags & 8) == (config.styledIndirect && !ids.empty()));
      if (flags & 8) {
        const uint64_t baseBytes = gridBytes(result.grid);
        require(baseBytes + 16 < device.storageRange);
        const uint64_t maximum = std::min(uint64_t(config.styleStorageMiB) << 20,
                                          uint64_t(device.storageRange) - baseBytes - 15);
        result.styles = input.array<uint32_t>(maximum / 4);
        sparse::validate(result.styles, {layout.pageSize, layout.pages, uint32_t(result.grid.probes.size())}, ids, maximum);
      }
      if (flags & 16) {
        const uint64_t maximum = std::min(uint64_t(device.storageRange), ProbeVisibility::maximumBytes);
        const auto offsets = input.array<uint32_t>(result.grid.probes.size() + 1);
        require(offsets.size() * sizeof(uint32_t) <= maximum);
        const auto entries = input.array<ProbeVisibility::Entry>(
            (maximum - offsets.size() * sizeof(uint32_t)) / sizeof(ProbeVisibility::Entry));
        const uint64_t maskBytes = offsets.size() * sizeof(uint32_t) + entries.size() * sizeof(ProbeVisibility::Entry);
        const auto direct = (flags & 32)
            ? input.array<ProbeVisibility::Direct>((maximum - maskBytes) / sizeof(ProbeVisibility::Direct))
            : std::vector<ProbeVisibility::Direct>{};
        require(!(flags & 32) || (!direct.empty() && direct.size() == entries.size()));
        result.localProbes = ProbeVisibility::restore(uint32_t(result.grid.probes.size()),
            uint32_t(source->lights.size()), offsets, entries, maximum, direct);
        localProbeOwners(result.grid, *source, *result.localProbes);
        const uint64_t combined = gridBytes(result.grid) +
            result.styles.size() * sizeof(uint32_t);
        require(((combined + 15) & ~uint64_t(15)) + result.localProbes->gpuBytes()
                <= device.storageRange);
      }
      result.directionalAttempted = (flags & 64) != 0;
      if (result.directionalAttempted) {
        result.directionalBudget = (flags & 128) ? input.pod<uint64_t>() : mixed::DirectionalFields::defaultBytes;
        require(result.directionalBudget > 0 && result.directionalBudget <= mixed::directionalStorageBudget(layout));
        result.directionals = input.array<uint32_t>(result.directionalBudget / 4);
        if (!result.directionals.empty())
          mixed::validateDirectionalCoverage(result.directionals, result.mixed, layout, cancelled);
      }
      input.finish();
      return std::optional<Snapshot>{std::move(result)};
    } catch (const std::exception &error) {
      if (!cancelled.stop_requested())
        std::fprintf(stderr, "Bake cache miss: %s\n", error.what());
      return {};
    }
  }
  bool write(const std::filesystem::path &file, uint64_t identity, const View &view,
             std::stop_token cancelled) {
    std::filesystem::path temporary;
    try {
      detail::check(cancelled);
      require(view.captures.size() <= probes::maxProbes &&
              view.specular.header.parameters[0] == view.captures.size());
      for (size_t i = 0; i < view.captures.size(); ++i) {
        const auto &capture = view.captures[i];
        const auto &record = view.specular.header.probes[i];
        require(probes::validProjectionSpace(capture.worldToLocal) &&
                capture.priority >= -32768 && capture.priority <= 32767 &&
                record.worldToLocal == capture.worldToLocal && record.minimum[3] == float(capture.priority));
      }
      require(view.directionals.empty() || view.directionalAttempted);
      require(!view.directionalAttempted || view.directDiffuse);
      const auto directionalBudget = view.directionalAttempted
          ? (view.directionalBudget ? view.directionalBudget : mixed::directionalStorageBudget(view.layout)) : 0;
      require(!view.directionalAttempted ||
          (directionalBudget > 0 && directionalBudget <= mixed::directionalStorageBudget(view.layout)));
      require(view.directionals.size_bytes() <= directionalBudget);
      if (!view.directionals.empty())
        mixed::validateDirectionalCoverage(view.directionals, view.mixed, view.layout, cancelled);
      if (view.localProbes) {
        require(view.mixed.lights.source != nullptr);
        localProbeOwners(view.grid, *view.mixed.lights.source, *view.localProbes);
      }
      require(view.grid.header.settings[2] >= 0 && view.grid.header.settings[2] <= 5 &&
              std::floor(view.grid.header.settings[2]) == view.grid.header.settings[2]);
      const auto origins = directoryOrigins(view.grid);
      require(view.grid.header.settings[3] == 0);
      if (!view.styles.empty()) {
        require(view.mixed.lights.source != nullptr);
        sparse::validate(view.styles, {view.layout.pageSize, view.layout.pages, uint32_t(view.grid.probes.size())},
                         styleIds(*view.mixed.lights.source), view.styles.size_bytes());
      }
      for (uint32_t i = 0; i < view.grid.probes.size(); ++i)
        probeOffsets(view.grid.header, view.grid.probes[i], origins.empty() ? 0 : origins[i].relocationSpacing);
      require(view.surfaceSamples || std::any_of(view.grid.probes.begin(), view.grid.probes.end(),
                                                 [](const auto &p) { return p.statistics[3] > .5f; }));
      std::filesystem::create_directories(file.parent_path());
      std::random_device random;
      temporary = file;
      temporary += ".tmp-" + std::to_string((uint64_t(random()) << 32) ^ random());
      if (std::filesystem::exists(temporary))
        return false;
      detail::Writer output(temporary, identity, cancelled);
      const bool hasPolicy = !view.mixed.lights.directLighting.lights.empty();
      if (hasPolicy) {
        require(view.mixed.lights.source != nullptr);
        validateDirectPolicy(*view.mixed.lights.source, view.mixed.lights.directLighting);
      }
      output.pod(uint32_t(view.directDiffuse) | uint32_t(view.sun) << 1 | uint32_t(hasPolicy) << 2 |
                 uint32_t(!view.styles.empty()) << 3 | uint32_t(view.localProbes != nullptr) << 4 |
                 uint32_t(view.localProbes && view.localProbes->directional()) << 5 |
                 uint32_t(view.directionalAttempted) << 6 | uint32_t(view.directionalAttempted) << 7);
      output.pod(view.surfaceSamples);
      output.pod(view.grid.header);
      output.array(view.grid.probes);
      output.array(view.grid.tasks);
      if (indexed(view.grid.header)) output.array(view.grid.lookup);
      view.grid.depth.validate(uint32_t(view.grid.probes.size()));
      output.pod(view.grid.depth.maximumDistance);
      output.array(view.grid.depth.records);
      const auto &layout = view.layout;
      output.pod(layout.pageSize);
      output.pod(layout.pages);
      output.pod(layout.luxelMetres);
      output.pod(layout.seamSamples);
      output.pod(layout.curvedSeamSamples);
      output.array(layout.charts);
      output.pod(uint64_t(layout.mappings.size()));
      for (const auto &mapping : layout.mappings) {
        output.pod(mapping.width);
        output.pod(mapping.height);
        output.array(mapping.texcoords);
      }
      output.array(view.mixed.charts);
      output.array(view.mixed.directCharts);
      if (hasPolicy)
        output.array(view.mixed.lights.directLighting.lights);
      std::vector<uint32_t> identities;
      for (const auto &[id, index] : view.mixed.lights.identities)
        identities.push_back(index);
      std::sort(identities.begin(), identities.end());
      output.array(identities);
      output.pod(view.mixed.overflow);
      output.array(view.mixed.occlusion.words());
      output.array(view.mixed.masks.entries());
      output.array(view.mixed.masks.bytes());
      output.pod(uint32_t(view.captures.size()));
      for (const auto &capture : view.captures) {
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
      output.pod(view.specular.header.parameters);
      for (uint32_t block = 0; block < probeHeaderBlocks(uint32_t(view.captures.size())); ++block) {
        ProbeHeaderBlock stored;
        std::copy_n(view.specular.header.probes.begin() + block * probeHeaderBlockSlots,
                    stored.size(), stored.begin());
        output.pod(stored);
      }
      output.array(view.specular.texels);
      output.array(view.pixels);
      if (!view.styles.empty()) output.array(view.styles);
      if (view.localProbes) {
        output.array(view.localProbes->offsets());
        output.array(view.localProbes->entries());
        if (view.localProbes->directional()) output.array(view.localProbes->direct());
      }
      if (view.directionalAttempted) {
        output.pod(directionalBudget);
        output.array(view.directionals);
      }
      output.finish();
      detail::check(cancelled);
      replace(temporary, file);
      return true;
    } catch (const std::exception &error) {
      if (!temporary.empty()) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
      }
      if (!cancelled.stop_requested())
        std::fprintf(stderr, "Bake cache write unavailable: %s\n", error.what());
      return false;
    }
  }
} // namespace remix::forward::bake::cache
