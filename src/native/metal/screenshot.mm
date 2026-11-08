// Screenshots on Metal: runtime/screenshot.cpp itself (requests, sets and
// their encoders), with the readback a blit into a shared buffer.
#include "../runtime/screenshot.cpp"
#include "metal/gpu.h"

namespace remix {
  void PresentedScreenshot::record(void *commandBuffer, void *source, HdrNits nits) {
    if (m_requested.empty() || !m_captured.empty())
      return;
    id<MTLCommandBuffer> command = (__bridge id<MTLCommandBuffer>)commandBuffer;
    id<MTLTexture> texture = (__bridge id<MTLTexture>)source;
    m_half = texture.pixelFormat == MTLPixelFormatRGBA16Float;
    m_bgra = texture.pixelFormat == MTLPixelFormatBGRA8Unorm || texture.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB;
    m_nits = nits;
    const uint32_t width = uint32_t(texture.width), height = uint32_t(texture.height);
    const size_t rowBytes = size_t(width) * (m_half ? 8 : 4), size = rowBytes * height;
    if (!size || size > 512u * 1024u * 1024u) {
      SDL_Log("Screenshot dimensions exceed the readback limit");
      m_requested.clear();
      return;
    }
    m_extent.width = width;
    m_extent.height = height;
    id<MTLBuffer> readback = metal::buffer(command.device, nullptr, size);
    id<MTLBlitCommandEncoder> blit = metal::blitEncoder(command, @"Screenshot readback");
    [blit copyFromTexture:texture
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(width, height, 1)
                        toBuffer:readback
               destinationOffset:0
          destinationBytesPerRow:rowBytes
        destinationBytesPerImage:size];
    [blit endEncoding];
    m_readback = (__bridge_retained void *)readback;
    m_command = (__bridge_retained void *)command;
    m_captured = std::exchange(m_requested, {});
    m_capturedSet = m_set;
  }

  void PresentedScreenshot::complete(bool wait) {
    if (m_captured.empty() || !m_command)
      return;
    id<MTLCommandBuffer> command = (__bridge id<MTLCommandBuffer>)m_command;
    if (wait)
      [command waitUntilCompleted];
    else if (command.status < MTLCommandBufferStatusCompleted)
      return;
    try {
      if (command.status != MTLCommandBufferStatusCompleted)
        throw metal::Error("the frame's command buffer failed");
      id<MTLBuffer> readback = (__bridge id<MTLBuffer>)m_readback;
      write([&](void *pixels, size_t bytes) { std::memcpy(pixels, readback.contents, bytes); });
    } catch (...) {
      SDL_Log("Screenshot readback failed: %s", m_captured.c_str());
    }
    reset();
  }

  void PresentedScreenshot::reset(bool keepRequest) {
    if (m_readback)
      CFRelease(m_readback);
    if (m_command)
      CFRelease(m_command);
    m_readback = m_command = nullptr;
    if (!keepRequest)
      m_requested.clear();
    m_captured.clear();
  }
} // namespace remix
