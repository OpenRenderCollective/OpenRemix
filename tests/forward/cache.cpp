#include "forward/bake/cache.h"
#include "forward/bake/cache_io.h"
#include "scene/fixtures.h"
#include <fstream>
#include <random>

using namespace remix;
using namespace remix::test;
namespace bc = remix::forward::bake::cache;
namespace f = remix::forward;
int main(int argc, char **argv) {
  require(argc == 2, "Supply a scratch directory");
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
  qualityKey([](auto &c) { c.directDiffuse = !c.directDiffuse; });
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
  require(save() && !load(), "Reflection lookup counts must match their captured data");
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
  std::puts("Bake cache: identity, complete payloads, chart UVs, mixed light bindings, corruption and "
            "cancellation passed");
}
