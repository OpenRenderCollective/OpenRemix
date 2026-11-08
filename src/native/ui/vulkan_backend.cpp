// Compile the pinned backend through our adapter without modifying vendor code.
// Immutable objects and graphics compilation share the device pipeline service.
#include "runtime/pipelines.h"
#define vkCreateShaderModule remix::pipeline::createShaderModule
#define vkDestroyShaderModule remix::pipeline::destroyShaderModule
#define vkCreateSampler remix::pipeline::createSampler
#define vkDestroySampler remix::pipeline::destroySampler
#define vkCreateDescriptorSetLayout remix::pipeline::createDescriptorSetLayout
#define vkDestroyDescriptorSetLayout remix::pipeline::destroyDescriptorSetLayout
#define vkCreatePipelineLayout remix::pipeline::createPipelineLayout
#define vkDestroyPipelineLayout remix::pipeline::destroyPipelineLayout
#define vkCreateGraphicsPipelines remix::pipeline::createGraphicsPipelines
#define vkDestroyPipeline remix::pipeline::destroyPipeline
#include "../../../third_party/imgui/backends/imgui_impl_vulkan.cpp"

namespace remix::ui {
  bool prepareVulkanPipeline(bool request) {
    auto *data = ImGui_ImplVulkan_GetBackendData();
    if (!data)
      return false;
    if (request && !data->Pipeline)
      ImGui_ImplVulkan_CreateMainPipeline(&data->VulkanInitInfo.PipelineInfoMain);
    return data->Pipeline != VK_NULL_HANDLE;
  }
} // namespace remix::ui
