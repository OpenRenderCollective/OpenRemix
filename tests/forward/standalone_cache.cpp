#include "forward/probes/standalone_cache.h"
#include "scene/fixtures.h"
#include <cstring>
#include <fstream>

namespace p = remix::forward::probes;
namespace cache = p::standalone_cache;
using remix::test::require;
int main(int argc, char **argv) {
  require(argc == 2, "Cache test needs an output directory");
  const auto file = std::filesystem::path(argv[1]) / "sample.reflection";
  std::filesystem::create_directories(file.parent_path());
  std::filesystem::remove(file);
  p::CaptureConfig config;
  config.reflectionResolution = 8;
  config.textureResolution = 16;
  cache::Device device{1, 2, 3, {4, 8, 4096, 256, 128u << 20}};
  const auto identity = cache::key(123, 456, config, 4, .5f, device);
  auto execution = config;
  execution.batch = 1;
  execution.gpuBudgetMs = 0;
  execution.diskCache = false;
  require(
      cache::key(123, 456, execution, 4, .5f, device) == identity,
      "Execution controls or disk policy changed reflection content identity");
  auto quality = config;
  ++quality.samples;
  require(
      cache::key(123, 456, quality, 4, .5f, device) != identity &&
          cache::key(124, 456, config, 4, .5f, device) != identity &&
          cache::key(123, 457, config, 4, .5f, device) != identity &&
          cache::key(123, 456, config, 3, .5f, device) != identity &&
          cache::key(123, 456, config, 4, .25f, device) != identity,
      "Lighting, placement or shader change reused stale reflection identity");
  auto changedDevice = device;
  ++changedDevice.driver;
  require(cache::key(123, 456, config, 4, .5f, changedDevice) != identity,
          "Driver identity missing from cache provenance");
  cache::Snapshot snapshot;
  p::Capture capture;
  capture.bounds.include({-1, -1, -1});
  capture.bounds.include({1, 1, 1});
  capture.influence = capture.bounds;
  capture.resolution = 8;
  capture.radiance.resize(6 * 8 * 8, {1, 2, 3, 4});
  snapshot.captures.push_back(capture);
  snapshot.data = p::filter(snapshot.captures, 8);
  require(!cache::read(file, identity, device.limits),
          "Missing reflection file accepted");
  require(cache::write(file, identity, snapshot),
          "Standalone reflection cache write failed");
  auto restored = cache::read(file, identity, device.limits);
  require(restored &&
              restored->captures[0].radiance == snapshot.captures[0].radiance &&
              restored->data.texels == snapshot.data.texels &&
              std::memcmp(&restored->data.header, &snapshot.data.header,
                          sizeof(p::Header)) == 0,
          "Standalone cache changed raw radiance, filtered texels or "
          "projection metadata");
  require(!cache::read(file, identity + 1, device.limits),
          "Stale reflection key accepted");
  auto wrong = device.limits;
  wrong.resolution = 16;
  require(!cache::read(file, identity, wrong),
          "Wrong reflection resolution accepted");
  std::stop_source stop;
  stop.request_stop();
  require(!cache::write(file, identity, snapshot, stop.get_token()) &&
              cache::read(file, identity, device.limits).has_value(),
          "Cancelled write replaced valid reflection cache");
  {
    std::fstream bytes(file, std::ios::binary | std::ios::in | std::ios::out);
    bytes.seekp(sizeof(remix::forward::cache_io::Header));
    char corrupt = 99;
    bytes.write(&corrupt, 1);
  }
  require(!cache::read(file, identity, device.limits),
          "Corrupt reflection payload accepted");
  require(cache::write(file, identity, snapshot),
          "Corrupt reflection file could not be replaced");
  std::filesystem::resize_file(file,
                               sizeof(remix::forward::cache_io::Header) + 1);
  require(!cache::read(file, identity, device.limits),
          "Truncated reflection payload accepted");
  auto retained = std::make_unique<cache::Snapshot>(snapshot);
  require(cache::persistence().submit(file, identity, std::move(retained)),
          "Reflection persistence admission failed");
  cache::persistence().waitIdle();
  require(cache::read(file, identity, device.limits).has_value(),
          "Background reflection write did not persist");
  cache::persistence().stop();
  std::filesystem::remove(file);
}
