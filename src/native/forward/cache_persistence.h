#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace remix::forward::cache_io {
// Immutable CPU snapshots only. Never retains a renderer, device or frame.
// One active and one pending write, with a shared retained-memory ceiling.
template <typename Snapshot> class Persistence {
public:
  using Charge = std::function<uint64_t(const Snapshot &)>;
  using Writer = std::function<bool(const std::filesystem::path &, uint64_t,
                                    const Snapshot &)>;
  Persistence(uint64_t byteBudget, Charge charge, Writer write,
              const char *name)
      : budget(byteBudget), retainedCharge(std::move(charge)),
        writer(std::move(write)), label(name) {}
  ~Persistence();
  Persistence(const Persistence &) = delete;
  Persistence &operator=(const Persistence &) = delete;
  bool submit(std::filesystem::path file, uint64_t key,
              std::unique_ptr<const Snapshot> snapshot);
  void waitIdle(); // orderly shutdown/tests only, never frame publication
  void stop();     // drains and joins; later submissions may lazily restart
private:
  struct Request {
    std::filesystem::path file;
    uint64_t key = 0, charge = 0;
    std::unique_ptr<const Snapshot> snapshot;
  };
  void run();
  uint64_t budget, retained = 0;
  Charge retainedCharge;
  Writer writer;
  std::string label;
  std::mutex mutex;
  std::condition_variable wake, idle;
  Request pending;
  bool active = false, stopping = false;
  std::thread worker;
};
template <typename Snapshot> Persistence<Snapshot>::~Persistence() { stop(); }
template <typename Snapshot> void Persistence<Snapshot>::stop() {
  {
    std::lock_guard lock(mutex);
    stopping = true;
  }
  wake.notify_one();
  // Completed maps remain useful after a map change or renderer switch.
  // Orderly process shutdown drains them; no renderer/GPU lifetime is held.
  if (worker.joinable())
    worker.join();
  {
    std::lock_guard lock(mutex);
    stopping = false;
  }
}
template <typename Snapshot>
bool Persistence<Snapshot>::submit(std::filesystem::path file, uint64_t key,
                                   std::unique_ptr<const Snapshot> snapshot) {
  if (!snapshot)
    return false;
  const auto charge = retainedCharge(*snapshot);
  {
    std::lock_guard lock(mutex);
    if (stopping || pending.snapshot || charge > budget ||
        charge > budget - retained)
      return false;
    if (!worker.joinable())
      worker = std::thread([this] { run(); });
    retained += charge;
    pending = {std::move(file), key, charge, std::move(snapshot)};
  }
  wake.notify_one();
  return true;
}
template <typename Snapshot> void Persistence<Snapshot>::waitIdle() {
  std::unique_lock lock(mutex);
  idle.wait(lock, [&] { return !active && !pending.snapshot; });
}
template <typename Snapshot> void Persistence<Snapshot>::run() {
  for (;;) {
    Request request;
    {
      std::unique_lock lock(mutex);
      wake.wait(lock, [&] { return stopping || pending.snapshot; });
      if (!pending.snapshot)
        return;
      request = std::move(pending);
      active = true;
    }
    const auto started = std::chrono::steady_clock::now();
    bool saved = false;
    try {
      saved = writer(request.file, request.key, *request.snapshot);
      if (!saved)
        std::fprintf(stderr, "Background cache write failed; published "
                             "lighting remains active\n");
    } catch (...) {
      std::fprintf(stderr, "Background cache writer failed; published lighting "
                           "remains active\n");
    }
    std::fprintf(stderr,
                 "%s cache persistence: key=%llu retained_bytes=%llu "
                 "outcome=%s wall_ms=%.3f\n",
                 label.c_str(), static_cast<unsigned long long>(request.key),
                 static_cast<unsigned long long>(request.charge),
                 saved ? "saved" : "failed",
                 std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - started)
                     .count());
    request.snapshot
        .reset(); // release CPU storage before admitting another payload
    {
      std::lock_guard lock(mutex);
      retained -= request.charge;
      active = false;
    }
    idle.notify_all();
  }
}
} // namespace remix::forward::cache_io
