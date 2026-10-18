#pragma once
#include "forward/bake/cache.h"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace remix::forward::bake::cache {
  // CPU-only immutable payloads outlive the renderer that published them. Disk
  // persistence never owns GPU objects or gates a frame/preparation ticket.
  class Persistence {
  public:
    using Writer = std::function<bool(const std::filesystem::path &, uint64_t, const View &)>;
    explicit Persistence(uint64_t byteBudget = 4ull << 30, Writer writer = {});
    ~Persistence();
    Persistence(const Persistence &) = delete;
    Persistence &operator=(const Persistence &) = delete;
    // At most one active and one pending snapshot. Admission is nonblocking;
    // optional persistence is skipped when either bound would be exceeded.
    bool submit(std::filesystem::path file, uint64_t key, std::unique_ptr<const Snapshot> snapshot);
    // Used by orderly shutdown and tests, never by frame publication.
    void waitIdle();
    // Drain and join before SDK unload. Later submissions may lazily restart.
    void stop();
    static uint64_t retainedCharge(const Snapshot &snapshot);

  private:
    struct Request {
      std::filesystem::path file;
      uint64_t key = 0, charge = 0;
      std::unique_ptr<const Snapshot> snapshot;
    };
    void run();
    uint64_t budget, retained = 0;
    Writer writer;
    std::mutex mutex;
    std::condition_variable wake, idle;
    Request pending;
    bool active = false, stopping = false;
    std::thread worker;
  };
  Persistence &persistence();
} // namespace remix::forward::bake::cache
