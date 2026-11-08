#pragma once
#include "runtime/hdr_nits.h"
#include <remix/remix_c.h>
#if !defined(REMIX_METAL)
#include <vulkan/vulkan.h>
#endif
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace remix {
  // The files of a screenshot set (requestSet): an SDR PNG always, and while
  // HDR10 output is active the chosen HDR ones beside it.
  struct ScreenshotFormats {
    bool hdrPng = true, jxr = true, exr = true;
  };

  // One on-demand readback. All GPU access is on the presentation thread;
  // encoding and writing run on a worker of their own. Metal builds record
  // and complete it in metal/screenshot.mm.
  class PresentedScreenshot {
  public:
    PresentedScreenshot() = default;
    PresentedScreenshot(const PresentedScreenshot &) = delete;
    PresentedScreenshot &operator=(const PresentedScreenshot &) = delete;
    ~PresentedScreenshot();
    // One file, by its extension: .png (8-bit SDR; HDR10 tone mapped to SDR as
    // SDR output would show it), .jxr or .exr (scRGB half floats: HDR10's
    // light, or SDR white at 80 nits).
    remixapi_ErrorCode request(const char *path, bool supported);
    // A set at `base` plus suffixes: base.png (SDR), and with HDR10
    // base-hdr.png (16-bit BT.2020 PQ, cICP-tagged), base.jxr and base.exr as
    // `formats` choose.
    remixapi_ErrorCode requestSet(const std::filesystem::path &base, ScreenshotFormats formats, bool supported);
    // `image` in `layout`, which it is left in: 8-bit RGBA or BGRA, or
    // RGBA16F (HDR10's back buffer: gamma 2.2, BT.709, 1.0 at paper white,
    // tone mapped up to `nits`' peak).
#if defined(REMIX_METAL)
    // `texture` (an id<MTLTexture>) copied by `command` (an id<MTLCommandBuffer>).
    void record(void *command, void *texture, HdrNits nits = {});
    void complete(bool wait = false);
    void reset(bool keepRequest = false);
#else
    void record(VkPhysicalDevice physical, VkDevice device, VkCommandBuffer command, VkImage image, VkExtent2D extent,
                VkFormat format, VkImageLayout layout, HdrNits nits = {});
    void submitted(VkFence fence);
    void complete(VkDevice device, bool wait = false);
    void reset(VkDevice device, bool keepRequest = false);
#endif
    // A request the next record() will capture.
    bool pending() const { return !m_requested.empty() && m_captured.empty(); }
    // What the last finished screenshot wrote (or why it failed); empty before one.
    std::string status() const;

  private:
    std::string m_requested, m_captured;
    bool m_set = false, m_capturedSet = false;
    std::filesystem::path m_base; // a set's (m_requested only marks it)
    ScreenshotFormats m_formats;
    HdrNits m_nits;
#if defined(REMIX_METAL)
    void *m_readback = nullptr, *m_command = nullptr; // retained id<MTLBuffer>, id<MTLCommandBuffer>
    struct {
      uint32_t width = 0, height = 0;
    } m_extent;
#else
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkExtent2D m_extent = {};
    VkFence m_fence = VK_NULL_HANDLE;
#endif
    bool m_bgra = false, m_half = false;
    // The captured pixels: `read` copies them into the buffer it is given
    // (bytes long), then a worker encodes and writes them.
    void write(const std::function<void(void *pixels, size_t bytes)> &read);
    // The previous capture's encoder, joined before the next one starts.
    std::thread m_writer;
    mutable std::mutex m_statusMutex;
    std::string m_status;
  };
} // namespace remix
