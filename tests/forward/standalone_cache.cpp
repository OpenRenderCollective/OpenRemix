#include "forward/probes/capture_cache.h"
#include "scene/fixtures.h"
#include <cstring>
#include <fstream>

namespace p = remix::forward::probes;
namespace cache = p::capture_cache;
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
  const auto identity = cache::standaloneKey(123, 456, config, 4, .5f, device);
  auto execution = config;
  execution.batch = 1;
  execution.gpuBudgetMs = 0;
  execution.diskCache = false;
  require(
      cache::standaloneKey(123, 456, execution, 4, .5f, device) == identity,
      "Execution controls or disk policy changed reflection content identity");
  auto quality = config;
  ++quality.samples;
  require(
      cache::standaloneKey(123, 456, quality, 4, .5f, device) != identity &&
          cache::standaloneKey(124, 456, config, 4, .5f, device) != identity &&
          cache::standaloneKey(123, 457, config, 4, .5f, device) != identity &&
          cache::standaloneKey(123, 456, config, 3, .5f, device) != identity &&
          cache::standaloneKey(123, 456, config, 4, .25f, device) != identity,
      "Lighting, placement or shader change reused stale reflection identity");
  auto changedDevice = device;
  ++changedDevice.driver;
  require(cache::standaloneKey(123, 456, config, 4, .5f, changedDevice) !=
              identity,
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
  const auto directory = file.parent_path() / "shared-case";
  const auto shared =
      cache::storeShared(directory, snapshot.captures, snapshot.data);
  require(shared != 0, "Shared reflection write failed");
  const auto blob = cache::sharedPath(directory, shared);
  const auto timestamp = std::filesystem::last_write_time(blob);
  require(cache::storeShared(directory, snapshot.captures, snapshot.data) ==
                  shared &&
              std::filesystem::last_write_time(blob) == timestamp,
          "Identical content rewrote immutable reflection blob");
  auto sharedRead = cache::readShared(directory, shared, device.limits);
  require(sharedRead && sharedRead->data.texels == snapshot.data.texels,
          "Shared reflection roundtrip failed");
  auto changed = snapshot;
  changed.captures[0].radiance[0][0] += 1;
  const auto other =
      cache::storeShared(directory, changed.captures, changed.data);
  require(other && other != shared, "Changed raw radiance reused content key");
  require(!cache::readShared(directory, shared + 2, device.limits) &&
              !cache::storeShared(directory, snapshot.captures, snapshot.data,
                                  stop.get_token()),
          "Missing object or cancelled write accepted");
  {
    std::fstream bytes(blob, std::ios::binary | std::ios::in | std::ios::out);
    bytes.seekp(sizeof(remix::forward::cache_io::Header));
    char corrupt = 99;
    bytes.write(&corrupt, 1);
  }
  require(!cache::readShared(directory, shared, device.limits),
          "Corrupt shared object accepted");
  require(cache::storeShared(directory, snapshot.captures, snapshot.data) ==
                  shared &&
              cache::readShared(directory, shared, device.limits).has_value(),
          "Corrupt object was not repaired");
  require(cache::write(blob, shared, changed),
          "Collision fixture write failed");
  require(!cache::readShared(directory, shared, device.limits) &&
              cache::storeShared(directory, snapshot.captures, snapshot.data) ==
                  0,
          "Valid mismatched object was accepted or overwritten");
  std::filesystem::remove(blob);
  std::filesystem::remove(cache::sharedPath(directory, other));
  std::filesystem::remove(directory / "reflections" / "shared");
  std::filesystem::remove(directory / "reflections");
  std::filesystem::remove(directory);
  auto retained = std::make_unique<cache::Snapshot>(snapshot);
  require(cache::persistence().submit(file, identity, std::move(retained)),
          "Reflection persistence admission failed");
  cache::persistence().waitIdle();
  require(cache::read(file, identity, device.limits).has_value(),
          "Background reflection write did not persist");
  cache::persistence().stop();
  std::filesystem::remove(file);
}
