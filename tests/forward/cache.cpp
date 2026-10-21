#include "forward/bake/cache.h"
#include "forward/mixed/directional_fields.h"
#include "forward/bake/cache_io.h"
#include "forward/bake/sparse_sh.h"
#include "scene/fixtures.h"
#include <fstream>
#include <random>

using namespace remix;
using namespace remix::test;
namespace bc = remix::forward::bake::cache;
namespace f = remix::forward;
int main(int argc, char **argv) {
  require(argc == 2, "Supply a scratch directory");
  for (size_t offset : {size_t(0), size_t((1u << 23) - 1), size_t(1u << 23), size_t((1u << 24) + 1)})
    for (bool styled : {false, true}) for (bool depth : {false, true}) {
      const auto bits = std::bit_cast<uint32_t>(f::bake::publicationSettings(offset, styled, depth));
      require((bits & 0x7fffffffu) / 2 == offset && bool(bits & 1) == styled && bool(bits >> 31) == depth,
              "Published local offsets and style flags retain exact uint32 bits beyond float precision");
    }
  const auto root = std::filesystem::path(argv[1]) / ("case-" + std::to_string(std::random_device{}()));
  std::filesystem::create_directories(root);
  const auto file = root / "lighting.bake";
  NativeResources resources;
  const auto mat = material(resources, 1, 1, 1, 1);
  const auto handle = resources.createRetained(instance(quad(resources, 2, mat)));
  auto source = std::make_shared<scene::BakeScene>();
  source->ready = true;
  source->draws = resources.retained(handle)->records;
  source->charts.push_back({0, 0, 5, 5, {{0, 0}, {4, 4}, {4, 0}, {0, 0}, {0, 4}, {4, 4}}});
  scene::SphereLight lamp;
  lamp.id = 17;
  lamp.radius = .25f;
  lamp.position[2] = -1;
  lamp.radiance[0] = 2;
  source->lights.push_back(lamp);
  lamp.id = 18;
  source->lights.push_back(lamp);
  scene::freezeBakeScene(*source);
  f::bake::Config config;
  config.lightmapPageSize = 8;
  config.lightmapTexels = 64;
  config.maxProbes = 64;
  config.reflectionProbes = 1;
  config.reflectionResolution = 8;
  bc::Device device{1, 2, 3, 4096, 256, 128u << 20, {}};
  const auto identity = bc::key(source->contentHash, config, .001f, device);
  auto execution = config;
  execution.batch = 1024;
  execution.gpuBudgetMs = 0;
  execution.diskCache = false;
  require(bc::key(source->contentHash, execution, .001f, device) == identity,
          "Cache identity excludes scheduling and cache preferences");
  execution.samples *= 2;
  require(bc::key(source->contentHash, execution, .001f, device) != identity &&
              bc::key(source->contentHash + 1, config, .001f, device) != identity &&
              bc::key(source->contentHash, config, .002f, device) != identity,
          "Quality, frozen source content and light cutoff invalidate cached lighting");
  auto updatedDevice = device;
  updatedDevice.driver++;
  require(bc::key(source->contentHash, config, .001f, updatedDevice) != identity,
          "GPU driver identity participates in cache compatibility");
  auto qualityKey = [&](auto edit) {
    auto changed = config;
    edit(changed);
    require(bc::key(source->contentHash, changed, .001f, device) != identity,
            "Every bake quality field participates in disk cache identity");
  };
  {
    auto grouped = config;
    grouped.groupedStyledDirect = true;
    require(bc::key(source->contentHash, grouped, .001f, device) != identity,
            "An analytic-only cache needs a distinct capability for directional probe capture");
    f::bake::DirectPolicy aggregate;
    aggregate.lights.assign(source->lights.size(), f::bake::DirectMode::BakedMixed);
    require(bc::key(source->contentHash, grouped, .001f, device, aggregate) ==
                bc::key(source->contentHash, config, .001f, device, aggregate),
            "Grouped publication reuses aggregate caches that already retain directional probes");
  }
  qualityKey([](auto &c) { c.directDiffuse = !c.directDiffuse; });
  qualityKey([](auto &c) { c.aggregateDiffuse = !c.aggregateDiffuse; });
  qualityKey([](auto &c) { c.fullyBakedLights = !c.fullyBakedLights; });
  qualityKey([](auto &c) { c.bakedGeometryLights = !c.bakedGeometryLights; });
  qualityKey([](auto &c) { c.antialiasSun = !c.antialiasSun; });
  qualityKey([](auto &c) { c.denoiseLightmaps = !c.denoiseLightmaps; });
  qualityKey([](auto &c) { c.relocateProbes = !c.relocateProbes; });
  qualityKey([](auto &c) { c.spacing += 1; });
  qualityKey([](auto &c) { c.maxProbes *= 2; });
  qualityKey([](auto &c) { c.bounces += 1; });
  qualityKey([](auto &c) { c.textureResolution *= 2; });
  qualityKey([](auto &c) { c.reflectionProbes += 1; });
  qualityKey([](auto &c) { c.reflectionResolution *= 2; });
  qualityKey([](auto &c) { c.reflectionDetailTolerance += .1f; });
  qualityKey([](auto &c) { c.lightmapPageSize *= 2; });
  qualityKey([](auto &c) { c.lightmapTexels *= 2; });
  qualityKey([](auto &c) { c.lightmapFitBudget = !c.lightmapFitBudget; });
  qualityKey([](auto &c) { c.lightmapLuxelSize *= 2; });
  bc::Snapshot data;
  data.grid.header.originSpacing = {0, 0, 0, 1};
  data.grid.header.dimensions = {2, 2, 2, 8};
  data.grid.probes.resize(8);
  for (uint32_t i = 0; i < 8; ++i) {
    data.grid.tasks.push_back(i);
    data.grid.probes[i].statistics = {0, 1, 256, 1};
    data.grid.probes[i].irradiance[0] = {1, 2, 3, 0};
    data.grid.probes[i].irradiance[7][3] = 1;
    data.grid.probes[i].irradiance[8][3] = float(i) / 7.f;
  }
  data.layout.pageSize = 8;
  data.layout.pages = 1;
  data.layout.luxelMetres = .25f;
  data.layout.charts.push_back({0, 0, 0, 5, 5});
  data.layout.mappings.resize(1);
  data.layout.mappings[0].width = data.layout.mappings[0].height = 5;
  data.layout.mappings[0].texcoords = source->charts[0].texcoords;
  data.mixed.lights.source = source;
  data.mixed.lights.identities.emplace(17, 0);
  data.mixed.lights.identities.emplace(18, 1);
  data.mixed.charts.push_back({0, f::mixed::none, f::mixed::none, f::mixed::none});
  data.mixed.directCharts = data.mixed.charts;
  data.mixed.occlusion = f::mixed::Occlusion(1, 2);
  data.mixed.occlusion.mark(0, 0, f::mixed::Visibility::Clear);
  std::vector<uint8_t> sparseValues(25);
  constexpr uint8_t colors[] = {0, 1, 254, 255};
  for (uint32_t i = 0; i < sparseValues.size(); ++i)
    sparseValues[i] = colors[i % 4];
  const auto sparseBytes = f::mixed::mask::encodeCompact(sparseValues, 5, 5);
  require(sparseBytes.encoding == f::mixed::mask::tiled && sparseBytes.bytes.size() == 24,
          "Cache fixture includes a palette tile clipped to a small chart");
  const f::mixed::Masks::Entry sparse{0, 1, 0, 5, 5, sparseBytes.encoding};
  data.mixed.masks = f::mixed::Masks::restore({&sparse, 1}, sparseBytes.bytes, data.layout, 2);
  f::probes::Capture capture;
  capture.bounds.include({-1, -1, -1});
  capture.bounds.include({1, 1, 1});
  capture.resolution = 8;
  capture.radiance.resize(6 * 8 * 8, {1, 2, 3, 10});
  data.captures.push_back(capture);
  data.specular = f::probes::filter(data.captures, 8);
  data.pixels.resize(64 * f::lightmap::atlasLayers * 4, 0x3c00);
  data.surfaceSamples = 25;
  data.directDiffuse = true;
  auto save = [&] {
    return bc::write(file, identity,
                     {data.grid, data.layout, data.mixed, data.captures, data.specular, data.pixels,
                      data.surfaceSamples, data.directDiffuse, data.sun});
  };
  auto load = [&] { return bc::read(file, identity, source, config, device); };
  require(!load(), "Missing cache is an ordinary miss");
  require(save(), "Completed lighting is saved");
  auto restored = load();
  require(restored && restored->pixels == data.pixels &&
              f::probes::serialize(restored->specular) == f::probes::serialize(data.specular) &&
              restored->surfaceSamples == 25 && restored->directDiffuse,
          "Atlas, reflection data and direct-light policy survive the cache round trip");
  {
    auto depthConfig = config;
    depthConfig.directionalProbeDepth = true;
    const auto depthKey = bc::key(source->contentHash, depthConfig, .001f, device);
    require(depthKey != identity, "Directional probe visibility owns a distinct bake policy identity");
    data.grid.depth.maximumDistance = 100;
    data.grid.depth.records.resize(data.grid.probes.size());
    for (auto &record : data.grid.depth.records) record.fill(0x3c003c00u);
    auto ownedGrid = data.grid;
    ownedGrid.points.resize(ownedGrid.probes.size());
    ownedGrid.origins.resize(ownedGrid.probes.size());
    bc::Snapshot persisted;
    persisted.takeGrid(ownedGrid);
    require(ownedGrid.probes.empty() && persisted.grid.points.empty() && persisted.grid.origins.empty() &&
                persisted.grid.depth.records == data.grid.depth.records,
            "Real snapshot ownership moves every persistent grid field and releases only transient arrays");
    const auto depthFile = root / "directional-depth.bake";
    require(bc::write(depthFile, depthKey,
        {persisted.grid, data.layout, data.mixed, data.captures, data.specular, data.pixels,
         data.surfaceSamples, data.directDiffuse, data.sun}), "Directional probe depth snapshot is persisted");
    const auto imported = bc::read(depthFile, depthKey, source, depthConfig, device);
    require(imported && imported->grid.depth.maximumDistance == 100 &&
                imported->grid.depth.records == data.grid.depth.records,
            "Directional depth moments and distance survive a cache round trip exactly");
    data.grid.depth.records[0][0] = 0x7c000000u;
    require(!bc::write(root / "invalid-depth.bake", depthKey,
        {data.grid, data.layout, data.mixed, data.captures, data.specular, data.pixels,
         data.surfaceSamples, data.directDiffuse, data.sun}), "Malformed directional depth cannot be saved");
    data.grid.depth = {};
  }
  for (uint32_t count : {32u, 33u, 64u}) {
    auto extendedConfig = config;
    extendedConfig.reflectionProbes = 64;
    const auto extendedKey = bc::key(source->contentHash, extendedConfig, .001f, device);
    const auto extendedFile = root / ("reflection-" + std::to_string(count) + ".bake");
    std::vector<f::probes::Capture> captures(count, capture);
    for (uint32_t i = 0; i < count; ++i) {
      captures[i].priority = int32_t(i);
      for (auto &pixel : captures[i].radiance) pixel[0] = float(i + 1);
    }
    const auto specular = f::probes::filter(captures, 8);
    require(bc::write(extendedFile, extendedKey,
        {data.grid,data.layout,data.mixed,captures,specular,data.pixels,
         data.surfaceSamples,data.directDiffuse,data.sun}), "Extended reflection headers can be saved");
    const auto extended = bc::read(extendedFile,extendedKey,source,extendedConfig,device);
    require(extended && extended->captures.size() == count &&
                f::probes::serialize(extended->specular) == f::probes::serialize(specular) &&
                extended->specular.header.probes[count-1].minimum[3] == float(count-1),
            "Extended capture caches preserve second-word slots, radiance and priorities exactly");
  }
  require(restored->grid.probes[3].irradiance[7][3] == 1 &&
              restored->grid.probes[3].irradiance[8][3] == data.grid.probes[3].irradiance[8][3],
          "Probe static sunlight visibility survives the disk cache without a second allocation");
  require(!restored->localProbes, "Existing caches without local probe masks remain explicitly uncovered");
  require(!restored->directionalAttempted && restored->directionals.empty(),
          "Old cache payloads remain readable without claiming complete directional capture");
  {
    auto wideSource = std::make_shared<scene::BakeScene>(*source);
    for (uint32_t light = 2; light < 7; ++light) {
      auto extra = wideSource->lights[0];
      extra.id = 17 + light;
      wideSource->lights.push_back(extra);
    }
    auto wide = data.mixed;
    wide.lights.source = wideSource;
    for (uint32_t light = 2; light < 7; ++light)
      wide.lights.identities.emplace(wideSource->lights[light].id, light);
    wide.occlusion = f::mixed::Occlusion(1, 7);
    wide.occlusion.mark(0, 0, f::mixed::Visibility::Clear);
    for (uint32_t light = 2; light < 7; ++light)
      wide.occlusion.mark(0, light, f::mixed::Visibility::Clear);
    const auto directionalFile = root / "complete-directionals.bake";
    const auto encode = [&](uint32_t count, bool wrongRectangle = false) {
      f::mixed::DirectionalFields fields(1, 7);
      auto rectangle = data.layout.charts[0];
      if (wrongRectangle) --rectangle.width;
      std::vector<f::mixed::directional::Field> values(rectangle.width * rectangle.height);
      for (uint32_t light = 0; light < count; ++light) {
        for (auto &value : values) value.fill(light == 6 ? 0 : (light & 1 ? 0xb400 : 0x3800));
        require(fields.add(0, light, rectangle, values), "Seven-light cache fixture fits its storage budget");
      }
      return fields.words();
    };
    auto words = encode(7);
    uint64_t requestedBudget = 0;
    const auto saveDirectional = [&](bool attempted = true) {
      return bc::write(directionalFile, identity,
          {data.grid, data.layout, wide, data.captures, data.specular, data.pixels,
           data.surfaceSamples, data.directDiffuse, data.sun, {}, nullptr, attempted, words, requestedBudget});
    };
    const auto loadDirectional = [&] { return bc::read(directionalFile, identity, wideSource, config, device); };
    require(saveDirectional(), "Complete fields beyond four owners are persisted");
    const auto imported = loadDirectional();
    require(imported && imported->directionalAttempted && imported->directionals == words &&
                imported->directionalBudget == f::mixed::directionalStorageBudget(data.layout) &&
                f::mixed::DirectionalFields::sample(imported->directionals, 0, 6, 2, 2)->front() == 0,
            "Cache round trip preserves every signed field and explicit known-black seventh owner");
    std::vector<char> original;
    {
      std::ifstream input(directionalFile, std::ios::binary);
      original.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    const auto commitDamaged = [&](std::vector<char> &damaged) {
      bc::detail::Header header;
      std::memcpy(&header, damaged.data(), sizeof(header));
      header.bytes = damaged.size() - sizeof(header);
      header.checksum = scene::hashBytes(damaged.data() + sizeof(header), header.bytes, 1469598103934665603ull);
      std::memcpy(damaged.data(), &header, sizeof(header));
      std::ofstream output(directionalFile, std::ios::binary | std::ios::trunc);
      output.write(damaged.data(), std::streamsize(damaged.size()));
    };
    const size_t budgetOffset = original.size() - words.size() * sizeof(uint32_t) - 2 * sizeof(uint64_t);
    auto legacy = original;
    legacy.erase(legacy.begin() + budgetOffset, legacy.begin() + budgetOffset + sizeof(uint64_t));
    uint32_t flags;
    std::memcpy(&flags, legacy.data() + sizeof(bc::detail::Header), sizeof(flags));
    flags &= ~128u;
    std::memcpy(legacy.data() + sizeof(bc::detail::Header), &flags, sizeof(flags));
    commitDamaged(legacy);
    const auto legacyImport = loadDirectional();
    require(legacyImport && legacyImport->directionalBudget == f::mixed::DirectionalFields::defaultBytes &&
                legacyImport->directionals == words,
            "Earlier optional directional payloads import with their original 128 MiB allowance");
    auto invalidBudget = original;
    const uint64_t excessive = f::mixed::directionalStorageBudget(data.layout) + 1;
    std::memcpy(invalidBudget.data() + budgetOffset, &excessive, sizeof(excessive));
    commitDamaged(invalidBudget);
    require(!loadDirectional(), "Checksummed cache budget cannot exceed its atlas-derived allowance");
    const auto replaceTail = [&](const std::vector<uint32_t> &replacement) {
      auto damaged = original;
      damaged.resize(original.size() - words.size() * sizeof(uint32_t) - sizeof(uint64_t));
      const uint64_t count = replacement.size();
      const size_t start = damaged.size();
      damaged.resize(start + sizeof(count) + replacement.size() * sizeof(uint32_t));
      std::memcpy(damaged.data() + start, &count, sizeof(count));
      std::memcpy(damaged.data() + start + sizeof(count), replacement.data(), replacement.size() * sizeof(uint32_t));
      commitDamaged(damaged);
    };
    replaceTail(encode(6));
    require(!loadDirectional(), "Checksummed codec-valid cache with missing ownership is rejected on import");
    replaceTail(encode(7, true));
    require(!loadDirectional(), "Checksummed cache cannot substitute another chart rectangle");
    require(saveDirectional(), "Valid complete capture restores the cache after malformed import tests");
    requestedBudget = words.size() * sizeof(uint32_t) - 4;
    require(!saveDirectional(), "Writer enforces the actual attempted encoded budget");
    requestedBudget = 0;
    require(!saveDirectional(false) && loadDirectional()->directionals == words,
            "Unattempted capture cannot publish complete fields or replace valid cached data");
    words = encode(6);
    require(!saveDirectional(), "A valid codec with an omitted positive owner is not complete coverage");
    words = encode(7, true);
    require(!saveDirectional(), "Codec-valid fields must match exact chart rectangles");
    words = encode(7);
    words.back() ^= 0xffffffffu;
    require(!saveDirectional(), "Malformed field payload cannot replace a completed cache");
    words.clear();
    require(saveDirectional(), "Whole-replacement memory decline is an explicit cache outcome");
    const auto declined = loadDirectional();
    require(declined && declined->directionalAttempted && declined->directionals.empty(),
            "Declined capture remains distinct from old unattempted cache data");
    require(saveDirectional(false), "Legacy payload can replace the optional extension");
    require(!loadDirectional()->directionalAttempted, "Absent optional extension remains explicitly unattempted");
  }
  {
    const f::bake::VisibilityTask pairs[] = {{0, 0, 0}, {0, 1, 0}, {7, 1, 0}};
    const float values[] = {0, .375f, 1};
    f::bake::ProbeVisibility local(8, 2, pairs, 1024);
    local.observe(pairs, values);
    const auto localFile = root / "local-probes.bake";
    auto saveLocal = [&](const auto &masks) {
      return bc::write(localFile, identity,
          {data.grid, data.layout, data.mixed, data.captures, data.specular, data.pixels,
           data.surfaceSamples, data.directDiffuse, data.sun, {}, &masks});
    };
    auto loadLocal = [&] { return bc::read(localFile, identity, source, config, device); };
    require(saveLocal(local), "Completed sparse local probe visibility is saved");
    const auto imported = loadLocal();
    require(imported && imported->localProbes && imported->localProbes->sample(0, 0) == 0 &&
            imported->localProbes->sample(0, 1) == .375f && imported->localProbes->sample(7, 1) == 1 &&
            !imported->localProbes->sample(1, 0) && imported->pixels == data.pixels,
            "Sparse blocked/partial/clear masks preserve ownership and unknown coverage across cache reload");
    f::bake::ProbeVisibility directional(8, 2, pairs, 4096, true);
    std::array<f::bake::VisibilityResult, 3> directionalValues{};
    for (size_t i = 0; i < 3; ++i) {
      directionalValues[i].visibility = values[i];
      for (uint32_t coefficient = 0; coefficient < 9; ++coefficient)
        directionalValues[i].irradiance[coefficient] = float(int(i * 9 + coefficient) - 10) * .1f;
    }
    directional.observe(pairs, directionalValues);
    require(saveLocal(directional), "Attributed directional probes save their optional cache payload");
    const auto directReload = loadLocal();
    require(directReload && directReload->localProbes &&
                directReload->localProbes->words() == directional.words(),
            "Disk cache preserves signed directional probe values and exact GPU words");
    require(saveLocal(local), "Scalar-only payload remains readable after directional cache replacement");
    f::bake::ProbeVisibility unfinished(8, 2, pairs, 1024);
    require(!saveLocal(unfinished) && loadLocal(), "Incomplete masks cannot replace a completed cache");
    data.grid.probes[0].statistics[3] = 0;
    require(!saveLocal(local), "Unusable probe records cannot advertise cached local coverage");
    data.grid.probes[0].statistics[3] = 1;
    auto movingSource = std::make_shared<scene::BakeScene>(*source);
    movingSource->lights[1].dynamic = true;
    require(!bc::read(localFile, identity, movingSource, config, device),
            "Moving lights cannot inherit static probe masks even with a supplied file key");
    std::vector<char> damaged;
    {
      std::ifstream input(localFile, std::ios::binary);
      damaged.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    const size_t tailBytes = 2 * sizeof(uint64_t) + local.offsets().size_bytes() + local.entries().size_bytes();
    const uint32_t invalidStart = 1;
    std::memcpy(damaged.data() + damaged.size() - tailBytes + sizeof(uint64_t), &invalidStart, sizeof(invalidStart));
    const uint64_t checksum = scene::hashBytes(damaged.data() + sizeof(bc::detail::Header),
        damaged.size() - sizeof(bc::detail::Header), 1469598103934665603ull);
    std::memcpy(damaged.data() + offsetof(bc::detail::Header, checksum), &checksum, sizeof(checksum));
    {
      std::ofstream output(localFile, std::ios::binary | std::ios::trunc);
      output.write(damaged.data(), std::streamsize(damaged.size()));
    }
    require(!loadLocal(), "Even a checksummed local table must obey sparse offset semantics");
    require(saveLocal(local) && loadLocal(), "A valid local table atomically repairs a malformed cache");
  }
  data.grid.probes[0].irradiance[8][3] = 2;
  require(!save(), "Out-of-range probe sunlight cannot replace a completed lighting cache");
  data.grid.probes[0].irradiance[8][3] = 0;
  data.grid.probes[0].irradiance[7][3] = .5f;
  require(!save(), "Ambiguous probe sunlight availability cannot enter the cache");
  data.grid.probes[0].irradiance[7][3] = 1;
  {
    auto relocated = data;
    auto relocationConfig = config;
    relocationConfig.relocateProbes = true;
    const auto relocationKey = bc::key(source->contentHash, relocationConfig, .001f, device);
    const auto relocationFile = file.parent_path() / "relocation.bake";
    relocated.grid.header.settings[2] = 1;
    relocated.grid.probes[0].irradiance[0][3] = .3f;
    auto saveRelocated = [&] {
      return bc::write(relocationFile, relocationKey,
                       {relocated.grid, relocated.layout, relocated.mixed, relocated.captures,
                        relocated.specular, relocated.pixels, relocated.surfaceSamples,
                        relocated.directDiffuse, relocated.sun});
    };
    require(saveRelocated(), "Relocated probe payload can be saved independently");
    const auto imported = bc::read(relocationFile, relocationKey, source, relocationConfig, device);
    require(imported && imported->grid.header.settings[2] == 1 &&
            imported->grid.probes[0].irradiance[0][3] == .3f && imported->grid.points.empty(),
            "Trace offsets survive cached publication without transient point arrays");
    auto disabledRelocation = config;
    disabledRelocation.relocateProbes = false;
    require(!bc::read(relocationFile, relocationKey, source, disabledRelocation, device),
            "Relocated payloads require explicit enabled policy even when supplied the file key");
    relocated.grid.probes[0].irradiance[0][3] = .6f;
    require(!saveRelocated(), "Offsets outside the cell movement bound cannot replace a completed cache");
    require(bool(bc::read(relocationFile, relocationKey, source, relocationConfig, device)),
            "Failed relocation writes preserve the previously completed cache");
  }
  {
    const auto originalCaptures = data.captures;
    const auto originalSpecular = data.specular;
    auto &oriented = data.captures[0];
    oriented.worldToLocal = {{{0, 1, 0, -20}, {-1, 0, 0, 10}, {0, 0, 1, -30}}};
    oriented.position = {10, 20.25f, 30};
    oriented.priority = -12;
    data.specular = f::probes::filter(data.captures, 8);
    require(save(), "Oriented captures and signed priorities can be cached");
    const auto roundtrip = load();
    require(roundtrip && roundtrip->captures[0].worldToLocal == oriented.worldToLocal &&
                roundtrip->captures[0].priority == -12 && roundtrip->captures[0].position == oriented.position &&
                roundtrip->specular.header.probes[0].worldToLocal == oriented.worldToLocal &&
                roundtrip->specular.header.probes[0].minimum[3] == -12,
            "Cached projection frames, priorities and independent capture origins round-trip exactly");
    data.specular.header.probes[0].worldToLocal[0][0] = 2;
    require(!save(), "Mismatched cached projection frames cannot replace a valid cache");
    data.captures = originalCaptures;
    data.specular = originalSpecular;
  }
  const float confidence = data.specular.header.probes[0].position[3];
  data.specular.header.probes[0].position[3] = 0;
  require(save(), "A legacy cache without projection confidence is saved");
  const auto legacy = load();
  require(legacy && legacy->specular.header.probes[0].position[3] == confidence &&
              legacy->specular.texels == data.specular.texels && legacy->pixels == data.pixels,
          "Cached raw captures refresh projection preference without changing baked lighting");
  data.specular.header.probes[0].position[3] = confidence;
  require(save(), "The current cache is restored for corruption checks");
  require(restored->mixed.lights.match(source->lights[0]) == 0 &&
              restored->mixed.occlusion.state(0, 0) == f::mixed::Visibility::Clear &&
              restored->mixed.occlusion.clearCount() == 1,
          "Mixed-light ownership and constant visibility are reconstructed against the current source");
  require(restored->mixed.masks.words(1) == data.mixed.masks.words(1),
          "Sparse partial visibility survives an actual disk cache round trip");
  const auto originalMesh =
      f::lightmap::meshes(*source, data.layout, {}, data.mixed.charts, data.mixed.directCharts);
  const auto restoredMesh = f::lightmap::meshes(*source, restored->layout, {}, restored->mixed.charts,
                                                restored->mixed.directCharts);
  require(originalMesh[0].indices == restoredMesh[0].indices &&
              originalMesh[0].vertices[0].coordinate == restoredMesh[0].vertices[0].coordinate &&
              restored->layout.points.empty(),
          "Rendering chart UVs reload without regenerating surface sample work lists");
  require(!bc::read(file, identity + 1, source, config, device),
          "A key mismatch cannot publish stale lighting");
  {
    std::fstream edit(file, std::ios::binary | std::ios::in | std::ios::out);
    edit.seekp(7);
    edit.put('1');
  }
  require(!load(), "The old 16-slot cache wire layout is rejected before decoding probe metadata");
  require(save(), "A new probe layout can replace an older cache version");
  {
    std::fstream edit(file, std::ios::binary | std::ios::in | std::ios::out);
    edit.seekp(-1, std::ios::end);
    edit.put(0);
  }
  require(!load(), "A corrupt payload fails its checksum");
  require(save(), "Atomic replacement repairs an existing corrupt cache");
  std::filesystem::resize_file(file, std::filesystem::file_size(file) - 1);
  require(!load(), "Truncated cache files fall back to tracing");
  require(save(), "A truncated cache can be replaced");
  data.pixels[0] = 0x7c00;
  require(save() && !load(), "Nonfinite atlas coefficients are refused before GPU upload");
  data.pixels[0] = 0x3c00;
  data.specular.header.parameters[0] = 2;
  require(!save(), "Mismatched reflection lookup counts are refused before replacing the cache");
  data.specular.header.parameters[0] = 1;
  data.layout.charts[0].page = 2;
  require(save() && !load(), "Even a checksummed payload must obey atlas bounds");
  data.layout.charts[0].page = 0;
  data.mixed.charts[0][0] = 99;
  require(save() && !load(), "Cached light channels must reference a current captured light");
  data.mixed.charts[0][0] = 0;
  {
    bc::detail::Writer output(file, identity, {});
    output.pod(uint32_t(0));
    output.pod(uint32_t(25));
    output.pod(data.grid.header);
    output.pod(UINT64_MAX);
    output.finish();
  }
  require(!load(), "Hostile array counts are rejected before allocation");
  require(save(), "Restore a complete cache before cancellation");
  std::stop_source stop;
  stop.request_stop();
  require(!bc::write(file, identity,
                     {data.grid, data.layout, data.mixed, data.captures, data.specular, data.pixels, 25, true,
                      false},
                     stop.get_token()) &&
              load(),
          "A cancelled writer preserves the previously completed cache");
  bool rejected = false;
  try {
    const uint32_t invalid = 3;
    f::mixed::Occlusion invalidStates(1, 1, {&invalid, 1});
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "Reserved constant-visibility states are refused on restore");
  require(!bc::write(file / "blocked" / "lighting.bake", identity,
                     {data.grid, data.layout, data.mixed, data.captures, data.specular, data.pixels, 25, true,
                      false}) &&
              load(),
          "Unavailable cache storage preserves the existing completed data");
  {
    const uint32_t selected[] = {0}, other[] = {1};
    const auto policy = f::bake::directPolicy(*source, selected);
    const auto aggregateKey = bc::key(source->contentHash, config, .001f, device, policy);
    require(aggregateKey != identity && aggregateKey != bc::key(source->contentHash, config, .001f,
                                                               device, f::bake::directPolicy(*source, other)),
            "Aggregate membership and estimator shaders participate in cache identity");
    auto aggregate = data.mixed;
    aggregate.lights.directLighting = policy;
    aggregate.charts[0].fill(f::mixed::none);
    aggregate.directCharts[0].fill(f::mixed::none);
    const auto aggregateFile = root / "aggregate.bake";
    const auto saveAggregate = [&] {
      return bc::write(aggregateFile, aggregateKey,
                       {data.grid, data.layout, aggregate, data.captures, data.specular, data.pixels,
                        data.surfaceSamples, data.directDiffuse, data.sun});
    };
    const auto loadAggregate = [&] {
      return bc::read(aggregateFile, aggregateKey, source, config, device, {}, policy);
    };
    require(saveAggregate(), "Aggregate ownership is serialized separately from mixed channel owners");
    const auto reloaded = loadAggregate();
    require(reloaded && reloaded->mixed.lights.directLighting.lights == policy.lights &&
                reloaded->mixed.lights.bakedDiffuse(0) && !reloaded->mixed.lights.bakedDiffuse(1) &&
                reloaded->mixed.lights.match(source->lights[0]) == 0 && reloaded->pixels == data.pixels,
            "Cache reload preserves aggregate membership, live matching and atlas contents");
    require(!bc::read(aggregateFile, identity, source, config, device),
            "Legacy and aggregated diffuse caches cannot be exchanged");
    require(!bc::read(aggregateFile, aggregateKey, source, config, device),
            "Even a matching file key cannot override the caller's expected light policy");
    aggregate.charts[0][0] = 0;
    require(saveAggregate() && !loadAggregate(),
            "An aggregate light cannot also own a mixed visibility channel on cache import");
    aggregate.charts[0][0] = f::mixed::none;
    require(saveAggregate(), "A valid aggregate snapshot replaces the invalid-owner fixture");
    const auto identityRange = aggregate.lights.identities.equal_range(source->lights[0].id);
    aggregate.lights.identities.erase(identityRange.first, identityRange.second);
    require(saveAggregate() && !loadAggregate(),
            "Missing aggregate identity cannot silently drop ownership during runtime matching");
    aggregate.lights.identities.emplace(source->lights[0].id, 0);
    require(saveAggregate(), "Complete aggregate ownership is restored before the writer failure fixture");
    aggregate.lights.directLighting.lights[0] = f::bake::DirectMode::Dynamic;
    require(!saveAggregate() && loadAggregate().has_value(),
            "Invalid moving-light membership cannot replace a completed aggregate cache");
  }
  {
    const uint32_t selected[] = {0, 1};
    const auto policy = f::bake::directPolicy(*source, selected, f::bake::DirectMode::BakedMixed);
    const auto correctedKey = bc::key(source->contentHash, config, .001f, device, policy);
    require(correctedKey != bc::key(source->contentHash, config, .001f, device,
                                    f::bake::directPolicy(*source, selected)),
            "Retained aggregate visibility has a different cache identity from diffuse-only ownership");
    auto corrected = data.mixed;
    corrected.lights.directLighting = policy;
    const auto correctedFile = root / "aggregate-mixed.bake";
    const auto saveCorrected = [&] {
      return bc::write(correctedFile, correctedKey,
                       {data.grid, data.layout, corrected, data.captures, data.specular, data.pixels,
                        data.surfaceSamples, data.directDiffuse, data.sun});
    };
    const auto loadCorrected = [&] {
      return bc::read(correctedFile, correctedKey, source, config, device, {}, policy);
    };
    require(saveCorrected(), "Aggregate mixed irradiance and attributed channels can share a cache");
    const auto reloaded = loadCorrected();
    require(reloaded && reloaded->mixed.charts == corrected.charts &&
                reloaded->mixed.directCharts == corrected.directCharts &&
                reloaded->mixed.masks.words(1) == corrected.masks.words(1) &&
                std::equal(reloaded->mixed.occlusion.words().begin(), reloaded->mixed.occlusion.words().end(),
                           corrected.occlusion.words().begin(), corrected.occlusion.words().end()) &&
                reloaded->pixels == data.pixels && reloaded->mixed.lights.bakedDiffuse(0) &&
                reloaded->mixed.lights.retainsVisibility(0) &&
                reloaded->mixed.lights.match(source->lights[0]) == 0,
            "Aggregate cache reload preserves diffuse ownership, unit SH, indexed masks and static visibility");
    require(!bc::read(correctedFile, correctedKey, source, config, device, {},
                      f::bake::directPolicy(*source, selected)),
            "A supplied key cannot reinterpret aggregate mixed coverage as diffuse-only ownership");
    corrected.lights.directLighting.lights[0] = static_cast<f::bake::DirectMode>(255);
    require(!saveCorrected() && loadCorrected(),
            "Unknown ownership modes cannot replace a completed aggregate mixed cache");
  }
  {
    auto styledSource = std::make_shared<scene::BakeScene>(*source);
    styledSource->lights[0].style = 32;
    auto styledPlan = data.mixed;
    styledPlan.lights.source = styledSource;
    auto styledConfig = config;
    styledConfig.styledIndirect = true;
    styledConfig.styleStorageMiB = 1;
    const auto styledKey = bc::key(source->contentHash, styledConfig, .001f, device);
    require(styledKey != identity, "Attributed storage has a separate cache identity");
    auto changedBudget = styledConfig;
    changedBudget.styleStorageMiB = 2;
    require(bc::key(source->contentHash, changedBudget, .001f, device) != styledKey,
            "Encoded style budget participates in attributed cache identity");
    auto layout = data.layout;
    layout.points.resize(1);
    layout.owners.resize(64, f::lightmap::unowned);
    layout.owners[0] = 0;
    f::bake::Result surface;
    surface.irradiance[1] = {-1, 2, -3, 0};
    f::bake::sparse::Builder builder({8, 1, 8}, 1u << 20);
    builder.add(32, layout, {&surface, 1}, data.grid.probes);
    auto words = builder.serialize();
    const auto styledFile = root / "styled.bake";
    auto saveStyled = [&] {
      return bc::write(styledFile, styledKey,
          {data.grid, data.layout, styledPlan, data.captures, data.specular, data.pixels,
           data.surfaceSamples, data.directDiffuse, data.sun, words});
    };
    auto loadStyled = [&] { return bc::read(styledFile, styledKey, styledSource, styledConfig, device); };
    require(saveStyled(), "Attributed cache writes validated surface and probe SH");
    const auto restored = loadStyled();
    require(restored && restored->styles == words && restored->pixels == data.pixels &&
                restored->grid.probes.size() == data.grid.probes.size() &&
                std::memcmp(restored->grid.probes.data(), data.grid.probes.data(), data.grid.probes.size() * sizeof(f::bake::Result)) == 0,
            "Attributed cache retains exact style words and shared base pixels/probes");
    require(!bc::read(styledFile, styledKey, styledSource, config, device),
            "A caller-supplied key cannot enable style payloads under legacy policy");
    auto wrongGroup = std::make_shared<scene::BakeScene>(*styledSource);
    wrongGroup->lights[0].style = 97;
    require(!bc::read(styledFile, styledKey, wrongGroup, styledConfig, device),
            "A matching file key cannot override captured style ownership");
    auto constrained = device;
    constrained.storageRange = uint32_t(sizeof(f::bake::GridHeader) + data.grid.probes.size() * sizeof(f::bake::Result) + 32);
    require(!bc::read(styledFile, styledKey, styledSource, styledConfig, constrained),
            "Combined probe and style allocation obeys device storage range before allocation");
    words[1]++;
    require(!saveStyled() && loadStyled().has_value(), "Invalid sparse data cannot replace a completed cache");
    words[1]--;
    data.grid.header.settings[3] = 1;
    require(!saveStyled() && loadStyled().has_value(), "Cached headers cannot request GPU reads beyond their probe array");
    data.grid.header.settings[3] = 0;
  }
  {
    auto indexed = data.grid;
    indexed.header.dimensions = {4, 2, 2, 8};
    indexed.header.settings[2] = 2;
    indexed.lookup.resize(16);
    for (uint32_t row = 0; row < 4; ++row) {
      indexed.lookup[row * 4] = 8 - row * 2;
      indexed.lookup[row * 4 + 3] = 7 - row * 2;
    }
    const auto sparseFile = root / "indexed-grid.bake";
    auto saveIndexed = [&] {
      return bc::write(sparseFile, identity,
          {indexed, data.layout, data.mixed, data.captures, data.specular, data.pixels,
           data.surfaceSamples, data.directDiffuse, data.sun});
    };
    require(saveIndexed(), "Sparse lattice directories serialize separately from compact probe records");
    const auto restored = bc::read(sparseFile, identity, source, config, device);
    require(restored && restored->grid.lookup == indexed.lookup && restored->grid.origins.size() == 8 &&
                restored->grid.header.dimensions == indexed.header.dimensions,
            "Sparse cache restore preserves holes, dimensions and permuted physical records");
    require(restored->grid.origins.front().position == f::Vec3{3, 1, 1} &&
                restored->grid.origins.back().position == f::Vec3{0, 0, 0},
            "A restored directory reconstructs each physical record's canonical world origin");
    auto constrained = device;
    constrained.storageRange = uint32_t(sizeof(f::bake::GridHeader) + indexed.probes.size() * sizeof(f::bake::Result) + 32);
    require(!bc::read(sparseFile, identity, source, config, constrained),
            "Combined physical records and padded directory obey the device storage limit");
    std::ifstream input(sparseFile, std::ios::binary);
    const std::vector<char> original((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    const size_t lookupOffset = sizeof(bc::detail::Header) + 2 * sizeof(uint32_t) + sizeof(f::bake::GridHeader) +
        3 * sizeof(uint64_t) + indexed.probes.size() * sizeof(f::bake::Result) + indexed.tasks.size() * sizeof(uint32_t);
    for (auto change : {std::pair{0u, 0u}, std::pair{1u, 8u}, std::pair{1u, 9u}}) {
      auto damaged = original;
      std::memcpy(damaged.data() + lookupOffset + change.first * sizeof(uint32_t), &change.second, sizeof(uint32_t));
      bc::detail::Header header;
      std::memcpy(&header, damaged.data(), sizeof(header));
      header.checksum = scene::hashBytes(damaged.data() + sizeof(header), header.bytes, 1469598103934665603ull);
      std::memcpy(damaged.data(), &header, sizeof(header));
      std::ofstream output(sparseFile, std::ios::binary | std::ios::trunc);
      output.write(damaged.data(), std::streamsize(damaged.size()));
      output.close();
      require(!bc::read(sparseFile, identity, source, config, device),
              "Even checksummed sparse directories reject orphaned, duplicate and invalid record owners");
    }
    require(saveIndexed(), "A valid sparse directory repairs a corrupted payload atomically");
    indexed.lookup[1] = indexed.lookup[0];
    require(!saveIndexed(), "Repeated compact owners cannot replace a completed cache");
    indexed.lookup[1] = 9;
    require(!saveIndexed(), "Out-of-range compact records are rejected before persistence");
    indexed.lookup[1] = 0;
    indexed.lookup[0] = 0;
    require(!saveIndexed() && bc::read(sparseFile, identity, source, config, device),
            "Orphan compact records are rejected while preserving the previous valid cache");
  }
  {
    auto refined = data.grid;
    refined.header.dimensions = {2, 2, 2, 35};
    refined.header.settings[2] = 4;
    refined.probes.resize(35, data.grid.probes.front());
    refined.tasks.clear();
    for (uint32_t i = 0; i < 35; ++i) refined.tasks.push_back(i);
    refined.lookup.assign(48, 0);
    for (uint32_t i = 0; i < 8; ++i) refined.lookup[i] = i + 1;
    refined.lookup[8] = f::bake::adaptiveDirectoryMagic;
    refined.lookup[9] = 8; // The local patch explicitly selects a twofold subdivision.
    refined.lookup[10] = 48;
    refined.lookup[11] = 8;
    refined.lookup[12] = f::bake::patchReference(20, 2);
    for (uint32_t i = 0; i < 27; ++i) refined.lookup[20 + i] = 9 + i;
    const auto adaptiveFile = root / "adaptive-grid.bake";
    require(bc::write(adaptiveFile, identity,
                {refined, data.layout, data.mixed, data.captures, data.specular, data.pixels,
                 data.surfaceSamples, data.directDiffuse, data.sun}),
            "Adaptive fields persist physical records beyond the coarse lattice node count");
    const auto restored = bc::read(adaptiveFile, identity, source, config, device);
    require(restored && restored->grid.lookup == refined.lookup && restored->grid.origins.size() == 35 &&
                restored->grid.origins.back().spacing == .5f &&
                restored->grid.origins.back().position == f::Vec3{1, 1, 1},
            "Adaptive cache restore retains fine topology and per-record canonical spacing");
    refined.header.settings[2] = 5;
    refined.probes.back().irradiance[0][3] = .4f;
    const auto saveMoved = [&] {
      return bc::write(adaptiveFile, identity,
          {refined, data.layout, data.mixed, data.captures, data.specular, data.pixels,
           data.surfaceSamples, data.directDiffuse, data.sun});
    };
    auto movedConfig = config;
    movedConfig.relocateProbes = true;
    require(saveMoved() && bc::read(adaptiveFile, identity, source, movedConfig, device),
            "An aligned transition node retains its coarser trace relocation when referenced by a finer patch");
    refined.probes[21].irradiance[0][3] = .4f;
    require(!saveMoved(), "A genuinely fine midpoint cannot claim a coarser relocation allowance");
    refined.probes[21].irradiance[0][3] = 0;
    refined.lookup[12] |= 1u << 26;
    require(!saveMoved(), "A cache cannot authorize stitching to a nonexistent neighbouring region");
  }
  {
    bc::recordLookup(file, identity, source->contentHash, 123, true);
    bc::recordLookup(file, identity, source->contentHash, 123, false);
    bc::recordLookup(root / "missing.bake", identity + 1, source->contentHash, 124, false);
    std::ifstream receipts(root / "cache-events.jsonl");
    std::string hit, rejected, missing;
    std::getline(receipts, hit);
    std::getline(receipts, rejected);
    std::getline(receipts, missing);
    require(hit.find("\"outcome\":\"hit\"") != std::string::npos &&
            rejected.find("\"outcome\":\"rejected\"") != std::string::npos &&
            missing.find("\"outcome\":\"missing\"") != std::string::npos &&
            hit.find("\"policy\":\"123\"") != std::string::npos &&
            missing.find("\"policy\":\"124\"") != std::string::npos,
            "Cache lookup receipts distinguish hits, absent payloads and rejected metadata independently of policy/input identity");
  }
  std::puts("Bake cache: identity, complete payloads, chart UVs, mixed light bindings, corruption and "
            "cancellation passed");
}
