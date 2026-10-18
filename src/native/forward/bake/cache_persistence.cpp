#include "forward/bake/cache_persistence.h"
#include <chrono>
#include <cstdio>

namespace remix::forward::bake::cache {
  namespace {
    View view(const Snapshot &s) {
      return {s.grid, s.layout, s.mixed, s.captures, s.specular, s.pixels,
              s.surfaceSamples, s.directDiffuse, s.sun, s.styles,
              s.localProbes ? &*s.localProbes : nullptr};
    }
    template <typename T> uint64_t storage(const std::vector<T> &v) {
      return uint64_t(v.capacity()) * sizeof(T);
    }
  }
  uint64_t Persistence::retainedCharge(const Snapshot &s) {
    uint64_t bytes = sizeof(Snapshot) + storage(s.grid.probes) + storage(s.grid.tasks) + storage(s.grid.points) +
        storage(s.layout.charts) + storage(s.layout.mappings) + storage(s.pixels) + storage(s.styles) +
        storage(s.layout.points) + storage(s.layout.pointCharts) + storage(s.layout.addresses) +
        storage(s.layout.receiverSamples) + storage(s.layout.footprints) + storage(s.layout.sampleCenters) +
        storage(s.layout.owners) + storage(s.mixed.charts) + storage(s.mixed.directCharts) +
        storage(s.mixed.tasks) + storage(s.mixed.remaining) + storage(s.mixed.next) +
        storage(s.captures) + storage(s.specular.texels);
    for (const auto &remaining : s.mixed.remaining) bytes += storage(remaining);
    for (const auto &mapping : s.layout.mappings) bytes += storage(mapping.texcoords);
    for (const auto &capture : s.captures) bytes += storage(capture.radiance);
    // Private compressed containers expose logical storage. Charge twice their
    // size plus per-entry metadata to cover vector growth and identity tables.
    bytes += 2 * (s.mixed.occlusion.words().size_bytes() + s.mixed.masks.bytes().size_bytes() +
                   s.mixed.masks.entries().size_bytes()) + s.mixed.masks.entries().size() * 64;
    bytes += storage(s.mixed.lights.directLighting.lights) + s.mixed.lights.identities.size() * 64;
    if (s.localProbes)
      bytes += 2 * (s.localProbes->offsets().size_bytes() + s.localProbes->entries().size_bytes());
    // The captured source is shared with the published runtime, not duplicated.
    return bytes;
  }
  Persistence::Persistence(uint64_t byteBudget, Writer write)
      : budget(byteBudget), writer(write ? std::move(write) :
            Writer([](const auto &file, uint64_t key, const View &data) { return cache::write(file, key, data); })) {}
  Persistence::~Persistence() {
    stop();
  }
  void Persistence::stop() {
    {
      std::lock_guard lock(mutex);
      stopping = true;
    }
    wake.notify_one();
    // Completed maps remain useful after a map change or renderer switch.
    // Orderly process shutdown drains them; no renderer/GPU lifetime is held.
    if (worker.joinable()) worker.join();
    {
      std::lock_guard lock(mutex);
      stopping = false;
    }
  }
  bool Persistence::submit(std::filesystem::path file, uint64_t key, std::unique_ptr<const Snapshot> snapshot) {
    if (!snapshot) return false;
    const auto charge = retainedCharge(*snapshot);
    {
      std::lock_guard lock(mutex);
      if (stopping || pending.snapshot || charge > budget || charge > budget - retained) return false;
      if (!worker.joinable()) worker = std::thread([this] { run(); });
      retained += charge;
      pending = {std::move(file), key, charge, std::move(snapshot)};
    }
    wake.notify_one();
    return true;
  }
  void Persistence::waitIdle() {
    std::unique_lock lock(mutex);
    idle.wait(lock, [&] { return !active && !pending.snapshot; });
  }
  void Persistence::run() {
    for (;;) {
      Request request;
      {
        std::unique_lock lock(mutex);
        wake.wait(lock, [&] { return stopping || pending.snapshot; });
        if (!pending.snapshot) return;
        request = std::move(pending);
        active = true;
      }
      const auto started = std::chrono::steady_clock::now();
      bool saved = false;
      try {
        saved = writer(request.file, request.key, view(*request.snapshot));
        if (!saved)
          std::fprintf(stderr, "Background bake cache write failed; published lighting remains active\n");
      } catch (...) {
        std::fprintf(stderr, "Background bake cache writer failed; published lighting remains active\n");
      }
      std::fprintf(stderr, "Forward bake cache persistence: key=%llu retained_bytes=%llu outcome=%s wall_ms=%.3f\n",
                   static_cast<unsigned long long>(request.key), static_cast<unsigned long long>(request.charge),
                   saved ? "saved" : "failed",
                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
      request.snapshot.reset(); // release CPU storage before admitting another payload
      {
        std::lock_guard lock(mutex);
        retained -= request.charge;
        active = false;
      }
      idle.notify_all();
    }
  }
  Persistence &persistence() {
    static Persistence service;
    return service;
  }
} // namespace remix::forward::bake::cache
