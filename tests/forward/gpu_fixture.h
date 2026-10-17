#pragma once
#include "forward/ray_scene.h"
#include "forward/resources.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace remix::forward::test {
  inline void require(bool value, const char *message) {
    if (!value) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
  inline std::atomic<uint32_t> validationErrors = 0;
  inline VKAPI_ATTR VkBool32 VKAPI_CALL validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                   VkDebugUtilsMessageTypeFlagsEXT,
                                                   const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++validationErrors;
      std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
  }
  // By default, a headless ray-query device with only its required optional
  // features. The core Vulkan 1.1 mode enables no optional features/extensions;
  // graphics tests can require a queue that also supports attachment rendering.
  struct Fixture {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    Fixture(bool hardware = true, bool graphics = false) {
      const bool enabled = std::getenv("REMIX_VULKAN_VALIDATION");
      const char *layer = "VK_LAYER_KHRONOS_validation";
      const char *debug = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
      const VkDebugUtilsMessengerCreateInfoEXT callback{
          .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
          .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
          .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
          .pfnUserCallback = validation};
      const VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                  .pApplicationName = "Forward+ hardware transport tests",
                                  .apiVersion = hardware ? VK_API_VERSION_1_2 : VK_API_VERSION_1_1};
      const VkInstanceCreateInfo info{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                      .pNext = enabled ? &callback : nullptr,
                                      .pApplicationInfo = &app,
                                      .enabledLayerCount = uint32_t(enabled),
                                      .ppEnabledLayerNames = &layer,
                                      .enabledExtensionCount = uint32_t(enabled),
                                      .ppEnabledExtensionNames = &debug};
      check(vkCreateInstance(&info, nullptr, &instance));
      if (enabled) {
        const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        check(create(instance, &callback, nullptr, &messenger));
      }
      uint32_t count = 0;
      check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
      std::vector<VkPhysicalDevice> devices(count);
      check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
      for (auto candidate : devices) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (hardware ? !RayScene::supported(candidate) : properties.apiVersion < VK_API_VERSION_1_1)
          continue;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());
        for (uint32_t i = 0; i < count; ++i)
          if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
              (!graphics || (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))) {
            physical = candidate;
            family = i;
            break;
          }
        if (physical)
          break;
      }
      if (!physical) {
        std::puts(hardware ? "SKIP: no Vulkan 1.2 hardware ray-query device" : "SKIP: no core Vulkan device");
        std::exit(77);
      }
      const float priority = 1;
      const VkDeviceQueueCreateInfo queueInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                              .queueFamilyIndex = family,
                                              .queueCount = 1,
                                              .pQueuePriorities = &priority};
      VkPhysicalDeviceRayQueryFeaturesKHR ray{
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, .rayQuery = VK_TRUE};
      VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
          .pNext = &ray,
          .accelerationStructure = VK_TRUE};
      VkPhysicalDeviceVulkan12Features vulkan12{.sType =
                                                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                .pNext = &acceleration,
                                                .bufferDeviceAddress = VK_TRUE};
      const char *extensions[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                                  VK_KHR_RAY_QUERY_EXTENSION_NAME,
                                  VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
      const VkDeviceCreateInfo deviceInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                          .pNext = hardware ? &vulkan12 : nullptr,
                                          .queueCreateInfoCount = 1,
                                          .pQueueCreateInfos = &queueInfo,
                                          .enabledExtensionCount = hardware ? 3u : 0u,
                                          .ppEnabledExtensionNames = hardware ? extensions : nullptr};
      check(vkCreateDevice(physical, &deviceInfo, nullptr, &device));
      vkGetDeviceQueue(device, family, 0, &queue);
      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(physical, &properties);
      std::printf("%s device: %s\n", hardware ? "Hardware ray-query" : "Core Vulkan", properties.deviceName);
      const VkCommandPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                             .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                             .queueFamilyIndex = family};
      check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool));
      const VkCommandBufferAllocateInfo commands{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                 .commandPool = pool,
                                                 .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                 .commandBufferCount = 1};
      check(vkAllocateCommandBuffers(device, &commands, &command));
    }
    ~Fixture() {
      if (device) {
        vkDeviceWaitIdle(device);
        vkDestroyCommandPool(device, pool, nullptr);
        vkDestroyDevice(device, nullptr);
      }
      if (messenger) {
        const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        destroy(instance, messenger, nullptr);
      }
      vkDestroyInstance(instance, nullptr);
    }
    void begin() {
      check(vkResetCommandBuffer(command, 0));
      const VkCommandBufferBeginInfo info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      check(vkBeginCommandBuffer(command, &info));
    }
    void finish() {
      check(vkEndCommandBuffer(command));
      const VkSubmitInfo info{
          .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
      check(vkQueueSubmit(queue, 1, &info, VK_NULL_HANDLE));
      check(vkQueueWaitIdle(queue));
    }
  };
} // namespace remix::forward::test
