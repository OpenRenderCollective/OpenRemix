#include "upscale/sdk_backend.h"
#include "runtime/pipelines.h"
#include <atomic>
#include <bit>
#include <string_view>
// After <bit>, which the SDK's utility header uses without including.
#include <FidelityFX/host/ffx_interface.h>
namespace remix::upscale {
  namespace {
    thread_local ContextAllocations *allocations{};
    thread_local DeferredViews *deferredViews{};
    // The backend's own capability query: one function for every interface.
    std::atomic<FfxGetDeviceCapabilitiesFunc> sdkCapabilities{};
    template <auto Create, auto Destroy> struct Object;
    template <class Info, class Handle,
              VkResult(VKAPI_PTR Create)(VkDevice, const Info *, const VkAllocationCallbacks *, Handle *),
              void(VKAPI_PTR Destroy)(VkDevice, Handle, const VkAllocationCallbacks *)>
    struct Object<Create, Destroy> {
      static VKAPI_ATTR VkResult VKAPI_CALL create(VkDevice device, const Info *info,
                                                   const VkAllocationCallbacks *allocator, Handle *handle) {
        auto result = Create(device, info, allocator, handle);
        if (result != VK_SUCCESS)
          throw result;
        if (allocations) {
          const auto value = *handle;
          allocations->add({uint64_t(value), uintptr_t(Destroy)}, [=] { Destroy(device, value, allocator); });
        }
        return result;
      }
      static VKAPI_ATTR void VKAPI_CALL destroy(VkDevice device, Handle handle,
                                                const VkAllocationCallbacks *allocator) {
        if (allocations)
          allocations->remove({uint64_t(handle), uintptr_t(Destroy)});
        Destroy(device, handle, allocator);
      }
    };
    VKAPI_ATTR void VKAPI_CALL destroyImageView(VkDevice device, VkImageView view,
                                                const VkAllocationCallbacks *allocator) {
      if (allocations)
        allocations->remove({uint64_t(view), uintptr_t(vkDestroyImageView)});
      if (deferredViews && view)
        deferredViews->retire(view);
      else
        vkDestroyImageView(device, view, allocator);
    }
    VKAPI_ATTR VkResult VKAPI_CALL createPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                   const VkComputePipelineCreateInfo *infos,
                                                   const VkAllocationCallbacks *allocator, VkPipeline *pipelines) {
      auto result = pipeline::createComputePipelines(device, cache, count, infos, allocator, pipelines);
      if (allocations)
        for (uint32_t i = 0; i < count; ++i)
          if (pipelines[i]) {
            const auto pipeline = pipelines[i];
            allocations->add({uint64_t(pipeline), uintptr_t(vkDestroyPipeline)},
                             [=] { pipeline::destroyPipeline(device, pipeline, allocator); });
          }
      if (result != VK_SUCCESS)
        throw result;
      return result;
    }
    VKAPI_ATTR void VKAPI_CALL destroyPipeline(VkDevice device, VkPipeline pipeline,
                                               const VkAllocationCallbacks *allocator) {
      if (allocations)
        allocations->remove({uint64_t(pipeline), uintptr_t(vkDestroyPipeline)});
      pipeline::destroyPipeline(device, pipeline, allocator);
    }
  } // namespace
  ContextAllocations::ContextAllocations() { allocations = this; }
  ContextAllocations::~ContextAllocations() {
    allocations = nullptr;
    for (auto i = order.rbegin(); i != order.rend(); ++i) {
      auto found = live.find(*i);
      if (found != live.end()) {
        found->second();
        live.erase(found);
      }
    }
  }
  void ContextAllocations::commit() {
    live.clear();
    order.clear();
    allocations = nullptr;
  }
  void ContextAllocations::add(Key handle, std::function<void()> destroy) {
    live[handle] = std::move(destroy);
    order.push_back(handle);
  }
  void ContextAllocations::remove(Key handle) { live.erase(handle); }
  DeferredViews::DeferredViews(VkDevice device, uint32_t frames) : device(device), retired(frames) {}
  DeferredViews::~DeferredViews() {
    for (const auto &views : retired)
      for (auto view : views)
        vkDestroyImageView(device, view, nullptr);
  }
  void DeferredViews::advance() {
    for (auto view : retired.front())
      vkDestroyImageView(device, view, nullptr);
    retired.erase(retired.begin());
    retired.emplace_back();
  }
  void DeferredViews::retire(VkImageView view) { retired.back().push_back(view); }
  DeferredViews::Scope::Scope(DeferredViews &views) { deferredViews = &views; }
  DeferredViews::Scope::~Scope() { deferredViews = nullptr; }
  void usePortableShaders(FfxInterface &backend) {
    sdkCapabilities = backend.fpGetDeviceCapabilities;
    backend.fpGetDeviceCapabilities = [](FfxInterface *sdk, FfxDeviceCapabilities *caps) {
      auto result = sdkCapabilities.load()(sdk, caps);
      caps->waveLaneCountMax = caps->waveLaneCountMin;
      caps->fp16Supported = false;
      return result;
    };
  }
  VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL sdkDeviceProc(VkDevice device, const char *name) {
    const std::string_view symbol(name);
#define OBJECT(createFn, destroyFn)                                                                                    \
  if (symbol == #createFn)                                                                                             \
    return reinterpret_cast<PFN_vkVoidFunction>(&Object<createFn, destroyFn>::create);                                 \
  if (symbol == #destroyFn)                                                                                            \
    return reinterpret_cast<PFN_vkVoidFunction>(&Object<createFn, destroyFn>::destroy);
    OBJECT(vkAllocateMemory, vkFreeMemory)
    OBJECT(vkCreateBuffer, vkDestroyBuffer)
    OBJECT(vkCreateBufferView, vkDestroyBufferView)
    OBJECT(vkCreateImage, vkDestroyImage)
    if (symbol == "vkCreateImageView")
      return reinterpret_cast<PFN_vkVoidFunction>(&Object<vkCreateImageView, vkDestroyImageView>::create);
    if (symbol == "vkDestroyImageView")
      return reinterpret_cast<PFN_vkVoidFunction>(destroyImageView);
    if (symbol == "vkCreateShaderModule")
      return reinterpret_cast<PFN_vkVoidFunction>(
          &Object<pipeline::createShaderModule, pipeline::destroyShaderModule>::create);
    if (symbol == "vkDestroyShaderModule")
      return reinterpret_cast<PFN_vkVoidFunction>(
          &Object<pipeline::createShaderModule, pipeline::destroyShaderModule>::destroy);
    if (symbol == "vkCreatePipelineLayout")
      return reinterpret_cast<PFN_vkVoidFunction>(
          &Object<pipeline::createPipelineLayout, pipeline::destroyPipelineLayout>::create);
    if (symbol == "vkDestroyPipelineLayout")
      return reinterpret_cast<PFN_vkVoidFunction>(
          &Object<pipeline::createPipelineLayout, pipeline::destroyPipelineLayout>::destroy);
    OBJECT(vkCreateDescriptorPool, vkDestroyDescriptorPool)
    if (symbol == "vkCreateDescriptorSetLayout")
      return reinterpret_cast<PFN_vkVoidFunction>(
          &Object<pipeline::createDescriptorSetLayout, pipeline::destroyDescriptorSetLayout>::create);
    if (symbol == "vkDestroyDescriptorSetLayout")
      return reinterpret_cast<PFN_vkVoidFunction>(
          &Object<pipeline::createDescriptorSetLayout, pipeline::destroyDescriptorSetLayout>::destroy);
    if (symbol == "vkCreateSampler")
      return reinterpret_cast<PFN_vkVoidFunction>(&Object<pipeline::createSampler, pipeline::destroySampler>::create);
    if (symbol == "vkDestroySampler")
      return reinterpret_cast<PFN_vkVoidFunction>(&Object<pipeline::createSampler, pipeline::destroySampler>::destroy);
#undef OBJECT
    if (symbol == "vkCreateComputePipelines")
      return reinterpret_cast<PFN_vkVoidFunction>(createPipelines);
    if (symbol == "vkDestroyPipeline")
      return reinterpret_cast<PFN_vkVoidFunction>(destroyPipeline);
    // The SDK uses the extension spelling of this Vulkan 1.1 core entry point.
    if (symbol == "vkGetBufferMemoryRequirements2KHR")
      return vkGetDeviceProcAddr(device, "vkGetBufferMemoryRequirements2");
    return vkGetDeviceProcAddr(device, name);
  }
} // namespace remix::upscale
