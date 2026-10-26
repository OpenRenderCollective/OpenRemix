#pragma once
#include <cstdlib>
#include <cstring>
#include <optional>

namespace remix::forward::bake {
  inline bool coarseTransferCpuReportsRequested() {
    static const bool requested = [] {
      const auto value = std::getenv("REMIX_DEBUG_COARSE_TRANSFER_CPU_REPORTS");
      return value && std::strcmp(value, "1") == 0;
    }();
    return requested;
  }
  inline bool coarseTransferPreparationRequested() {
    static const bool requested = [] {
      const auto value = std::getenv("REMIX_DEBUG_COARSE_TRANSFER_PREPARE");
      return value && std::strcmp(value, "1") == 0;
    }();
    return requested || coarseTransferCpuReportsRequested();
  }
  inline bool modelChartsRequested(bool enabled) {
    static const std::optional<bool> requested = []() -> std::optional<bool> {
      const auto value = std::getenv("REMIX_DEBUG_BAKE_MODEL_CHARTS");
      if (!value) return {};
      return std::strcmp(value, "1") == 0;
    }();
    return requested.value_or(enabled);
  }

  inline bool directionalProbeDepthRequested() {
    static const bool requested = [] {
      const auto value = std::getenv("REMIX_DEBUG_DIRECTIONAL_PROBE_DEPTH");
      return value && std::strcmp(value, "1") == 0;
    }();
    return requested;
  }
  // Process-level developer override. Publication and shader specialization
  // must agree; runtime table offsets still select complete allocation fallback.
  inline bool directionalFieldsRequested() {
    static const bool requested = [] {
      const auto value = std::getenv("REMIX_DEBUG_DIRECTIONAL_FIELDS");
      return !value || std::strcmp(value, "0") != 0;
    }();
    return requested;
  }
}
