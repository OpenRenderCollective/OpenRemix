#include "forward/bake/local_reflection_cache.h"
#include "forward/bake/cache_persistence.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>

using namespace remix::forward;
namespace cache = remix::forward::bake::local_cache;
namespace p = remix::forward::probes;
namespace {
  void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
  }
  std::vector<char> bytes(const std::filesystem::path &file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }
  cache::Snapshot fixture() {
    cache::Snapshot result;
    auto source = std::make_shared<remix::scene::BakeScene>();
    source->sceneId = 100; source->revision = 1; source->contentHash = 7; source->ready = true;
    auto surface = std::make_shared<remix::scene::Surface>();
    for (const Vec3 point : {Vec3{0,0,0}, Vec3{1,0,0}, Vec3{0,1,0}}) {
      remix::scene::Vertex vertex{};
      std::copy_n(point.begin(), 3, vertex.position); vertex.normal[2] = 1;
      surface->vertices.push_back(vertex);
    }
    surface->indices = {0,1,2};
    remix::scene::Draw draw; draw.surface = surface; source->draws.push_back(draw);
    source->charts.emplace_back().texcoords.resize(3);
    result.source = source;
    auto evidence = std::make_shared<p::ReceiverCoverageEvidence>();
    evidence->sourceContext = p::coveragePlanContext(*source, {});
    evidence->fits.available = true; evidence->fits.supported = 1;
    p::Capture capture;
    capture.bounds.include({-1,-1,-1}); capture.bounds.include({1,1,1});
    capture.position = {.25f,.25f,.5f}; capture.resolution = 8;
    p::FittedRegion geometry;
    geometry.bounds = capture.bounds; geometry.site.position = capture.position; geometry.geometryEvaluated = true;
    for (auto &face : geometry.faces) {
      face.solidAngleFraction = 1.f / 6; face.supportFraction = 1;
      face.hitBounds.include({-1,-1,0}); face.hitBounds.include({1,1,0});
      face.plane = p::ProjectionPlaneAudit{0,0,0,1,64};
    }
    p::ReceiverProjection variant;
    variant.projection = variant.influence = capture.bounds; variant.geometry = geometry; variant.coversReceiver = true;
    evidence->fits.attempts.push_back({0,{variant}});
    evidence->observations = {{0,0,p::ReceiverVisibility::Visible}, {1,0,p::ReceiverVisibility::Blocked}};
    result.evidence = evidence;
    result.image.data.header.parameters = {1,8,4,510};
    result.image.data.header.probes[0] = p::projectionRecord(capture);
    result.image.data.texels.resize(510, {1,.5f,3,0});
    result.image.selection.header = result.image.data.header;
    result.image.selection.context = p::coveragePlanContext(*source, {});
    result.image.selection.layers = {{0,0}};
    return result;
  }
}
int main(int argc, char **argv) {
  require(argc == 2, "Local cache test has an owned output directory");
  const std::filesystem::path directory = argv[1];
  std::filesystem::create_directories(directory);
  auto source = fixture();
  bake::Config config; config.reflectionResolution = 8; config.reflectionDetailTolerance = source.detailTolerance;
  const auto file = directory / "local.bin", roundtrip = directory / "roundtrip.bin";
  require(cache::write(file, 42, source), "Valid local reflections persist");
  const auto original = bytes(file);
  const auto baseFile = directory / "existing-lighting.bake";
  { std::ofstream out(baseFile, std::ios::binary); out << "existing valid lighting must remain untouched"; }
  const auto baseBytes = bytes(baseFile);
  auto companion = std::make_unique<bake::cache::Snapshot>();
  companion->localReflectionsOnly = true;
  companion->localReflections = std::make_shared<cache::Snapshot>(fixture());
  bake::cache::Persistence persistence;
  require(persistence.submit(baseFile, 42, std::move(companion)), "Companion-only payload is admitted");
  persistence.waitIdle();
  require(bytes(baseFile) == baseBytes && cache::read(cache::path(baseFile), 42, source.source, config, {}),
          "Background companion upgrade persists local data without rewriting the base lighting cache");

  auto current = std::make_shared<remix::scene::BakeScene>(*source.source);
  current->sceneId += 1; current->revision += 4; current->chartRevision += 7; current->reflectionRevision += 2;
  auto restored = cache::read(file, 42, current, config, {});
  require(restored && p::matchesCoverageContext(restored->evidence->sourceContext, *current, {}) &&
              p::matchesCoverageContext(restored->image.selection.context, *current, {}),
          "Identical content safely rebinds runtime generation IDs");
  require(cache::write(roundtrip, 42, *restored) && original == bytes(roundtrip),
          "Fits, observations, filtered HDR mips and mappings round-trip byte for byte");
  require(cache::retainedBytes(*restored) >= restored->image.data.texels.size() * sizeof(Vec4),
          "Persistence budget includes retained reflection pixels");
  require(!cache::read(file, 43, current, config, {}), "Wrong lighting key cannot reuse local radiance");
  auto changed = std::make_shared<remix::scene::BakeScene>(*current); ++changed->contentHash;
  require(!cache::read(file, 42, changed, config, {}), "Changed lighting content rejects local radiance");
  changed = std::make_shared<remix::scene::BakeScene>(*current); changed->sceneScale *= 2;
  require(!cache::read(file, 42, changed, config, {}), "Changed unit scale rejects local radiance");
  changed = std::make_shared<remix::scene::BakeScene>(*current);
  changed->draws[0].surface = std::make_shared<remix::scene::Surface>(*current->draws[0].surface);
  changed->draws[0].surface->vertices[1].position[0] += .125f;
  require(!cache::read(file, 42, changed, config, {}), "Receiver geometry fingerprint is independent of the supplied content hash");
  changed = std::make_shared<remix::scene::BakeScene>(*current); changed->reflectionHints.push_back({1,2,3});
  require(!cache::read(file, 42, changed, config, {}), "Changed reflection hints reject stale candidate origins");
  auto policy = config; policy.reflectionDetailTolerance += .5f;
  require(!cache::read(file, 42, current, policy, {}), "Changed fitting policy rejects cached local bounds");
  policy = config; policy.reflectionResolution = 16;
  require(!cache::read(file, 42, current, policy, {}), "Changed resolution rejects incompatible image layers");
  auto bad = fixture(); bad.image.data.texels[0][0] = std::numeric_limits<float>::quiet_NaN();
  require(!cache::write(file, 42, bad) && original == bytes(file), "Invalid HDR data cannot replace a valid cache file");
  bad = fixture(); bad.image.selection.layers[0].candidate = 192;
  require(!cache::write(file, 42, bad), "Invalid candidate ownership is refused before persistence");
  bad = fixture(); bad.image.data.header.probes[0].position[0] += .125f; bad.image.selection.header = bad.image.data.header;
  require(!cache::write(file, 42, bad), "Image geometry must match its candidate evidence");
  bad = fixture();
  auto duplicates = std::make_shared<p::ReceiverCoverageEvidence>(*bad.evidence);
  duplicates->observations.push_back(duplicates->observations[0]); bad.evidence = duplicates;
  require(!cache::write(file, 42, bad), "Duplicate observations cannot inflate cached coverage");
  auto corrupt = original; corrupt.back() ^= 1;
  const auto damaged = directory / "damaged.bin";
  { std::ofstream out(damaged, std::ios::binary); out.write(corrupt.data(), std::streamsize(corrupt.size())); }
  require(!cache::read(damaged, 42, current, config, {}), "Corrupted local radiance fails checksum verification");
  { std::ofstream out(damaged, std::ios::binary); out.write(original.data(), 17); }
  require(!cache::read(damaged, 42, current, config, {}), "Truncated local cache is a safe miss");
  std::stop_source cancelled; cancelled.request_stop();
  require(!cache::read(file, 42, current, config, {}, cancelled.get_token()) &&
              !cache::write(file, 42, source, cancelled.get_token()) && original == bytes(file),
          "Cancellation preserves the previous valid local cache");
  std::puts("Local reflection cache identity, generation rebinding, evidence, HDR data and corruption guards passed");
}
