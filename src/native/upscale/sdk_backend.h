#pragma once
#include <functional>
#include <map>
#include <vector>
#include <vulkan/vulkan.h>
struct FfxInterface;
namespace remix::upscale {
  // Context creation records no GPU work. If the SDK returns halfway through an
  // allocation sequence, reclaim every live object before falling back to native.
  class ContextAllocations {
  public:
    ContextAllocations();
    ~ContextAllocations();
    void commit();
    using Key = std::pair<uint64_t, uintptr_t>;
    void add(Key handle, std::function<void()> destroy);
    void remove(Key handle);

  private:
    std::map<Key, std::function<void()>> live;
    std::vector<Key> order;
  };
  // The SDK's backend frees the image views it made of an effect's registered
  // images FFX_MAX_QUEUED_FRAMES (4) resource releases later. An effect that
  // releases once a frame keeps them four frames; frame interpolation releases
  // twice (its prepare stage and its dispatch), so the views of one frame's
  // prepare stage go while the next frame is recorded, when the first may still
  // be executing. While one of these is current on the recording thread (Scope),
  // the views the SDK destroys are kept instead and destroyed `frames` advance()
  // calls later. Destruction of the object frees the rest: the device is idle.
  class DeferredViews {
  public:
    DeferredViews(VkDevice device, uint32_t frames);
    ~DeferredViews();
    DeferredViews(const DeferredViews &) = delete;
    DeferredViews &operator=(const DeferredViews &) = delete;
    // Once per recorded frame, before its SDK calls.
    void advance();
    void retire(VkImageView view);
    struct Scope {
      explicit Scope(DeferredViews &views);
      ~Scope();
    };

  private:
    VkDevice device;
    std::vector<std::vector<VkImageView>> retired; // oldest frame first
  };
  VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL sdkDeviceProc(VkDevice device, const char *name);
  // The SDK queries physical FP16 support, but it may not be enabled on our
  // logical device, and it selects 64-wide subgroup permutations that are not
  // embedded. Makes the backend report neither: its effects then ask for the
  // portable full-precision shaders (shader_blobs.cpp).
  void usePortableShaders(FfxInterface &backend);
} // namespace remix::upscale
