#include "metal/gpu.h"
#include <chrono>
#include <xz.h>
#include <sys/sysctl.h>
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <set>
#include <thread>
#include <mutex>
#include <tuple>

namespace remix::metal {
  namespace {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif
    // XZ streams listed by shaders::libraries (scripts/shaders/generate.py metal).
    constexpr unsigned char compressed[] = {
#embed "shaders/embedded.bin"
    };
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
    // Per device: one MTLLibrary and function per stage, kept for the process.
    struct Loaded {
      id<MTLLibrary> library = nil;
      id<MTLFunction> function = nil;
    };
    // Never destroyed: a background compile may finish while the process
    // exits, after static destructors would have run.
    struct Cache {
      std::mutex mutex;
      std::map<std::pair<void *, uint32_t>, Loaded> loaded;
      std::map<std::tuple<void *, uint32_t, uint32_t>, id<MTLComputePipelineState>> computes;
      std::set<std::tuple<void *, uint32_t, uint32_t>> compiling;
      // Every pipeline asked for (compiled now or in the background), for the
      // preparation notice's progress.
      std::set<std::tuple<void *, uint32_t, uint32_t>> requested;
    };
    Cache &cache() {
      static Cache *instance = new Cache;
      return *instance;
    }


    // Pipeline compiles in flight, process-wide. Each runs in Apple's compiler
    // service and the lighting kernels take gigabytes there: unbounded,
    // concurrent compiles exhaust memory. REMIX_PIPELINE_THREADS overrides.
    class CompileSlots {
    public:
      struct Slot {
        CompileSlots &slots;
        explicit Slot(CompileSlots &owner) : slots(owner) {
          std::unique_lock lock(slots.m_mutex);
          slots.m_available.wait(lock, [&] { return slots.m_used < slots.m_limit; });
          ++slots.m_used;
        }
        ~Slot() {
          {
            const std::lock_guard lock(slots.m_mutex);
            --slots.m_used;
          }
          slots.m_available.notify_one();
        }
      };
      CompileSlots() {
        if (const char *value = std::getenv("REMIX_PIPELINE_THREADS"); value && std::atoi(value) > 0) {
          m_limit = unsigned(std::atoi(value));
          return;
        }
        // Measured on an M5 (16 GB): a lighting kernel takes ~15 s and ~730 MB
        // in the compiler service alone; 8 at once finish in ~44 s (5.5 s
        // each). Cores but two, and no more than one per 2 GB of memory.
        uint64_t memory = 0;
        int cores = 0;
        size_t size = sizeof(memory);
        sysctlbyname("hw.memsize", &memory, &size, nullptr, 0);
        size = sizeof(cores);
        sysctlbyname("hw.ncpu", &cores, &size, nullptr, 0);
        m_limit = std::clamp(std::min(unsigned(std::max(cores - 2, 1)), unsigned(memory >> 31)), 1u, 12u);
      }

    private:
      std::mutex m_mutex;
      std::condition_variable m_available;
      unsigned m_used = 0, m_limit = 1;
    };
    CompileSlots &compileSlots() {
      static CompileSlots *slots = new CompileSlots; // never destroyed, as the cache
      return *slots;
    }
    std::map<std::tuple<void *, uint8_t, uint8_t, uint8_t>, id<MTLSamplerState>> samplers;

    std::vector<uint8_t> decompress(const shaders::Library &source) {
      static std::once_flag crc;
      std::call_once(crc, [] { xz_crc32_init(); });
      if (source.offset + source.compressedBytes > sizeof(compressed))
        throw Error("Embedded Metal library table is inconsistent");
      std::vector<uint8_t> bytes(source.bytes);
      xz_dec *decoder = xz_dec_init(XZ_SINGLE, 0);
      if (!decoder)
        throw std::bad_alloc();
      xz_buf buffer{.in = compressed + source.offset,
                    .in_pos = 0,
                    .in_size = source.compressedBytes,
                    .out = bytes.data(),
                    .out_pos = 0,
                    .out_size = source.bytes};
      const auto result = xz_dec_run(decoder, &buffer);
      xz_dec_end(decoder);
      if (result != XZ_STREAM_END || buffer.out_pos != source.bytes)
        throw Error("Embedded Metal library failed to decompress");
      return bytes;
    }
  } // namespace

  const shaders::Library &library(shaders::Stage stage) { return shaders::libraries[size_t(stage)]; }

  namespace {
    Loaded &load(id<MTLDevice> device, shaders::Stage stage) {
      const std::lock_guard lock(cache().mutex);
      auto &entry = cache().loaded[{(__bridge void *)device, uint32_t(stage)}];
      if (entry.function)
        return entry;
      const auto &source = library(stage);
      auto bytes = decompress(source);
      // The dispatch data copies the bytes; the library outlives this call.
      dispatch_data_t data =
          dispatch_data_create(bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
      NSError *error = nil;
      entry.library = [device newLibraryWithData:data error:&error];
      check(entry.library, source.function, error);
      entry.function = [entry.library newFunctionWithName:@(source.function)];
      check(entry.function, source.function);
      return entry;
    }
  } // namespace

  id<MTLFunction> function(id<MTLDevice> device, shaders::Stage stage) { return load(device, stage).function; }

  id<MTLComputePipelineState> computePipeline(id<MTLDevice> device, shaders::Stage stage, uint32_t constants) {
    const std::tuple key{(__bridge void *)device, uint32_t(stage), constants};
    {
      const std::lock_guard lock(cache().mutex);
      if (auto found = cache().computes.find(key); found != cache().computes.end())
        return found->second;
    }
    {
      const std::lock_guard lock(cache().mutex);
      cache().requested.insert(key);
    }
    // Compiled outside the lock (the tracing kernels take seconds), one of a
    // bounded number at a time.
    const CompileSlots::Slot slot(compileSlots());
    {
      const std::lock_guard lock(cache().mutex);
      if (auto found = cache().computes.find(key); found != cache().computes.end())
        return found->second;
    }
    const auto &source = library(stage);
    auto &entry = load(device, stage);
    id<MTLFunction> function = entry.function;
    if (function.functionConstantsDictionary.count) {
      // SPIR-V specialization constants are Metal function constants at
      // their constant_id; every declared one is set, as VkSpecializationInfo.
      auto *values = [MTLFunctionConstantValues new];
      for (NSString *name in function.functionConstantsDictionary) {
        MTLFunctionConstant *constant = function.functionConstantsDictionary[name];
        // Bools at their constant_id's bit; the one integer constant (the
        // sparse launch mode, 13) from bit 16.
        if (constant.type == MTLDataTypeBool && constant.index < 16) {
          const bool value = (constants >> constant.index) & 1;
          [values setConstantValue:&value type:MTLDataTypeBool atIndex:constant.index];
        } else if (constant.type == MTLDataTypeUInt) {
          const uint32_t value = constants >> 16;
          [values setConstantValue:&value type:MTLDataTypeUInt atIndex:constant.index];
        }
      }
      NSError *error = nil;
      function = [entry.library newFunctionWithName:@(source.function) constantValues:values error:&error];
      check(function, source.function, error);
    }
    auto *descriptor = [MTLComputePipelineDescriptor new];
    descriptor.computeFunction = function;
    descriptor.label = @(source.function);
    NSError *error = nil;
    id<MTLComputePipelineState> state = [device newComputePipelineStateWithDescriptor:descriptor
                                                                              options:MTLPipelineOptionNone
                                                                           reflection:nil
                                                                                error:&error];
    check(state, source.function, error);
    const std::lock_guard lock(cache().mutex);
    return cache().computes.emplace(key, state).first->second;
  }

  id<MTLComputePipelineState> computePipelineAsync(id<MTLDevice> device, shaders::Stage stage, uint32_t constants) {
    const std::tuple key{(__bridge void *)device, uint32_t(stage), constants};
    {
      const std::lock_guard lock(cache().mutex);
      if (auto found = cache().computes.find(key); found != cache().computes.end())
        return found->second;
      if (!cache().compiling.insert(key).second)
        return nil;
      cache().requested.insert(key);
    }
    // Background compiles run concurrently, bounded by the compile slots
    // (computePipeline): one at a time left a scene's variants minutes behind.
    static dispatch_queue_t queue = dispatch_queue_create(
        "OpenRemix Metal specializations",
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_CONCURRENT, QOS_CLASS_UTILITY, 0));
    dispatch_async(queue, ^{
      @autoreleasepool {
        // A failed specialization stays marked: the base pipeline remains in
        // use, as with a Vulkan specialization that fails to compile.
        try {
          const auto start = std::chrono::steady_clock::now();
          computePipeline(device, stage, constants);
          // When a frame starts using it (its cost may differ from the base pipeline's).
          NSLog(@"OpenRemix Metal specialization ready: %s 0x%x in %.1f s", library(stage).function, constants,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        } catch (const std::exception &error) {
          NSLog(@"Metal pipeline specialization failed: %s", error.what());
        }
      }
    });
    return nil;
  }

  CompileProgress compileProgress() {
    const std::lock_guard lock(cache().mutex);
    CompileProgress progress{0, uint64_t(cache().requested.size())};
    for (const auto &key : cache().requested)
      progress.completed += cache().computes.count(key);
    return progress;
  }

  std::string pipelineReport() {
    const std::lock_guard lock(cache().mutex);
    size_t pending = 0;
    for (const auto &key : cache().compiling)
      pending += !cache().computes.count(key);
    std::string text = std::to_string(cache().computes.size()) + " compute pipelines compiled, " + std::to_string(pending) +
                       " compiling in the background\n";
    char line[160];
    for (const auto &[key, state] : cache().computes) {
      std::snprintf(line, sizeof(line), "%-40s constants 0x%x\n", library(shaders::Stage(std::get<1>(key))).function,
                    std::get<2>(key));
      text += line;
    }
    return text;
  }

  id<MTLSamplerState> sampler(id<MTLDevice> device, uint8_t filter, uint8_t wrapU, uint8_t wrapV) {
    const std::lock_guard lock(cache().mutex);
    auto &entry = samplers[{(__bridge void *)device, filter, wrapU, wrapV}];
    if (entry)
      return entry;
    auto address = [](uint8_t mode) {
      switch (mode) {
      case 2:
        return MTLSamplerAddressModeMirrorRepeat;
      case 3:
        return MTLSamplerAddressModeClampToEdge;
      case 4:
        return MTLSamplerAddressModeClampToBorderColor;
      default:
        return MTLSamplerAddressModeRepeat;
      }
    };
    auto *descriptor = [MTLSamplerDescriptor new];
    descriptor.minFilter = descriptor.magFilter = filter ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    descriptor.mipFilter = filter ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
    descriptor.sAddressMode = address(wrapU);
    descriptor.tAddressMode = address(wrapV);
    descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.borderColor = MTLSamplerBorderColorTransparentBlack;
    // Argument buffers may only hold samplers created for them.
    descriptor.supportArgumentBuffers = YES;
    entry = [device newSamplerStateWithDescriptor:descriptor];
    check(entry, "sampler");
    return entry;
  }

  ArgumentSet::ArgumentSet(id<MTLDevice> device, shaders::Stage stage, uint32_t set) : m_set(set), m_stage(stage) {
    m_encoder = [function(device, stage) newArgumentEncoderWithBufferIndex:set];
    check(m_encoder, "argument encoder");
    for (auto &buffer : m_buffers) {
      buffer = [device newBufferWithLength:std::max<NSUInteger>(m_encoder.encodedLength, 16)
                                   options:MTLResourceStorageModeShared];
      check(buffer, "argument buffer");
    }
  }

  void ArgumentSet::begin(uint32_t slot) {
    m_slot = slot % gpuFrameCount;
    m_external = nil;
    m_offset = 0;
    [m_encoder setArgumentBuffer:m_buffers[m_slot] offset:0];
    m_reads.clear();
    m_writes.clear();
  }

  void ArgumentSet::begin(id<MTLBuffer> buffer, NSUInteger offset) {
    m_external = buffer;
    m_offset = offset;
    [m_encoder setArgumentBuffer:buffer offset:offset];
    m_reads.clear();
    m_writes.clear();
  }

  const shaders::Binding *ArgumentSet::find(uint32_t binding) const {
    const auto &source = library(m_stage);
    for (uint32_t i = 0; i < source.bindingCount; ++i) {
      const auto &b = shaders::bindings[source.firstBinding + i];
      if (b.set == m_set && b.binding == binding)
        return &b;
    }
    return nullptr;
  }

  bool ArgumentSet::has(uint32_t binding) const { return find(binding) != nullptr; }

  void ArgumentSet::track(id<MTLResource> resource, bool write) {
    if (resource)
      (write ? m_writes : m_reads).push_back(resource);
  }

  void ArgumentSet::buffer(uint32_t binding, id<MTLBuffer> buffer, uint64_t offset) {
    if (const auto *b = find(binding)) {
      [m_encoder setBuffer:buffer offset:offset atIndex:b->id];
      track(buffer, true);
    }
  }

  void ArgumentSet::texture(uint32_t binding, id<MTLTexture> texture, id<MTLSamplerState> sampler, uint32_t element) {
    if (const auto *b = find(binding)) {
      [m_encoder setTexture:texture atIndex:b->id + element];
      if (b->sampler != shaders::NONE && sampler)
        [m_encoder setSamplerState:sampler atIndex:b->sampler + element];
      // Storage images are written; usage is per resource, so read-write covers both.
      track(texture, (texture.usage & MTLTextureUsageShaderWrite) != 0);
    }
  }

  void ArgumentSet::textures(uint32_t binding, const std::vector<id<MTLTexture>> &textures,
                             const std::vector<id<MTLSamplerState>> &samplers) {
    const auto *b = find(binding);
    if (!b)
      return;
    const auto count = std::min<size_t>(textures.size(), b->count);
    [m_encoder setTextures:textures.data() withRange:NSMakeRange(b->id, count)];
    if (b->sampler != shaders::NONE)
      [m_encoder setSamplerStates:samplers.data() withRange:NSMakeRange(b->sampler, count)];
    m_reads.insert(m_reads.end(), textures.begin(), textures.begin() + count);
  }

  void ArgumentSet::sampler(uint32_t binding, id<MTLSamplerState> sampler) {
    if (const auto *b = find(binding))
      [m_encoder setSamplerState:sampler atIndex:b->id];
  }

  void ArgumentSet::acceleration(uint32_t binding, id<MTLAccelerationStructure> structure) {
    if (const auto *b = find(binding)) {
      [m_encoder setAccelerationStructure:structure atIndex:b->id];
      track(structure, false);
    }
  }

  void ArgumentSet::use(id<MTLComputeCommandEncoder> encoder) const {
    if (!m_reads.empty())
      [encoder useResources:m_reads.data() count:m_reads.size() usage:MTLResourceUsageRead];
    if (!m_writes.empty())
      [encoder useResources:m_writes.data() count:m_writes.size() usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    [encoder useResource:current() usage:MTLResourceUsageRead];
  }

  void ArgumentSet::use(id<MTLRenderCommandEncoder> encoder, MTLRenderStages stages) const {
    if (!m_reads.empty())
      [encoder useResources:m_reads.data() count:m_reads.size() usage:MTLResourceUsageRead stages:stages];
    if (!m_writes.empty())
      [encoder useResources:m_writes.data()
                      count:m_writes.size()
                      usage:MTLResourceUsageRead | MTLResourceUsageWrite
                     stages:stages];
  }
} // namespace remix::metal
