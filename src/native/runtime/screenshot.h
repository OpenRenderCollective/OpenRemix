#pragma once
#include "runtime/hdr_output.h"
#include <remix/remix_c.h>
#include <vulkan/vulkan.h>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace remix {
  // The files of a screenshot set (requestSet): an SDR PNG always, and while
  // HDR10 output is active the chosen HDR ones beside it.
  struct ScreenshotFormats {
    bool hdrPng = true, jxr = true, exr = true;
  };

  // One on-demand readback. All Vulkan access is on the presentation thread;
  // encoding and writing run on a worker of their own.
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
    void record(VkPhysicalDevice physical, VkDevice device, VkCommandBuffer command, VkImage image,
                VkExtent2D extent, VkFormat format, VkImageLayout layout, HdrNits nits = {});
    void submitted(VkFence fence);
    void complete(VkDevice device, bool wait = false);
    void reset(VkDevice device, bool keepRequest = false);
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
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkExtent2D m_extent = {};
    bool m_bgra = false, m_half = false;
    VkFence m_fence = VK_NULL_HANDLE;
    // The previous capture's encoder, joined before the next one starts.
    std::thread m_writer;
    mutable std::mutex m_statusMutex;
    std::string m_status;
  };
} // namespace remix
