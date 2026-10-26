#include "raster/renderer.h"
#include "runtime/pipelines.h"
#include "scene/fixtures.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <remix/remix_native_ext.h>
#include <thread>

using namespace remix::test;
namespace {
  using remix::RasterRenderer;
  void vk(VkResult result) { require(result == VK_SUCCESS, "Vulkan call"); }

  std::atomic<unsigned> errors = 0;
  VKAPI_ATTR VkBool32 VKAPI_CALL message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                         VkDebugUtilsMessageTypeFlagsEXT,
                                         const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++errors;
      std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
  }

  struct Target {
    RasterRenderer raster;
    std::unique_ptr<remix::pipeline::Service> pipelines;
    VkInstance instanceHandle = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    VkImage color;
    VkImageView view;
    VkDeviceMemory imageMemory, readMemory;
    VkBuffer readBuffer;
    VkRenderPass pass, resumePass;
    VkFramebuffer framebuffer;
    VkCommandPool pool;
    VkCommandBuffer command;
    VkFence fence;
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
      VkPhysicalDeviceMemoryProperties memory;
      vkGetPhysicalDeviceMemoryProperties(physical, &memory);
      for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) {
          return i;
        }
      }
      require(false, "Memory type");
      return 0;
    }
    Target() {
      const bool validation = std::getenv("REMIX_VULKAN_VALIDATION");
      const char *layer = "VK_LAYER_KHRONOS_validation";
      const char *extensions[] = {
          VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
#if defined(__APPLE__)
          VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
#endif
      };
      const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                     .pApplicationName = "Remix raster readback",
                                     .apiVersion = VK_API_VERSION_1_1};
      const VkInstanceCreateInfo create = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
#if defined(__APPLE__)
                                           .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
#endif
                                           .pApplicationInfo = &app,
                                           .enabledLayerCount = validation ? 1u : 0u,
                                           .ppEnabledLayerNames = &layer,
                                           .enabledExtensionCount = uint32_t(std::size(extensions)),
                                           .ppEnabledExtensionNames = extensions};
      vk(vkCreateInstance(&create, nullptr, &instanceHandle));
      const VkDebugUtilsMessengerCreateInfoEXT debug = {
          .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
          .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
          .messageType =
              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
          .pfnUserCallback = message};
      vk(reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(
          instanceHandle, "vkCreateDebugUtilsMessengerEXT"))(instanceHandle, &debug, nullptr, &messenger));
      uint32_t count = 0;
      vk(vkEnumeratePhysicalDevices(instanceHandle, &count, nullptr));
      require(count > 0, "Vulkan device");
      std::vector<VkPhysicalDevice> devices(count);
      vk(vkEnumeratePhysicalDevices(instanceHandle, &count, devices.data()));
      physical = devices[0];
      vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
      std::vector<VkQueueFamilyProperties> families(count);
      vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
      uint32_t family = 0;
      while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
        ++family;
      }
      require(family < count, "Graphics family");
      const float priority = 1;
      const VkDeviceQueueCreateInfo q = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                         .queueFamilyIndex = family,
                                         .queueCount = 1,
                                         .pQueuePriorities = &priority};
#if defined(__APPLE__)
      const char *portability = "VK_KHR_portability_subset";
#endif
      const VkDeviceCreateInfo d = {
          .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
          .queueCreateInfoCount = 1,
          .pQueueCreateInfos = &q,
#if defined(__APPLE__)
          .enabledExtensionCount = 1,
          .ppEnabledExtensionNames = &portability,
#endif
      };
      vk(vkCreateDevice(physical, &d, nullptr, &device));
      vkGetDeviceQueue(device, family, 0, &queue);
      VkPhysicalDeviceProperties properties{};
      vkGetPhysicalDeviceProperties(physical, &properties);
      pipelines =
          std::make_unique<remix::pipeline::Service>(device, properties, false);
      raster.initializeGpu(physical, device);
      const VkImageCreateInfo image = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                       .imageType = VK_IMAGE_TYPE_2D,
                                       .format = VK_FORMAT_R8G8B8A8_UNORM,
                                       .extent = {64, 64, 1},
                                       .mipLevels = 1,
                                       .arrayLayers = 1,
                                       .samples = VK_SAMPLE_COUNT_1_BIT,
                                       .tiling = VK_IMAGE_TILING_OPTIMAL,
                                       .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
      vk(vkCreateImage(device, &image, nullptr, &color));
      VkMemoryRequirements mem;
      vkGetImageMemoryRequirements(device, color, &mem);
      VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                    .allocationSize = mem.size,
                                    .memoryTypeIndex =
                                        memoryType(mem.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
      vk(vkAllocateMemory(device, &alloc, nullptr, &imageMemory));
      vk(vkBindImageMemory(device, color, imageMemory, 0));
      const VkImageViewCreateInfo v = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                       .image = color,
                                       .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                       .format = VK_FORMAT_R8G8B8A8_UNORM,
                                       .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
      vk(vkCreateImageView(device, &v, nullptr, &view));
      const VkAttachmentDescription attachments[] = {
          {.format = VK_FORMAT_R8G8B8A8_UNORM,
           .samples = VK_SAMPLE_COUNT_1_BIT,
           .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
           .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
           .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
           .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
          {.format = raster.depthFormat(),
           .samples = VK_SAMPLE_COUNT_1_BIT,
           .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
           .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
           .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
           .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
      const VkAttachmentReference colorRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                  depthRef = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
      const VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            .colorAttachmentCount = 1,
                                            .pColorAttachments = &colorRef,
                                            .pDepthStencilAttachment = &depthRef};
      const VkSubpassDependency deps[] = {
          {.srcSubpass = VK_SUBPASS_EXTERNAL,
           .dstSubpass = 0,
           .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
           .dstStageMask =
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
           .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
           .dstAccessMask =
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT},
          {.srcSubpass = 0,
           .dstSubpass = VK_SUBPASS_EXTERNAL,
           .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
           .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
           .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
           .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT}};
      const VkRenderPassCreateInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                         .attachmentCount = 2,
                                         .pAttachments = attachments,
                                         .subpassCount = 1,
                                         .pSubpasses = &subpass,
                                         .dependencyCount = 2,
                                         .pDependencies = deps};
      vk(remix::pipeline::createRenderPass(device, &rp, nullptr, &pass));
      {
        // Re-entered after the scene's depth was captured (soft sprites):
        // clears colour, keeps the depth.
        auto resumed = rp;
        VkAttachmentDescription kept[2] = {attachments[0], attachments[1]};
        kept[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        kept[1].initialLayout = kept[1].finalLayout;
        resumed.pAttachments = kept;
        vk(remix::pipeline::createRenderPass(device, &resumed, nullptr, &resumePass));
      }
      raster.configureTarget(pass, {64, 64});
      const VkImageView views[] = {view, raster.depthView()};
      const VkFramebufferCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                          .renderPass = pass,
                                          .attachmentCount = 2,
                                          .pAttachments = views,
                                          .width = 64,
                                          .height = 64,
                                          .layers = 1};
      vk(vkCreateFramebuffer(device, &fb, nullptr, &framebuffer));
      const VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                         .size = 64 * 64 * 4,
                                         .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
      vk(vkCreateBuffer(device, &buffer, nullptr, &readBuffer));
      vkGetBufferMemoryRequirements(device, readBuffer, &mem);
      alloc.allocationSize = mem.size;
      alloc.memoryTypeIndex = memoryType(mem.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
      vk(vkAllocateMemory(device, &alloc, nullptr, &readMemory));
      vk(vkBindBufferMemory(device, readBuffer, readMemory, 0));
      const VkCommandPoolCreateInfo cp = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                          .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                          .queueFamilyIndex = family};
      vk(vkCreateCommandPool(device, &cp, nullptr, &pool));
      const VkCommandBufferAllocateInfo cb = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                              .commandPool = pool,
                                              .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                              .commandBufferCount = 1};
      vk(vkAllocateCommandBuffers(device, &cb, &command));
      const VkFenceCreateInfo f = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      vk(vkCreateFence(device, &f, nullptr, &fence));
    }
    ~Target() {
      vkDeviceWaitIdle(device);
      vkDestroyFramebuffer(device, framebuffer, nullptr);
      raster.shutdownGpu();
      remix::pipeline::destroyRenderPass(device, pass, nullptr);
      remix::pipeline::destroyRenderPass(device, resumePass, nullptr);
      vkDestroyImageView(device, view, nullptr);
      vkDestroyImage(device, color, nullptr);
      vkFreeMemory(device, imageMemory, nullptr);
      vkDestroyBuffer(device, readBuffer, nullptr);
      vkFreeMemory(device, readMemory, nullptr);
      vkDestroyFence(device, fence, nullptr);
      vkDestroyCommandPool(device, pool, nullptr);
      pipelines.reset();
      vkDestroyDevice(device, nullptr);
      reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(
          instanceHandle, "vkDestroyDebugUtilsMessengerEXT"))(instanceHandle, messenger, nullptr);
      vkDestroyInstance(instanceHandle, nullptr);
    }
    void outputEncoding(bool enabled) {
      vk(vkDeviceWaitIdle(device));
      vkDestroyFramebuffer(device, framebuffer, nullptr);
      raster.configureTarget(pass, {64, 64}, enabled);
      const VkImageView views[] = {view, raster.depthView()};
      const VkFramebufferCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                          .renderPass = pass,
                                          .attachmentCount = 2,
                                          .pAttachments = views,
                                          .width = 64,
                                          .height = 64,
                                          .layers = 1};
      vk(vkCreateFramebuffer(device, &fb, nullptr, &framebuffer));
    }
    // captureDepth: as a traced frame does for soft sprites, the scene's depth
    // (here: what the first pass drew) is captured and the frame drawn again.
    std::vector<uint8_t> render(bool captureDepth = false) {
      vk(vkResetCommandBuffer(command, 0));
      const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      vk(vkBeginCommandBuffer(command, &begin));
      while (!raster.preparePipelines(false))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      raster.prepare(command);
      VkClearValue clear[2] = {};
      clear[1].depthStencil.depth = 1;
      const VkRenderPassBeginInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                        .renderPass = pass,
                                        .framebuffer = framebuffer,
                                        .renderArea = {{0, 0}, {64, 64}},
                                        .clearValueCount = 2,
                                        .pClearValues = clear};
      vkCmdBeginRenderPass(command, &rp, VK_SUBPASS_CONTENTS_INLINE);
      raster.record(command);
      vkCmdEndRenderPass(command);
      if (captureDepth) {
        require(raster.wantsSceneDepth(), "Soft sprite asks for the scene depth");
        raster.captureSceneDepth(command);
        // The colour image again as an attachment.
        const VkImageMemoryBarrier back = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                           .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                           .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                           .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                           .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                           .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                           .image = color,
                                           .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &back);
        auto again = rp;
        again.renderPass = resumePass;
        vkCmdBeginRenderPass(command, &again, VK_SUBPASS_CONTENTS_INLINE);
        raster.record(command);
        vkCmdEndRenderPass(command);
      }
      const VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                      .imageExtent = {64, 64, 1}};
      vkCmdCopyImageToBuffer(command, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readBuffer, 1, &copy);
      const VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                       .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                       .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                           &barrier, 0, nullptr, 0, nullptr);
      vk(vkEndCommandBuffer(command));
      vk(vkResetFences(device, 1, &fence));
      const VkSubmitInfo submit = {
          .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
      vk(vkQueueSubmit(queue, 1, &submit, fence));
      vk(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
      void *mapped;
      vk(vkMapMemory(device, readMemory, 0, VK_WHOLE_SIZE, 0, &mapped));
      const VkMappedMemoryRange range = {
          .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = readMemory, .size = VK_WHOLE_SIZE};
      vk(vkInvalidateMappedMemoryRanges(device, 1, &range));
      std::vector<uint8_t> pixels(64 * 64 * 4);
      std::memcpy(pixels.data(), mapped, pixels.size());
      vkUnmapMemory(device, readMemory);
      raster.finishFrame();
      return pixels;
    }
  };
  void pixel(const std::vector<uint8_t> &data, int x, int y, int r, int g, int b, const char *what) {
    auto p = data.data() + (y * 64 + x) * 4;
    if (std::abs(p[0] - r) > 2 || std::abs(p[1] - g) > 2 || std::abs(p[2] - b) > 2) {
      std::fprintf(stderr, "%s: pixel %d,%d = %u,%u,%u; expected %d,%d,%d\n", what, x, y, p[0], p[1], p[2], r,
                   g, b);
      std::exit(1);
    }
  }
  // Sprite overlays (remixapi_InstanceInfoSpriteEXT): a blended raster-only
  // quad over the whole target whose vertices all sample one texel, so every
  // pixel is the shader's value for those texels.
  struct Sprite {
    float uv[2], frame[3], sequence[4], weight2;
    uint32_t color = 0xffffffff;
  };
  remixapi_MeshHandle spriteQuad(RasterRenderer &r, uint64_t hash, remixapi_MaterialHandle mat, const Sprite &sprite) {
    remixapi_HardcodedVertex vertices[4] = {};
    const float positions[4][3] = {{-.8f, -.8f, .5f}, {.8f, -.8f, .5f}, {.8f, .8f, .5f}, {-.8f, .8f, .5f}};
    for (int i = 0; i < 4; ++i) {
      std::copy_n(positions[i], 3, vertices[i].position);
      vertices[i].color = sprite.color;
      std::copy_n(sprite.uv, 2, vertices[i].texcoord);
      std::copy_n(sprite.frame, 3, vertices[i].normal);
      const float extra[5] = {sprite.sequence[0], sprite.sequence[1], sprite.sequence[2], sprite.sequence[3],
                              sprite.weight2};
      uint32_t bits[5];
      std::memcpy(bits, extra, sizeof(bits));
      vertices[i]._pad0 = bits[0], vertices[i]._pad1 = bits[1], vertices[i]._pad2 = bits[2];
      vertices[i]._pad3 = bits[3], vertices[i]._pad4 = bits[4];
    }
    const uint32_t indices[] = {0, 2, 1, 0, 3, 2};
    remixapi_MeshInfoSurfaceTriangles surface = {};
    surface.vertices_values = vertices;
    surface.vertices_count = 4;
    surface.indices_values = indices;
    surface.indices_count = 6;
    surface.material = mat;
    remixapi_MeshInfoSpriteVerticesEXT sprites = {};
    sprites.sType = REMIXAPI_STRUCT_TYPE_MESH_SPRITE_VERTICES_EXT;
    sprites.enabled = 1;
    remixapi_MeshInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.pNext = &sprites;
    info.hash = hash;
    info.surfaces_values = &surface;
    info.surfaces_count = 1;
    remixapi_MeshHandle result;
    r.createMesh(info, result);
    return result;
  }
  // Draws `mesh` as a sprite overlay with Vulkan blend factors src / dst.
  void drawSprite(RasterRenderer &r, remixapi_MeshHandle mesh, uint32_t flags, uint32_t src, uint32_t dst, float z = 0,
                  float softDistance = 0, float fadeStart = 0, float fadeEnd = 0) {
    remixapi_InstanceInfoBlendEXT blend = {};
    blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
    blend.alphaTestCompareOp = 7;
    blend.alphaBlendEnabled = 1;
    blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = src;
    blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = dst;
    blend.writeMask = 15;
    // Colour: texture x vertex; alpha: texture x vertex.
    blend.textureColorOperation = 3, blend.textureColorArg1Source = 1, blend.textureColorArg2Source = 2;
    blend.textureAlphaOperation = 3, blend.textureAlphaArg1Source = 1, blend.textureAlphaArg2Source = 2;
    remixapi_InstanceIdentityEXT identity = {};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.pNext = &blend;
    identity.classification = REMIXAPI_INSTANCE_RASTER_ONLY_EXT;
    identity.rasterVisible = 1;
    remixapi_InstanceInfoSpriteEXT sprite = {};
    sprite.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_SPRITE_EXT;
    sprite.pNext = &identity;
    sprite.flags = flags;
    sprite.softDistance = softDistance;
    sprite.softFadeStart = fadeStart;
    sprite.softFadeEnd = fadeEnd;
    auto info = instance(mesh, z);
    info.pNext = &sprite;
    info.doubleSided = 1;
    r.drawInstance(info);
  }
  void spriteTests(Target &target) {
    auto &r = target.raster;
    camera(r);
    // Four texels in a row (linear values): A, B, C, D at u = 1/8, 3/8, 5/8, 7/8.
    const uint8_t texels[] = {26, 51, 0, 102, /* A */ 0, 153, 0, 204, /* B */ 255, 0, 0, 255, /* C */ 0, 255, 0, 128 /* D */};
    remixapi_TextureInfo tex = {};
    tex.hash = 0x51;
    tex.width = 4;
    tex.height = 1;
    tex.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
    tex.data = texels;
    tex.dataSize = sizeof(texels);
    remixapi_TextureHandle handle;
    r.createTexture(tex, handle);
#if defined(_WIN32)
    const auto sheet = material(r, 0x52, 1, 1, 1, 1, L"0x51");
#else
    const auto sheet = material(r, 0x52, 1, 1, 1, 1, "0x51");
#endif
    constexpr uint32_t one = 1, srcAlpha = 6, invSrcAlpha = 7, dstColor = 4, srcColor = 2;
    const float a[2] = {.125f, .5f}, b[2] = {.375f, .5f}, c[2] = {.625f, .5f}, d[2] = {.875f, .5f};
    uint64_t hash = 0x60;
    const auto shot = [&](const Sprite &sprite, uint32_t flags, uint32_t src = one, uint32_t dst = one) {
      drawSprite(r, spriteQuad(r, hash++, sheet, sprite), flags, src, dst);
      return target.render();
    };
    // Frames C (red) and D (green), additive.
    pixel(shot({{c[0], c[1]}, {d[0], d[1], .25f}}, REMIXAPI_SPRITE_FRAME_BLEND_EXT), 32, 32, 191, 64, 0,
          "Sprite frame blend 1/4");
    pixel(shot({{c[0], c[1]}, {d[0], d[1], .75f}}, REMIXAPI_SPRITE_FRAME_BLEND_EXT), 32, 32, 64, 191, 0,
          "Sprite frame blend 3/4");
    pixel(shot({{c[0], c[1]}, {d[0], d[1], .75f}}, 0), 32, 32, 255, 0, 0, "Sprite without flags: first frame");
    // Alpha over: the blended alpha (1, 128/255 -> 0.749 at 1/2) covers black.
    pixel(shot({{c[0], c[1]}, {d[0], d[1], .5f}}, REMIXAPI_SPRITE_FRAME_BLEND_EXT, srcAlpha, invSrcAlpha), 32, 32, 96,
          96, 0, "Sprite frame blend, alpha over");
    // The brighter frame: 0.59 w against 0.3 (1 - w).
    pixel(shot({{c[0], c[1]}, {d[0], d[1], .25f}}, REMIXAPI_SPRITE_FRAME_MAX_LUMINANCE_EXT), 32, 32, 255, 0, 0,
          "Sprite max-luminance frame, first");
    pixel(shot({{c[0], c[1]}, {d[0], d[1], .75f}}, REMIXAPI_SPRITE_FRAME_MAX_LUMINANCE_EXT), 32, 32, 0, 255, 0,
          "Sprite max-luminance frame, second");
    // Green/alpha images of A (g .2, a .4, r .102) and B (g .6, a .8).
    const auto green = [&](float w, float expected, const char *what) {
      const int value = int(std::lround(expected * 255));
      pixel(shot({{a[0], a[1]}, {b[0], b[1], w}}, REMIXAPI_SPRITE_GREEN_ALPHA_EXT), 32, 32, value, value, value, what);
    };
    green(.1f, .102f * .1f + .2f * .3f + .4f * .7f, "Sprite green/alpha, first pair");
    green(.5f, .102f * .5f + .4f * .5f + .6f * .5f, "Sprite green/alpha, across the frames");
    green(.9f, .102f * .9f + .6f * .7f + .8f * .3f, "Sprite green/alpha, second pair");
    // A second sequence (C, D at 1/4: (.75, .25, 0, .875)) on the first frame A.
    const Sprite second = {{a[0], a[1]}, {b[0], b[1], 0}, {c[0], c[1], d[0], d[1]}, .25f};
    pixel(shot(second, REMIXAPI_SPRITE_SEQUENCE_ADD_EXT), 32, 32, 109, 57, 0, "Sprite second sequence, added");
    pixel(shot(second, REMIXAPI_SPRITE_SEQUENCE_REPLACE_EXT), 32, 32, 191, 64, 0, "Sprite second sequence, replacing");
    pixel(shot(second, REMIXAPI_SPRITE_SEQUENCE_OVER_EXT), 32, 32, 171, 62, 0, "Sprite second sequence, over");
    // The brighter of C and D, weighed by weight1 (0: C).
    pixel(shot(second, REMIXAPI_SPRITE_SEQUENCE_REPLACE_EXT | REMIXAPI_SPRITE_SEQUENCE_MAX_LUMINANCE_EXT), 32, 32, 255,
          0, 0, "Sprite second sequence, brighter frame");
    // Soft depth: a red wall 2.5 from the eye, a white sprite 2.3 from it.
    remixapi_CameraInfoParameterizedEXT params = {};
    params.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
    params.position = {0, 0, -2};
    params.forward = {0, 0, 1};
    params.right = {1, 0, 0};
    params.up = {0, 1, 0};
    params.aspect = 1;
    params.fovYInDegrees = 90;
    params.nearPlane = .1f;
    params.farPlane = 10;
    remixapi_CameraInfo cam = {};
    cam.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    cam.pNext = &params;
    r.setupCamera(cam);
    const auto wall = quad(r, 0x53, material(r, 0x54, 1, 0, 0));
    const auto white = quad(r, 0x55, material(r, 0x56, 1, 1, 1));
    const auto soft = [&](float distance, float start, float end, bool capture) {
      r.drawInstance(instance(wall));
      drawSprite(r, white, REMIXAPI_SPRITE_SOFT_DEPTH_EXT, srcAlpha, invSrcAlpha, -.2f, distance, start, end);
      return target.render(capture);
    };
    pixel(soft(.4f, 0, 0, true), 32, 32, 255, 128, 128, "Soft sprite: half way into its distance");
    pixel(soft(.1f, 0, 0, true), 32, 32, 255, 255, 255, "Soft sprite: beyond its distance");
    pixel(soft(.8f, 0, 0, true), 32, 32, 255, 64, 64, "Soft sprite: a quarter of its distance");
    pixel(soft(.4f, 1, 2, true), 32, 32, 255, 255, 255, "Soft sprite: scene beyond the fade range");
    pixel(soft(.8f, 2, 3, true), 32, 32, 255, 128, 128, "Soft sprite: scene half way through the fade range");
    pixel(soft(.4f, 0, 0, false), 32, 32, 255, 255, 255, "Soft sprite without a captured depth: hard");
    rejected([&] { drawSprite(r, white, REMIXAPI_SPRITE_SOFT_DEPTH_EXT, srcAlpha, invSrcAlpha); });
    rejected([&] { drawSprite(r, white, 0x200, srcAlpha, invSrcAlpha); });
    camera(r);
    // Modulate: grey (display 64/255, alpha 1 or 0) on a mid-grey surface.
    const uint8_t dark[] = {64, 64, 64, 255, 64, 64, 64, 0};
    tex.hash = 0x57;
    tex.width = 2;
    tex.format = REMIXAPI_FORMAT_R8G8B8A8_SRGB;
    tex.data = dark;
    tex.dataSize = sizeof(dark);
    r.createTexture(tex, handle);
#if defined(_WIN32)
    const auto blood = material(r, 0x58, 1, 1, 1, 1, L"0x57");
#else
    const auto blood = material(r, 0x58, 1, 1, 1, 1, "0x57");
#endif
    const auto modulate = [&](remixapi_MaterialHandle ground, float u, uint32_t flags, uint32_t src, uint32_t dst) {
      r.drawInstance(instance(quad(r, hash++, ground), .2f));
      drawSprite(r, spriteQuad(r, hash++, blood, {{u, .5f}}), flags, src, dst);
      return target.render();
    };
    // A target that blends linear values: dst (k src)^2.2.
    const auto half = material(r, 0x59, .5f, .5f, .5f);
    constexpr uint32_t fade = REMIXAPI_SPRITE_MODULATE_EXT | REMIXAPI_SPRITE_MODULATE_FADE_EXT;
    const int mod2x = int(std::lround(.5 * std::pow(2 * 64 / 255., 2.2) * 255));
    const int multiply = int(std::lround(.5 * std::pow(64 / 255., 2.2) * 255));
    pixel(modulate(half, .25f, fade, dstColor, srcColor), 32, 32, mod2x, mod2x, mod2x, "Mod2x sprite, linear target");
    pixel(modulate(half, .75f, fade, dstColor, srcColor), 32, 32, 128, 128, 128, "Mod2x sprite faded out is neutral");
    pixel(modulate(half, .75f, REMIXAPI_SPRITE_MODULATE_EXT, dstColor, srcColor), 32, 32, mod2x, mod2x, mod2x,
          "Mod2x sprite without the fade");
    pixel(modulate(half, .25f, fade, dstColor, 0), 32, 32, multiply, multiply, multiply, "Multiplying sprite");
    pixel(modulate(half, .75f, fade, dstColor, 0), 32, 32, 128, 128, 128, "Multiplying sprite faded out");
    // A target that blends display values: dst x src x 2 as the host has it.
    target.outputEncoding(true);
    const auto display = material(r, 0x5a, .21763764f, .21763764f, .21763764f);
    pixel(modulate(display, .25f, fade, dstColor, srcColor), 32, 32, 64, 64, 64, "Mod2x sprite, display target");
    pixel(modulate(display, .75f, fade, dstColor, srcColor), 32, 32, 128, 128, 128,
          "Mod2x sprite faded out, display target");
    target.outputEncoding(false);
    std::puts("Sprite overlays: frames, green/alpha, sequences, soft depth and modulation passed");
  }
  void gpuTests(const std::filesystem::path &directory) {
    Target target;
    auto &r = target.raster;
    camera(r);
    auto red = material(r, 1, 1, 0, 0), green = material(r, 2, 0, 1, 0), white = material(r, 3, 1, 1, 1);
    auto nearMesh = quad(r, 4, red), farMesh = quad(r, 5, green);
    r.drawInstance(instance(nearMesh, -.2f));
    r.drawInstance(instance(farMesh, .2f));
    pixel(target.render(), 32, 32, 255, 0, 0, "Depth occlusion and front face");
    remixapi_InstanceRasterStateEXT state = {};
    state.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_RASTER_STATE_EXT;
    state.depthTest = 1;
    state.depthWrite = 0;
    state.depthCompare = 3;
    auto stateDraw = instance(nearMesh, -.2f);
    stateDraw.pNext = &state;
    r.drawInstance(stateDraw);
    r.drawInstance(instance(farMesh, .2f));
    pixel(target.render(), 32, 32, 0, 255, 0, "Native depth write disabled");
    state.depthTest = 0;
    state.depthWrite = 1;
    stateDraw = instance(farMesh, .2f);
    stateDraw.pNext = &state;
    r.drawInstance(instance(nearMesh, -.2f));
    r.drawInstance(stateDraw);
    pixel(target.render(), 32, 32, 0, 255, 0, "Native depth test disabled");
    state.depthTest = 1;
    state.depthCompare = 2; // equal
    stateDraw = instance(farMesh, -.2f);
    stateDraw.pNext = &state;
    r.drawInstance(instance(nearMesh, -.2f));
    r.drawInstance(stateDraw);
    pixel(target.render(), 32, 32, 0, 255, 0, "Native equal depth overlay");
    state.depthCompare = 3;
    state.disableColorWrites = 1;
    stateDraw = instance(nearMesh, -.2f);
    stateDraw.pNext = &state;
    r.drawInstance(stateDraw);
    r.drawInstance(instance(farMesh, .2f));
    pixel(target.render(), 32, 32, 0, 0, 0, "Native depth-only prepass");
    state.depthCompare = 8;
    rejected([&] { r.drawInstance(stateDraw); });
    auto backwards = instance(nearMesh);
    backwards.transform.matrix[0][0] = -1;
    r.drawInstance(backwards);
    pixel(target.render(), 32, 32, 0, 0, 0, "Backface culling");
    backwards.doubleSided = 1;
    r.drawInstance(backwards);
    pixel(target.render(), 32, 32, 255, 0, 0, "Double-sided geometry");
    pixel(target.render(), 32, 32, 0, 0, 0, "Empty frame cleared old geometry");
    auto colored = quad(r, 6, white, 0xff0000ff);
    r.drawInstance(instance(colored));
    pixel(target.render(), 32, 32, 0, 0, 255, "ARGB world vertex colors");
    remixapi_CameraInfoParameterizedEXT params = {};
    params.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
    params.position = {0, 0, -2};
    params.forward = {0, 0, 1};
    params.right = {1, 0, 0};
    params.up = {0, 1, 0};
    params.aspect = 1;
    params.fovYInDegrees = 90;
    params.nearPlane = .1f;
    params.farPlane = 10;
    remixapi_CameraInfo cam = {};
    cam.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    cam.pNext = &params;
    r.setupCamera(cam);
    r.drawInstance(instance(colored));
    auto perspective = target.render();
    pixel(perspective, 32, 32, 0, 0, 255, "Parameterized camera");
    pixel(perspective, 10, 32, 0, 0, 0, "Perspective projection extent");
    camera(r);
    cam.type = REMIXAPI_CAMERA_TYPE_VIEW_MODEL;
    r.setupCamera(cam);
    r.drawInstance(instance(farMesh, -.4f));
    auto viewModel = instance(colored);
    viewModel.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL;
    r.drawInstance(viewModel);
    pixel(target.render(), 32, 32, 0, 0, 255, "View-model camera and depth reset");
    auto glass = material(r, 7, 0, 1, 0, .5f);
    auto translucent = quad(r, 8, glass);
    r.drawInstance(instance(nearMesh, .2f));
    r.drawInstance(instance(translucent, -.2f));
    pixel(target.render(), 32, 32, 128, 128, 0, "Alpha blending");
    // Resource replacement and destruction after submission must preserve this frame.
    r.drawInstance(instance(nearMesh));
    material(r, 1, 0, 0, 1);
    r.destroyMesh(nearMesh);
    r.destroyMaterial(red);
    pixel(target.render(), 32, 32, 255, 0, 0, "Queued resource versions");
    auto skinned = quad(r, 9, white, 0xffffffff, true);
    auto d = instance(skinned);
    remixapi_Transform bone = {};
    bone.matrix[0][0] = bone.matrix[1][1] = bone.matrix[2][2] = .25f;
    bone.matrix[0][3] = .5f;
    remixapi_InstanceInfoBoneTransformsEXT skin = {};
    skin.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
    skin.boneTransforms_values = &bone;
    skin.boneTransforms_count = 1;
    d.pNext = &skin;
    r.drawInstance(d);
    bone.matrix[0][3] = -100; // Caller memory may change immediately.
    auto pixels = target.render();
    pixel(pixels, 48, 32, 255, 255, 255, "CPU skinning and deep copy");
    pixel(pixels, 24, 32, 0, 0, 0, "Skinned translation");
    bone.matrix[0][3] = -.5f;
    r.drawInstance(d);
    pixels = target.render();
    pixel(pixels, 16, 32, 255, 255, 255, "New skinned vertices uploaded on the next frame");
    pixel(pixels, 48, 32, 0, 0, 0, "Previous skinned upload was retired");
    remixapi_Transform transforms[2] = {};
    for (int i = 0; i < 2; ++i) {
      transforms[i].matrix[0][0] = transforms[i].matrix[1][1] = transforms[i].matrix[2][2] = .25f;
      transforms[i].matrix[0][3] = i ? .5f : -.5f;
    }
    remixapi_InstanceInfoGpuInstancingEXT instancing = {};
    instancing.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_GPU_INSTANCING_EXT;
    instancing.instanceTransforms_values = transforms;
    instancing.instanceTransforms_count = 2;
    d = instance(colored);
    d.pNext = &instancing;
    r.drawInstance(d);
    transforms[0].matrix[0][3] = 10;
    pixels = target.render();
    pixel(pixels, 16, 32, 0, 0, 255, "Expanded instancing left");
    pixel(pixels, 48, 32, 0, 0, 255, "Expanded instancing right");
    const uint8_t texels[] = {255, 0, 0, 255, 0, 255, 0, 0};
    remixapi_TextureInfo tex = {};
    tex.hash = 10;
    tex.width = 2;
    tex.height = 1;
    tex.format = REMIXAPI_FORMAT_R8G8B8A8_SRGB;
    tex.data = texels;
    tex.dataSize = 8;
    remixapi_TextureHandle th;
    r.createTexture(tex, th);
#if defined(_WIN32)
    auto cutout = material(r, 11, 1, 1, 1, 1, L"0xa", 4);
#else
    auto cutout = material(r, 11, 1, 1, 1, 1, "0xa", 4);
#endif
    auto cutMesh = quad(r, 12, cutout);
    r.drawInstance(instance(cutMesh));
    pixels = target.render();
    pixel(pixels, 16, 32, 255, 0, 0, "Texture orientation");
    pixel(pixels, 48, 32, 0, 0, 0, "Alpha cutout");
    ui(r, 0, 0xff0000ff);
    pixel(target.render(), 32, 32, 255, 0, 0, "RGBA UI vertex colors");
    remixapi_UIVertex inventory[8] = {};
    for (int i = 0; i < 8; ++i) {
      inventory[i].x = (i % 4 == 0 || i % 4 == 3) ? 16.f : 48.f;
      inventory[i].y = i % 4 < 2 ? 16.f : 48.f;
      inventory[i].z = i < 4 ? .2f : .8f;
      inventory[i].color = i < 4 ? 0xff0000ff : 0xff00ff00;
    }
    const uint32_t inventoryIndices[] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    remixapi_UIDrawCommand inventoryCommands[] = {{0, 6, 0, 0, 1}, {0, 6, 6, 0, 1}};
    remixapi_UIDrawList inventoryUi = {};
    inventoryUi.sType = REMIXAPI_STRUCT_TYPE_UI_DRAW_LIST;
    inventoryUi.displayWidth = inventoryUi.displayHeight = 64;
    inventoryUi.pVertices = inventory;
    inventoryUi.vertexCount = 8;
    inventoryUi.pIndices = inventoryIndices;
    inventoryUi.indexCount = 12;
    inventoryUi.pCommands = inventoryCommands;
    inventoryUi.commandCount = 2;
    r.submitUi(&inventoryUi);
    pixel(target.render(), 32, 32, 255, 0, 0, "Inventory model depth");
    inventoryCommands[1].flags = 0;
    r.submitUi(&inventoryUi);
    pixel(target.render(), 32, 32, 0, 255, 0, "Flat UI painter order");
    const uint8_t repeating[] = {255, 0, 0, 255, 0, 255, 0, 255};
    r.registerUiTexture(98, 2, 1, REMIXAPI_FORMAT_R8G8B8A8_UNORM, repeating, sizeof(repeating));
    for (int i = 0; i < 4; ++i) {
      inventory[i].u = (i == 0 || i == 3) ? 0.f : 2.f;
      inventory[i].color = 0xffffffff;
    }
    inventoryUi.commandCount = 1;
    inventoryCommands[0].textureId = 98;
    inventoryCommands[0].flags = 2;
    r.submitUi(&inventoryUi);
    pixel(target.render(), 36, 32, 255, 0, 0, "Native UI repeats texture coordinates");
    inventoryCommands[0].flags = 0;
    r.submitUi(&inventoryUi);
    pixel(target.render(), 36, 32, 0, 255, 0, "Legacy UI clamps texture coordinates");
    const uint8_t grey[] = {128, 128, 128, 255};
    r.registerUiTexture(99, 1, 1, REMIXAPI_FORMAT_R8G8B8A8_UNORM, grey, 4);
    ui(r, 99, 0xffffffff);
    pixel(target.render(), 32, 32, 56, 56, 56, "UI display-color decoding into linear target");
    const uint8_t blue[] = {0, 0, 255, 255};
    r.registerUiTexture(1, 1, 1, REMIXAPI_FORMAT_R8G8B8A8_UNORM, blue, 4);
    ui(r, 1, 0xffffffff, true, .9f);
    r.freeUiTexture(1);
    r.drawInstance(instance(farMesh, -.4f));
    pixels = target.render();
    pixel(pixels, 32, 32, 0, 0, 255, "UI has separate depth and retains freed texture");
    r.overlay(blue, 1, 1, REMIXAPI_FORMAT_R8G8B8A8_UNORM, 1);
    pixels = target.render();
    pixel(pixels, 0, 0, 0, 0, 255, "Screen overlay");
    r.overlay(nullptr, 0, 0, REMIXAPI_FORMAT_R8G8B8A8_UNORM, 0);
    r.setTint(1, 0, 0, .5f);
    pixel(target.render(), 32, 32, 128, 0, 0, "Screen tint");
    r.setTint(0, 0, 0, 0);
    target.outputEncoding(true);
    const auto greyMaterial = material(r, 100, .21763764f, .21763764f, .21763764f);
    r.drawInstance(instance(quad(r, 101, greyMaterial)));
    pixel(target.render(), 32, 32, 128, 128, 128, "UNORM-only output encoding");
    target.outputEncoding(false);
    spriteTests(target);
    camera(r);
    // Persistent scene + UI reference artifact.
    r.drawInstance(instance(farMesh));
    ui(r, 0, 0x800000ff);
    pixels = target.render();
    std::ofstream out(directory / "raster-readback.ppm", std::ios::binary);
    out << "P6\n64 64\n255\n";
    for (size_t i = 0; i < pixels.size(); i += 4) {
      out.write(reinterpret_cast<const char *>(pixels.data() + i), 3);
    }
    std::puts(
        "Raster GPU readback: depth, culling, textures, cutouts, blending, skinning, UI and lifetime passed");
  }
} // namespace
int main(int argc, char **argv) {
  require(argc >= 2, "Supply scratch directory");
  const std::filesystem::path directory(argv[1]);
  std::filesystem::create_directories(directory);
  gpuTests(directory);
  require(!errors.load(), "Vulkan validation errors");
}
