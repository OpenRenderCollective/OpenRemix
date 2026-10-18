#include "runtime/pipeline_executables.h"
#include <sstream>
#include <vector>

namespace remix::pipeline {
std::string executableStatistics(VkDevice device, VkPipeline pipeline) noexcept {
  try {
    const auto properties = reinterpret_cast<PFN_vkGetPipelineExecutablePropertiesKHR>(
        vkGetDeviceProcAddr(device, "vkGetPipelineExecutablePropertiesKHR"));
    const auto statistics = reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(
        vkGetDeviceProcAddr(device, "vkGetPipelineExecutableStatisticsKHR"));
    if (!properties || !statistics) return "executable statistics unavailable\n";
    const VkPipelineInfoKHR info{.sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, .pipeline = pipeline};
    uint32_t count = 0;
    if (properties(device, &info, &count, nullptr) != VK_SUCCESS || count > 128)
      return "executable property count unavailable\n";
    std::vector<VkPipelineExecutablePropertiesKHR> executables(count,
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
    if (properties(device, &info, &count, executables.data()) != VK_SUCCESS)
      return "executable properties unavailable\n";
    std::ostringstream out;
    for (uint32_t index = 0; index < count; ++index) {
      const auto &entry = executables[index];
      out << "executable " << index << " " << entry.name << ": " << entry.description
          << "; stages " << entry.stages << "; subgroup " << entry.subgroupSize << '\n';
      const VkPipelineExecutableInfoKHR executable{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, .pipeline = pipeline,
          .executableIndex = index};
      uint32_t size = 0;
      if (statistics(device, &executable, &size, nullptr) != VK_SUCCESS || size > 4096) {
        out << "statistics count unavailable\n";
        continue;
      }
      std::vector<VkPipelineExecutableStatisticKHR> values(size,
          {.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
      if (statistics(device, &executable, &size, values.data()) != VK_SUCCESS) {
        out << "statistics unavailable\n";
        continue;
      }
      for (uint32_t i = 0; i < size; ++i) {
        const auto &value = values[i];
        out << "  " << value.name << " = ";
        switch (value.format) {
        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: out << bool(value.value.b32); break;
        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: out << value.value.i64; break;
        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: out << value.value.u64; break;
        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR: out << value.value.f64; break;
        default: out << "unknown format"; break;
        }
        out << " (" << value.description << ")\n";
      }
    }
    return out.str();
  } catch (...) {
    return {};
  }
}
}
