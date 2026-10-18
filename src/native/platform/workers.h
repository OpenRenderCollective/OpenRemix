#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#define REMIX_WORKERS_MXCSR 1
#endif

namespace remix {
  // A few persistent threads for independent per-frame CPU work (for example
  // CPU skinning of separate instances). run() blocks; the calling thread takes
  // part, and every index runs exactly once. Results must not depend on which
  // thread runs an index, so callers stay output-identical to a serial loop.
  // Owners destroy the pool at Shutdown, not from a DLL detach path.
  class WorkerPool {
  public:
    static unsigned defaultThreads() {
      const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
      return std::min(4u, std::max(1u, hardware / 2));
    }
    explicit WorkerPool(unsigned threads = defaultThreads()) {
      for (unsigned i = 0; i < threads; ++i)
        m_threads.emplace_back([this] { work(); });
    }
    ~WorkerPool() {
      {
        std::lock_guard lock(m_mutex);
        m_stop = true;
      }
      m_wake.notify_all();
      for (auto &thread : m_threads)
        thread.join();
    }
    WorkerPool(const WorkerPool &) = delete;
    WorkerPool &operator=(const WorkerPool &) = delete;
    size_t threads() const { return m_threads.size() + 1; }

    void run(size_t count, const std::function<void(size_t)> &body) {
      if (!count)
        return;
      if (count == 1 || m_threads.empty()) {
        for (size_t i = 0; i < count; ++i)
          body(i);
        return;
      }
      {
        std::lock_guard lock(m_mutex);
        m_body = &body;
        m_count = count;
        m_next.store(0, std::memory_order_relaxed);
        m_active = m_threads.size();
        m_error = nullptr;
#if defined(REMIX_WORKERS_MXCSR)
        // Same floating-point modes (denormal flushing, rounding) as the caller.
        m_csr = _mm_getcsr();
#endif
        ++m_generation;
      }
      m_wake.notify_all();
      drain();
      std::unique_lock lock(m_mutex);
      m_done.wait(lock, [this] { return m_active == 0; });
      m_body = nullptr;
      if (m_error)
        std::rethrow_exception(std::exchange(m_error, nullptr));
    }

  private:
    void drain() {
      for (size_t i; (i = m_next.fetch_add(1, std::memory_order_relaxed)) < m_count;) {
        try {
          (*m_body)(i);
        } catch (...) {
          std::lock_guard lock(m_mutex);
          if (!m_error)
            m_error = std::current_exception();
          m_next.store(m_count, std::memory_order_relaxed);
        }
      }
    }
    void work() {
      uint64_t seen = 0;
      for (;;) {
        {
          std::unique_lock lock(m_mutex);
          m_wake.wait(lock, [&] { return m_stop || m_generation != seen; });
          if (m_stop)
            return;
          seen = m_generation;
#if defined(REMIX_WORKERS_MXCSR)
          _mm_setcsr(m_csr);
#endif
        }
        drain();
        std::lock_guard lock(m_mutex);
        if (--m_active == 0)
          m_done.notify_one();
      }
    }
    std::vector<std::thread> m_threads;
    std::mutex m_mutex;
    std::condition_variable m_wake, m_done;
    const std::function<void(size_t)> *m_body = nullptr;
    size_t m_count = 0, m_active = 0;
    std::atomic<size_t> m_next{0};
    uint64_t m_generation = 0;
    [[maybe_unused]] unsigned m_csr = 0; // x86 MXCSR only
    std::exception_ptr m_error;
    bool m_stop = false;
  };
} // namespace remix
