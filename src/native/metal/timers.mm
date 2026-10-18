#include "metal/timers.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace remix::metal {
  namespace {
    PassTimers *s_active = nullptr;
    constexpr NSUInteger sampleCount = 1024; // two per encoder
  } // namespace

  bool PassTimers::initialize(id<MTLDevice> device) {
    m_device = device;
    m_supported = false;
    if (![device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary])
      return false;
    id<MTLCounterSet> timestamps = nil;
    for (id<MTLCounterSet> set in device.counterSets)
      if ([set.name isEqualToString:MTLCommonCounterSetTimestamp])
        timestamps = set;
    if (!timestamps)
      return false;
    for (auto &slot : m_slots) {
      auto *descriptor = [MTLCounterSampleBufferDescriptor new];
      descriptor.counterSet = timestamps;
      descriptor.storageMode = MTLStorageModeShared;
      descriptor.sampleCount = sampleCount;
      descriptor.label = @"GPU pass timers";
      NSError *error = nil;
      slot.samples = [device newCounterSampleBufferWithDescriptor:descriptor error:&error];
      if (!slot.samples)
        return false;
    }
    [device sampleTimestamps:&m_cpuStart gpuTimestamp:&m_gpuStart];
    m_supported = true;
    return true;
  }

  void PassTimers::beginFrame(uint32_t slot, bool enabled, uint64_t frame) {
    m_slot = slot % gpuFrameCount;
    auto &s = m_slots[m_slot];
    if (s.pending)
      resolve(s);
    s.ranges.clear();
    s.frame = frame;
    m_recording = enabled && m_supported;
    s_active = m_recording ? this : nullptr;
  }

  void PassTimers::endFrame() {
    if (m_recording)
      m_slots[m_slot].pending = !m_slots[m_slot].ranges.empty();
    m_recording = false;
    s_active = nullptr;
  }

  void PassTimers::setSizes(uint32_t renderWidth, uint32_t renderHeight, uint32_t outputWidth, uint32_t outputHeight) {
    auto &sizes = m_slots[m_slot].sizes;
    sizes[0] = renderWidth;
    sizes[1] = renderHeight;
    sizes[2] = outputWidth;
    sizes[3] = outputHeight;
  }

  PassTimers *PassTimers::active() { return s_active; }

  bool PassTimers::reserve(const char *name, uint32_t width, uint32_t height, NSUInteger &start, NSUInteger &end) {
    auto &s = m_slots[m_slot];
    if (!m_recording || (s.ranges.size() + 1) * 2 > sampleCount)
      return false;
    gpu::PassRange range;
    std::strncpy(range.name, name, sizeof(range.name) - 1);
    range.width = width;
    range.height = height;
    range.begin = uint32_t(s.ranges.size() * 2);
    range.end = range.begin + 1;
    start = range.begin;
    end = range.end;
    s.ranges.push_back(range);
    return true;
  }

  void PassTimers::resolve(Slot &slot) {
    slot.pending = false;
    const NSUInteger count = slot.ranges.size() * 2;
    NSData *data = [slot.samples resolveCounterRange:NSMakeRange(0, count)];
    if (!data || data.length < count * sizeof(MTLCounterResultTimestamp))
      return;
    const auto *results = static_cast<const MTLCounterResultTimestamp *>(data.bytes);
    std::vector<uint64_t> stamps(count);
    for (NSUInteger i = 0; i < count; ++i)
      stamps[i] = results[i].timestamp;
    // Nanoseconds per GPU tick: sampleTimestamps' CPU side is in nanoseconds.
    MTLTimestamp cpu = 0, gpu = 0;
    [m_device sampleTimestamps:&cpu gpuTimestamp:&gpu];
    const double period = gpu > m_gpuStart ? double(cpu - m_cpuStart) / double(gpu - m_gpuStart) : 1.0;
    // An encoder that did no GPU work leaves its samples at the error value
    // or at a value unrelated to the frame: a sample further than a second
    // from the frame's median, or before the sample recorded ahead of it,
    // takes its predecessor's value (a zero-length range).
    std::vector<uint64_t> sorted;
    for (NSUInteger i = 0; i < count; ++i)
      if (stamps[i] != MTLCounterErrorValue)
        sorted.push_back(stamps[i]);
    if (sorted.empty())
      return;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const uint64_t median = sorted[sorted.size() / 2];
    const double second = 1e9 / std::max(period, 1e-9);
    uint64_t previous = 0;
    bool started = false;
    for (NSUInteger i = 0; i < count; ++i) {
      const uint64_t s = stamps[i];
      const bool valid = s != MTLCounterErrorValue &&
                         std::abs(double(s) - double(median)) < second && (!started || s >= previous);
      if (valid) {
        previous = s;
        started = true;
      }
      stamps[i] = valid || !started ? s : previous;
    }
    // Leading invalid samples take the first valid one.
    for (NSUInteger i = count; i-- > 0;)
      if (stamps[i] == MTLCounterErrorValue || std::abs(double(stamps[i]) - double(median)) >= second)
        stamps[i] = i + 1 < count ? stamps[i + 1] : median;
    // As Vulkan's bottom-of-pipe timestamps (platform/gpu_timings.h): a pass
    // runs from the end of the work before it to its own end, so consecutive
    // passes tile the frame. An encoder's start sample is taken when it is
    // scheduled, before it waits for earlier work, so only the first pass
    // starts at its own sample.
    for (size_t i = 1; i < slot.ranges.size(); ++i)
      stamps[slot.ranges[i].begin] = stamps[slot.ranges[i - 1].end];
    // The frame spans the first sample to the last (the frame's encoders in order).
    uint32_t first = 0, last = 0;
    for (uint32_t i = 0; i < count; ++i) {
      if (stamps[i] < stamps[first])
        first = i;
      if (stamps[i] > stamps[last])
        last = i;
    }
    m_latest = gpu::resolve(slot.ranges, stamps.data(), first, last, 64, period);
    m_latest.frame = slot.frame;
    m_latest.renderWidth = slot.sizes[0];
    m_latest.renderHeight = slot.sizes[1];
    m_latest.outputWidth = slot.sizes[2];
    m_latest.outputHeight = slot.sizes[3];
  }

  id<MTLComputeCommandEncoder> computeEncoder(id<MTLCommandBuffer> command, NSString *label, uint32_t width,
                                              uint32_t height) {
    NSUInteger start = 0, end = 0;
    auto *timers = PassTimers::active();
    id<MTLComputeCommandEncoder> encoder = nil;
    if (timers && timers->reserve(label.UTF8String, width, height, start, end)) {
      auto *pass = [MTLComputePassDescriptor computePassDescriptor];
      pass.sampleBufferAttachments[0].sampleBuffer = timers->buffer();
      pass.sampleBufferAttachments[0].startOfEncoderSampleIndex = start;
      pass.sampleBufferAttachments[0].endOfEncoderSampleIndex = end;
      encoder = [command computeCommandEncoderWithDescriptor:pass];
    } else
      encoder = [command computeCommandEncoder];
    encoder.label = label;
    return encoder;
  }

  id<MTLBlitCommandEncoder> blitEncoder(id<MTLCommandBuffer> command, NSString *label) {
    NSUInteger start = 0, end = 0;
    auto *timers = PassTimers::active();
    id<MTLBlitCommandEncoder> encoder = nil;
    if (timers && timers->reserve(label.UTF8String, 0, 0, start, end)) {
      auto *pass = [MTLBlitPassDescriptor blitPassDescriptor];
      pass.sampleBufferAttachments[0].sampleBuffer = timers->buffer();
      pass.sampleBufferAttachments[0].startOfEncoderSampleIndex = start;
      pass.sampleBufferAttachments[0].endOfEncoderSampleIndex = end;
      encoder = [command blitCommandEncoderWithDescriptor:pass];
    } else
      encoder = [command blitCommandEncoder];
    encoder.label = label;
    return encoder;
  }

  id<MTLRenderCommandEncoder> renderEncoder(id<MTLCommandBuffer> command, MTLRenderPassDescriptor *pass,
                                            NSString *label, uint32_t width, uint32_t height) {
    NSUInteger start = 0, end = 0;
    auto *timers = PassTimers::active();
    if (timers && timers->reserve(label.UTF8String, width, height, start, end)) {
      // Vertex start to fragment end: the whole pass.
      pass.sampleBufferAttachments[0].sampleBuffer = timers->buffer();
      pass.sampleBufferAttachments[0].startOfVertexSampleIndex = start;
      pass.sampleBufferAttachments[0].endOfVertexSampleIndex = MTLCounterDontSample;
      pass.sampleBufferAttachments[0].startOfFragmentSampleIndex = MTLCounterDontSample;
      pass.sampleBufferAttachments[0].endOfFragmentSampleIndex = end;
    }
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    encoder.label = label;
    return encoder;
  }

  id<MTLAccelerationStructureCommandEncoder> accelerationEncoder(id<MTLCommandBuffer> command, NSString *label) {
    NSUInteger start = 0, end = 0;
    auto *timers = PassTimers::active();
    id<MTLAccelerationStructureCommandEncoder> encoder = nil;
    if (timers && timers->reserve(label.UTF8String, 0, 0, start, end)) {
      auto *pass = [MTLAccelerationStructurePassDescriptor accelerationStructurePassDescriptor];
      pass.sampleBufferAttachments[0].sampleBuffer = timers->buffer();
      pass.sampleBufferAttachments[0].startOfEncoderSampleIndex = start;
      pass.sampleBufferAttachments[0].endOfEncoderSampleIndex = end;
      encoder = [command accelerationStructureCommandEncoderWithDescriptor:pass];
    } else
      encoder = [command accelerationStructureCommandEncoder];
    encoder.label = label;
    return encoder;
  }
  void markTimed(id<MTLCommandBuffer> command, NSString *label) {
    auto *timers = PassTimers::active();
    if (!timers)
      return;
    static id<MTLBuffer> scratch = nil;
    if (!scratch)
      scratch = [command.device newBufferWithLength:16 options:MTLResourceStorageModePrivate];
    id<MTLBlitCommandEncoder> blit = blitEncoder(command, label);
    [blit fillBuffer:scratch range:NSMakeRange(0, 4) value:0];
    [blit endEncoding];
  }
} // namespace remix::metal
