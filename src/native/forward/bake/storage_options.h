#pragma once
#include <cstdlib>
#include <cstring>

namespace remix::forward::bake {
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
