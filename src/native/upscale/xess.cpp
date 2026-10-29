#include "upscale/xess.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <windows.h>
#include <xess/xess_vk.h>

namespace remix::upscale {
namespace xess {
namespace {
// The entry points are resolved from the DLL; nothing links libxess.lib.
struct Api {
  decltype(&xessGetVersion) getVersion{};
  decltype(&xessVKGetRequiredInstanceExtensions) instanceExtensions{};
  decltype(&xessVKGetRequiredDeviceExtensions) deviceExtensions{};
  decltype(&xessVKGetRequiredDeviceFeatures) deviceFeatures{};
  decltype(&xessVKCreateContext) createContext{};
  decltype(&xessVKBuildPipelines) buildPipelines{};
  decltype(&xessVKInit) init{};
  decltype(&xessVKExecute) execute{};
  decltype(&xessDestroyContext) destroyContext{};
  decltype(&xessGetOptimalInputResolution) optimalInputResolution{};
  decltype(&xessGetProperties) properties{};
  std::string version;
};
std::filesystem::path moduleDirectory() {
  HMODULE module{};
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&moduleDirectory), &module))
    throw std::runtime_error("XeSS: module path unavailable");
  std::wstring path(32768, L'\0');
  const auto size = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
  if (!size || size == path.size())
    throw std::runtime_error("XeSS: module path unavailable");
  path.resize(size);
  return std::filesystem::path(path).parent_path();
}
Api load() {
  const auto directory = moduleDirectory();
  HMODULE module{};
  for (const auto &candidate :
       {directory / "xess", directory / "upscale", directory}) {
    std::error_code error;
    const auto path = candidate / "libxess.dll";
    if (!std::filesystem::is_regular_file(path, error))
      continue;
    module = LoadLibraryExW(path.c_str(), nullptr,
                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module)
      throw std::runtime_error("XeSS: libxess.dll could not be loaded (error " +
                               std::to_string(GetLastError()) + ")");
    break;
  }
  if (!module)
    throw std::runtime_error("XeSS: libxess.dll not packaged");
  Api api;
  auto resolve = [&](auto &function, const char *name) {
    function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
        GetProcAddress(module, name));
    if (!function)
      throw std::runtime_error(std::string("XeSS: libxess.dll lacks ") + name);
  };
  resolve(api.getVersion, "xessGetVersion");
  resolve(api.instanceExtensions, "xessVKGetRequiredInstanceExtensions");
  resolve(api.deviceExtensions, "xessVKGetRequiredDeviceExtensions");
  resolve(api.deviceFeatures, "xessVKGetRequiredDeviceFeatures");
  resolve(api.createContext, "xessVKCreateContext");
  resolve(api.buildPipelines, "xessVKBuildPipelines");
  resolve(api.init, "xessVKInit");
  resolve(api.execute, "xessVKExecute");
  resolve(api.destroyContext, "xessDestroyContext");
  resolve(api.optimalInputResolution, "xessGetOptimalInputResolution");
  resolve(api.properties, "xessGetProperties");
  xess_version_t version{};
  if (api.getVersion(&version) == XESS_RESULT_SUCCESS)
    api.version = "XeSS " + std::to_string(version.major) + "." +
                  std::to_string(version.minor) + "." +
                  std::to_string(version.patch);
  else
    api.version = "XeSS";
  return api;
}
const Api &api() {
  // A failure is remembered: the DLL does not appear while the process runs.
  static std::once_flag once;
  static Api loaded;
  static std::string failure;
  std::call_once(once, [] {
    try {
      loaded = load();
    } catch (const std::exception &e) {
      failure = e.what();
    }
  });
  if (!failure.empty())
    throw std::runtime_error(failure);
  return loaded;
}
const char *describe(xess_result_t result) {
  switch (result) {
  case XESS_RESULT_ERROR_UNSUPPORTED_DEVICE:
    return "unsupported device";
  case XESS_RESULT_ERROR_UNSUPPORTED_DRIVER:
    return "unsupported driver";
  case XESS_RESULT_ERROR_UNINITIALIZED:
    return "uninitialized";
  case XESS_RESULT_ERROR_INVALID_ARGUMENT:
    return "invalid argument";
  case XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY:
    return "out of device memory";
  case XESS_RESULT_ERROR_DEVICE:
    return "device error";
  case XESS_RESULT_ERROR_NOT_IMPLEMENTED:
    return "not implemented";
  case XESS_RESULT_ERROR_INVALID_CONTEXT:
    return "invalid context";
  case XESS_RESULT_ERROR_OPERATION_IN_PROGRESS:
    return "operation in progress";
  case XESS_RESULT_ERROR_UNSUPPORTED:
    return "unsupported";
  case XESS_RESULT_ERROR_CANT_LOAD_LIBRARY:
    return "library cannot be loaded";
  case XESS_RESULT_ERROR_WRONG_CALL_ORDER:
    return "wrong call order";
  default:
    return "unknown error";
  }
}
// Warnings (positive results) succeed.
void check(xess_result_t result, const char *what) {
  if (result < XESS_RESULT_SUCCESS)
    throw std::runtime_error(std::string(what) + " failed (" +
                             describe(result) + ", " +
                             std::to_string(int(result)) + ")");
}
std::mutex registryMutex;
std::map<VkDevice, std::string> registry;
} // namespace

uint32_t instanceApiVersion() {
  try {
    uint32_t count = 0, version = 0;
    const char *const *names{};
    if (api().instanceExtensions(&count, &names, &version) !=
            XESS_RESULT_SUCCESS ||
        count)
      return 0;
    return std::max(version, uint32_t(VK_API_VERSION_1_1));
  } catch (const std::exception &) {
    return 0;
  }
}
std::string enableDevice(VkInstance instance, VkPhysicalDevice physical,
                         uint32_t instanceApiVersion,
                         std::vector<const char *> &extensions,
                         void *&features) {
  try {
    api(); // why the library is unavailable, if it is
    const uint32_t required = xess::instanceApiVersion();
    if (!required)
      return "XeSS: libxess.dll asks for instance extensions the runtime does "
             "not enable";
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (instanceApiVersion < required || properties.apiVersion < required)
      return "XeSS needs Vulkan " +
             std::to_string(VK_API_VERSION_MAJOR(required)) + "." +
             std::to_string(VK_API_VERSION_MINOR(required));
    uint32_t count = 0;
    const char *const *names{};
    check(api().deviceExtensions(instance, physical, &count, &names),
          "XeSS device extension query");
    // Features last: the chain is changed in place.
    check(api().deviceFeatures(instance, physical, &features),
          "XeSS device feature query");
    for (uint32_t i = 0; i < count; ++i)
      if (std::none_of(extensions.begin(), extensions.end(),
                       [&](auto p) { return std::strcmp(p, names[i]) == 0; }))
        extensions.push_back(names[i]);
    return {};
  } catch (const std::exception &e) {
    return e.what();
  }
}
void registerDevice(VkDevice device, const std::string &unavailable) {
  std::lock_guard lock(registryMutex);
  registry[device] = unavailable;
}
void unregisterDevice(VkDevice device) {
  std::lock_guard lock(registryMutex);
  registry.erase(device);
}
} // namespace xess

struct XessUpscaler::Impl {
  xess_context_handle_t context{};
  VkExtent2D display{};
  uint64_t memory{};
  std::string provider;
  // For finishInitialization.
  xess_vk_init_params_t params{};
  bool initialized{};
  std::string failure;
  ~Impl() {
    if (context)
      xess::api().destroyContext(context);
  }
};
XessUpscaler::XessUpscaler() : m(std::make_unique<Impl>()) {}
XessUpscaler::~XessUpscaler() = default;
void XessUpscaler::create(VkInstance instance, VkPhysicalDevice physical,
                          VkDevice device, VkExtent2D render,
                          VkExtent2D display, uint32_t quality) {
  {
    std::lock_guard lock(xess::registryMutex);
    const auto found = xess::registry.find(device);
    if (found == xess::registry.end())
      throw std::runtime_error("XeSS was not enabled on this device");
    if (!found->second.empty())
      throw std::runtime_error(found->second);
  }
  if (!instance)
    throw std::runtime_error("XeSS: Vulkan instance unavailable");
  const auto &api = xess::api();
  auto next = std::make_unique<Impl>();
  xess::check(api.createContext(instance, physical, device, &next->context),
              "XeSS context creation");
  // The presets of the setting's names (upscale::xessRenderSize).
  constexpr xess_quality_settings_t presets[] = {
      XESS_QUALITY_SETTING_AA,
      XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS,
      XESS_QUALITY_SETTING_ULTRA_QUALITY,
      XESS_QUALITY_SETTING_QUALITY,
      XESS_QUALITY_SETTING_BALANCED,
      XESS_QUALITY_SETTING_PERFORMANCE,
      XESS_QUALITY_SETTING_ULTRA_PERFORMANCE};
  const auto preset = presets[std::min(quality, 6u)];
  const xess_2d_t output{display.width, display.height};
  xess_2d_t optimal{}, smallest{}, largest{};
  xess::check(api.optimalInputResolution(next->context, &output, preset,
                                         &optimal, &smallest, &largest),
              "XeSS input resolution query");
  if (render.width < smallest.x || render.height < smallest.y ||
      render.width > largest.x || render.height > largest.y)
    throw std::runtime_error(
        "XeSS: render size " + std::to_string(render.width) + "x" +
        std::to_string(render.height) + " outside " +
        std::to_string(smallest.x) + "x" + std::to_string(smallest.y) + " to " +
        std::to_string(largest.x) + "x" + std::to_string(largest.y));
  // Low-resolution pixel motion and device depth (nearer is smaller), as
  // NGX takes them; HDR color whose exposure XeSS finds itself, as DLSS does.
  next->params.outputResolution = output;
  next->params.qualitySetting = preset;
  next->params.initFlags = XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE;
  // The kernels compile here, so that xessVKInit (finishInitialization), on
  // the thread that owns the queue, only uploads.
  xess::check(api.buildPipelines(next->context, VK_NULL_HANDLE, true,
                                 next->params.initFlags),
              "XeSS pipeline build");
  xess_properties_t properties{};
  if (api.properties(next->context, &output, &properties) ==
      XESS_RESULT_SUCCESS)
    next->memory =
        properties.tempBufferHeapSize + properties.tempTextureHeapSize;
  next->display = display;
  next->provider = api.version;
  m = std::move(next);
}
void XessUpscaler::finishInitialization() {
  if (!m->context || m->initialized || !m->failure.empty())
    return;
  try {
    xess::check(xess::api().init(m->context, &m->params),
                "XeSS initialization");
    m->initialized = true;
  } catch (const std::exception &e) {
    m->failure = e.what();
  }
}
void XessUpscaler::dispatch(VkCommandBuffer command,
                            const XessDispatch &frame) {
  if (!m->failure.empty())
    throw std::runtime_error(m->failure);
  if (!m->context || !m->initialized)
    throw std::runtime_error("XeSS context unavailable");
  auto view = [](Input input, VkFormat format, VkExtent2D size) {
    return xess_vk_image_view_info{
        input.view, input.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        format,     size.width,  size.height};
  };
  xess_vk_execute_params_t params{};
  params.colorTexture =
      view(frame.color, VK_FORMAT_R16G16B16A16_SFLOAT, frame.render);
  params.velocityTexture =
      view(frame.motion, VK_FORMAT_R32G32_SFLOAT, frame.render);
  params.depthTexture = view(frame.depth, VK_FORMAT_R32_SFLOAT, frame.render);
  params.outputTexture =
      view(frame.output, VK_FORMAT_R16G16B16A16_SFLOAT, m->display);
  params.jitterOffsetX = frame.jitter[0];
  params.jitterOffsetY = frame.jitter[1];
  params.exposureScale = 1;
  params.resetHistory = frame.reset;
  params.inputWidth = frame.render.width;
  params.inputHeight = frame.render.height;
  xess::check(xess::api().execute(m->context, command, &params),
              "XeSS evaluation");
}
uint64_t XessUpscaler::memoryBytes() const { return m->memory; }
const std::string &XessUpscaler::provider() const { return m->provider; }
} // namespace remix::upscale
