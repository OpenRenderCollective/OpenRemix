#pragma once
// Objective-C++ only: Metal objects and helpers shared by the Metal backend.
// Compiled with ARC; C++ structs hold strong references to Metal objects.
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "metal/shaders/embedded.h"
#include "metal/timers.h"
#include "platform/gpu_frames.h"
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace remix::metal {
  // A Metal failure: the runtime fails the frame, as with a VkResult.
  struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
  };
  inline void check(id object, const char *what) {
    if (!object)
      throw Error(std::string("Metal: ") + what);
  }
  inline void check(id object, const char *what, NSError *error) {
    if (!object)
      throw Error(std::string("Metal: ") + what + (error ? std::string(": ") + error.localizedDescription.UTF8String : ""));
  }

  // The functions of the embedded libraries (scripts/shaders/metal.py),
  // decompressed and loaded on first use.
  id<MTLFunction> function(id<MTLDevice> device, shaders::Stage stage);
  const shaders::Library &library(shaders::Stage stage);
  // A compute pipeline; constants are boolean specialization constants by
  // constant_id (bit i: constant_id i), as VkSpecializationInfo, for the
  // constants the stage declares.
  id<MTLComputePipelineState> computePipeline(id<MTLDevice> device, shaders::Stage stage, uint32_t constants = 0);
  // The pipeline when compiled; otherwise starts compiling it in the
  // background (once) and returns nil.
  id<MTLComputePipelineState> computePipelineAsync(id<MTLDevice> device, shaders::Stage stage, uint32_t constants);
  // The compute pipelines compiled and compiling, by stage and constants.
  std::string pipelineReport();
  // Compute pipelines compiled of those asked for (preparation progress).
  struct CompileProgress {
    uint64_t completed = 0, requested = 0;
  };
  CompileProgress compileProgress();

  // One descriptor set of a stage as a Metal argument buffer, written by the
  // stage's Vulkan bindings (embedded.h). Resources referenced only through
  // the buffer are listed for useResources.
  class ArgumentSet {
  public:
    ArgumentSet() = default;
    ArgumentSet(id<MTLDevice> device, shaders::Stage stage, uint32_t set);
    explicit operator bool() const { return m_encoder != nil; }
    // Starts a new argument buffer (a ring entry per frame slot).
    void begin(uint32_t slot);
    // Starts one in caller-owned memory (encodedLength bytes at offset).
    void begin(id<MTLBuffer> buffer, NSUInteger offset);
    NSUInteger encodedLength() const { return m_encoder.encodedLength; }
    NSUInteger offset() const { return m_offset; }
    void buffer(uint32_t binding, id<MTLBuffer> buffer, uint64_t offset = 0);
    void texture(uint32_t binding, id<MTLTexture> texture, id<MTLSamplerState> sampler = nil, uint32_t element = 0);
    void textures(uint32_t binding, const std::vector<id<MTLTexture>> &textures,
                  const std::vector<id<MTLSamplerState>> &samplers);
    void acceleration(uint32_t binding, id<MTLAccelerationStructure> structure);
    // A separate sampler (HLSL SamplerState) at its own binding.
    void sampler(uint32_t binding, id<MTLSamplerState> sampler);
    bool has(uint32_t binding) const;
    id<MTLBuffer> current() const { return m_external ? m_external : m_buffers[m_slot]; }
    // Resources reached only through the argument buffer.
    const std::vector<id<MTLResource>> &reads() const { return m_reads; }
    const std::vector<id<MTLResource>> &writes() const { return m_writes; }
    void use(id<MTLComputeCommandEncoder> encoder) const;
    void use(id<MTLRenderCommandEncoder> encoder, MTLRenderStages stages) const;

  private:
    const shaders::Binding *find(uint32_t binding) const;
    void track(id<MTLResource> resource, bool write);
    id<MTLArgumentEncoder> m_encoder = nil;
    id<MTLBuffer> m_buffers[gpuFrameCount] = {};
    id<MTLBuffer> m_external = nil;
    NSUInteger m_offset = 0;
    uint32_t m_slot = 0, m_set = 0;
    shaders::Stage m_stage{};
    std::vector<id<MTLResource>> m_reads, m_writes;
  };

  // Samplers by (filter, wrap U, wrap V, levels), as the Vulkan materials.
  id<MTLSamplerState> sampler(id<MTLDevice> device, uint8_t filter, uint8_t wrapU, uint8_t wrapV);

  // Shared (unified memory) buffers: Apple GPUs need no staging copies.
  inline id<MTLBuffer> buffer(id<MTLDevice> device, const void *data, size_t size) {
    id<MTLBuffer> result = data ? [device newBufferWithBytes:data length:std::max<size_t>(size, 16)
                                                     options:MTLResourceStorageModeShared]
                                : [device newBufferWithLength:std::max<size_t>(size, 16)
                                                      options:MTLResourceStorageModeShared];
    check(result, "buffer allocation");
    return result;
  }
  inline id<MTLTexture> texture2d(id<MTLDevice> device, MTLPixelFormat format, uint32_t width, uint32_t height,
                                  MTLTextureUsage usage, uint32_t levels = 1,
                                  MTLStorageMode storage = MTLStorageModePrivate) {
    auto *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                         width:std::max(1u, width)
                                                                        height:std::max(1u, height)
                                                                     mipmapped:NO];
    descriptor.mipmapLevelCount = std::max(1u, levels);
    descriptor.usage = usage;
    descriptor.storageMode = storage;
    id<MTLTexture> result = [device newTextureWithDescriptor:descriptor];
    check(result, "texture allocation");
    return result;
  }
} // namespace remix::metal
