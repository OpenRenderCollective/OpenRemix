#include "forward/bake/cache_persistence.h"
#include "scene/fixtures.h"
#include <future>
#include <stdexcept>

using namespace remix::forward::bake;
using remix::test::require;
using namespace std::chrono_literals;

namespace {
  std::unique_ptr<const cache::Snapshot> snapshot(uint16_t marker) {
    auto data = std::make_unique<cache::Snapshot>();
    data->pixels.resize(128, marker);
    data->directionalAttempted = true;
    data->directionals.resize(16, marker);
    return data;
  }
}
int main() {
  const auto charge = cache::Persistence::retainedCharge(*snapshot(1));
  cache::Snapshot withScratch;
  const auto emptyCharge = cache::Persistence::retainedCharge(withScratch);
  withScratch.layout.points.resize(128);
  require(cache::Persistence::retainedCharge(withScratch) >= emptyCharge + 128 * sizeof(Point),
          "Admission accounts for accidentally retained sampling scratch as well as encoded pixels");
  const auto scratchCharge = cache::Persistence::retainedCharge(withScratch);
  withScratch.directionals.resize(128);
  require(cache::Persistence::retainedCharge(withScratch) >= scratchCharge + 128 * sizeof(uint32_t),
          "Complete directional payloads participate in background writer admission");
  const auto directionalCharge = cache::Persistence::retainedCharge(withScratch);
  withScratch.grid.depth.records.resize(128);
  withScratch.grid.lookup.resize(128);
  withScratch.grid.origins.resize(128);
  require(cache::Persistence::retainedCharge(withScratch) >= directionalCharge +
              128 * (sizeof(ProbeDepth::Record) + sizeof(uint32_t) + sizeof(Grid::Origin)),
          "Probe depth, directory and retained origin storage obey the background writer memory budget");
  std::promise<void> started, release;
  auto released = release.get_future().share();
  std::vector<uint64_t> written;
  cache::Persistence writer(2 * charge, [&](const auto &, uint64_t key, const cache::View &data) {
    require(data.pixels.size() == 128 && data.pixels.front() == key,
            "The writer owns the completed snapshot independently of its submitting caller");
    require(data.directionalAttempted && data.directionals.size() == 16 && data.directionals.front() == key,
            "Directional capability and payload survive asynchronous snapshot ownership transfer");
    if (key == 1) {
      started.set_value();
      released.wait();
    }
    written.push_back(key);
    if (key == 1) throw std::runtime_error("Injected storage failure");
    return true;
  });
  require(writer.submit("first", 1, snapshot(1)), "First completed snapshot is admitted");
  require(started.get_future().wait_for(5s) == std::future_status::ready, "Slow writer started");
  require(writer.submit("second-map", 2, snapshot(2)),
          "A later map can publish while an earlier map's write is blocked");
  require(!writer.submit("third-map", 3, snapshot(3)),
          "Slow storage cannot accumulate an unbounded map queue");
  // Both submissions returned while disk I/O was deliberately unable to finish.
  release.set_value();
  writer.waitIdle();
  require(written == std::vector<uint64_t>{1, 2},
          "A failed old-map write cannot discard the next map or terminate persistence");
  require(writer.submit("reused-budget", 4, snapshot(4)), "Retired snapshots release their admission charge");
  writer.waitIdle();
  writer.stop();
  require(writer.submit("restart-after-sdk-shutdown", 5, snapshot(5)),
          "A later SDK startup can restart persistence after its worker was joined");
  writer.waitIdle();
  cache::Persistence tooSmall(charge - 1, [](const auto &, uint64_t, const auto &) {
    require(false, "Rejected snapshots never enter the writer");
    return false;
  });
  require(!tooSmall.submit("oversized", 1, snapshot(1)), "Byte budget rejects oversized payloads without blocking");

  std::promise<void> shutdownStarted, shutdownRelease;
  auto shutdownGate = shutdownRelease.get_future().share();
  bool saved = false;
  auto draining = std::make_unique<cache::Persistence>(charge, [&](const auto &, uint64_t, const auto &) {
    shutdownStarted.set_value();
    shutdownGate.wait();
    saved = true;
    return true;
  });
  require(draining->submit("shutdown", 1, snapshot(1)), "Shutdown fixture admitted");
  require(shutdownStarted.get_future().wait_for(5s) == std::future_status::ready, "Shutdown writer started");
  auto shutdown = std::async(std::launch::async, [&] { draining.reset(); });
  require(shutdown.wait_for(20ms) == std::future_status::timeout,
          "Orderly shutdown retains in-flight completed maps until their save finishes");
  shutdownRelease.set_value();
  require(shutdown.wait_for(5s) == std::future_status::ready, "Shutdown drains safely after storage completes");
  shutdown.get();
  require(saved, "A normal exit does not cancel the just-completed map's cache");
}
