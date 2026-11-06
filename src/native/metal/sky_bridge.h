#pragma once
// Objective-C++ only: sky/bridge.h's Bridge over the Metal device, for
// sky/renderer.cpp in Metal builds (sky/host.h). Each dispatch, copy and clear is an encoder of its own on
// hazard-tracked resources, so Metal orders them and barrier() has nothing
// left to do. Constants, staging and argument buffers come from rings of the
// current frame slot; destroyed resources are released when their slot comes
// round again (after its command buffer completed).
#include "metal/gpu.h"
#include "sky/host.h"
#include "sky/shader_code.h"
#include <numos/gpu.h>
#include <numos/shaders.h>
#include <array>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

namespace remix::sky {
  class Bridge final : public numos::IGpu {
  public:
    // A pass of the host's own (an adapter), dispatched like NUMOS's: index
    // numos::passCount() + its position. Pass i is the Metal stage sky0 + i
    // (scripts/shaders/metal.py); the SPIR-V is not needed.
    struct HostPass {
      numos::PassInfo info;
      ShaderCode code;
    };
    Bridge(PhysicalDevice, Device device, std::vector<HostPass> hostPasses);
    ~Bridge() override;
    Bridge(const Bridge &) = delete;
    Bridge &operator=(const Bridge &) = delete;

    // After the slot's command buffer completed: releases what the slot
    // deferred and recycles its rings.
    void selectFrame(uint32_t slot);
    // Starts compiling the pipelines of the passes `needed` selects; true once all exist.
    bool preparePipelines(const std::function<bool(uint32_t pass)> &needed, uint32_t &ready, uint32_t &total);
    // Brackets the IGpu calls of a frame recorded into `command`.
    void begin(id<MTLCommandBuffer> command);
    void end();

    // Host access to resources created or imported through this IGpu.
    id<MTLTexture> sampledView(numos::GpuHandle texture);
    id<MTLSamplerState> sampler(numos::GpuHandle sampler) const;
    // A host texture NUMOS may bind (sampled and storage-written as its desc
    // says); image and view are the same texture.
    numos::GpuHandle importTexture(Image image, ImageView view, const numos::TextureDesc &desc);
    // Read as each pass that traces it is recorded.
    numos::GpuHandle importAccelerationStructure(AccelerationStructure scene);
    // Forgets an import without releasing the host's object.
    void forget(numos::GpuHandle handle);

    numos::GpuHandle createTexture(const numos::TextureDesc &desc) override;
    numos::GpuHandle createBuffer(const numos::BufferDesc &desc) override;
    numos::GpuHandle createSampler(const numos::SamplerDesc &desc) override;
    void destroy(numos::GpuHandle resource) override;
    ConstantsRegion uploadConstants(const void *data, size_t size) override;
    void updateBuffer(numos::GpuHandle buffer, size_t offset, size_t size, const void *data) override;
    void copyBuffer(numos::GpuHandle dst, size_t dstOffset, numos::GpuHandle src, size_t srcOffset,
                    size_t size) override;
    void copyTexture(numos::GpuHandle dst, numos::GpuHandle src) override;
    void clearTexture(numos::GpuHandle texture, const float rgba[4]) override;
    void uploadTexture(numos::GpuHandle texture, uint32_t mip, const void *data, size_t size) override;
    void dispatch(const numos::DispatchDesc &desc) override;
    void barrier() override {}
    bool readBuffer(numos::GpuHandle readback, void *dst, size_t size) override;
    void beginZone(const char *name) override;
    void endZone() override;

  private:
    struct Resource {
      enum class Kind : uint8_t { Texture, Buffer, Sampler, Accel } kind = Kind::Texture;
      bool imported = false;
      numos::TextureDesc texture;
      id<MTLTexture> image = nil;
      std::map<uint64_t, id<MTLTexture>> views;
      numos::BufferDesc buffer;
      id<MTLBuffer> handle = nil;
      uint64_t copyFrame = 0;
      id<MTLSamplerState> sampler = nil;
      AccelerationStructure scene = nullptr;
    };
    struct Ring {
      id<MTLBuffer> buffer = nil;
      size_t used = 0;
    };
    struct Slot {
      numos::GpuHandle constants = 0;
      size_t constantsUsed = 0;
      std::vector<Ring> staging, arguments;
      std::vector<Resource> retired;
    };
    struct Pass {
      numos::PassInfo info;
      metal::shaders::Stage stage{};
      metal::ArgumentSet set;
      id<MTLComputePipelineState> pipeline = nil;
    };

    numos::GpuHandle add(Resource resource);
    Resource &get(numos::GpuHandle handle);
    const Resource &get(numos::GpuHandle handle) const;
    id<MTLTexture> view(Resource &texture, uint32_t mip, uint32_t count, bool storage);
    // Room in one of the slot's rings (staging or arguments), growing it as needed.
    std::pair<id<MTLBuffer>, size_t> allocate(std::vector<Ring> &rings, size_t size, size_t alignment,
                                             size_t chunk);
    std::pair<id<MTLBuffer>, size_t> stage(const void *data, size_t size);
    // Copies a buffer holding one texel image per (mip, layer) or depth slice into the texture.
    void copyToTexture(id<MTLBuffer> source, size_t offset, size_t rowBytes, Resource &target, uint32_t mip);
    numos::GpuHandle placeholder(const numos::PassBindingInfo &binding);
    id<MTLCommandBuffer> command() const;

    id<MTLDevice> m_device;
    std::vector<Pass> m_passes;
    std::unordered_map<numos::GpuHandle, Resource> m_resources;
    numos::GpuHandle m_next = 1;
    std::array<Slot, gpuFrameCount> m_slots;
    uint32_t m_slot = 0;
    uint64_t m_frame = 0;
    id<MTLCommandBuffer> m_command = nil;
    std::map<uint32_t, numos::GpuHandle> m_placeholders;
    std::vector<std::string> m_zones;
  };
} // namespace remix::sky
