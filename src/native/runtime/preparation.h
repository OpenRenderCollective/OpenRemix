#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace remix::pipeline {
  class LatestConfiguration {
  public:
    uint64_t request(const std::string &key) {
      if (!generation || requested != key) {
        requested = key;
        ++generation;
      }
      return generation;
    }
    bool accepts(uint64_t revision) const { return revision == generation; }

  private:
    uint64_t generation = 0;
    std::string requested;
  };
  // Also used by CPU tests: the compiler is an injected callback. No Vulkan or
  // API lock is involved in scheduling, polling or running a job.
  //
  // The first worker runs every job in request order. Additional workers only
  // take Parallel jobs (independent pipeline compiles), so Serial jobs such as
  // configuration preparation never run concurrently with each other, and a
  // Serial job starts only after every job requested before it has finished.
  // Background jobs (prewarmed pipeline variants) are taken only by a worker
  // with no other work to take, never hold a Serial job back, and become
  // Parallel when promoted (requested for use).
  class PreparationQueue {
  public:
    enum class State { Queued, Running, Complete, Failed, Cancelled };
    enum class Kind { Serial, Parallel, Background };
    struct Job {
      std::atomic<State> state{State::Queued};
      std::atomic<Kind> kind{Kind::Serial};
      std::atomic<bool> cancelRequested{false};
      std::exception_ptr failure;
      std::function<void()> work;
      bool ready() const { return state.load() >= State::Complete; }
      void check() const;
    };
    using Ticket = std::shared_ptr<Job>;
    explicit PreparationQueue(unsigned workers = 1);
    ~PreparationQueue();
    Ticket request(const std::string &key, std::function<void()> work, Kind kind = Kind::Serial);
    void cancel(const Ticket &job);
    // A queued Background job joins the Parallel jobs.
    void promote(const Ticket &job);
    void stop();
    bool onWorker() const;
    // A preparation job can need a pipeline already queued behind it. Claim
    // that job inline on the same worker, rather than waiting on ourselves; a
    // job another worker already runs is awaited.
    void finishOnWorker(const Ticket &job);
    void checkCancelled();

  private:
    void execute(const Ticket &job);
    void run(bool primary);
    std::mutex mutex;
    std::condition_variable wake, finished;
    std::deque<Ticket> pending, background;
    std::vector<Ticket> active;
    unsigned foregroundActive = 0; // of `active`, the Serial and Parallel jobs
    std::unordered_map<std::string, Ticket> jobs;
    bool stopping = false;
    std::vector<std::thread> workers;
    static thread_local PreparationQueue *current;
    // Jobs executing on this thread, innermost last (inline dependencies nest).
    static thread_local std::vector<Job *> running;
  };
} // namespace remix::pipeline
