/*
 * Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "runtime/runtime.h"
#include "upscale/ngx.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace remix {
  namespace {
    constexpr const char *kRequiredDeviceExtensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
#if defined(__APPLE__)
        "VK_KHR_portability_subset",
#endif
    };

    bool succeeded(VkResult result) { return result == VK_SUCCESS; }
  } // namespace

  remixapi_ErrorCode RemixNativeRuntime::createInstance() {
    std::vector<const char *> extensions =
        m_headless ? std::vector<const char *>{} : m_window.getVulkanInstanceExtensions();
    if (!m_headless && extensions.empty()) {
      return REMIXAPI_ERROR_CODE_HRESULT_DXVK_INSTANCE_EXTENSION_FAIL;
    }
#if defined(__APPLE__)
    extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif

    m_ngx = std::make_shared<upscale::Ngx>();
    const auto ngxExtensions = m_ngx->instanceExtensions();
    for (const auto &extension : ngxExtensions)
      if (std::none_of(extensions.begin(), extensions.end(), [&](auto p) { return extension == p; }))
        extensions.push_back(extension.c_str());
    // Debug labels name GPU timer passes in Nsight and RenderDoc (gpu_timers.h).
    uint32_t available = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &available, nullptr);
    std::vector<VkExtensionProperties> availableExtensions(available);
    vkEnumerateInstanceExtensionProperties(nullptr, &available, availableExtensions.data());
    if (std::any_of(availableExtensions.begin(), availableExtensions.end(),
                    [](const auto &e) { return std::strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0; }) &&
        std::none_of(extensions.begin(), extensions.end(),
                     [](auto p) { return std::strcmp(p, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0; }))
      extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    uint32_t loaderVersion = VK_API_VERSION_1_1;
    if (auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion")))
      enumerate(&loaderVersion);
    m_instanceApiVersion = std::min(loaderVersion, uint32_t(VK_API_VERSION_1_2));
    const VkApplicationInfo applicationInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Remix Shell",
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1001, 0),
        .pEngineName = "Remix Shell",
        .engineVersion = VK_MAKE_API_VERSION(0, 0, 1001, 0),
        .apiVersion = m_instanceApiVersion,
    };
    const char *validationLayer = "VK_LAYER_KHRONOS_validation";
    const bool validation = std::getenv("REMIX_VULKAN_VALIDATION") != nullptr;
    const VkInstanceCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
#if defined(__APPLE__)
        .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
#endif
        .pApplicationInfo = &applicationInfo,
        .enabledLayerCount = validation ? 1u : 0u,
        .ppEnabledLayerNames = validation ? &validationLayer : nullptr,
        .enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };
    return vkCreateInstance(&createInfo, nullptr, &m_instance) == VK_SUCCESS
               ? REMIXAPI_ERROR_CODE_SUCCESS
               : REMIXAPI_ERROR_CODE_HRESULT_VK_CREATE_INSTANCE_FAIL;
  }

  remixapi_ErrorCode RemixNativeRuntime::selectPhysicalDevice(QueueSelection &selection) const {
    uint32_t deviceCount = 0;
    if (!succeeded(vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr)) || deviceCount == 0) {
      return REMIXAPI_ERROR_CODE_HRESULT_NO_REQUIRED_GPU_FEATURES;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    if (!succeeded(vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data()))) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

    int bestScore = std::numeric_limits<int>::min();
    for (VkPhysicalDevice device : devices) {
      if (!m_headless && !supportsDeviceExtension(device, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        continue;
      }

      uint32_t familyCount = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
      std::vector<VkQueueFamilyProperties> families(familyCount);
      vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());

      for (uint32_t family = 0; family < familyCount; family++) {
        VkBool32 supportsPresent = m_headless;
        if (!m_headless)
          vkGetPhysicalDeviceSurfaceSupportKHR(device, family, m_surface, &supportsPresent);
        const VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT;
        if ((families[family].queueFlags & required) != required || !supportsPresent) {
          continue;
        }

        VkPhysicalDeviceProperties properties = {};
        vkGetPhysicalDeviceProperties(device, &properties);
        int score = (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 : 0) +
                    ((families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) ? 1 : 0);
        if (score > bestScore) {
          bestScore = score;
          selection.physicalDevice = device;
          selection.family = family;
        }
      }
    }

    return selection.physicalDevice != VK_NULL_HANDLE
               ? REMIXAPI_ERROR_CODE_SUCCESS
               : REMIXAPI_ERROR_CODE_HRESULT_GRAPHICS_QUEUE_FAMILY_MISSING;
  }

  remixapi_ErrorCode RemixNativeRuntime::createDevice(const QueueSelection &selection) {
    constexpr float kQueuePriority = 1.0f;
    const VkDeviceQueueCreateInfo queueInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = selection.family,
        .queueCount = 1,
        .pQueuePriorities = &kQueuePriority,
    };
    std::vector<const char *> extensions(std::begin(kRequiredDeviceExtensions),
                                         std::end(kRequiredDeviceExtensions));
#if defined(__linux__)
    if (m_headless) {
      extensions.clear();
      for (const char *extension :
           {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME}) {
        if (!supportsDeviceExtension(selection.physicalDevice, extension))
          return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
        extensions.push_back(extension);
      }
    }
#endif
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(selection.physicalDevice, &properties);
    VkPhysicalDeviceRayQueryFeaturesKHR ray = {.sType =
                                                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, .pNext = &ray};
    VkPhysicalDeviceVulkan12Features vulkan12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &acceleration};
    VkPhysicalDeviceVulkan11Features vulkan11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &vulkan12};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                          .pNext = &vulkan11};
    m_traceUnavailable.clear();
    auto missing = [&](bool supported, const char *name) {
      if (!supported) {
        if (!m_traceUnavailable.empty())
          m_traceUnavailable += ", ";
        m_traceUnavailable += name;
      }
    };
    const bool version12 =
        m_instanceApiVersion >= VK_API_VERSION_1_2 && properties.apiVersion >= VK_API_VERSION_1_2;
    missing(version12, "Vulkan 1.2");
    const char *traceExtensions[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                                     VK_KHR_RAY_QUERY_EXTENSION_NAME,
                                     VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
    bool extensionsSupported = true;
    for (const char *extension : traceExtensions) {
      const bool supported = supportsDeviceExtension(selection.physicalDevice, extension);
      missing(supported, extension);
      extensionsSupported &= supported;
    }
    uint32_t queueCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(selection.physicalDevice, &queueCount, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queueCount);
    vkGetPhysicalDeviceQueueFamilyProperties(selection.physicalDevice, &queueCount, queues.data());
    missing((queues[selection.family].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0,
            "graphics/compute/present queue");
    if (version12 && extensionsSupported) {
      vkGetPhysicalDeviceFeatures2(selection.physicalDevice, &features);
      missing(acceleration.accelerationStructure, "accelerationStructure");
      missing(ray.rayQuery, "rayQuery");
      missing(vulkan12.bufferDeviceAddress, "bufferDeviceAddress");
      missing(features.features.shaderInt64, "shaderInt64");
      missing(vulkan12.shaderSampledImageArrayNonUniformIndexing, "nonuniform sampled-image indexing");
      missing(properties.limits.maxPerStageDescriptorSamplers >= PathTracer::textureLimit &&
                  properties.limits.maxPerStageDescriptorSampledImages >= PathTracer::textureLimit &&
                  properties.limits.maxDescriptorSetSamplers >= PathTracer::textureLimit &&
                  properties.limits.maxDescriptorSetSampledImages >= PathTracer::textureLimit,
              "4096 sampled-image descriptors");
      missing(properties.limits.maxPerStageResources >= PathTracer::textureLimit + 14,
              "4110 compute-stage resources");
      missing(properties.limits.maxPerStageDescriptorStorageImages >= 10 &&
                  properties.limits.maxDescriptorSetStorageImages >= 10,
              "ten tracing storage images");
      missing(properties.limits.maxColorAttachments >= 6 &&
                  properties.limits.maxFragmentOutputAttachments >= 6,
              "six G-buffer color attachments");
      for (auto targetFormat : {VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
                                VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_SFLOAT}) {
        VkFormatProperties target;
        vkGetPhysicalDeviceFormatProperties(selection.physicalDevice, targetFormat, &target);
        const auto required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        missing((target.optimalTilingFeatures & required) == required, "G-buffer attachment/storage format");
      }
      VkFormatProperties stateFormat;
      vkGetPhysicalDeviceFormatProperties(selection.physicalDevice, VK_FORMAT_R32G32B32A32_UINT,
                                          &stateFormat);
      missing((stateFormat.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0,
              "RGBA32UI tracing state storage");
      VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationProperties = {
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
      VkPhysicalDeviceProperties2 tracingProperties = {
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &accelerationProperties};
      vkGetPhysicalDeviceProperties2(selection.physicalDevice, &tracingProperties);
      missing(accelerationProperties.maxPerStageDescriptorAccelerationStructures >= 2 &&
                  accelerationProperties.maxDescriptorSetAccelerationStructures >= 2,
              "two acceleration-structure descriptors");
      VkFormatProperties format;
      vkGetPhysicalDeviceFormatProperties(selection.physicalDevice, VK_FORMAT_R32G32B32A32_SFLOAT, &format);
      missing((format.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0,
              "RGBA32F storage images");
      missing((format.optimalTilingFeatures &
               (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)) ==
                  (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT),
              "RGBA32F sky attachment blending");
    }
    m_traceSupported = m_traceUnavailable.empty();
    m_denoiseCapabilities = denoise::capabilities(selection.physicalDevice);
    const bool sharcSupported = REMIX_HAS_SHARC && m_traceSupported && vulkan11.storageBuffer16BitAccess &&
                                vulkan12.shaderFloat16 && vulkan12.shaderBufferInt64Atomics;
    // The NUMOS sky's shaders use scalar block layout and storage images of
    // the extended formats, some declared without a format.
    VkPhysicalDeviceFeatures skyFeatures{};
    vkGetPhysicalDeviceFeatures(selection.physicalDevice, &skyFeatures);
    m_skyUnavailable = !m_traceSupported                                ? m_traceUnavailable
                       : !vulkan12.scalarBlockLayout                     ? "scalar block layout"
                       : !skyFeatures.shaderStorageImageExtendedFormats  ? "extended storage image formats"
                       : !skyFeatures.shaderStorageImageReadWithoutFormat ? "storage image reads without format"
                       : !skyFeatures.shaderStorageImageWriteWithoutFormat ? "storage image writes without format"
                                                                            : "";
    features.features = {};
    vulkan11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &vulkan12};
    vulkan12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &acceleration};
    acceleration = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
                    .pNext = &ray};
    ray = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    if (m_traceSupported) {
      extensions.insert(extensions.end(), std::begin(traceExtensions), std::end(traceExtensions));
      features.features.shaderInt64 = VK_TRUE;
      vulkan12.bufferDeviceAddress = VK_TRUE;
      vulkan12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
      acceleration.accelerationStructure = ray.rayQuery = VK_TRUE;
      if (sharcSupported) {
        vulkan11.storageBuffer16BitAccess = VK_TRUE;
        vulkan12.shaderFloat16 = vulkan12.shaderBufferInt64Atomics = VK_TRUE;
      }
      if (m_skyUnavailable.empty()) {
        vulkan12.scalarBlockLayout = VK_TRUE;
        features.features.shaderStorageImageExtendedFormats = VK_TRUE;
        features.features.shaderStorageImageReadWithoutFormat = VK_TRUE;
        features.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
      }
    }
    if (m_denoiseCapabilities.taaSupported) {
      features.features.shaderStorageImageExtendedFormats = VK_TRUE;
      VkPhysicalDeviceFeatures available{};
      vkGetPhysicalDeviceFeatures(selection.physicalDevice, &available);
      // FSR's output shader uses unformatted writes, independently of NRD;
      // its frame generation also reads the generated frame that way.
      features.features.shaderStorageImageWriteWithoutFormat = available.shaderStorageImageWriteWithoutFormat;
      features.features.shaderStorageImageReadWithoutFormat = available.shaderStorageImageReadWithoutFormat;
    }
    if (m_denoiseCapabilities.reblurShSupported) {
      features.features.shaderStorageImageReadWithoutFormat = VK_TRUE;
      features.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    }
    // BC sampling for "Keep BC textures compressed". The setting can change at
    // runtime, so the feature is enabled whenever supported; enabling it alone
    // changes no rendering.
    {
      VkPhysicalDeviceFeatures available{};
      vkGetPhysicalDeviceFeatures(selection.physicalDevice, &available);
      m_textureCompression = available.textureCompressionBC == VK_TRUE;
      features.features.textureCompressionBC = available.textureCompressionBC;
      // Material samplers filter anisotropically wherever supported (raster
      // and traced surfaces with UV gradients; see RasterRenderer::Gpu).
      features.features.samplerAnisotropy = available.samplerAnisotropy;
      // Line polygon mode for wireframe instances (remixapi_InstanceInfoWireframeEXT).
      features.features.fillModeNonSolid = available.fillModeNonSolid;
    }
    // Physical-device queries need only support, not device enablement.
    m_memoryBudget = m_instanceApiVersion >= VK_API_VERSION_1_1 &&
                     supportsDeviceExtension(selection.physicalDevice, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (!m_traceSupported)
      features.pNext = nullptr;
    const auto ngxExtensions = m_ngx->deviceExtensions(m_instance, selection.physicalDevice);
    if (m_ngx->capabilities().sdk) {
      // NGX initialization can create its own shaders even when the renderer's
      // temporal descriptor limits are unavailable. Enable supported image
      // access features independently of NRD/FSR capability checks.
      VkPhysicalDeviceFeatures available{};
      vkGetPhysicalDeviceFeatures(selection.physicalDevice, &available);
      features.features.shaderStorageImageExtendedFormats = available.shaderStorageImageExtendedFormats;
      features.features.shaderStorageImageReadWithoutFormat = available.shaderStorageImageReadWithoutFormat;
      features.features.shaderStorageImageWriteWithoutFormat = available.shaderStorageImageWriteWithoutFormat;
    }
    for (const auto &extension : ngxExtensions)
      if (std::none_of(extensions.begin(), extensions.end(), [&](auto p) { return extension == p; }))
        extensions.push_back(extension.c_str());
    const bool pipelineFeedback = supportsDeviceExtension(
        selection.physicalDevice,
        VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME);
    if (pipelineFeedback)
      extensions.push_back(VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME);
    // NVIDIA Reflex. Its sleep is signalled on a timeline semaphore (core in
    // Vulkan 1.2, enabled through the 1.2 features the tracer's chain holds).
    m_lowLatencySupported = !m_headless && m_traceSupported && m_instanceApiVersion >= VK_API_VERSION_1_2 &&
                            supportsDeviceExtension(selection.physicalDevice, VK_NV_LOW_LATENCY_2_EXTENSION_NAME);
    if (m_lowLatencySupported) {
      vulkan12.timelineSemaphore = VK_TRUE;
      if (std::none_of(extensions.begin(), extensions.end(),
                       [](auto p) { return std::strcmp(p, VK_NV_LOW_LATENCY_2_EXTENSION_NAME) == 0; }))
        extensions.push_back(VK_NV_LOW_LATENCY_2_EXTENSION_NAME);
    }
    // Present metering (frame generation): the driver spaces a frame's
    // generated and rendered images itself (presentImages).
    VkPhysicalDevicePresentMeteringFeaturesNV metering = {
        .sType = presentMeteringFeaturesType};
    m_presentMeteringSupported = false;
    if (!m_headless && m_instanceApiVersion >= VK_API_VERSION_1_1 &&
        supportsDeviceExtension(selection.physicalDevice, VK_NV_PRESENT_METERING_EXTENSION_NAME)) {
      VkPhysicalDeviceFeatures2 available = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                             .pNext = &metering};
      vkGetPhysicalDeviceFeatures2(selection.physicalDevice, &available);
      if (metering.presentMetering) {
        m_presentMeteringSupported = true;
        extensions.push_back(VK_NV_PRESENT_METERING_EXTENSION_NAME);
        metering.pNext = features.pNext;
        features.pNext = &metering;
      }
    }
    const VkDeviceCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueInfo,
        .enabledExtensionCount = uint32_t(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };
    if (!succeeded(vkCreateDevice(selection.physicalDevice, &createInfo, nullptr, &m_device))) {
      return REMIXAPI_ERROR_CODE_HRESULT_VK_CREATE_DEVICE_FAIL;
    }

    m_ngx->setDevice(m_instance, selection.physicalDevice, m_device);
    m_physicalDevice = selection.physicalDevice;
    vkGetPhysicalDeviceProperties(m_physicalDevice, &m_properties);
    m_queueFamily = selection.family;
    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);
    if (m_lowLatencySupported) {
      m_setLatencySleepMode =
          reinterpret_cast<PFN_vkSetLatencySleepModeNV>(vkGetDeviceProcAddr(m_device, "vkSetLatencySleepModeNV"));
      m_latencySleep = reinterpret_cast<PFN_vkLatencySleepNV>(vkGetDeviceProcAddr(m_device, "vkLatencySleepNV"));
      m_setLatencyMarker =
          reinterpret_cast<PFN_vkSetLatencyMarkerNV>(vkGetDeviceProcAddr(m_device, "vkSetLatencyMarkerNV"));
      const VkSemaphoreTypeCreateInfo timeline{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                               .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
                                               .initialValue = 0};
      const VkSemaphoreCreateInfo semaphore{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &timeline};
      m_latencyValue = 0;
      m_lowLatencySupported = m_setLatencySleepMode && m_latencySleep && m_setLatencyMarker &&
                              vkCreateSemaphore(m_device, &semaphore, nullptr, &m_latencySemaphore) == VK_SUCCESS;
    }
    m_gpuTimers.initialize(m_physicalDevice, m_device, m_queueFamily);
    m_pipelines = std::make_unique<pipeline::Service>(m_device, m_properties,
                                                      pipelineFeedback);
    m_hostPasses.initialize(m_physicalDevice, m_device);
    // Ray queries are enabled with tracing.
    m_particles = std::make_unique<particles::Renderer>(m_physicalDevice, m_device, m_traceSupported);
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  bool RemixNativeRuntime::supportsDeviceExtension(VkPhysicalDevice device, const char *name) const {
    uint32_t count = 0;
    if (!succeeded(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr))) {
      return false;
    }
    std::vector<VkExtensionProperties> extensions(count);
    if (!succeeded(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data()))) {
      return false;
    }
    return std::any_of(extensions.begin(), extensions.end(), [name](const auto &extension) {
      return std::strcmp(extension.extensionName, name) == 0;
    });
  }
} // namespace remix
