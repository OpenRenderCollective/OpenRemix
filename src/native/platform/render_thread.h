#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#define REMIX_RENDER_THREAD_MXCSR 1
#endif

namespace remix {
  // One dedicated thread that runs the frames handed to it, one at a time
  // (asynchronous presentation). submit() starts a job at once on the thread
  // and returns; a job that is still running when the next submit() arrives
  // is waited for first, so at most one frame is in flight on the CPU side.
  //
  // Ownership: a job owns whatever it touches from the moment submit() hands
  // it over until wait() (or the next submit()) observes it finished. The
  // owner of this object serializes submit(), wait() and stop() (the runtime
  // calls them under the API mutex); a caller that observed idle() and does
  // not submit keeps exclusive access to the job's state for as long as it
  // holds that serialization.
  //
  // The job runs with the submitting thread's floating-point modes (MXCSR:
  // denormal flushing, rounding), as it would have on that thread. It must not
  // throw; if it does, the exception is kept for takeError().
  class RenderThread {
  public:
    RenderThread() = default;
    ~RenderThread() { stop(); }
    RenderThread(const RenderThread &) = delete;
    RenderThread &operator=(const RenderThread &) = delete;

    bool started() const { return m_thread.joinable(); }

    // Waits for the running job, then hands job to the thread (started on
    // first use). Returns the milliseconds spent waiting.
    double submit(std::function<void()> job) {
      const double waited = wait();
      if (!m_thread.joinable())
        m_thread = std::thread([this] { run(); });
      {
        std::lock_guard lock(m_mutex);
        m_job = std::move(job);
#if defined(REMIX_RENDER_THREAD_MXCSR)
        m_csr = _mm_getcsr();
#endif
        m_busy = true;
        ++m_submitted;
      }
      m_wake.notify_one();
      return waited;
    }

    // Blocks until no job runs. Returns the milliseconds spent waiting (zero
    // when idle).
    double wait() {
      std::unique_lock lock(m_mutex);
      if (!m_busy)
        return 0;
      const auto start = std::chrono::steady_clock::now();
      m_done.wait(lock, [this] { return !m_busy; });
      return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    bool idle() const {
      std::lock_guard lock(m_mutex);
      return !m_busy;
    }

    // Jobs handed over and jobs completed since construction.
    uint64_t submitted() const {
      std::lock_guard lock(m_mutex);
      return m_submitted;
    }
    uint64_t completed() const {
      std::lock_guard lock(m_mutex);
      return m_completed;
    }

    // An exception a job let escape, once.
    std::exception_ptr takeError() {
      std::lock_guard lock(m_mutex);
      return std::exchange(m_error, nullptr);
    }

    // Waits for the running job and joins the thread. The next submit()
    // starts a new one.
    void stop() {
      if (!m_thread.joinable())
        return;
      wait();
      {
        std::lock_guard lock(m_mutex);
        m_stop = true;
      }
      m_wake.notify_one();
      m_thread.join();
      std::lock_guard lock(m_mutex);
      m_stop = false;
    }

  private:
    void run() {
      for (;;) {
        std::function<void()> job;
        {
          std::unique_lock lock(m_mutex);
          m_wake.wait(lock, [this] { return m_stop || m_job; });
          if (!m_job)
            return; // Stopping while idle.
          job = std::move(m_job);
          m_job = nullptr;
#if defined(REMIX_RENDER_THREAD_MXCSR)
          _mm_setcsr(m_csr);
#endif
        }
        std::exception_ptr error;
        try {
          job();
        } catch (...) {
          error = std::current_exception();
        }
        // The job's captures are released before it counts as finished.
        job = nullptr;
        {
          std::lock_guard lock(m_mutex);
          if (error && !m_error)
            m_error = error;
          m_busy = false;
          ++m_completed;
        }
        m_done.notify_all();
      }
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_wake, m_done;
    std::function<void()> m_job;
    std::exception_ptr m_error;
    uint64_t m_submitted = 0, m_completed = 0;
    [[maybe_unused]] unsigned m_csr = 0; // x86 MXCSR only
    bool m_busy = false, m_stop = false;
    std::thread m_thread;
  };
} // namespace remix
