#pragma once
#include <cstdint>

namespace remix::forward::probes {
// Shared CPU/GPU stage identity; no provider resources or scheduler dependencies.
enum class CaptureStep : uint8_t {
  PlanSites, ValidateSites, Fit, CheckGeometry, Select, Capture, Complete, None = 255
};
}
