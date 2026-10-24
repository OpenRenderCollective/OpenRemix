#include "upscale/ngx.h"
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#if REMIX_HAS_DLSS
#include <nvsdk_ngx_helpers_vk.h>
// RR helpers depend on declarations in the SR helpers above.
#include <nvsdk_ngx_helpers_dlssd_vk.h>
#include <nvsdk_ngx_helpers_dlssg_vk.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

namespace remix::upscale {
namespace {
std::recursive_mutex ngxMutex;
}

namespace {
[[maybe_unused]] void unavailable() { throw std::runtime_error("DLSS SDK unavailable in this build"); }
#if REMIX_HAS_DLSS
constexpr const char *project = "1c6bfafe-5047-4dba-8dfa-70bb55c5ae16";
void check(NVSDK_NGX_Result r, const char *operation) {
  if (NVSDK_NGX_FAILED(r))
    throw std::runtime_error(std::string(operation) + " failed (NGX " + std::to_string(uint32_t(r)) + ")");
}
void vkCheck(VkResult r) {
  if (r != VK_SUCCESS)
    throw r;
}
NVSDK_NGX_PerfQuality_Value preset(uint32_t quality) {
  const NVSDK_NGX_PerfQuality_Value values[] = {
      NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_MaxQuality,
      NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf,
      NVSDK_NGX_PerfQuality_Value_UltraPerformance};
  return values[std::min(quality, 4u)];
}
std::filesystem::path moduleDirectory() {
#ifdef _WIN32
  HMODULE module{};
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&moduleDirectory), &module))
    throw std::runtime_error("DLSS module path unavailable");
  std::wstring path(32768, L'\0');
  const auto size = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
  if (!size || size == path.size())
    throw std::runtime_error("DLSS module path unavailable");
  path.resize(size);
  return std::filesystem::path(path).parent_path();
#else
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void *>(&moduleDirectory), &info))
    throw std::runtime_error("DLSS module path unavailable");
  return std::filesystem::absolute(info.dli_fname).parent_path();
#endif
}
bool fault(const char *name) {
  const char *value = std::getenv("REMIX_NGX_FAULT");
  return value && std::string(value) == name;
}
#endif
} // namespace
struct Ngx::Impl {
  NgxCapabilities caps;
#if REMIX_HAS_DLSS
  VkDevice device{};
  // Features by index: 0 Super Resolution, 1 Ray Reconstruction, 2 Frame Generation.
  static constexpr unsigned featureCount = 3;
  bool initialized{}, attempted{}, instanceReady[featureCount]{}, deviceReady[featureCount]{};
  NVSDK_NGX_Parameter *parameters{};
  // cnnLibraries: Ray Reconstruction's CNN model (an older release's RR
  // library alone), searched first while that model is loaded.
  std::filesystem::path libraries, cnnLibraries;
  std::wstring libraryPath, cnnPath, cachePath;
  const wchar_t *paths[2]{};
  NVSDK_NGX_FeatureCommonInfo common{};
  bool cnnLoaded{};
  // Features created and not yet released: NGX restarts only without any.
  unsigned liveFeatures{};
  Impl() {
    caps.sdk = true;
    try {
      libraries = moduleDirectory() / "dlss" / "310.9.1";
      // Meson's development artifacts also live in an isolated adjacent directory.
      if (!std::filesystem::is_directory(libraries))
        libraries = moduleDirectory() / "upscale";
      libraryPath = libraries.wstring();
      cnnLibraries = moduleDirectory() / "dlss" / "310.2.1";
      if (!std::filesystem::is_directory(cnnLibraries))
        cnnLibraries = moduleDirectory() / "upscale" / "cnn";
      cnnPath = cnnLibraries.wstring();
      search(false);
      char *cache = SDL_GetPrefPath("openremix", "ngx-310.9.1");
      if (!cache)
        throw std::runtime_error("Writable NGX cache unavailable");
      cachePath = std::filesystem::path(cache).wstring();
      SDL_free(cache);
    } catch (const std::exception &e) {
      caps.reason = e.what();
    }
  }
  // NGX loads each feature's library from the first path that has it.
  void search(bool cnn) {
    paths[0] = cnn ? cnnPath.c_str() : libraryPath.c_str();
    paths[1] = libraryPath.c_str();
    common.PathListInfo.Path = paths;
    common.PathListInfo.Length = cnn ? 2 : 1;
  }
  bool cnnPackaged() const {
    std::error_code error;
#ifdef _WIN32
    return std::filesystem::is_regular_file(cnnLibraries / "nvngx_dlssd.dll", error);
#else
    return std::filesystem::is_regular_file(cnnLibraries / "libnvidia-ngx-dlssd.so.310.2.1", error);
#endif
  }
  bool packaged(unsigned feature) const {
    std::error_code error;
#ifdef _WIN32
    static constexpr const char *names[featureCount] = {"nvngx_dlss.dll", "nvngx_dlssd.dll", "nvngx_dlssg.dll"};
#else
    static constexpr const char *names[featureCount] = {"libnvidia-ngx-dlss.so.310.9.1",
                                                        "libnvidia-ngx-dlssd.so.310.9.1",
                                                        "libnvidia-ngx-dlssg.so.310.9.1"};
#endif
    return std::filesystem::is_regular_file(libraries / names[feature], error);
  }
  NVSDK_NGX_FeatureDiscoveryInfo discovery(unsigned feature) const {
    NVSDK_NGX_FeatureDiscoveryInfo result{};
    result.SDKVersion = NVSDK_NGX_Version_API;
    result.FeatureID = feature == 2   ? NVSDK_NGX_Feature_FrameGeneration
                       : feature == 1 ? NVSDK_NGX_Feature_RayReconstruction
                                      : NVSDK_NGX_Feature_SuperSampling;
    result.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    result.Identifier.v.ProjectDesc = {project, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "0.1003"};
    result.ApplicationDataPath = cachePath.c_str();
    result.FeatureInfo = &common;
    return result;
  }
  ~Impl() {
    std::lock_guard lock(ngxMutex);
    if (parameters)
      NVSDK_NGX_VULKAN_DestroyParameters(parameters);
    if (initialized)
      NVSDK_NGX_VULKAN_Shutdown1(device);
  }
#endif
};
Ngx::Ngx() : m(std::make_unique<Impl>()) {
#if !REMIX_HAS_DLSS
  m->caps.reason = "DLSS SDK disabled at build time";
#endif
}
Ngx::~Ngx() = default;
NgxCapabilities Ngx::capabilities() const {
  std::lock_guard lock(capabilitiesMutex);
  return deviceAssigned ? publishedCapabilities : m->caps;
}
void Ngx::setDevice(VkInstance value, VkPhysicalDevice gpu, VkDevice logical) {
  instance = value;
  physical = gpu;
  device = logical;
  std::lock_guard lock(capabilitiesMutex);
  publishedCapabilities = m->caps;
  if (publishedCapabilities.reason.empty())
    publishedCapabilities.reason = "DLSS will initialize when requested";
  deviceAssigned = true;
}
void Ngx::initializeDevice() { initialize(instance, physical, device); }
std::vector<std::string> Ngx::instanceExtensions() {
  std::vector<std::string> result;
#if REMIX_HAS_DLSS
  uint32_t count = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> supported(count);
  vkEnumerateInstanceExtensionProperties(nullptr, &count, supported.data());
  for (unsigned feature = 0; feature < Impl::featureCount; ++feature) {
    if (!m->caps.reason.empty() || !m->packaged(feature))
      continue;
    auto info = m->discovery(feature);
    VkExtensionProperties *properties{};
    auto status = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&info, &count, &properties);
    if (NVSDK_NGX_FAILED(status))
      continue;
    bool valid = true;
    for (uint32_t i = 0; i < count; ++i)
      valid &= std::any_of(supported.begin(), supported.end(), [&](auto &p) {
        return std::string(p.extensionName) == properties[i].extensionName;
      });
    m->instanceReady[feature] = valid;
    if (valid)
      for (uint32_t i = 0; i < count; ++i)
        result.emplace_back(properties[i].extensionName);
  }
#endif
  return result;
}
std::vector<std::string> Ngx::deviceExtensions([[maybe_unused]] VkInstance instance,
                                               [[maybe_unused]] VkPhysicalDevice physical) {
  std::vector<std::string> result;
#if REMIX_HAS_DLSS
  VkPhysicalDeviceProperties gpu{};
  vkGetPhysicalDeviceProperties(physical, &gpu);
  if (gpu.vendorID != 0x10de || fault("unavailable"))
    return result;
  uint32_t count = 0;
  vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> supported(count);
  vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, supported.data());
  for (unsigned feature = 0; feature < Impl::featureCount; ++feature) {
    if (!m->instanceReady[feature])
      continue;
    auto info = m->discovery(feature);
    VkExtensionProperties *properties{};
    auto status = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, physical, &info, &count,
                                                                         &properties);
    if (NVSDK_NGX_FAILED(status))
      continue;
    bool valid = true;
    for (uint32_t i = 0; i < count; ++i)
      valid &= std::any_of(supported.begin(), supported.end(), [&](auto &p) {
        return std::string(p.extensionName) == properties[i].extensionName;
      });
    m->deviceReady[feature] = valid;
    if (valid)
      for (uint32_t i = 0; i < count; ++i)
        result.emplace_back(properties[i].extensionName);
  }
#endif
  return result;
}
void Ngx::initialize([[maybe_unused]] VkInstance instance, [[maybe_unused]] VkPhysicalDevice physical,
                     [[maybe_unused]] VkDevice device) {
  std::lock_guard lock(ngxMutex);
#if REMIX_HAS_DLSS
  if (m->attempted)
    return;
  m->attempted = true;
  m->device = device;
  start(instance, physical, device);
#endif
  std::lock_guard publish(capabilitiesMutex);
  m->caps.initializationComplete = true;
  publishedCapabilities = m->caps;
}
#if REMIX_HAS_DLSS
void Ngx::start(VkInstance instance, VkPhysicalDevice physical, VkDevice device) {
  m->caps.sr = m->caps.rr = m->caps.fg = false;
  m->caps.reason.clear();
  m->caps.fgReason.clear();
  try {
    if (!m->deviceReady[0] && !m->deviceReady[1] && !m->deviceReady[2])
      throw std::runtime_error("Packaged DLSS libraries or required Vulkan extensions unavailable");
    if (fault("init"))
      throw std::runtime_error("Injected NGX initialization failure");
    check(NVSDK_NGX_VULKAN_Init_with_ProjectID(project, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "0.1003",
                                               m->cachePath.c_str(), instance, physical, device,
                                               vkGetInstanceProcAddr, vkGetDeviceProcAddr, &m->common),
          "NGX initialization");
    m->initialized = true;
    check(NVSDK_NGX_VULKAN_GetCapabilityParameters(&m->parameters), "NGX capabilities");
    int available = 0;
    m->parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    m->caps.sr = m->deviceReady[0] && available && !fault("sr-unavailable");
    available = 0;
    m->parameters->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &available);
    m->caps.rr = m->deviceReady[1] && available && !fault("rr-unavailable");
    if (!m->caps.rr)
      m->caps.reason = "DLSS Ray Reconstruction unavailable";
    if (!m->caps.sr && m->caps.rr)
      m->caps.reason = "DLSS Super Resolution unavailable";
    if (!m->caps.sr && !m->caps.rr)
      m->caps.reason = "DLSS SR and Ray Reconstruction unavailable";
    // Frame Generation: its own library, extensions, GPU and driver floor.
    available = 0;
    m->parameters->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &available);
    m->caps.fg = m->deviceReady[2] && available && !fault("fg-unavailable");
    if (m->caps.fg) {
      unsigned int generated = 0;
      m->parameters->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &generated);
      m->caps.fgMultiFrameMax = std::max(generated, 1u);
    } else if (!m->packaged(2)) {
      m->caps.fgReason = "nvngx_dlssg library not packaged";
    } else if (!m->deviceReady[2]) {
      m->caps.fgReason = "Vulkan extensions required by DLSS Frame Generation unavailable";
    } else {
      int updateDriver = 0, initResult = 0;
      unsigned int major = 0, minor = 0;
      m->parameters->Get(NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, &updateDriver);
      m->parameters->Get(NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMajor, &major);
      m->parameters->Get(NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMinor, &minor);
      m->parameters->Get(NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult, &initResult);
      m->caps.fgReason = updateDriver ? "DLSS Frame Generation needs driver " + std::to_string(major) + "." +
                                            std::to_string(minor) + " or newer"
                                      : "DLSS Frame Generation unsupported on this GPU (NGX " +
                                            std::to_string(uint32_t(initResult)) + ")";
    }
  } catch (const std::exception &e) {
    m->caps.reason = e.what();
    m->caps.fgReason = e.what();
  }
  m->caps.rrCnnPackaged = m->cnnPackaged();
  m->caps.rrCnn = m->cnnLoaded;
}
#endif
bool Ngx::useRayReconstructionModel([[maybe_unused]] bool cnn, std::string &reason) {
  std::lock_guard lock(ngxMutex);
#if REMIX_HAS_DLSS
  if (!m->initialized || cnn == m->cnnLoaded)
    return cnn == m->cnnLoaded;
  if (cnn && !m->cnnPackaged()) {
    reason = "The CNN Ray Reconstruction library (DLSS 310.2.1) is not packaged";
    return false;
  }
  if (m->liveFeatures) {
    reason = "The Ray Reconstruction model changes while no other DLSS feature is in use (turn frame generation off)";
    return false;
  }
  // NGX loads a feature's library once: restart it with the other search order.
  if (m->parameters)
    NVSDK_NGX_VULKAN_DestroyParameters(m->parameters);
  m->parameters = nullptr;
  NVSDK_NGX_VULKAN_Shutdown1(m->device);
  m->initialized = false;
  m->search(cnn);
  m->cnnLoaded = cnn;
  start(instance, physical, m->device);
  if (!m->caps.rr && cnn) {
    reason = "The CNN Ray Reconstruction model is unavailable (" + m->caps.reason + ")";
    if (m->parameters)
      NVSDK_NGX_VULKAN_DestroyParameters(m->parameters);
    m->parameters = nullptr;
    if (m->initialized)
      NVSDK_NGX_VULKAN_Shutdown1(m->device);
    m->initialized = false;
    m->search(false);
    m->cnnLoaded = false;
    start(instance, physical, m->device);
  }
  m->caps.initializationComplete = true;
  std::lock_guard publish(capabilitiesMutex);
  publishedCapabilities = m->caps;
  return cnn == m->cnnLoaded;
#else
  reason = "DLSS SDK disabled at build time";
  return !cnn;
#endif
}
VkExtent2D Ngx::renderSize([[maybe_unused]] VkExtent2D display, [[maybe_unused]] uint32_t quality,
                           [[maybe_unused]] bool rr) {
  std::lock_guard lock(ngxMutex);
#if REMIX_HAS_DLSS
  if (!(rr ? m->caps.rr : m->caps.sr))
    throw std::runtime_error(m->caps.reason);
  // Vulkan headers omit the RR optimal-settings helper. Invoke the same SDK callback.
  void *callback{};
  auto *p = m->parameters;
  check(p->Get(rr ? NVSDK_NGX_Parameter_DLSSDOptimalSettingsCallback
                  : NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback,
               &callback),
        "NGX optimal settings callback");
  if (!callback)
    throw std::runtime_error("NGX optimal settings callback missing");
  p->Set(NVSDK_NGX_Parameter_Width, display.width);
  p->Set(NVSDK_NGX_Parameter_Height, display.height);
  p->Set(NVSDK_NGX_Parameter_PerfQualityValue, int(preset(quality)));
  p->Set(NVSDK_NGX_Parameter_RTXValue, 0);
  check(reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback>(callback)(p), "NGX optimal settings");
  VkExtent2D result{};
  check(p->Get(NVSDK_NGX_Parameter_OutWidth, &result.width), "NGX render width");
  check(p->Get(NVSDK_NGX_Parameter_OutHeight, &result.height), "NGX render height");
  if (!result.width || !result.height || result.width > display.width || result.height > display.height)
    throw std::runtime_error("Invalid NGX render dimensions");
  return result;
#else
  unavailable();
  return {};
#endif
}
struct NgxFeature::Impl {
#if REMIX_HAS_DLSS
  std::shared_ptr<Ngx> context;
  NVSDK_NGX_Parameter *parameters{};
  NVSDK_NGX_Handle *feature{};
  VkExtent2D render{}, display{};
  bool rr{};
  VkCommandPool initializationPool{};
  VkCommandBuffer initializationCommand{};
  VkFence initializationFence{};
  bool initializationSubmitted = false;
  ~Impl() {
    std::lock_guard lock(ngxMutex);
    if (context) {
      const auto device = context->m->device;
      if (initializationFence)
        vkDestroyFence(device, initializationFence, nullptr);
      if (initializationPool)
        vkDestroyCommandPool(device, initializationPool, nullptr);
    }
    if (feature) {
      NVSDK_NGX_VULKAN_ReleaseFeature(feature);
      --context->m->liveFeatures;
    }
    if (parameters)
      NVSDK_NGX_VULKAN_DestroyParameters(parameters);
  }
#endif
};
NgxFeature::NgxFeature() : m(std::make_unique<Impl>()) {}
NgxFeature::~NgxFeature() = default;
void NgxFeature::initialize([[maybe_unused]] std::shared_ptr<Ngx> context, [[maybe_unused]] uint32_t family,
                            [[maybe_unused]] VkExtent2D render, [[maybe_unused]] VkExtent2D display,
                            [[maybe_unused]] uint32_t quality, [[maybe_unused]] bool rr,
                            [[maybe_unused]] uint32_t rrPreset) {
  std::lock_guard lock(ngxMutex);
#if REMIX_HAS_DLSS
  m->context = std::move(context);
  m->render = render;
  m->display = display;
  m->rr = rr;
  if (fault(rr ? "rr-create" : "sr-create"))
    throw std::runtime_error("Injected NGX feature creation failure");
  auto device = m->context->m->device;
  check(NVSDK_NGX_VULKAN_AllocateParameters(&m->parameters), "NGX parameters");
  VkCommandPool pool{};
  VkCommandBuffer command{};
  VkFence fence{};
  try {
    VkCommandPoolCreateInfo pi{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                               .queueFamilyIndex = family};
    vkCheck(vkCreateCommandPool(device, &pi, nullptr, &pool));
    VkCommandBufferAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                   .commandPool = pool,
                                   .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                   .commandBufferCount = 1};
    vkCheck(vkAllocateCommandBuffers(device, &ai, &command));
    VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkCheck(vkBeginCommandBuffer(command, &bi));
    // HDR input is scene radiance before the runtime's exposure; the exposure
    // texture stays 1. DLSS's history rejection works on exposed color, so it
    // computes its own exposure: with 1, dark scenes look black to it and
    // stale history (a moving view model's outline) survives as ghosts.
    const int flags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                      NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    if (rr) {
      // The caller's preset (rrPreset, 0: E). SDK 310.9.1 preset F
      // records uninitialized internal image reads and transfer hazards on the
      // Linux 615.71.09 driver; preset E passes sync validation on Turing and
      // uses the same input and sampling contract.
      for (const char *key : {NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA,
                              NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality,
                              NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced,
                              NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance,
                              NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance})
        m->parameters->Set(key, int(rrPreset ? rrPreset : uint32_t(NVSDK_NGX_RayReconstruction_Hint_Render_Preset_E)));
      NVSDK_NGX_DLSSD_Create_Params desc{};
      desc.InWidth = render.width;
      desc.InHeight = render.height;
      desc.InTargetWidth = display.width;
      desc.InTargetHeight = display.height;
      desc.InPerfQualityValue = preset(quality);
      desc.InFeatureCreateFlags = flags;
      desc.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
      desc.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
      desc.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
      check(NGX_VULKAN_CREATE_DLSSD_EXT1(device, command, 1, 1, &m->feature, m->parameters, &desc),
            "DLSS RR creation");
      ++m->context->m->liveFeatures;
    } else {
      // Keep SR on the validated K model for every quality mode; L/M have
      // the same internal uninitialized-image issue on the tested driver.
      for (const char *key : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                              NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                              NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                              NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                              NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance})
        m->parameters->Set(key, int(NVSDK_NGX_DLSS_Hint_Render_Preset_K));
      NVSDK_NGX_DLSS_Create_Params desc{};
      desc.Feature = {render.width, render.height, display.width, display.height, preset(quality)};
      desc.InFeatureCreateFlags = flags;
      check(NGX_VULKAN_CREATE_DLSS_EXT1(device, command, 1, 1, &m->feature, m->parameters, &desc),
            "DLSS SR creation");
      ++m->context->m->liveFeatures;
    }
    vkCheck(vkEndCommandBuffer(command));
    VkFenceCreateInfo fi{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCheck(vkCreateFence(device, &fi, nullptr, &fence));
    m->initializationPool = pool;
    m->initializationCommand = command;
    m->initializationFence = fence;

  } catch (...) {
    if (fence)
      vkDestroyFence(device, fence, nullptr);
    if (pool)
      vkDestroyCommandPool(device, pool, nullptr);
    throw;
  }

#else
  unavailable();
#endif
}
bool NgxFeature::finishInitialization(VkQueue queue) {
#if REMIX_HAS_DLSS
  if (!m->initializationPool)
    return true;
  const auto device = m->context->m->device;
  if (!m->initializationSubmitted) {
    VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1,
                        .pCommandBuffers = &m->initializationCommand};
    vkCheck(vkQueueSubmit(queue, 1, &submit, m->initializationFence));
    m->initializationSubmitted = true;
  }
  const auto status = vkGetFenceStatus(device, m->initializationFence);
  if (status == VK_NOT_READY)
    return false;
  vkCheck(status);
  vkDestroyFence(device, m->initializationFence, nullptr);
  vkDestroyCommandPool(device, m->initializationPool, nullptr);
  m->initializationFence = VK_NULL_HANDLE;
  m->initializationPool = VK_NULL_HANDLE;
#else
  (void)queue;
#endif
  return true;
}
uint64_t NgxFeature::memoryBytes() const {
  std::lock_guard lock(ngxMutex);
#if REMIX_HAS_DLSS
  if (!m->context)
    return 0;
  auto *p = m->context->m->parameters;
  void *callback{};
  unsigned long long bytes = 0;
  p->Get(m->rr ? NVSDK_NGX_Parameter_DLSSDGetStatsCallback : NVSDK_NGX_Parameter_DLSSGetStatsCallback,
         &callback);
  if (callback && NVSDK_NGX_SUCCEED(reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetStatsCallback>(callback)(p)))
    p->Get(NVSDK_NGX_Parameter_SizeInBytes, &bytes);
  return bytes;
#else
  return 0;
#endif
}
void NgxFeature::record([[maybe_unused]] VkCommandBuffer command, [[maybe_unused]] Input color,
                        [[maybe_unused]] Input depth, [[maybe_unused]] Input motion,
                        [[maybe_unused]] Input output, [[maybe_unused]] Input exposure,
                        [[maybe_unused]] const ReconstructionGuides &guides,
                        [[maybe_unused]] const scene::Matrix &view,
                        [[maybe_unused]] const scene::Matrix &projection,
                        [[maybe_unused]] const float *jitter, [[maybe_unused]] float milliseconds,
                        [[maybe_unused]] bool reset) {
  std::lock_guard lock(ngxMutex);
#if REMIX_HAS_DLSS
  if (fault(m->rr ? "rr-evaluate" : "sr-evaluate"))
    throw std::runtime_error("Injected NGX evaluation failure");
  auto resource = [](Input input, VkFormat format, VkExtent2D size, bool output = false) {
    return NVSDK_NGX_Create_ImageView_Resource_VK(input.view, input.image,
                                                  {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, format, size.width,
                                                  size.height, output);
  };
  auto c = resource(color, VK_FORMAT_R16G16B16A16_SFLOAT, m->render);
  auto d = resource(depth, VK_FORMAT_R32_SFLOAT, m->render);
  auto mv = resource(motion, VK_FORMAT_R32G32_SFLOAT, m->render);
  auto exp = resource(exposure, VK_FORMAT_R32_SFLOAT, {1, 1});
  auto out = resource(output, VK_FORMAT_R16G16B16A16_SFLOAT, m->display, true);
  if (m->rr) {
    auto diffuse = resource(guides.diffuse, VK_FORMAT_R16G16B16A16_SFLOAT, m->render);
    auto specular = resource(guides.specular, VK_FORMAT_R16G16B16A16_SFLOAT, m->render);
    auto normal = resource(guides.normalRoughness, VK_FORMAT_R16G16B16A16_SFLOAT, m->render);
    auto distance = resource(guides.hitDistance, VK_FORMAT_R32_SFLOAT, m->render);
    // GLSL column-major / column-vector storage is byte-identical to the
    // transposed row-major / row-vector matrices required by NGX.
    auto worldToView = view, viewToClip = projection;
    NVSDK_NGX_VK_DLSSD_Eval_Params desc{};
    desc.pInColor = &c;
    desc.pInOutput = &out;
    desc.pInDepth = &d;
    desc.pInMotionVectors = &mv;
    desc.pInDiffuseAlbedo = &diffuse;
    desc.pInSpecularAlbedo = &specular;
    desc.pInNormals = &normal;
    desc.pInSpecularHitDistance = &distance;
    desc.pInWorldToViewMatrix = worldToView.data();
    desc.pInViewToClipMatrix = viewToClip.data();
    desc.InJitterOffsetX = -jitter[0];
    desc.InJitterOffsetY = -jitter[1];
    desc.InMVScaleX = desc.InMVScaleY = 1;
    desc.InReset = reset;
    desc.InRenderSubrectDimensions = {m->render.width, m->render.height};
    desc.pInExposureTexture = &exp;
    desc.InPreExposure = desc.InExposureScale = 1;
    desc.InFrameTimeDeltaInMsec = milliseconds;
    check(NGX_VULKAN_EVALUATE_DLSSD_EXT(command, m->feature, m->parameters, &desc), "DLSS RR evaluation");
  } else {
    NVSDK_NGX_VK_DLSS_Eval_Params desc{};
    desc.Feature.pInColor = &c;
    desc.Feature.pInOutput = &out;
    desc.pInDepth = &d;
    desc.pInMotionVectors = &mv;
    desc.InJitterOffsetX = -jitter[0];
    desc.InJitterOffsetY = -jitter[1];
    desc.InMVScaleX = desc.InMVScaleY = 1;
    desc.InReset = reset;
    desc.InRenderSubrectDimensions = {m->render.width, m->render.height};
    desc.pInExposureTexture = &exp;
    desc.InPreExposure = desc.InExposureScale = 1;
    desc.InFrameTimeDeltaInMsec = milliseconds;
    check(NGX_VULKAN_EVALUATE_DLSS_EXT(command, m->feature, m->parameters, &desc), "DLSS SR evaluation");
  }
#else
  unavailable();
#endif
}

struct NgxFrameGeneration::Impl {
#if REMIX_HAS_DLSS
  std::shared_ptr<Ngx> context;
  NVSDK_NGX_Parameter *parameters{};
  NVSDK_NGX_Handle *feature{};
  VkExtent2D render{}, display{};
  VkFormat format{};
  VkCommandPool initializationPool{};
  VkCommandBuffer initializationCommand{};
  VkFence initializationFence{};
  bool initializationSubmitted = false;
  ~Impl() {
    std::lock_guard lock(ngxMutex);
    if (context) {
      const auto device = context->m->device;
      if (initializationFence)
        vkDestroyFence(device, initializationFence, nullptr);
      if (initializationPool)
        vkDestroyCommandPool(device, initializationPool, nullptr);
    }
    if (feature) {
      NVSDK_NGX_VULKAN_ReleaseFeature(feature);
      --context->m->liveFeatures;
    }
    if (parameters)
      NVSDK_NGX_VULKAN_DestroyParameters(parameters);
  }
#endif
};
NgxFrameGeneration::NgxFrameGeneration() : m(std::make_unique<Impl>()) {}
NgxFrameGeneration::~NgxFrameGeneration() = default;
void NgxFrameGeneration::initialize([[maybe_unused]] std::shared_ptr<Ngx> context, [[maybe_unused]] uint32_t family,
                                    [[maybe_unused]] VkExtent2D display, [[maybe_unused]] VkFormat format,
                                    [[maybe_unused]] VkExtent2D render) {
  std::lock_guard lock(ngxMutex);
#if REMIX_HAS_DLSS
  m->context = std::move(context);
  m->render = render;
  m->display = display;
  m->format = format;
  if (!m->context->m->caps.fg)
    throw std::runtime_error(m->context->m->caps.fgReason.empty() ? "DLSS Frame Generation unavailable"
                                                                  : m->context->m->caps.fgReason);
  if (fault("fg-create"))
    throw std::runtime_error("Injected NGX feature creation failure");
  auto device = m->context->m->device;
  check(NVSDK_NGX_VULKAN_AllocateParameters(&m->parameters), "NGX parameters");
  VkCommandPool pool{};
  VkCommandBuffer command{};
  VkFence fence{};
  try {
    VkCommandPoolCreateInfo pi{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    vkCheck(vkCreateCommandPool(device, &pi, nullptr, &pool));
    VkCommandBufferAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                   .commandPool = pool,
                                   .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                   .commandBufferCount = 1};
    vkCheck(vkAllocateCommandBuffers(device, &ai, &command));
    VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkCheck(vkBeginCommandBuffer(command, &bi));
    NVSDK_NGX_DLSSG_Create_Params desc{};
    desc.Width = display.width;
    desc.Height = display.height;
    desc.NativeBackbufferFormat = uint32_t(format);
    desc.RenderWidth = render.width;
    desc.RenderHeight = render.height;
    desc.DynamicResolutionScaling = false;
    check(NGX_VK_CREATE_DLSSG(command, 1, 1, &m->feature, m->parameters, &desc), "DLSS Frame Generation creation");
    ++m->context->m->liveFeatures;
    vkCheck(vkEndCommandBuffer(command));
    VkFenceCreateInfo fi{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCheck(vkCreateFence(device, &fi, nullptr, &fence));
    m->initializationPool = pool;
    m->initializationCommand = command;
    m->initializationFence = fence;
  } catch (...) {
    if (fence)
      vkDestroyFence(device, fence, nullptr);
    if (pool)
      vkDestroyCommandPool(device, pool, nullptr);
    throw;
  }
#else
  unavailable();
#endif
}
bool NgxFrameGeneration::finishInitialization(VkQueue queue) {
#if REMIX_HAS_DLSS
  if (!m->initializationPool)
    return true;
  const auto device = m->context->m->device;
  if (!m->initializationSubmitted) {
    VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1,
                        .pCommandBuffers = &m->initializationCommand};
    vkCheck(vkQueueSubmit(queue, 1, &submit, m->initializationFence));
    m->initializationSubmitted = true;
  }
  const auto status = vkGetFenceStatus(device, m->initializationFence);
  if (status == VK_NOT_READY)
    return false;
  vkCheck(status);
  vkDestroyFence(device, m->initializationFence, nullptr);
  vkDestroyCommandPool(device, m->initializationPool, nullptr);
  m->initializationFence = VK_NULL_HANDLE;
  m->initializationPool = VK_NULL_HANDLE;
#else
  (void)queue;
#endif
  return true;
}
void NgxFrameGeneration::record([[maybe_unused]] VkCommandBuffer command, [[maybe_unused]] Input backbuffer,
                                [[maybe_unused]] Input depth, [[maybe_unused]] Input motion,
                                [[maybe_unused]] Input hudless, [[maybe_unused]] Input output,
                                [[maybe_unused]] const FrameGenerationCamera &camera,
                                [[maybe_unused]] uint32_t generated, [[maybe_unused]] uint32_t index) {
  std::lock_guard lock(ngxMutex);
#if REMIX_HAS_DLSS
  if (fault("fg-evaluate"))
    throw std::runtime_error("Injected NGX evaluation failure");
  auto resource = [](Input input, VkFormat format, VkExtent2D size, bool output = false) {
    return NVSDK_NGX_Create_ImageView_Resource_VK(input.view, input.image,
                                                  {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, format, size.width,
                                                  size.height, output);
  };
  auto color = resource(backbuffer, m->format, m->display);
  auto d = resource(depth, VK_FORMAT_R32_SFLOAT, m->render);
  auto mv = resource(motion, VK_FORMAT_R32G32_SFLOAT, m->render);
  auto out = resource(output, m->format, m->display, true);
  auto withoutHud = resource(hudless, m->format, m->display);
  NVSDK_NGX_VK_DLSSG_Eval_Params eval{};
  eval.pBackbuffer = &color;
  eval.pDepth = &d;
  eval.pMVecs = &mv;
  eval.pOutputInterpFrame = &out;
  NVSDK_NGX_DLSSG_Opt_Eval_Params options{};
  options.multiFrameCount = std::max(generated, 1u);
  options.multiFrameIndex = std::clamp(index, 1u, options.multiFrameCount);
  if (hudless.image) {
    eval.pHudless = &withoutHud;
    options.hudLessSubrectSize = {m->display.width, m->display.height};
  }
  // scene::Matrix (column vectors, column-major) is byte for byte the
  // row-vector, row-major matrix NGX takes, as for Ray Reconstruction.
  static_assert(sizeof(options.cameraViewToClip) == sizeof(scene::Matrix));
  std::memcpy(options.cameraViewToClip, camera.viewToClip.data(), sizeof(scene::Matrix));
  std::memcpy(options.clipToCameraView, camera.clipToView.data(), sizeof(scene::Matrix));
  std::memcpy(options.clipToPrevClip, camera.clipToPrevClip.data(), sizeof(scene::Matrix));
  std::memcpy(options.prevClipToClip, camera.prevClipToClip.data(), sizeof(scene::Matrix));
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      options.clipToLensClip[i][j] = i == j; // no lens distortion
  // Motion is in pixels of its own image, current to previous, which is what
  // NGX takes with a scale of one (as Super Resolution does). The header's
  // wording ("normalize to [-1,1]") suggests a fraction of the frame, but
  // scaled by 1 / size the vectors were as good as absent and generated
  // frames warped whenever the camera turned; negated they warp far worse.
  options.mvecScale[0] = options.mvecScale[1] = 1;
  for (int i = 0; i < 3; ++i) {
    options.cameraPos[i] = camera.position[i];
    options.cameraUp[i] = camera.up[i];
    options.cameraRight[i] = camera.right[i];
    options.cameraFwd[i] = camera.forward[i];
  }
  options.cameraNear = camera.nearPlane;
  options.cameraFar = camera.farPlane;
  options.cameraFOV = camera.fieldOfView;
  options.cameraAspectRatio = camera.aspect;
  options.colorBuffersHDR = false;
  options.depthInverted = camera.depthInverted;
  options.cameraMotionIncluded = true;
  options.reset = camera.reset;
  options.mvecsSubrectSize = {m->render.width, m->render.height};
  options.depthSubrectSize = {m->render.width, m->render.height};
  options.backbufferSubrectSize = {m->display.width, m->display.height};
  options.outputInterpSubrectSize = {m->display.width, m->display.height};
  check(NGX_VK_EVALUATE_DLSSG(command, m->feature, m->parameters, &eval, &options), "DLSS Frame Generation evaluation");
#else
  unavailable();
#endif
}
} // namespace remix::upscale
