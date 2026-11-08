#pragma once
#include "api/state.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace remix::test {
  inline thread_local bool lastFrameWaited = false;
  // Image/history tests count frames of the activated configuration. Keep
  // submitting and processing input while its replacement is being prepared.
  // Runtime responsiveness tests deliberately use unwrapped frame submission.
  template <class F> auto preparedFrame(F submit) {
    return [submit](auto &&...args) mutable {
      bool waited = false;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(600);
      for (;;) {
        submit(args...);
        const auto status = api::g_runtime.preparationStatus();
        if (!status.failure.empty()) {
          std::fprintf(stderr, "Preparation failed: %s\n", status.failure.c_str());
          std::exit(1);
        }
        if (!status.pending) {
          lastFrameWaited = waited;
          return;
        }
        waited = true;
        if (std::chrono::steady_clock::now() > deadline) {
          std::fprintf(stderr, "Configuration did not finish preparing\n");
          std::exit(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    };
  }
  template <class F> remixapi_ErrorCode requestScreenshot(remixapi_Interface &api, const char *path, F &&frame) {
    auto result = api.RequestPresentedScreenshot(path);
    if (result)
      return result;
    frame();
    // Preserve first-frame motion diagnostics when no preparation was needed.
    // If preparation drew a panel over that frame, recapture the activated path.
    if (lastFrameWaited) {
      result = api.RequestPresentedScreenshot(path);
      if (!result)
        frame();
    }
    return result;
  }
} // namespace remix::test
