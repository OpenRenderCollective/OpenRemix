#pragma once
#include <string>
#include <vulkan/vulkan.h>

namespace remix::pipeline {
// Requires pipelineExecutableInfo and CAPTURE_STATISTICS at pipeline creation.
// Diagnostic queries must never turn a successful compile into a renderer failure.
std::string executableStatistics(VkDevice device, VkPipeline pipeline) noexcept;
}
