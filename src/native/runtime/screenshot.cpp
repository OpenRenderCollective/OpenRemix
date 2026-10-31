#include "runtime/screenshot.h"
#include "platform/utf8_path.h"
#include "runtime/jxr_writer.h"
#include <SDL3/SDL.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <image/stb_image_write.h>

namespace remix {
  namespace {
    std::filesystem::path nativePath(const char *path) { return utf8Path(path); }
    void check(VkResult result) {
      if (result != VK_SUCCESS)
        throw result;
    }
    // An IEEE half's bits to float.
    float halfFloat(uint16_t h) {
      const uint32_t sign = uint32_t(h & 0x8000u) << 16, exponent = (h >> 10) & 31u, mantissa = h & 0x3ffu;
      uint32_t bits;
      if (exponent == 31)
        bits = sign | 0x7f800000u | mantissa << 13;
      else if (exponent)
        bits = sign | (exponent + 112) << 23 | mantissa << 13;
      else if (mantissa) {
        float value = float(mantissa) / 16777216.f; // 2^-24
        return sign ? -value : value;
      } else
        bits = sign;
      float value;
      std::memcpy(&value, &bits, sizeof(value));
      return value;
    }
    // A float to the nearest IEEE half (ties to even); beyond the range: infinity.
    uint16_t floatHalf(float value) {
      uint32_t bits;
      std::memcpy(&bits, &value, sizeof(bits));
      const uint16_t sign = uint16_t(bits >> 16 & 0x8000u);
      bits &= 0x7fffffffu;
      if (bits >= 0x7f800000u)
        return sign | (bits > 0x7f800000u ? 0x7e00u : 0x7c00u);
      if (bits >= 0x477ff000u) // rounds to 65536 or more
        return sign | 0x7c00u;
      if (bits < 0x38800000u) { // below the smallest normal half: a subnormal
        float magnitude;
        std::memcpy(&magnitude, &bits, sizeof(magnitude));
        return sign | uint16_t(std::nearbyint(magnitude * 16777216.f));
      }
      const uint32_t rounded = bits + 0xfffu + (bits >> 13 & 1u);
      return sign | uint16_t((rounded - 0x38000000u) >> 13);
    }
    using Vec3 = std::array<float, 3>;
    // GLSL mat3 constructors list columns.
    using Mat3 = std::array<float, 9>;
    Vec3 operator*(const Mat3 &m, const Vec3 &v) {
      return {m[0] * v[0] + m[3] * v[1] + m[6] * v[2], m[1] * v[0] + m[4] * v[1] + m[7] * v[2],
              m[2] * v[0] + m[5] * v[1] + m[8] * v[2]};
    }
    // pathtrace/shaders/display.glsl and color.glsl.
    constexpr Mat3 inset = {.72634649f, .13423698f, .07263955f, .16665483f, .70013888f,
                            .06012051f, .10699868f, .16562414f, .86723994f};
    constexpr Mat3 outset = {1.45021560f,  -.25347057f, -.10389769f, -.33533138f, 1.51071102f,
                             -.07664130f, -.11488422f, -.25724045f, 1.18053899f};
    constexpr Mat3 bt709To2020 = {.6274038960f, .0690972894f, .0163914388f, .3292830384f, .9195403951f,
                                  .0880133078f, .0433130657f, .0113623156f, .8955952532f};
    constexpr float anchor = .18f;
    float reinhardPiecewiseSmooth(float x, float peak) {
      if (x <= anchor)
        return x;
      const float range = peak - anchor, t = (x - anchor) / range;
      return anchor + range * t / (1 + t * std::exp(-.01f / (t * t)));
    }
    // HDR10's prism tone curve with its peak (peak / paper white nits) undone
    // and SDR's (peak 1) applied, per channel in the inset space: the SDR
    // image SDR output would have shown. A table over the curve's range above
    // the anchor (identity below it).
    class SdrCurve {
    public:
      explicit SdrCurve(float peak) : m_peak(std::max(peak, 1.f)) {
        if (m_peak <= 1.0001f)
          return;
        m_table.resize(4097);
        for (size_t i = 0; i < m_table.size(); ++i) {
          const double u = double(i) / double(m_table.size() - 1);
          if (u >= 1) {
            m_table[i] = 1;
            continue;
          }
          // The curve's t (x above the anchor over its range) for u: t / (1 + t s(t)) rises from 0 to 1.
          double low = 0, high = 1e9;
          for (int k = 0; k < 100; ++k) {
            const double t = .5 * (low + high);
            (t / (1 + t * std::exp(-.01 / (t * t))) < u ? low : high) = t;
          }
          const double x = anchor + (m_peak - anchor) * .5 * (low + high);
          m_table[i] = reinhardPiecewiseSmooth(float(x), 1);
        }
      }
      float operator()(float y) const {
        if (m_table.empty() || y <= anchor)
          return y;
        const float u = std::min((y - anchor) / (m_peak - anchor), 1.f) * float(m_table.size() - 1);
        const size_t i = std::min(size_t(u), m_table.size() - 2);
        return m_table[i] + (m_table[i + 1] - m_table[i]) * (u - float(i));
      }

    private:
      float m_peak;
      std::vector<float> m_table;
    };
    uint32_t crc32(const unsigned char *data, size_t size, uint32_t crc = 0) {
      static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t n = 0; n < 256; ++n) {
          uint32_t c = n;
          for (int k = 0; k < 8; ++k)
            c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
          t[n] = c;
        }
        return t;
      }();
      crc = ~crc;
      for (size_t i = 0; i < size; ++i)
        crc = table[(crc ^ data[i]) & 255] ^ (crc >> 8);
      return ~crc;
    }
    void putBig32(std::vector<unsigned char> &out, uint32_t v) {
      for (int s = 24; s >= 0; s -= 8)
        out.push_back(uint8_t(v >> s));
    }
    void pngChunk(std::vector<unsigned char> &out, const char *type, const unsigned char *data, size_t size) {
      putBig32(out, uint32_t(size));
      const size_t start = out.size();
      out.insert(out.end(), type, type + 4);
      out.insert(out.end(), data, data + size);
      putBig32(out, crc32(out.data() + start, out.size() - start));
    }
    // A 16-bit RGB PNG of BT.2020 PQ samples, tagged with a cICP chunk
    // (primaries 9 BT.2020, transfer 16 ST.2084, identity matrix, full range):
    // HDR PNG as browsers and HDR-aware viewers read it.
    std::vector<unsigned char> pqPng(const std::vector<uint16_t> &rgb, uint32_t width, uint32_t height) {
      const size_t stride = size_t(width) * 6;
      std::vector<unsigned char> filtered(height * (stride + 1));
      for (uint32_t y = 0; y < height; ++y) {
        unsigned char *row = filtered.data() + y * (stride + 1);
        row[0] = 1; // Sub
        const uint16_t *source = rgb.data() + size_t(y) * width * 3;
        for (size_t i = 0; i < size_t(width) * 3; ++i) {
          const uint16_t v = source[i], left = i >= 3 ? source[i - 3] : 0;
          row[1 + 2 * i] = uint8_t((v >> 8) - (left >> 8));
          row[2 + 2 * i] = uint8_t((v & 255) - (left & 255));
        }
      }
      int size = 0;
      unsigned char *zlib = stbi_zlib_compress(filtered.data(), int(filtered.size()), &size, 6);
      if (!zlib)
        return {};
      std::vector<unsigned char> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
      std::vector<unsigned char> header;
      putBig32(header, width);
      putBig32(header, height);
      header.insert(header.end(), {16, 2, 0, 0, 0}); // 16-bit RGB, deflate, no interlace
      pngChunk(out, "IHDR", header.data(), header.size());
      const unsigned char cicp[] = {9, 16, 0, 1};
      pngChunk(out, "cICP", cicp, sizeof(cicp));
      pngChunk(out, "IDAT", zlib, size_t(size));
      STBIW_FREE(zlib);
      pngChunk(out, "IEND", nullptr, 0);
      return out;
    }
    void putLittle(std::vector<unsigned char> &out, const void *data, size_t size) {
      const auto *bytes = static_cast<const unsigned char *>(data);
      out.insert(out.end(), bytes, bytes + size);
    }
    void exrAttribute(std::vector<unsigned char> &out, const char *name, const char *type, const void *value,
                      uint32_t size) {
      out.insert(out.end(), name, name + std::strlen(name) + 1);
      out.insert(out.end(), type, type + std::strlen(type) + 1);
      putLittle(out, &size, 4);
      putLittle(out, value, size);
    }
    // An uncompressed scanline OpenEXR of half-float RGB (scRGB: linear
    // BT.709 primaries, whiteLuminance 80 nits at 1.0).
    std::vector<unsigned char> scRgbExr(const std::vector<uint16_t> &rgba, uint32_t width, uint32_t height) {
      std::vector<unsigned char> out = {0x76, 0x2f, 0x31, 0x01, 2, 0, 0, 0};
      std::vector<unsigned char> channels;
      for (const char *name : {"B", "G", "R"}) {
        channels.insert(channels.end(), name, name + 2);
        const int32_t half = 1, sampling = 1;
        putLittle(channels, &half, 4);
        channels.insert(channels.end(), {0, 0, 0, 0}); // pLinear, reserved
        putLittle(channels, &sampling, 4);
        putLittle(channels, &sampling, 4);
      }
      channels.push_back(0);
      exrAttribute(out, "channels", "chlist", channels.data(), uint32_t(channels.size()));
      const unsigned char none = 0, increasingY = 0;
      exrAttribute(out, "compression", "compression", &none, 1);
      const int32_t window[] = {0, 0, int32_t(width) - 1, int32_t(height) - 1};
      exrAttribute(out, "dataWindow", "box2i", window, sizeof(window));
      exrAttribute(out, "displayWindow", "box2i", window, sizeof(window));
      exrAttribute(out, "lineOrder", "lineOrder", &increasingY, 1);
      const float aspect = 1, center[] = {0, 0}, windowWidth = 1, white = 80;
      exrAttribute(out, "pixelAspectRatio", "float", &aspect, 4);
      exrAttribute(out, "screenWindowCenter", "v2f", center, sizeof(center));
      exrAttribute(out, "screenWindowWidth", "float", &windowWidth, 4);
      exrAttribute(out, "whiteLuminance", "float", &white, 4);
      out.push_back(0);
      const uint32_t lineBytes = width * 6;
      uint64_t offset = out.size() + uint64_t(height) * 8;
      for (uint32_t y = 0; y < height; ++y, offset += 8 + lineBytes)
        putLittle(out, &offset, 8);
      out.reserve(out.size() + size_t(height) * (8 + lineBytes));
      for (uint32_t y = 0; y < height; ++y) {
        const int32_t line = int32_t(y);
        putLittle(out, &line, 4);
        putLittle(out, &lineBytes, 4);
        const uint16_t *row = rgba.data() + size_t(y) * width * 4;
        for (int c : {2, 1, 0})
          for (uint32_t x = 0; x < width; ++x)
            putLittle(out, &row[size_t(x) * 4 + c], 2);
      }
      return out;
    }
    // Written beside the destination, then renamed over it: readers never
    // see a partial file.
    bool writeFile(const std::filesystem::path &path, const void *data, size_t size) {
      auto temporary = path;
      temporary += ".partial";
      {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(static_cast<const char *>(data), std::streamsize(size));
        output.flush();
        if (!output)
          return false;
      }
      std::error_code ec;
      std::filesystem::rename(temporary, path, ec);
      if (ec)
        std::filesystem::remove(temporary, ec);
      return !ec;
    }
    std::vector<unsigned char> sdrPng(const std::vector<unsigned char> &rgba, uint32_t width, uint32_t height) {
      std::vector<unsigned char> out;
      const auto writer = [](void *context, void *data, int size) {
        auto &bytes = *static_cast<std::vector<unsigned char> *>(context);
        bytes.insert(bytes.end(), static_cast<unsigned char *>(data), static_cast<unsigned char *>(data) + size);
      };
      if (!stbi_write_png_to_func(writer, &out, int(width), int(height), 4, rgba.data(), int(width * 4)))
        out.clear();
      return out;
    }
    struct Job {
      std::filesystem::path base, single;
      bool set = false, half = false;
      ScreenshotFormats formats;
      HdrNits nits;
      uint32_t width = 0, height = 0;
      std::vector<unsigned char> bytes; // 8-bit RGBA
      std::vector<uint16_t> halves;     // RGBA16F
    };
    // Encodes and writes a capture; returns its status line.
    std::string encode(Job &job) {
      const size_t pixels = size_t(job.width) * job.height;
      const auto nits = job.nits.clamped();
      // Linear BT.709 light, 1.0 at paper white (HDR10) or SDR white.
      std::vector<float> light;
      const auto linear = [&] {
        if (!light.empty())
          return;
        light.resize(pixels * 3);
        for (size_t i = 0; i < pixels; ++i)
          for (int c = 0; c < 3; ++c) {
            float v = job.half ? halfFloat(job.halves[i * 4 + c]) : job.bytes[i * 4 + c] / 255.f;
            v = v == v ? v : 0;
            light[i * 3 + c] = std::copysign(std::pow(std::abs(v), 2.2f), v);
          }
      };
      std::vector<std::filesystem::path> written;
      std::string failed;
      const auto save = [&](const std::filesystem::path &path, const std::vector<unsigned char> &data) {
        if (!data.empty() && writeFile(path, data.data(), data.size()))
          written.push_back(path);
        else
          failed += (failed.empty() ? "" : ", ") + path.filename().string();
      };
      const auto withSuffix = [&](const char *suffix) {
        auto path = job.base;
        path += suffix;
        return path;
      };
      const bool sdrWanted = job.set || job.single.extension() == ".png";
      if (sdrWanted) {
        if (job.half) {
          // SDR output's image of the HDR10 frame.
          linear();
          const SdrCurve curve(nits.peak / nits.paperWhite);
          job.bytes.assign(pixels * 4, 255);
          for (size_t i = 0; i < pixels; ++i) {
            Vec3 c = inset * Vec3{light[i * 3], light[i * 3 + 1], light[i * 3 + 2]};
            for (auto &v : c)
              v = curve(v);
            c = outset * c;
            for (int k = 0; k < 3; ++k)
              job.bytes[i * 4 + k] =
                  uint8_t(std::lround(std::pow(std::clamp(c[k], 0.f, 1.f), 1 / 2.2f) * 255));
          }
        }
        save(job.set ? withSuffix(".png") : job.single, sdrPng(job.bytes, job.width, job.height));
      }
      // HDR files: of HDR10's light, or of an SDR frame with its white at 80
      // nits (scRGB's 1.0).
      const float whiteNits = job.half ? nits.paperWhite : 80;
      const bool scRgbWanted = job.set ? job.half && (job.formats.jxr || job.formats.exr)
                                       : job.single.extension() != ".png";
      if (scRgbWanted) {
        linear();
        std::vector<uint16_t> scRgb(pixels * 4);
        const float scale = whiteNits / 80;
        for (size_t i = 0; i < pixels; ++i) {
          for (int c = 0; c < 3; ++c)
            scRgb[i * 4 + c] = floatHalf(light[i * 3 + c] * scale);
          scRgb[i * 4 + 3] = 0x3c00; // 1.0
        }
        const auto jxr = job.set ? withSuffix(".jxr") : job.single;
        if ((job.set && job.formats.jxr) || job.single.extension() == ".jxr") {
          std::string error;
          if (writeJxrScRgb(jxr, job.width, job.height, scRgb.data(), error))
            written.push_back(jxr);
          else {
            SDL_Log("Screenshot JPEG XR: %s", error.c_str());
            failed += (failed.empty() ? "" : ", ") + jxr.filename().string();
          }
        }
        if ((job.set && job.formats.exr) || job.single.extension() == ".exr")
          save(job.set ? withSuffix(".exr") : job.single, scRgbExr(scRgb, job.width, job.height));
      }
      if (job.set && job.half && job.formats.hdrPng) {
        linear();
        std::vector<uint16_t> pq(pixels * 3);
        const auto encodePq = [](float nits) {
          constexpr float m1 = 2610.f / 16384, m2 = 128 * (2523.f / 4096), c1 = 3424.f / 4096,
                          c2 = 32 * (2413.f / 4096), c3 = 32 * (2392.f / 4096);
          const float y = std::pow(std::clamp(nits / 10000, 0.f, 1.f), m1);
          return std::pow((c1 + c2 * y) / (1 + c3 * y), m2);
        };
        for (size_t i = 0; i < pixels; ++i) {
          const Vec3 wide = bt709To2020 * Vec3{light[i * 3], light[i * 3 + 1], light[i * 3 + 2]};
          for (int c = 0; c < 3; ++c)
            pq[i * 3 + c] = uint16_t(std::lround(encodePq(wide[c] * nits.paperWhite) * 65535));
        }
        save(withSuffix("-hdr.png"), pqPng(pq, job.width, job.height));
      }
      std::string status;
      if (!written.empty()) {
        status = "Saved " + written.front().filename().string();
        for (size_t i = 1; i < written.size(); ++i)
          status += ", " + written[i].filename().string();
        status += " in " + written.front().parent_path().string();
      }
      if (!failed.empty()) {
        SDL_Log("Screenshot write failed: %s", failed.c_str());
        status += (status.empty() ? "" : "; ") + std::string("failed: ") + failed;
      }
      return status;
    }
  } // namespace

  PresentedScreenshot::~PresentedScreenshot() {
    if (m_writer.joinable())
      m_writer.join();
  }

  remixapi_ErrorCode PresentedScreenshot::request(const char *path, bool supported) {
    const std::filesystem::path destination = nativePath(path);
    if (!destination.is_absolute())
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    const auto extension = destination.extension();
    if ((extension != ".png" && extension != ".jxr" && extension != ".exr") || !supported)
      return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    if (!m_requested.empty() || !m_captured.empty())
      return REMIXAPI_ERROR_CODE_ALREADY_EXISTS;
    m_requested = path;
    m_set = false;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode PresentedScreenshot::requestSet(const std::filesystem::path &base, ScreenshotFormats formats,
                                                    bool supported) {
    if (!base.is_absolute())
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    if (!supported)
      return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    if (!m_requested.empty() || !m_captured.empty())
      return REMIXAPI_ERROR_CODE_ALREADY_EXISTS;
    m_requested = "screenshot set";
    m_base = base;
    m_set = true;
    m_formats = formats;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  std::string PresentedScreenshot::status() const {
    std::lock_guard lock(m_statusMutex);
    return m_status;
  }

  void PresentedScreenshot::record(VkPhysicalDevice physical, VkDevice device, VkCommandBuffer command,
                                   VkImage image, VkExtent2D extent, VkFormat format, VkImageLayout layout,
                                   HdrNits nits) {
    if (m_requested.empty() || !m_captured.empty())
      return;
    m_half = format == VK_FORMAT_R16G16B16A16_SFLOAT;
    m_nits = nits;
    const VkDeviceSize size = VkDeviceSize(extent.width) * extent.height * (m_half ? 8 : 4);
    if (!size || size > 512u * 1024u * 1024u) {
      SDL_Log("Screenshot dimensions exceed the readback limit");
      m_requested.clear();
      return;
    }
    m_extent = extent;
    m_bgra = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
    const VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                     .size = size,
                                     .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    check(vkCreateBuffer(device, &info, nullptr, &m_buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, m_buffer, &requirements);
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    uint32_t type = properties.memoryTypeCount;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
      if ((requirements.memoryTypeBits & (1u << i)) &&
          (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        type = i;
        break;
      }
    }
    if (type == properties.memoryTypeCount)
      throw VK_ERROR_FEATURE_NOT_PRESENT;
    const VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                             .allocationSize = requirements.size,
                                             .memoryTypeIndex = type};
    check(vkAllocateMemory(device, &allocation, nullptr, &m_memory));
    check(vkBindBufferMemory(device, m_buffer, m_memory, 0));
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                                    .oldLayout = layout,
                                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = image,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    const VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageExtent = {extent.width, extent.height, 1}};
    vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_buffer, 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = layout;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
    const VkBufferMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .buffer = m_buffer,
                                        .size = VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr,
                         1, &host, 0, nullptr);
    m_captured = std::exchange(m_requested, {});
    m_capturedSet = m_set;
  }

  void PresentedScreenshot::submitted(VkFence fence) {
    if (!m_captured.empty() && !m_fence)
      m_fence = fence;
  }

  void PresentedScreenshot::complete(VkDevice device, bool wait) {
    if (m_captured.empty() || !m_fence)
      return;
    const auto ready = wait ? vkWaitForFences(device, 1, &m_fence, VK_TRUE, 1000000000)
                            : vkGetFenceStatus(device, m_fence);
    if (ready != VK_SUCCESS)
      return;
    try {
      Job job;
      job.set = m_capturedSet;
      if (job.set)
        job.base = m_base;
      else
        job.single = nativePath(m_captured.c_str());
      job.half = m_half;
      job.formats = m_formats;
      job.nits = m_nits;
      job.width = m_extent.width;
      job.height = m_extent.height;
      // Allocate before mapping, so allocation failure cannot leave memory mapped.
      const size_t pixels = size_t(m_extent.width) * m_extent.height;
      (m_half ? job.halves.resize(pixels * 4) : job.bytes.resize(pixels * 4));
      void *mapped = nullptr;
      check(vkMapMemory(device, m_memory, 0, VK_WHOLE_SIZE, 0, &mapped));
      const VkMappedMemoryRange range = {
          .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = m_memory, .size = VK_WHOLE_SIZE};
      const auto result = vkInvalidateMappedMemoryRanges(device, 1, &range);
      if (result == VK_SUCCESS) {
        if (m_half)
          std::memcpy(job.halves.data(), mapped, job.halves.size() * sizeof(uint16_t));
        else
          std::memcpy(job.bytes.data(), mapped, job.bytes.size());
      }
      vkUnmapMemory(device, m_memory);
      check(result);
      for (size_t i = 0; i < job.bytes.size(); i += 4) {
        if (m_bgra)
          std::swap(job.bytes[i], job.bytes[i + 2]);
        job.bytes[i + 3] = 255;
      }
      // Encoding takes long enough to show as a hitch: a worker writes the
      // files (the previous capture's first, if it still runs).
      if (m_writer.joinable())
        m_writer.join();
      m_writer = std::thread([this, job = std::move(job)]() mutable {
        std::string status;
        try {
          if (job.set) {
            std::error_code ec;
            std::filesystem::create_directories(job.base.parent_path(), ec);
          }
          status = encode(job);
        } catch (...) {
          status = "Screenshot encoding failed";
          SDL_Log("%s", status.c_str());
        }
        std::lock_guard lock(m_statusMutex);
        m_status = std::move(status);
      });
    } catch (...) {
      SDL_Log("Screenshot readback failed: %s", m_captured.c_str());
    }
    reset(device);
  }

  void PresentedScreenshot::reset(VkDevice device, bool keepRequest) {
    if (m_buffer)
      vkDestroyBuffer(device, m_buffer, nullptr);
    if (m_memory)
      vkFreeMemory(device, m_memory, nullptr);
    m_buffer = VK_NULL_HANDLE;
    m_memory = VK_NULL_HANDLE;
    m_fence = VK_NULL_HANDLE;
    if (!keepRequest)
      m_requested.clear();
    m_captured.clear();
  }
} // namespace remix
