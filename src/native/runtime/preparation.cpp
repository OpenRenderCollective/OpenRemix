#include "runtime/preparation.h"
#include <algorithm>
#include <stdexcept>

namespace remix::pipeline {
  thread_local PreparationQueue *PreparationQueue::current = nullptr;
  thread_local std::vector<PreparationQueue::Job *> PreparationQueue::running;
  void PreparationQueue::Job::check() const {
    const auto value = state.load();
    if (value == State::Failed)
      std::rethrow_exception(failure);
    if (value != State::Complete)
      throw std::runtime_error("Preparation cancelled or incomplete");
  }
  PreparationQueue::PreparationQueue(unsigned count) {
    workers.reserve(std::max(count, 1u));
    workers.emplace_back([this] { run(true); });
    for (unsigned i = 1; i < count; ++i)
      workers.emplace_back([this] { run(false); });
  }
  PreparationQueue::~PreparationQueue() { stop(); }
  bool PreparationQueue::onWorker() const { return current == this; }
  PreparationQueue::Ticket PreparationQueue::request(const std::string &key, std::function<void()> work, Kind kind) {
    std::lock_guard lock(mutex);
    if (auto it = jobs.find(key); it != jobs.end())
      return it->second;
    auto job = std::make_shared<Job>();
    job->kind = kind;
    if (stopping)
      job->state = State::Cancelled;
    else {
      job->work = std::move(work);
      (kind == Kind::Background ? background : pending).push_back(job);
    }
    jobs.emplace(key, job);
    // Any worker may take a Parallel or Background job; only the first takes a
    // Serial one.
    if (kind == Kind::Serial)
      wake.notify_all();
    else
      wake.notify_one();
    return job;
  }
  void PreparationQueue::promote(const Ticket &job) {
    if (!job)
      return;
    std::lock_guard lock(mutex);
    if (job->kind != Kind::Background || job->state != State::Queued)
      return;
    const auto found = std::find(background.begin(), background.end(), job);
    if (found != background.end())
      background.erase(found);
    job->kind = Kind::Parallel;
    pending.push_back(job);
    wake.notify_one();
  }
  void PreparationQueue::cancel(const Ticket &job) {
    if (!job)
      return;
    std::function<void()> discarded;
    {
      std::lock_guard lock(mutex);
      job->cancelRequested = true;
      if (job->state == State::Queued) {
        discarded = std::move(job->work);
        job->state = State::Cancelled;
        finished.notify_all();
      }
    }
  }
  void PreparationQueue::stop() {
    std::vector<std::function<void()>> discarded;
    {
      std::lock_guard lock(mutex);
      stopping = true;
      for (auto &job : active)
        job->cancelRequested = true;
      for (auto *queue : {&pending, &background}) {
        for (auto &job : *queue) {
          if (job->state != State::Queued)
            continue;
          discarded.push_back(std::move(job->work));
          job->state = State::Cancelled;
        }
        queue->clear();
      }
      finished.notify_all();
    }
    wake.notify_all();
    for (auto &worker : workers)
      if (worker.joinable() && !onWorker())
        worker.join();
    // Captures may own SDK objects. Release them only after the executing SDK
    // call has returned, and without the queue mutex held.
    discarded.clear();
  }
  void PreparationQueue::finishOnWorker(const Ticket &job) {
    if (!onWorker())
      throw std::logic_error("Only the compiler worker can execute jobs");
    checkCancelled();
    execute(job);
    {
      // Claimed by another worker: its completion (or stop) publishes a state.
      std::unique_lock lock(mutex);
      finished.wait(lock, [&] { return job->ready(); });
    }
    job->check();
  }
  void PreparationQueue::checkCancelled() {
    if (!onWorker())
      return;
    std::lock_guard lock(mutex);
    for (const auto *job : running)
      if (job->cancelRequested)
        throw std::runtime_error("Preparation superseded or stopped");
  }
  void PreparationQueue::execute(const Ticket &job) {
    std::function<void()> work;
    {
      std::lock_guard lock(mutex);
      if (job->state != State::Queued)
        return;
      job->state = State::Running;
      active.push_back(job);
      foregroundActive += job->kind != Kind::Background;
      work = std::move(job->work);
    }
    const bool foreground = job->kind != Kind::Background;
    running.push_back(job.get());
    try {
      work();
      // Release captures before publishing completion (including device objects).
      work = {};
      job->state = State::Complete;
    } catch (...) {
      job->failure = std::current_exception();
      work = {};
      job->state = State::Failed;
    }
    running.pop_back();
    {
      std::lock_guard lock(mutex);
      active.erase(std::find(active.begin(), active.end(), job));
      foregroundActive -= foreground;
      finished.notify_all();
      if (!foregroundActive)
        wake.notify_all(); // A Serial job may be waiting for the helpers.
    }
  }
  void PreparationQueue::run(bool primary) {
    current = this;
    for (;;) {
      Ticket job;
      {
        std::unique_lock lock(mutex);
        // Jobs claimed inline or cancelled stay listed until a worker passes them.
        // A Serial job starts once every job requested before it has finished,
        // as with a single worker: helpers may still run earlier Parallel jobs.
        auto next = [&] {
          if (primary)
            return pending.empty() || pending.front()->kind == Kind::Parallel ||
                           pending.front()->state != State::Queued || !foregroundActive
                       ? pending.begin()
                       : pending.end();
          return std::find_if(pending.begin(), pending.end(), [](const Ticket &queued) {
            return queued->kind == Kind::Parallel || queued->state != State::Queued;
          });
        };
        // A Background job: for a helper with no Parallel job to take, or the
        // first worker with nothing queued at all (a waiting Serial job must
        // not wait for it too).
        // Two workers (one with few) stay out of Background work, so a
        // compile wanted now never waits for a prewarmed one to finish.
        auto takeBackground = [&] {
          const auto limit = std::max<size_t>(1, workers.size() > 2 ? workers.size() - 2 : 1);
          return !background.empty() && active.size() - foregroundActive < limit &&
                 (!primary || std::none_of(
                                  pending.begin(), pending.end(),
                                  [](const Ticket &queued) { return queued->state == State::Queued; }));
        };
        wake.wait(lock, [&] { return stopping || next() != pending.end() || takeBackground(); });
        if (stopping)
          break;
        if (const auto found = next(); found != pending.end()) {
          job = std::move(*found);
          pending.erase(found);
        } else {
          job = std::move(background.front());
          background.pop_front();
        }
      }
      execute(job);
    }
    current = nullptr;
  }
} // namespace remix::pipeline
