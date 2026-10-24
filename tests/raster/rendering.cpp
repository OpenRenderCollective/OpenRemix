#include "raster/renderer.h"
#include "forward/renderer.h"
#include "forward/bake/controller.h"
#include "forward/probes/authoring.h"
#include "forward/bake/sun_depth.h"
#include "forward/bake/cache_persistence.h"
#include "forward/bake/cache.h"
#include "forward/mixed/directional_fields.h"
#include "forward/lightmap/denoise.h"
#include "forward/ray_scene.h"
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
#include <stdexcept>
#include <remix/remix_native_ext.h>
#include <thread>

using namespace remix::test;
namespace {
  using remix::RasterRenderer;
#include "display_reference.h"
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
    VkExtent2D extent;
    std::unique_ptr<remix::forward::Renderer> forward;
    uint32_t frameIndex = 0, forwardGeneration = 0;
    float exposureSeconds = -1;
    double animationSeconds = -1;
    uint32_t queueFamily = VK_QUEUE_FAMILY_IGNORED;
    bool hardwareRay = false;
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
    Target(uint32_t width = 64, uint32_t height = 64, bool ray = false) : extent{width,height}, hardwareRay(ray) {
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
                                     .apiVersion = ray ? VK_API_VERSION_1_2 : VK_API_VERSION_1_1};
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
      if (ray && !remix::forward::RayScene::supported(physical)) {
        std::puts("SKIP: hardware ray-query baking unavailable");
        std::exit(77);
      }
      vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
      std::vector<VkQueueFamilyProperties> families(count);
      vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
      uint32_t family = 0;
      while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
        ++family;
      }
      require(family < count, "Graphics family");
      queueFamily = family;
      const float priority = 1;
      const VkDeviceQueueCreateInfo q = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                         .queueFamilyIndex = family,
                                         .queueCount = 1,
                                         .pQueuePriorities = &priority};
#if defined(__APPLE__)
      const char *portability = "VK_KHR_portability_subset";
#endif
      VkPhysicalDeviceRayQueryFeaturesKHR query{
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, .rayQuery = VK_TRUE};
      VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
          .pNext = &query, .accelerationStructure = VK_TRUE};
      VkPhysicalDeviceVulkan12Features features{
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
          .pNext = &acceleration, .bufferDeviceAddress = VK_TRUE};
      const char *rayExtensions[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                                     VK_KHR_RAY_QUERY_EXTENSION_NAME,
                                     VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
      VkDeviceCreateInfo d = {
          .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
          .queueCreateInfoCount = 1,
          .pQueueCreateInfos = &q,
#if defined(__APPLE__)
          .enabledExtensionCount = 1,
          .ppEnabledExtensionNames = &portability,
#endif
      };
      if (ray) {
        d.pNext = &features;
        d.enabledExtensionCount = uint32_t(std::size(rayExtensions));
        d.ppEnabledExtensionNames = rayExtensions;
      }
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
                                       .extent = {extent.width, extent.height, 1},
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
      raster.configureTarget(pass, extent);
      const VkImageView views[] = {view, raster.depthView()};
      const VkFramebufferCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                          .renderPass = pass,
                                          .attachmentCount = 2,
                                          .pAttachments = views,
                                          .width = extent.width,
                                          .height = extent.height,
                                          .layers = 1};
      vk(vkCreateFramebuffer(device, &fb, nullptr, &framebuffer));
      const VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                         .size = extent.width * extent.height * 4,
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
      forward.reset();
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
      raster.configureTarget(pass, extent, enabled);
      const VkImageView views[] = {view, raster.depthView()};
      const VkFramebufferCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                          .renderPass = pass,
                                          .attachmentCount = 2,
                                          .pAttachments = views,
                                          .width = extent.width,
                                          .height = extent.height,
                                          .layers = 1};
      vk(vkCreateFramebuffer(device, &fb, nullptr, &framebuffer));
    }
    void enableForward(remix::forward::Options options, bool preserveBaking = false, bool encodeOutput = false) {
      vk(vkDeviceWaitIdle(device));
      const auto baking = preserveBaking && forward ? forward->bakeController() : nullptr;
      const auto reflections = preserveBaking && forward ? forward->reflectionAuthoring() : nullptr;
      const auto exposure = preserveBaking && forward ? forward->exposureState() : nullptr;
      auto next = std::make_unique<remix::forward::Renderer>();
      auto job = pipelines->prepare("forward-test-" + std::to_string(++forwardGeneration), [&] {
          next->initialize(physical,device,pass,extent,options,encodeOutput,pipelines.get(),hardwareRay,baking,queueFamily,exposure,false,reflections);
      });
      while (!job->ready()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      job->check();
      forward = std::move(next);
    }
    // captureDepth: as a traced frame does for soft sprites, the scene's depth
    // (here: what the first pass drew) is captured and the frame drawn again.
    std::vector<uint8_t> render(bool captureDepth = false) {
      if (forward) forward->selectFrame(frameIndex++ % remix::gpuFrameCount);
      vk(vkResetCommandBuffer(command, 0));
      const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      vk(vkBeginCommandBuffer(command, &begin));
      RasterRenderer::DrawOwner owner;
      if (forward) owner = [this](const remix::scene::Draw &draw) { return forward->owns(draw); };
      while (!raster.preparePipelines(false, bool(forward), owner))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      raster.prepare(command,false,bool(forward), owner);
      if (forward) forward->prepare(command,raster,exposureSeconds,animationSeconds);
      VkClearValue clear[2] = {};
      clear[1].depthStencil.depth = 1;
      const VkRenderPassBeginInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                        .renderPass = pass,
                                        .framebuffer = framebuffer,
                                        .renderArea = {{0, 0}, extent},
                                        .clearValueCount = 2,
                                        .pClearValues = clear};
      vkCmdBeginRenderPass(command, &rp, VK_SUBPASS_CONTENTS_INLINE);
      if (forward) {
        forward->background(command);
        raster.record(command,true);
        forward->composite(command);
        raster.record(command,false,true);
      } else raster.record(command);
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
                                      .imageExtent = {extent.width, extent.height, 1}};
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
      std::vector<uint8_t> pixels(extent.width * extent.height * 4);
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
    const auto display = material(r, 0x5a, .21404114f, .21404114f, .21404114f);
    pixel(modulate(display, .25f, fade, dstColor, srcColor), 32, 32, 64, 64, 64, "Mod2x sprite, display target");
    pixel(modulate(display, .75f, fade, dstColor, srcColor), 32, 32, 128, 128, 128,
          "Mod2x sprite faded out, display target");
    target.outputEncoding(false);
    std::puts("Sprite overlays: frames, green/alpha, sequences, soft depth and modulation passed");
  }
  void liveGITests() {
    for (bool reflections : {false, true})
      for (uint32_t samples : {1u, 2u, 4u}) {
        Target target(64, 64, true);
        auto &r = target.raster;
        remix::forward::Options options;
        options.prism = false; // Inspect transport channels before the display transform mixes primaries.
        options.baking.lightmapFitBudget = false; // Controlled-density lighting fixtures.
        options.samples = samples;
        options.rtGI = true;
        options.rtGISamples = 16;
        options.rtGIBounces = 1;
        options.rtGIDistance = 10;
        options.rtReflections = reflections;
        options.water = reflections;
        options.rtReflectionDistance = .1f;
        options.rtReflectionBounces = 3;
        options.ssr = false;
        options.localShadowLights = 0;
        options.shadows = false;
        options.shadowResolution = 64;
        options.ambient = .9f;
        options.baking.reflectionProbes = 0;
        options.baking.maxProbes = 64;
        options.baking.samples = 64;
        options.baking.bounces = 1;
        options.baking.batch = 64;
        options.baking.lightmapPageSize = 64;
        options.baking.lightmapTexels = 4096;
        target.enableForward(options);
        const auto receiverMesh = quad(r, 9500, material(r, 9501, 1, 1, 1));
        remixapi_MaterialInfoTranslucentEXT liquidOptics{};
        liquidOptics.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
        liquidOptics.refractiveIndex = 1.333f;
        liquidOptics.transmittanceColor = {1, 1, 1};
        liquidOptics.transmittanceMeasurementDistance = 1;
        remixapi_MaterialInfo liquidInfo{};
        liquidInfo.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
        liquidInfo.hash = 9520;
        liquidInfo.pNext = &liquidOptics;
        remixapi_MaterialHandle liquidMaterial;
        r.createMaterial(liquidInfo, liquidMaterial);
        const auto liquidMesh = quad(r, 9521, liquidMaterial);
        remixapi_InstanceIdentityEXT identity{};
        identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
        identity.instanceId = 9502;
        identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
        identity.rasterVisible = 1;
        auto receiver = instance(receiverMesh);
        receiver.pNext = &identity;
        const auto retained = r.createRetained(receiver);
        auto dome = [&](uint64_t hash, remixapi_Float3D radiance) {
          remixapi_LightInfoDomeEXT shape{};
          shape.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
          for (uint32_t c = 0; c < 3; ++c)
            shape.transform.matrix[c][c] = 1;
          remixapi_LightInfo info{};
          info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
          info.hash = hash;
          info.pNext = &shape;
          info.radiance = radiance;
          remixapi_LightHandle light;
          r.createLight(info, light);
          return light;
        };
        const auto bakedRed = dome(9503, {1, 0, 0}), liveGreen = dome(9504, {0, 1, 0});
        remixapi_LightInfoDistantEXT distant{};
        distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
        distant.direction = {0, 0, 1};
        remixapi_LightInfo sunInfo{};
        sunInfo.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
        sunInfo.hash = 9510;
        sunInfo.pNext = &distant;
        sunInfo.radiance = {0, 0, 2};
        remixapi_LightHandle blueSun;
        r.createLight(sunInfo, blueSun);
        const float uv[] = {0, 0, 8, 8, 8, 0, 0, 0, 0, 8, 8, 8};
        const remixapi_BakeSurfaceChartEXT chart{retained, 0, 0, 0, 6, 9, 9, uv};
        remixapi_BakeSceneSurfacesInfoEXT request{};
        auto &base = request.bounds.scene;
        base = {sizeof(request),
                REMIXAPI_BAKE_SCENE_SURFACES_VERSION_EXT,
                95,
                1,
                1,
                REMIXAPI_BAKE_SCENE_READY_EXT,
                1,
                1,
                &retained,
                &bakedRed};
        for (uint32_t c = 0; c < 3; ++c) {
          request.bounds.minimum[c] = 10;
          request.bounds.maximum[c] = 11;
        }
        request.chartRevision = request.chartCount = 1;
        request.charts = &chart;
        r.setBakeScene(&base);
        const auto blackMesh = quad(r, 9505, material(r, 9506, 0, 0, 0));
        remixapi_MaterialInfoOpaqueEXT opaque{};
        opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
        opaque.opacityConstant = 1;
        opaque.alphaTestType = 7;
        remixapi_MaterialInfo glow{};
        glow.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
        glow.pNext = &opaque;
        glow.hash = 9507;
        glow.emissiveIntensity = 1;
        glow.emissiveColorConstant = {0, 0, 1};
        remixapi_MaterialHandle blueMaterial;
        r.createMaterial(glow, blueMaterial);
        const auto blueMesh = quad(r, 9508, blueMaterial);
        auto panel = instance(blackMesh, -.7f);
        panel.doubleSided = 1;
        panel.transform.matrix[0][0] = panel.transform.matrix[1][1] = 100;
        identity.instanceId = 9509;
        identity.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
        identity.rasterVisible = 0;
        panel.pNext = &identity;
        auto render = [&](int source = 0, bool direct = false) {
          camera(r);
          r.drawRetained(nullptr);
          if (reflections) {
            // Hidden by the receiver, but prepares the water fragment context.
            // Its .1 m reflection range must not overwrite the 10 m GI range.
            auto liquid = instance(liquidMesh, .2f);
            liquid.transform.matrix[0][0] = liquid.transform.matrix[1][1] = .4f;
            r.drawInstance(liquid);
          }
          r.drawLight(liveGreen);
          if (direct)
            r.drawLight(blueSun);
          if (source) {
            panel.mesh = source == 1 ? blackMesh : blueMesh;
            r.drawInstance(panel);
          }
          return target.render();
        };
        auto ready = [&]() {
          const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
          std::vector<uint8_t> image;
          do {
            image = render();
            if (target.forward->statistics().baking.active)
              return image;
            require(target.forward->statistics().baking.state != REMIXAPI_BAKE_FAILED_EXT,
                    "GI fixture surface bake succeeded");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          } while (std::chrono::steady_clock::now() < deadline);
          require(false, "GI fixture surface lighting published");
          return image;
        };
        auto image = ready();
        const size_t at = (32 * 64 + 32) * 4;
        std::printf("Live GI %ux, reflections %u: RGB %u %u %u\n", samples, unsigned(reflections),
                    unsigned(image[at]), unsigned(image[at + 1]), unsigned(image[at + 2]));
        require(target.forward->statistics().rayGI.active && image[at] < 3 && image[at + 1] > 150 &&
                    image[at + 2] < 3,
                "Live green GI replaces an active red surface lightmap instead of adding to it");
        options.prism = true;
        target.forward->updateLighting(options);
        image = render();
        const auto display = prismDisplay({0, 1, 0});
        for (uint32_t channel = 0; channel < 3; ++channel)
          require(std::abs(int(image[at + channel]) - int(std::lround(display[channel] * 255))) <= 2,
                  "Live GI uses the shared Prism transform after replacing baked irradiance");
        options.prism = false;
        target.forward->updateLighting(options);
        render();
        require(target.forward->statistics().rayGI.sceneBuilds == 0, "Unchanged GI reuses live acceleration");
        image = render(1);
        require(image[at] < 3 && image[at + 1] < 3 && image[at + 2] < 3,
                "Valid black live GI suppresses old baked and ambient fallback lighting");
        image = render(1, true);
        require(
            image[at] < 3 && image[at + 1] < 3 && image[at + 2] > 20 &&
                target.forward->statistics().rayGI.sceneBuilds == 0,
            "Replacing indirect illumination preserves independent direct sunlight without an AS rebuild");
        image = render(2);
        require(image[at] < 3 && image[at + 1] < 3 && image[at + 2] > 150,
                "An offscreen moving emissive actor supplies live diffuse illumination");
        image = render();
        require(image[at] < 3 && image[at + 1] > 150 && image[at + 2] < 3,
                "Removing a live GI occluder/emitter leaves no stale illumination");
        render();
        options.rtGISamples = 32;
        options.rtGIBounces = 2;
        target.forward->updateLighting(options);
        image = render();
        require(image[at + 1] > 150 && target.forward->statistics().rayGI.sceneBuilds == 0,
                "GI sampling budgets update completed slots without rebuilding the live scene");
        options.rtGISamples = 16;
        options.rtGIBounces = 1;
        target.forward->updateLighting(options);
        render(1);
        options.rtGIDistance = .1f;
        target.forward->updateLighting(options);
        image = render(1);
        require(image[at + 1] > 150 && target.forward->statistics().rayGI.sceneBuilds == 0,
                "GI range edits stream separately from geometry and reflection uniforms");
        options.rtGIDistance = 10;
        target.forward->updateLighting(options);
        image = render(1);
        require(image[at + 1] < 3, "Restoring GI range restores current occlusion");
        options.rtGI = false;
        target.enableForward(options);
        image = ready();
        require(image[at] > 150 && image[at + 1] < 3,
                "Disabling GI restores the captured red surface lightmap");
        require(errors.load() == 0, "Live fragment GI is Vulkan validation clean");
      }
    Target ordinary;
    remix::forward::Options options;
    options.baking.lightmapFitBudget = false; // Controlled-density lighting fixtures.
    options.rtGI = true;
    options.ssr = false;
    options.baking.enabled = false;
    options.baking.reflectionProbes = 0;
    options.ambient = 1;
    options.localShadowLights = 0;
    options.shadowResolution = 64;
    ordinary.enableForward(options);
    camera(ordinary.raster);
    ordinary.raster.drawInstance(
        instance(quad(ordinary.raster, 9590, material(ordinary.raster, 9591, 1, 1, 1))));
    const auto image = ordinary.render();
    require(image[(32 * 64 + 32) * 4] > 150 && !ordinary.forward->statistics().rayGI.available &&
                !ordinary.forward->statistics().rayGI.active,
            "Requesting GI without hardware features preserves the ordinary baked/ambient fallback");
  }
  void lightingViewTests() {
    for (uint32_t samples : {1u, 2u, 4u}) {
      Target target(64, 64);
      auto &r = target.raster;
      remix::forward::Options options;
      options.samples = samples;
      options.ambient = .2f;
      options.shadows = options.localShadows = options.ssr = false;
      options.baking.enabled = false;
      options.baking.reflectionProbes = 0;
      target.enableForward(options);
      remixapi_MaterialInfoOpaqueEXT opaque{};
      opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
      opaque.albedoConstant = {.8f, .2f, .05f};
      opaque.opacityConstant = 1;
      opaque.alphaTestType = 7;
      opaque.roughnessConstant = .5f;
      remixapi_MaterialInfo info{};
      info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
      info.pNext = &opaque;
      info.hash = 9790;
      info.emissiveColorConstant = {1, 0, 0};
      info.emissiveIntensity = 2;
      remixapi_MaterialHandle mat;
      r.createMaterial(info, mat);
      const auto mesh = quad(r, 9791, mat);
      remixapi_LightInfoDistantEXT sun{};
      sun.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
      sun.direction = {0, 0, 1};
      remixapi_LightInfo light{};
      light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
      light.pNext = &sun;
      light.hash = 9792;
      light.radiance = {0, 0, 6};
      remixapi_LightHandle sunHandle, localHandle;
      r.createLight(light, sunHandle);
      remixapi_LightInfoSphereEXT sphere{};
      sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
      sphere.position = {0, 0, -1};
      sphere.radius = .1f;
      light.pNext = &sphere;
      light.hash = 9793;
      light.radiance = {0, 100, 0};
      r.createLight(light, localHandle);
      auto render = [&] {
        camera(r);
        r.drawLight(sunHandle);
        r.drawLight(localHandle);
        r.drawInstance(instance(mesh));
        return target.render();
      };
      const auto shaded = render();
      const size_t at = (32 * 64 + 32) * 4;
      for (uint32_t view : {1u, 2u, 3u, 4u, 5u, 6u}) {
        options.debugView = view;
        target.enableForward(options, true);
        const auto image = render();
        if (view == 1 || view == 5)
          require(image[at] > 40 && std::abs(int(image[at]) - int(image[at + 1])) <= 1 &&
                      std::abs(int(image[at]) - int(image[at + 2])) <= 1,
                  "Neutral indirect view excludes material tint, emission and direct lights");
        else if (view == 4 || view == 6) {
          const auto expected = view == 6 ? prismDisplay({1, 0, 0}) : prismDisplay({.8f, .2f, .05f});
          pixel(image, 32, 32, int(std::round(expected[0] * 255)), int(std::round(expected[1] * 255)),
                int(std::round(expected[2] * 255)),
                view == 6 ? "Coverage marks an unbaked receiver red despite ambient and direct lighting"
                          : "Unlit albedo retains material colour without ambient, coloured lights or emission");
        } else
          require(image[at] < 3 && image[at + 1] > 5 && image[at + 2] > 5,
                  "Direct diffuse/specular views isolate green local and blue sun light from red emission/ambient");
      }
      options.debugView = 0;
      target.enableForward(options, true);
      require(render() == shaded, "Leaving diagnostics restores identical shaded output after shader specialization");
      const auto dryTargets = target.forward->statistics().targetBytes;
      options.water = false;
      target.enableForward(options, true);
      require(render() == shaded && target.forward->statistics().targetBytes == dryTargets,
              "Default water support preserves dry-scene pixels and allocates no full-screen snapshot images");
    }
  }
#include "finite_cone.h"
  void mixedLightingTests(bool localShadows = false, bool directDiffuse = false, bool styled = false,
                          bool aggregateDiffuse = false) {
    for (uint32_t samples : {1u, 2u, 4u}) {
      Target target(64, 64, true);
      auto &r = target.raster;
      remix::forward::Options options;
      options.baking.lightmapFitBudget = false; // Controlled-density lighting fixtures.
      options.samples = samples;
      options.ambient = 0;
      options.shadows = false;
      options.ssr = samples == 2;
      options.shadowResolution = 128;
      options.localShadows = localShadows;
      options.localShadowLights = localShadows ? 1 : 0;
      options.localShadowResolution = 512;
      options.localShadowFade = 0; // Spatial fixtures use immediate selection; elapsed transitions are tested separately.
      options.baking.maxProbes = 64;
      options.baking.spacing = 2;
      options.baking.samples = 256;
      options.baking.aggregateDiffuse = aggregateDiffuse;
      if (aggregateDiffuse)
        options.baking.samples = 2048;
      options.baking.bounces = 1;
      options.baking.batch = 16;
      options.baking.reflectionProbes = 0;
      options.baking.lightmapPageSize = 64;
      options.baking.lightmapTexels = 4096;
      options.baking.diskCache = false; // the initial cancellation must exercise real GPU work
      target.enableForward(options);
      remixapi_MaterialInfoOpaqueEXT metal{};
      metal.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
      metal.albedoConstant = {1, 1, 1};
      metal.opacityConstant = 1;
      metal.roughnessConstant = .45f;
      metal.metallicConstant = directDiffuse ? 0 : 1;
      metal.alphaTestType = 7;
      remixapi_MaterialInfo materialInfo{};
      materialInfo.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
      materialInfo.pNext = &metal;
      materialInfo.hash = 9700;
      remixapi_MaterialHandle materialHandle;
      r.createMaterial(materialInfo, materialHandle);
      const auto mesh = quad(r, 9701, materialHandle);
      remixapi_InstanceIdentityEXT identity{};
      identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      identity.instanceId = 9702;
      identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
      identity.rasterVisible = 1;
      auto receiver = instance(mesh);
      receiver.pNext = &identity;
      const auto retained = r.createRetained(receiver);
      const auto black = quad(r, 9703, material(r, 9704, 0, 0, 0));
      auto blocker = instance(black, -.4f);
      blocker.doubleSided = 1;
      blocker.transform.matrix[0][0] = .5f;
      blocker.transform.matrix[0][3] = -.4f;
      blocker.transform.matrix[1][1] = 3;
      identity.instanceId = 9705;
      identity.rasterVisible = 0;
      blocker.pNext = &identity;
      const uint64_t instances[] = {retained, r.createRetained(blocker)};
      remixapi_LightInfoSphereEXT sphere{};
      sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
      sphere.position = {0, 0, -.8f};
      sphere.radius = .3f;
      remixapi_LightInfo info{};
      info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
      info.pNext = &sphere;
      info.hash = 9706;
      info.radiance = {directDiffuse ? 8.f : 100.f, 0, 0};
      remixapi_LightHandle red;
      r.createLight(info, red);
      remixapi_LightHandle authoredRed = nullptr;
      if (styled) {
        info.hash = 9710;
        r.createLight(info, authoredRed);
        info.radiance = {};
        r.updateLight(red, info); // initially off, authored definition remains fully on
      }
      info.hash = 9707;
      info.isDynamic = 1;
      info.radiance = {0, directDiffuse ? 3.f : 40.f, 0};
      remixapi_LightHandle green;
      r.createLight(info, green);
      const float uv[] = {0, 0, 8, 8, 8, 0, 0, 0, 0, 8, 8, 8};
      const remixapi_BakeSurfaceChartEXT chart{retained, 0, 0, 0, 6, 9, 9, uv};
      remixapi_BakeSceneSurfacesInfoEXT request{};
      auto &base = request.bounds.scene;
      base = {sizeof(request),
              REMIXAPI_BAKE_SCENE_SURFACES_VERSION_EXT,
              97,
              1,
              1,
              REMIXAPI_BAKE_SCENE_READY_EXT,
              2,
              1,
              instances,
              &red};
      for (uint32_t c = 0; c < 3; ++c) {
        request.bounds.minimum[c] = 10;
        request.bounds.maximum[c] = 11;
      }
      request.chartRevision = request.chartCount = 1;
      request.charts = &chart;
      remixapi_BakeSceneStyledLightsInfoEXT styledRequest{};
      const remixapi_BakeStyledLightEXT styledInput{authoredRed, red, 32, 0};
      auto publish = [&] {
        if (!styled) { r.setBakeScene(&base); return; }
        styledRequest.reflections.surfaces = request;
        auto &styledBase = styledRequest.reflections.surfaces.bounds.scene;
        styledBase.structSize = sizeof(styledRequest);
        styledBase.version = REMIXAPI_BAKE_SCENE_STYLED_LIGHTS_VERSION_EXT;
        styledBase.lightCount = 0;
        styledBase.lights = nullptr;
        styledRequest.styledLightCount = 1;
        styledRequest.styledLights = &styledInput;
        r.setBakeScene(&styledBase);
      };
      publish();
      bool redExists = true;
      auto render = [&](bool dynamic) {
        camera(r);
        r.drawRetained(nullptr);
        if (redExists)
          r.drawLight(red);
        if (dynamic)
          r.drawLight(green);
        return target.render();
      };
      auto until = [&](const char *phase, bool ready) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        std::vector<uint8_t> pixels;
        do {
          pixels = render(false);
          const auto &stats = target.forward->statistics().baking;
          if ((ready && stats.state == REMIXAPI_BAKE_READY_EXT) ||
              (!ready && std::strstr(stats.status, phase)))
            return pixels;
          if (stats.state == REMIXAPI_BAKE_FAILED_EXT)
            std::fprintf(stderr, "%s\n", stats.status);
          require(stats.state != REMIXAPI_BAKE_FAILED_EXT, "Mixed visibility bake succeeded");
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        require(false, "Mixed visibility bake reached the requested phase");
        return pixels;
      };
      // Cancel this newly introduced dispatch mode while its GPU work is in flight.
      until("Baking per-light static visibility", false);
      r.setBakeScene(nullptr);
      for (uint32_t i = 0; i < remix::gpuFrameCount + 2; ++i)
        render(false);
      require(!target.forward->statistics().baking.active,
              "Cancelled direct-mask work cannot publish a stale volume");
      options.baking.diskCache = true;
      target.forward->updateLighting(options);
      ++base.revision;
      publish();
      auto pixels = until("", true);
      const size_t left = (32 * 64 + 16) * 4, right = (32 * 64 + 48) * 4;
      if (styled) {
        const auto frozen = r.bakeScene();
        const auto hash = target.forward->statistics().baking.activeHash;
        require(pixels[left] < 3 && pixels[right] < 3,
                "Initially-off styled light leaves neither direct light nor permanent shared indirect glow");
        auto setStyle = [&](float redValue, float blueValue = 0) {
          info.isDynamic = 0;
          info.radiance = {redValue, 0, blueValue};
          r.updateLight(red, info);
          publish();
          require(r.bakeScene() == frozen, "Live style intensity and colour reuse captured input identity");
          return render(false);
        };
        pixels = setStyle(8);
        const auto lit = pixels[right];
        require(pixels[left] < 3 && lit > 80 && target.forward->statistics().baking.activeHash == hash,
                "Turning on a styled light retains baked static wall occlusion and unit direct response");
        pixels = setStyle(4);
        require(pixels[left] < 3 && pixels[right] > 3 && pixels[right] < lit,
                "Partial style intensity scales direct lighting without rebaking");
        pixels = setStyle(0, 8);
        require(pixels[left + 2] < 3 && pixels[right + 2] > 80 && pixels[right] < 3,
                "Live styled colour changes preserve baked visibility and direct coefficient ownership");
        pixels = setStyle(0);
        require(pixels[right] < 3 && pixels[right + 2] < 3,
                "Switching off removes styled lighting with no baked RGB residue");
        pixels = setStyle(8);
        auto moving = instance(black, -.4f);
        moving.doubleSided = 1;
        moving.transform.matrix[0][0] = .15f;
        moving.transform.matrix[0][3] = .35f;
        moving.transform.matrix[1][1] = .04f;
        identity.instanceId = 9790;
        moving.pNext = &identity;
        const auto actor = r.createRetained(moving);
        pixels = render(false);
        require(pixels[right] < lit / 3 && pixels[left] < 3,
                "A styled baked light still receives independent current-object mixed shadows");
        r.destroyRetained(&actor, 1);
        pixels = render(false);
        require(std::abs(int(pixels[right]) - int(lit)) < 4,
                "Styled mixed shadows clear when their dynamic caster is removed");
        r.setBakeScene(nullptr);
        for (uint32_t frame = 0; frame < remix::gpuFrameCount + 2; ++frame) render(false);
        publish();
        pixels = until("", true);
        require(pixels[left] < 3 && std::abs(int(pixels[right]) - int(lit)) < 4 &&
                target.forward->statistics().baking.activeHash == hash,
                "Disk-cached styled visibility and unit direct data retain the same runtime identity");
        require(errors.load() == 0, "Styled mixed-light rendering is Vulkan validation clean");
        continue;
      }
      std::printf("Mixed mask %ux initial: left red %u, right red %u\n", samples, unsigned(pixels[left]),
                  unsigned(pixels[right]));
      require(
          pixels[left] < 3 && pixels[right] > 80,
          "Baked static visibility shadows direct metallic specular without a diffuse lightmap contribution");
      if (aggregateDiffuse) {
        const auto aggregate = pixels[right];
        options.baking.aggregateDiffuse = false;
        target.forward->updateLighting(options);
        pixels = until("", true);
        std::printf("Aggregate direct %ux: aggregate %u, attributed %u\n", samples,
                      unsigned(aggregate), unsigned(pixels[right]));
        require(pixels[left] < 3 && std::abs(int(pixels[right]) - int(aggregate)) < 15,
                "Published aggregate diffuse replaces attributed direct lighting without doubling energy");
        options.baking.aggregateDiffuse = true;
        target.forward->updateLighting(options);
        pixels = until("", true);
        const auto beforePreview = options.baking;
        options.baking.aggregateDiffuse = false;
        options.bakedLighting = true;
        options.bakedHighlightLights = 0;
        options.localShadows = false;
        target.forward->updateLighting(options);
        pixels = until("", true);
        const auto previewVolume = target.forward->bakeController()->volume();
        require(!options.baking.aggregateDiffuse && !options.selectedBakedLights &&
                    previewVolume->mixedLights->bakedDiffuse(0) &&
                    previewVolume->mixedLights->retainsVisibility(0) &&
                    target.forward->statistics().clusteredLights == 0 &&
                    target.forward->statistics().fullyBakedLights == 1 &&
                    target.forward->statistics().selectedHighlightLights == 0 &&
                    target.forward->statistics().selectedMixedShadowLights == 0 && pixels[left] < 3 && pixels[right] > 80,
                "Grouped preview publishes aggregate data and selected responses without editing raw preferences");
        options.bakedLighting = false;
        options.baking = beforePreview;
        options.localShadows = localShadows;
        target.forward->updateLighting(options);
        pixels = until("", true);
        require(target.forward->statistics().clusteredLights == 1 && pixels[left] < 3 && pixels[right] > 80,
                "Disabling grouped preview restores the requested aggregate mixed path");
        const auto aggregateVolume = target.forward->bakeController()->volume();
        options.selectedBakedLights = true;
        options.localShadows = false;
        target.forward->updateLighting(options);
        pixels = render(false);
        require(target.forward->statistics().clusteredLights == 0 &&
                    target.forward->statistics().fullyBakedLights == 1 && pixels[left] < 3 && pixels[right] > 80 &&
                    target.forward->bakeController()->volume() == aggregateVolume,
                "Unselected aggregate lights use shared diffuse without rebuilding or entering clusters");
        options.debugView = 3;
        target.enableForward(options, true);
        pixels = render(false);
        auto specularPeak = [](const std::vector<uint8_t> &image) {
          uint8_t peak = 0;
          for (size_t pixel = 0; pixel < image.size(); pixel += 4)
            peak = std::max(peak, image[pixel]);
          return peak;
        };
        require(specularPeak(pixels) == 0,
                "An unselected aggregate light leaves specular to the baked environment");
        options.bakedLighting = true;
        options.bakedHighlightLights = 16;
        options.bakedHighlightFade = 0;
        target.forward->updateLighting(options);
        pixels = until("", true);
        require(specularPeak(pixels) > 0 && pixels[left] < 3 &&
                    target.forward->statistics().clusteredLights == 1 &&
                    target.forward->statistics().selectedAggregateLights == 1 &&
                    target.forward->statistics().selectedHighlightLights == 1 &&
                    target.forward->statistics().selectedMixedShadowLights == 0 &&
                    target.forward->statistics().analyticLights == 0 &&
                    target.forward->statistics().aggregateLights == 0 &&
                    target.forward->statistics().requiredMixedLights == 0 &&
                    target.forward->statistics().shadowedLights == 0,
                "Independent preview highlights retain baked specular without any shadow-map slots");
        const auto independentHighlight = pixels;
        options.bakedLighting = false;
        options.selectedBakedLights = false;
        target.forward->updateLighting(options);
        pixels = until("", true);
        require(pixels == independentHighlight,
                "Fully selected independent highlights match unrestricted aggregate specular");
        options.selectedBakedLights = true;
        options.localShadows = localShadows;
        options.localShadowFade = 2;
        target.enableForward(options, true);
        pixels = render(false);
        require(specularPeak(pixels) == 0 && target.forward->statistics().clusteredLights == 1 &&
                    target.forward->statistics().selectedHighlightLights == 0 &&
                    target.forward->statistics().selectedMixedShadowLights == 0,
                "A newly selected aggregate light fades in its analytic highlight with its shadow slot");
        options.localShadowFade = 0;
        target.forward->updateLighting(options);
        pixels = render(false);
        std::printf("Selected aggregate specular %ux: peak %u\n", samples, unsigned(specularPeak(pixels)));
        require(specularPeak(pixels) > 0 && target.forward->statistics().selectedHighlightLights == 1 &&
                    target.forward->statistics().selectedMixedShadowLights == 1,
                "A selected aggregate light retains its direct analytic highlight");
        const auto selectedHighlight = pixels;
        options.selectedBakedLights = false;
        target.forward->updateLighting(options);
        require(render(false) == selectedHighlight && target.forward->statistics().selectedHighlightLights == 0 &&
                    target.forward->statistics().selectedMixedShadowLights == 0,
                "A fully selected aggregate highlight matches unrestricted mixed specular exactly");
        options.selectedBakedLights = true;
        target.forward->updateLighting(options);
        options.debugView = 0;
        target.enableForward(options, true);
        pixels = render(false);
        require(pixels[left] < 3 && std::abs(int(pixels[right]) - int(aggregate)) < 4,
                "Selected aggregate lighting retains static visibility and direct diffuse energy");
        auto selectedCaster = instance(black, -.4f);
        selectedCaster.doubleSided = 1;
        selectedCaster.transform.matrix[0][0] = .15f;
        selectedCaster.transform.matrix[0][3] = .35f;
        selectedCaster.transform.matrix[1][1] = .04f;
        auto selectedIdentity = identity;
        selectedIdentity.instanceId = 9786;
        selectedIdentity.rasterVisible = 0;
        selectedCaster.pNext = &selectedIdentity;
        const auto selectedActor = r.createRetained(selectedCaster);
        pixels = render(false);
        require(pixels[right] < aggregate / 3 && target.forward->statistics().staticShadowUpdates == 0,
                "Selected aggregate correction subtracts its moving shadow without redrawing frozen geometry");
        r.destroyRetained(&selectedActor, 1);
        pixels = render(false);
        require(std::abs(int(pixels[right]) - int(aggregate)) < 4,
                "Removing a selected aggregate caster restores baked diffuse without trails");
        options.selectedBakedLights = false;
        target.forward->updateLighting(options);
        options.baking.fullyBakedLights = true;
        target.forward->updateLighting(options);
        pixels = until("", true);
        const auto fullyBaked = target.forward->bakeController()->volume();
        require(target.forward->statistics().clusteredLights == 0 &&
                    target.forward->statistics().fullyBakedLights == 1,
                "Fully static lights leave the clustered list while retaining fallback records");
        require(fullyBaked && fullyBaked->mixedLights &&
                    fullyBaked->mixedLights->directLighting.lights[0] ==
                        remix::forward::bake::DirectMode::Baked &&
                    !fullyBaked->mixedLights->retainsVisibility(0) &&
                    pixels[left] < 3 && pixels[right] > 80,
                "Fully static direct lighting comes from the aggregate without mixed visibility data");
        if (localShadows) {
          pixels = render(true);
          const auto greenLit = pixels[right + 1];
          std::printf("Fully baked/mixed priority %ux: lit RGB %u %u %u\n", samples,
                      pixels[right], pixels[right + 1], pixels[right + 2]);
          require(greenLit > 20, "Dimmer mixed light contributes beside the brighter fully baked light");
          auto shadowCaster = instance(black, -.4f);
          shadowCaster.doubleSided = 1;
          shadowCaster.transform.matrix[0][0] = .15f;
          shadowCaster.transform.matrix[0][3] = .35f;
          shadowCaster.transform.matrix[1][1] = .04f;
          identity.instanceId = 9788;
          identity.rasterVisible = 0; // invisible/offscreen casters still need their mixed shadow
          shadowCaster.pNext = &identity;
          const auto caster = r.createRetained(shadowCaster);
          pixels = render(true);
          require(pixels[right + 1] < greenLit / 3 && pixels[right] > 80 &&
                      target.forward->statistics().staticShadowUpdates == 0,
                  "One shadow slot follows the mixed light while fully baked diffuse remains intact");
          r.destroyRetained(&caster, 1);
          pixels = render(true);
          require(std::abs(int(pixels[right + 1]) - int(greenLit)) < 4,
                  "Removing the caster restores mixed light without a stale fallback map");
          pixels = render(false);
        }
        {
          auto blueInfo = info;
          blueInfo.hash = 9796;
          blueInfo.isDynamic = 1;
          blueInfo.radiance = {0, 0, 3};
          remixapi_LightHandle blue{};
          r.createLight(blueInfo, blue);
          options.localShadows = false;
          options.clusterLights = 1;
          target.forward->updateLighting(options);
          auto overflow = [&](bool submitBaked) {
            camera(r);
            r.drawRetained(nullptr);
            if (submitBaked) r.drawLight(red);
            r.drawLight(green);
            r.drawLight(blue);
            return target.render();
          };
          const auto withBakedTail = overflow(true);
          require(target.forward->statistics().clusteredLights == 2 &&
                      target.forward->statistics().fullyBakedLights == 1,
                  "Light diagnostics distinguish mixed work from the immutable baked tail");
          const auto withoutBakedTail = overflow(false);
          require(target.forward->statistics().baking.active,
                  "Omitting a fully baked runtime record preserves the immutable bake");
          for (size_t component = 0; component < withBakedTail.size(); ++component)
            require(std::abs(int(withBakedTail[component]) - int(withoutBakedTail[component])) <= 1,
                    "Cluster overflow visits every mixed light without reevaluating the fully baked tail");
          r.destroyLight(blue);
          options.clusterLights = 0;
          options.localShadows = localShadows;
          target.forward->updateLighting(options);
        }
        camera(r);
        r.drawInstance(instance(mesh)); // no chart; the fixture's probe bounds are at 10..11
        r.drawLight(red);
        pixels = target.render();
        require(target.forward->statistics().baking.active && pixels[left] < 3 && pixels[right] > 80,
                "An uncovered receiver keeps analytic light and frozen-world occlusion beside an active bake");
        if (target.forward->statistics().rayGI.available) {
          options.rtGI = true;
          options.localShadows = false;
          target.enableForward(options, true);
          pixels = until("", true);
          require(target.forward->statistics().rayGI.active && pixels[left] < 3 && pixels[right] > 80 &&
                      target.forward->statistics().fullyBakedLights == 0,
                  "Live GI restores attributed static occlusion without fallback shadow maps");
          require(options.baking.fullyBakedLights && options.baking.aggregateDiffuse,
                  "Live GI leaves requested raster bake preferences intact");
          options.rtGI = false;
          options.localShadows = localShadows;
          target.enableForward(options, true);
          pixels = until("", true);
          require(target.forward->statistics().fullyBakedLights == 1 && pixels[left] < 3 && pixels[right] > 80,
                  "Returning from live GI restores fully baked lighting and static occlusion");
        }
        options.debugView = 3;
        target.enableForward(options, true);
        pixels = render(false);
        require(pixels[right] < 3 && pixels[right + 1] < 3 && pixels[right + 2] < 3,
                "Fully baked receivers do not reevaluate static analytic specular");
        options.baking.enabled = false;
        options.debugView = 0;
        target.enableForward(options, true);
        for (uint32_t frame = 0; frame <= remix::gpuFrameCount; ++frame) pixels = render(false);
        require(pixels[right] > 80,
                "Removing baked coverage restores analytic direct lighting fallback");
        options.debugView = 0;
        options.baking.enabled = true;
        options.baking.fullyBakedLights = false;
        target.enableForward(options, true);
        pixels = until("", true);
      }
      if (directDiffuse && !aggregateDiffuse) {
        const auto baked = pixels[right];
        options.baking.directDiffuse = false;
        target.forward->updateLighting(options);
        pixels = until("", true);
        std::printf("Baked direct %ux: baked %u, analytic %u\n", samples, unsigned(baked),
                    unsigned(pixels[right]));
        require(pixels[left] < 3 && std::abs(int(pixels[right]) - int(baked)) < 15,
                "Baked direct diffuse replaces the analytic diffuse lobe without doubling the light");
        options.baking.directDiffuse = true;
        target.forward->updateLighting(options);
        pixels = until("", true);
        const auto hash = target.forward->statistics().baking.activeHash;
        info.hash = 9706;
        info.isDynamic = 0;
        info.radiance = {0, 0, 8};
        r.updateLight(red, info);
        pixels = render(false);
        require(pixels[right] < 3 && pixels[right + 2] > 80 && pixels[left + 2] < 3 &&
                    target.forward->statistics().baking.activeHash == hash,
                "Unit direct coefficients use current colour while preserving per-light static provenance");
        info.radiance = {8, 0, 0};
        r.updateLight(red, info);
        pixels = render(false);
      }
      if (localShadows) {
        const auto lit = pixels[right];
        require(target.forward->statistics().staticShadowUpdates == 0,
                "Repeated frames reuse cached static maps");
        require(target.forward->statistics().shadowedLights > 0 &&
                    target.forward->statistics().emptyMovingShadowMaps == target.forward->statistics().shadowedLights,
                "Frozen-only receivers prove their moving shadow maps empty");
        // An actor can settle into STATIC classification without belonging to
        // the authored bake. It still needs a moving-caster shadow map.
        auto moving = instance(black, -.4f);
        moving.doubleSided = 1;
        moving.transform.matrix[0][0] = .15f;
        moving.transform.matrix[0][3] = .35f;
        moving.transform.matrix[1][1] = .04f;
        identity.instanceId = 9790;
        moving.pNext = &identity;
        const auto actor = r.createRetained(moving);
        pixels = render(false);
        require(target.forward->statistics().liveCasters > 0 &&
                    target.forward->statistics().emptyMovingShadowMaps < target.forward->statistics().shadowedLights,
                "An unbaked actor immediately disables the empty-map shortcut");
        const size_t adjacent = (27 * 64 + 48) * 4;
        std::printf("Local shadow %ux: lit %u, moving %u, adjacent %u\n", samples, unsigned(lit),
                    unsigned(pixels[right]), unsigned(pixels[adjacent]));
        require(pixels[right] < lit / 3 && pixels[adjacent] > lit / 2 && pixels[left] < 3,
                "A narrow dynamic shadow cuts its own direct light independently of the coarse lightmap");
        pixels = render(true);
        require(pixels[right] < lit / 3 && pixels[right + 1] > 20 &&
                    target.forward->statistics().shadowOverflow == 1,
                "A shadowed light cannot attenuate an unrelated light beyond the local-map budget");
        require(target.forward->statistics().staticShadowUpdates == 0,
                "Moving casters do not rebuild static maps");
        moving.transform.matrix[1][3] = .4f;
        r.updateRetained(actor, moving);
        pixels = render(false);
        require(std::abs(int(pixels[right]) - int(lit)) < 4,
                "Caster motion restores the old pixel without shadow trails");
        r.destroyRetained(&actor, 1);
        pixels = render(false);
        require(std::abs(int(pixels[right]) - int(lit)) < 4,
                "Caster removal clears its shadow on the next frame");
        require(target.forward->statistics().emptyMovingShadowMaps == target.forward->statistics().shadowedLights,
                "Caster removal restores the empty-map proof on the next frame");
        moving.transform.matrix[1][3] = 0;
        moving.mesh = quad(r, 9791, material(r, 9792, 0, 0, 0, 1, nullptr, 0));
        identity.instanceId = 9793;
        const auto cutout = r.createRetained(moving);
        pixels = render(false);
        require(std::abs(int(pixels[right]) - int(lit)) < 4 && target.forward->statistics().draws == 3,
                "Alpha-rejected caster fragments leave the local depth map clear");
        r.destroyRetained(&cutout, 1);
        if (aggregateDiffuse) {
          remix::forward::bake::cache::persistence().waitIdle();
          r.setBakeScene(nullptr);
          for (uint32_t frame = 0; frame < remix::gpuFrameCount + 2; ++frame) render(false);
          publish();
          pixels = until("", true);
          require(pixels[left] < 3 && std::abs(int(pixels[right]) - int(lit)) < 4 &&
                      std::strstr(target.forward->statistics().baking.status, "Cached lighting active"),
                  "Aggregate RGB, attributed mixed correction and runtime ownership survive cached publication");
          const auto originalHash = target.forward->statistics().baking.activeHash;
          info.isDynamic = 0;
          info.radiance = {0, 0, 8};
          r.updateLight(red, info);
          pixels = render(false); // Deliberately do not republish the capture yet.
          require(!target.forward->statistics().baking.active && pixels[right] < 3 &&
                      pixels[right + 2] > 80,
                  "A static RGB edit removes stale aggregate energy before the client republishes its snapshot");
          publish();
          pixels = until("", true);
          require(pixels[right] < 3 && pixels[right + 2] > 80 && pixels[left + 2] < 3 &&
                      target.forward->statistics().baking.activeHash != originalHash,
                  "Republished static light edits produce new aggregate lighting with wall occlusion");
          info.radiance = {8, 0, 0};
          r.updateLight(red, info);
          publish();
          pixels = until("", true);
          require(target.forward->statistics().baking.activeHash == originalHash &&
                      std::abs(int(pixels[right]) - int(lit)) < 4 &&
                      std::strstr(target.forward->statistics().baking.status, "Cached lighting active"),
                  "Restoring authored light content reuses its original aggregate cache");
          r.destroyLight(red);
          redExists = false;
          pixels = render(false);
          require(!target.forward->statistics().baking.active && pixels[right] < 3 && pixels[right + 2] < 3,
                  "Destroying a captured light cannot leave permanent aggregate illumination");
          base.lightCount = 0;
          base.lights = nullptr;
          publish();
          pixels = until("", true);
          require(pixels[right] < 3, "A republished light removal bakes a genuinely dark aggregate");
          info.hash = 9706;
          r.createLight(info, red);
          redExists = true;
          base.lightCount = 1;
          base.lights = &red;
          publish();
          pixels = until("", true);
          require(target.forward->statistics().baking.activeHash == originalHash &&
                      std::abs(int(pixels[right]) - int(lit)) < 4,
                  "Recreating the original authored light restores matching cached aggregate ownership");
          // Only the hidden wall belongs to this next bake. The visible quad
          // must receive direct diffuse from the grid, with no surface atlas
          // and no realtime local shadow maps to hide missing static visibility.
          options.baking.spacing = .125f;
          options.baking.maxProbes = 8192;
          options.localShadows = false;
          target.forward->updateLighting(options);
          base.instances = &instances[1];
          base.instanceCount = 1;
          ++base.revision;
          request.chartCount = 0;
          request.charts = nullptr;
          ++request.chartRevision;
          for (uint32_t c = 0; c < 3; ++c) {
            request.bounds.minimum[c] = c == 2 ? .25f : -1.f;
            request.bounds.maximum[c] = c == 2 ? .55f : 1.f;
          }
          publish();
          pixels = until("", true);
          const auto probeLit = pixels[right];
          std::printf("Aggregate probe receiver %ux: blocked %u, clear %u\n", samples,
                        unsigned(pixels[left]), unsigned(probeLit));
          require(pixels[left] < 15 && probeLit > 80,
                  "Uncharted receivers obtain aggregate direct diffuse and wall occlusion from probes alone");
          remix::forward::bake::cache::persistence().waitIdle();
          r.setBakeScene(nullptr);
          for (uint32_t frame = 0; frame < remix::gpuFrameCount + 2; ++frame) render(false);
          publish();
          pixels = until("", true);
          require(std::strstr(target.forward->statistics().baking.status, "Cached lighting active") &&
                      std::abs(int(pixels[right]) - int(probeLit)) < 4,
                  "Probe-only aggregate and attributed directional responses reload from disk together");
          options.localShadows = true;
          target.forward->updateLighting(options);
          moving.mesh = black;
          moving.transform.matrix[1][3] = 0;
          identity.instanceId = 9794;
          const auto probeActor = r.createRetained(moving);
          pixels = render(false);
          std::printf("Aggregate probe mixed %ux: lit %u, moving %u\n", samples,
                        unsigned(probeLit), unsigned(pixels[right]));
          require(pixels[right] < probeLit / 3 && pixels[adjacent] > probeLit / 2,
                  "A moving shadow subtracts its own direct light from a probe-lit dynamic receiver");
          r.destroyRetained(&probeActor, 1);
          pixels = render(false);
          require(std::abs(int(pixels[right]) - int(probeLit)) < 4,
                  "Removing a probe receiver caster restores aggregate diffuse without trails");
          options.localShadows = false;
          options.baking.aggregateDiffuse = false;
          target.forward->updateLighting(options);
          pixels = until("", true);
          std::printf("Aggregate probe comparison %ux: aggregate %u, analytic %u\n", samples,
                        unsigned(probeLit), unsigned(pixels[right]));
          require(pixels[left] < 15 && std::abs(int(pixels[right]) - int(probeLit)) < 20,
                  "Aggregate probe diffuse replaces analytic direct energy on an uncharted receiver");
          require(errors.load() == 0, "Aggregate mixed-light rendering is Vulkan validation clean");
          continue;
        }
        // Disable baking to exercise an uncharted receiver, then remove the
        // live caster. The complete frozen geometry must still shadow it.
        options.baking.enabled = false;
        target.forward->updateLighting(options);
        r.destroyRetained(&instances[1], 1);
        pixels = render(false);
        require(pixels[left] < 3 && pixels[right] > 80,
                "Uncharted receivers use complete cached static geometry");
        options.localShadows = false;
        target.forward->updateLighting(options);
        pixels = render(false);
        require(pixels[left] > 20, "Disabling local maps selects the direct-light fallback");
        const auto unshadowed = pixels[left];
        options.localShadows = true;
        options.localShadowFade = 2;
        target.forward->updateLighting(options);
        pixels = render(false);
        require(std::abs(int(pixels[left]) - int(unshadowed)) < 3,
                "New shadow selection starts at the unshadowed endpoint without changing light radiance");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        pixels = render(false);
        require(pixels[left] > 3 && pixels[left] < unshadowed - 2,
                "Local visibility blends through intermediate GPU pixel values during a timed fade");
        std::this_thread::sleep_for(std::chrono::milliseconds(1600));
        pixels = render(false);
        require(pixels[left] < 3 && target.forward->statistics().staticShadowUpdates == 0,
                "The completed fade reuses unchanged static depth maps at full shadow strength");
        options.localShadowFade = 0;
        target.forward->updateLighting(options);
        sphere.position.x = .1f;
        info.hash = 9706;
        info.isDynamic = 0;
        info.radiance = {100, 0, 0};
        r.updateLight(red, info);
        render(false);
        require(target.forward->statistics().staticShadowUpdates == 1,
                "Emitter motion refreshes static depth maps");
        r.setBakeScene(nullptr);
        pixels = render(false);
        require(pixels[left] > 20 && target.forward->statistics().staticShadowUpdates == 1,
                "Map unload removes cached static occluders immediately");
        // Freeze only the occluder: the visible receiver deliberately has no
        // chart or membership in this bake. With both local map families off,
        // its static direct occlusion must come from the probe volume.
        sphere.position.x = 0;
        r.updateLight(red, info);
        options.baking.enabled = true;
        options.baking.spacing = .125f;
        options.baking.maxProbes = 8192;
        options.baking.diskCache = true;
        options.localShadows = false;
        target.forward->updateLighting(options);
        identity.instanceId = 9798;
        blocker.pNext = &identity;
        const uint64_t probeBlocker = r.createRetained(blocker);
        base.instances = &probeBlocker;
        base.instanceCount = 1;
        ++base.revision;
        request.chartCount = 0;
        request.charts = nullptr;
        ++request.chartRevision;
        for (uint32_t c = 0; c < 3; ++c) {
          request.bounds.minimum[c] = c == 2 ? .25f : -1.f;
          request.bounds.maximum[c] = c == 2 ? .55f : 1.f;
        }
        publish();
        pixels = until("", true);
        std::printf("Local probe receiver %ux: blocked %u, clear %u\n", samples,
                    unsigned(pixels[left]), unsigned(pixels[right]));
        require(pixels[left] < 15 && pixels[right] > 80,
                "Uncharted receivers use baked local probe occlusion with all realtime local maps disabled");
        sphere.position.x = .1f;
        r.updateLight(red, info);
        pixels = render(false);
        require(pixels[left] > 20,
                "Changed emitter shape rejects probe masks and restores the ordinary direct fallback");
        sphere.position.x = 0;
        r.updateLight(red, info);
        pixels = render(false);
        require(pixels[left] < 15, "Restoring emitter shape reuses matching local probe visibility");
        remix::forward::bake::cache::persistence().waitIdle();
        r.setBakeScene(nullptr);
        for (uint32_t frame = 0; frame < remix::gpuFrameCount + 2; ++frame) render(false);
        publish();
        pixels = until("", true);
        require(pixels[left] < 15 && pixels[right] > 80 &&
                    std::strstr(target.forward->statistics().baking.status, "Cached lighting active"),
                "Cached local probe masks reach the shader without transient sampling arrays");
        options.localShadows = true;
        target.forward->updateLighting(options);
        const auto probeLit = pixels[right];
        moving.mesh = black;
        moving.transform.matrix[1][3] = 0;
        identity.instanceId = 9799;
        moving.pNext = &identity;
        const auto probeActor = r.createRetained(moving);
        pixels = render(false);
        require(pixels[left] < 15 && pixels[right] < probeLit / 3,
                "Baked local probe visibility composes with independent moving-object shadows");
        r.destroyRetained(&probeActor, 1);
        pixels = render(false);
        require(pixels[left] < 15 && std::abs(int(pixels[right]) - int(probeLit)) < 4,
                "Removing a moving caster restores the probe-lit receiver");
        require(errors.load() == 0, "Local shadow rendering is Vulkan validation clean");
        continue;
      }
      pixels = render(true);
      require(pixels[left] < 3 && pixels[right] > 80 && pixels[left + 1] > 20 && pixels[right + 1] > 20,
              "One light's static mask cannot darken an unrelated dynamic light");
      const auto hash = target.forward->statistics().baking.activeHash;
      info.hash = 9706;
      info.isDynamic = 0;
      info.radiance = {0, 0, 100};
      r.updateLight(red, info);
      pixels = render(false);
      require(pixels[left + 2] < 3 && pixels[right + 2] > 80 && pixels[right] < 3 &&
                  target.forward->statistics().baking.activeHash == hash &&
                  target.forward->statistics().baking.state == REMIXAPI_BAKE_READY_EXT,
              "Current light colour updates live specular while retaining the matching static penumbra");
      std::vector<remixapi_LightHandle> captured{red};
      for (uint32_t i = 0; i < 4; ++i) {
        info.hash = 9710 + i;
        info.radiance = {0, 40, 0};
        remixapi_LightHandle extra;
        r.createLight(info, extra);
        captured.push_back(extra);
      }
      base.lightCount = uint32_t(captured.size());
      base.lights = captured.data();
      ++base.revision;
      publish();
      const auto source = r.bakeScene();
      const auto overflow = std::max_element(source->lights.begin() + 1, source->lights.end(),
                                             [](const auto &a, const auto &b) { return a.id < b.id; });
      const auto fallback = captured[size_t(overflow - source->lights.begin())];
      until("", true);
      require(target.forward->bakeController()->volume()->occlusionParameters[3] != 0 &&
                  !std::strstr(target.forward->statistics().baking.status, "direct links"),
              "A fifth partial light retains a sparse mask instead of an unshadowed fallback");
      camera(r);
      r.drawRetained(nullptr);
      r.drawLight(fallback);
      pixels = target.render();
      require(pixels[left + 1] < 3 && pixels[right + 1] > 20,
              "A fifth partial light preserves both its blocked and exposed receiver regions");
      info.hash = 9706;
      info.radiance = {0, 0, 100};
      sphere.position.x = .05f;
      r.updateLight(red, info);
      pixels = render(false);
      require(pixels[left + 2] > 20,
              "Moving a light rejects its old mask instead of applying stale occlusion");
      options.baking.enabled = false;
      target.forward->updateLighting(options);
      pixels = render(false);
      require(pixels[left + 2] > 20 && !target.forward->statistics().baking.active,
              "Direct lights survive the ordinary fallback when static baking is disabled");
      r.setBakeScene(nullptr);
      for (uint32_t i = 0; i < remix::gpuFrameCount + 1; ++i)
        target.render();
    }
  }
#include "indexed_directionals.h"
#include "selected_directionals.h"
  void chartIsolationReadback(Target &target, const remix::forward::Options &options) {
    namespace lm = remix::forward::lightmap;
    // Poison unused atlas space magenta; the two tightly packed charts contain
    // pure green/blue constant irradiance. Even partial MSAA fragments must
    // never pick up red from outside their own first/last texel centres.
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(target.physical, &properties);
    const auto layout = lm::fit(*target.raster.bakeScene(),
                                {options.baking.lightmapPageSizeForBudget(),
                                 std::min(256u, properties.limits.maxImageArrayLayers / lm::atlasLayers),
                                 0, options.baking.lightmapTexels, 1,
                                 options.baking.lightmapLuxelSize});
    require(layout.pages > 0 && layout.charts.size() == 2,
            "Isolation fixture has two bounded charts under the controller's page/layer policy");
    const size_t pageTexels = size_t(layout.pageSize) * layout.pageSize;
    std::vector<uint16_t> pixels(layout.pages * pageTexels * lm::atlasLayers * 4);
    for (uint32_t page = 0; page < layout.pages; ++page)
      for (size_t texel = 0; texel < pageTexels; ++texel) {
        const auto offset = (page * lm::atlasLayers * pageTexels + texel) * 4;
        pixels[offset] = 0x4800;
        pixels[offset + 2] = 0x4800; // 8.0 in half precision
      }
    for (uint32_t chart = 0; chart < layout.charts.size(); ++chart) {
      const auto &a = layout.charts[chart];
      for (uint32_t y = a.y; y < a.y + a.height; ++y)
        for (uint32_t x = a.x; x < a.x + a.width; ++x) {
          const size_t texel = (a.page * lm::atlasLayers * pageTexels + size_t(y) * layout.pageSize + x) * 4;
          pixels[texel] = 0;
          pixels[texel + 1] = chart == 0 ? 0x4800 : 0;
          pixels[texel + 2] = chart == 1 ? 0x4800 : 0;
        }
    }
    vk(vkDeviceWaitIdle(target.device));
    remix::forward::Buffer staging;
    staging.create(target.physical, target.device, pixels.size() * sizeof(uint16_t),
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    staging.write(pixels.data(), pixels.size() * sizeof(uint16_t));
    auto image = std::make_unique<remix::forward::Image>();
    image->create(target.physical, target.device, {layout.pageSize, layout.pageSize},
                  VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                  VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, layout.pages * lm::atlasLayers);
    vk(vkResetCommandBuffer(target.command, 0));
    const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vk(vkBeginCommandBuffer(target.command, &begin));
    VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                 .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                 .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                 .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .image = image->handle,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layout.pages * lm::atlasLayers}};
    vkCmdPipelineBarrier(target.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);
    const VkBufferImageCopy copy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layout.pages * lm::atlasLayers},
                                 .imageExtent = {layout.pageSize, layout.pageSize, 1}};
    vkCmdCopyBufferToImage(target.command, staging.handle, image->handle,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(target.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    vk(vkEndCommandBuffer(target.command));
    vk(vkResetFences(target.device, 1, &target.fence));
    const VkSubmitInfo submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &target.command};
    vk(vkQueueSubmit(target.queue, 1, &submit, target.fence));
    vk(vkWaitForFences(target.device, 1, &target.fence, VK_TRUE, 5000000000ull));
    const auto volume = target.forward->bakeController()->volume();
    volume->surfaceAtlas.swap(image);
    // This fixture injects the complete twenty-layer source layout. Published
    // data may omit visibility/direct layers; its decoder flags must describe
    // the injected image rather than the replaced allocation.
    const auto previousOcclusion = volume->occlusionParameters[0];
    const auto previousDirectOffset = volume->surfaceDirectOffset;
    volume->occlusionParameters[0] &= 0x7fffffffu;
    volume->surfaceDirectOffset = 0;
    camera(target.raster);
    target.raster.drawRetained(nullptr);
    const auto readback = target.render();
    uint32_t maximumRed = 0;
    for (size_t i = 0; i < readback.size(); i += 4)
      maximumRed = std::max(maximumRed, uint32_t(readback[i]));
    const auto left = size_t((32 * 64 + 16) * 4), right = size_t((32 * 64 + 48) * 4);
    std::printf("Chart bounds isolation: red max %u, left %u/%u, right %u/%u\n", maximumRed,
                unsigned(readback[left + 1]), unsigned(readback[left + 2]), unsigned(readback[right + 1]),
                unsigned(readback[right + 2]));
    require(maximumRed < 3 && std::max(readback[left + 1], readback[right + 1]) > 80 &&
                std::max(readback[left + 2], readback[right + 2]) > 80,
            "Bounded atlas charts reject poisoned neighbours on MSAA edge fragments");
    volume->surfaceAtlas.swap(image); // render() waited for the last sampling fence
    volume->occlusionParameters[0] = previousOcclusion;
    volume->surfaceDirectOffset = previousDirectOffset;
    std::printf("Chart bounds %ux MSAA: maximum poisoned red %u\n", options.samples, maximumRed);
  }
  remixapi_MeshHandle curvedLightmapReceiver(remix::NativeResources &resources,
                                             remixapi_MaterialHandle material) {
    remixapi_HardcodedVertex vertices[4]{};
    const float positions[4][3] = {
        {-.8f, -.8f, .5f}, {.8f, -.8f, .5f}, {.8f, .8f, .65f}, {-.8f, .8f, .5f}};
    for (uint32_t i = 0; i < 4; ++i) {
      std::copy_n(positions[i], 3, vertices[i].position);
      vertices[i].color = 0xffffffff;
    }
    const uint32_t indices[] = {0, 2, 1, 0, 3, 2};
    remixapi_MeshInfoSurfaceTriangles surface{};
    surface.vertices_values = vertices;
    surface.vertices_count = 4;
    surface.indices_values = indices;
    surface.indices_count = 6;
    surface.material = material;
    remixapi_MeshInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = 9502;
    info.surfaces_values = &surface;
    info.surfaces_count = 1;
    remixapi_MeshHandle mesh;
    resources.createMesh(info, mesh);
    return mesh;
  }
  void surfaceLightmapTests(bool curved = false, bool split = false) {
    for (bool ssr : {false, true})
      for (uint32_t samples : {1u, 2u, 4u}) {
        Target target(64, 64, true);
        auto &r = target.raster;
        remix::forward::Options options;
        options.prism = false; // Compare transport/ownership without saturated-primary display compression.
        options.baking.lightmapFitBudget = false; // Controlled-density lighting fixtures.
        options.samples = samples;
        options.ambient = 0;
        options.shadows = false;
        options.ssr = ssr;
        options.shadowResolution = 128;
        options.baking.maxProbes = 64;
        options.baking.spacing = 2;
        options.baking.samples = 256;
        options.baking.bounces = 1;
        options.baking.batch = 16;
        options.baking.reflectionProbes = 0;
        options.baking.lightmapPageSize = 4096; // budget must reduce this to 64 without a 4K allocation
        options.baking.lightmapTexels = 4096;
        if (curved) options.baking.lightmapLuxelSize = .1f;
        if (samples == 1 && !curved) options.baking.lightmapFitBudget = true;
        // The 4x case needs a three-centimetre grid to fit this page. Doubling
        // alone would stop at four; exercise the refined layout on the GPU.
        if (samples == 4 && !curved) options.baking.lightmapLuxelSize = .01f;
        target.enableForward(options);
        const auto white = material(r, 9500, 1, 1, 1);
        const auto black = material(r, 9501, 0, 0, 0);
        const auto receiverMesh = curved ? curvedLightmapReceiver(r, white) : quad(r, 9502, white);
        const auto blockerMesh = quad(r, 9503, black);
        remixapi_InstanceIdentityEXT identity{};
        identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
        identity.instanceId = 9504;
        identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
        identity.rasterVisible = 1;
        auto receiver = instance(receiverMesh);
        if (split && ssr && samples == 4) receiver.transform.matrix[1][3] = .0125f;
        receiver.pNext = &identity;
        const auto retained = r.createRetained(receiver);
        auto blocker = instance(blockerMesh, -.2f);
        blocker.doubleSided = 1; // its back face must occlude rays from the receiver
        blocker.transform.matrix[0][0] = .5f;
        blocker.transform.matrix[0][3] = -.4f;
        blocker.transform.matrix[1][1] = 3;
        identity.instanceId = 9505;
        identity.rasterVisible = 0; // transport only: never hides the tested receiver from the camera
        blocker.pNext = &identity;
        const uint64_t authored[] = {retained, r.createRetained(blocker)};
        remixapi_LightInfoDomeEXT dome{};
        dome.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
        for (uint32_t c = 0; c < 3; ++c) dome.transform.matrix[c][c] = 1;
        remixapi_LightInfo light{};
        light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
        light.pNext = &dome;
        light.hash = 9506;
        light.radiance = {0, 1, 0};
        remixapi_LightHandle environment;
        r.createLight(light, environment);
        const uint32_t extent = samples == 4 ? 256 : 8;
        const float edge = float(extent);
        const float uv[] = {0, 0, edge, edge, edge, 0, 0, 0, 0, edge, edge, edge};
        const remixapi_BakeSurfaceChartEXT charts[] = {
            {retained, 0, 0, 0, split ? 3u : 6u, extent + 1, extent + 1, uv},
            {retained, 0, 0, 3, 3, extent + 1, extent + 1, uv + 6}};
        remixapi_BakeSceneSurfacesInfoEXT request{};
        auto &info = request.bounds.scene;
        info = {sizeof(request), REMIXAPI_BAKE_SCENE_SURFACES_VERSION_EXT, 95, 1, 1,
                REMIXAPI_BAKE_SCENE_READY_EXT, 2, 1, authored, &environment};
        for (uint32_t c = 0; c < 3; ++c) {
          // Surface lightmaps must work even where irradiance probes cannot be sampled.
          request.bounds.minimum[c] = 10;
          request.bounds.maximum[c] = 11;
        }
        request.chartRevision = 1;
        request.chartCount = split ? 2 : 1;
        request.charts = charts;
        r.setBakeScene(&info);
        auto until = [&](uint32_t state) {
          if (state == REMIXAPI_BAKE_TRACING_EXT) {
            options.baking.diskCache = false; // explicit tracing/cancellation checks cannot use a hit
            target.forward->updateLighting(options);
          }
          const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
          std::vector<uint8_t> pixels;
          do {
            camera(r);
            r.drawRetained(nullptr);
            pixels = target.render();
            const auto &bake = target.forward->statistics().baking;
            if (bake.state == state) return pixels;
            if (bake.state == REMIXAPI_BAKE_FAILED_EXT) std::fprintf(stderr, "%s\n", bake.status);
            require(bake.state != REMIXAPI_BAKE_FAILED_EXT, "Surface lightmap baking succeeded");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          } while (std::chrono::steady_clock::now() < deadline);
          require(false, "Surface lightmap reached its requested state");
          return pixels;
        };
        const auto first = until(REMIXAPI_BAKE_READY_EXT);
        if (split && ssr) chartIsolationReadback(target, options);
        const auto content = target.forward->statistics().bakeContent;
        require(content.grid && content.reflections && content.surfaces,
                "Published lighting diagnostics fingerprint the packed bake payloads");
        if (samples == 4 && !curved)
          require(std::strstr(target.forward->statistics().baking.status, "3025 surface texels") &&
                      std::strstr(target.forward->statistics().baking.status, "0.03 m surface luxels"),
                  "The intermediate world density publishes complete surface lighting within the atlas budget");
        const size_t left = (32 * 64 + 16) * 4, right = (32 * 64 + 48) * 4;
        require(first[right + 1] > 80 && first[left + 1] < first[right + 1] * .3f,
                "Surface atlas resolves static shadows while probes lie outside the receiver volume");
        const auto published = target.forward->bakeController()->volume();
        auto replacement = options;
        replacement.samples = samples == 4 ? 1 : 4;
        replacement.ssr = !ssr;
        target.enableForward(replacement, true);
        camera(r);
        r.drawRetained(nullptr);
        const auto preserved = target.render();
        require(target.forward->bakeController()->volume() == published &&
                    target.forward->statistics().bakeContent == content &&
                    target.forward->statistics().baking.state == REMIXAPI_BAKE_READY_EXT &&
                    preserved[right + 1] > 80 && preserved[left + 1] < preserved[right + 1] * .3f,
                "Replacement targets reuse the surface atlas and its static shadows on their first frame");
        target.enableForward(options, true);
        camera(r);
        r.drawInstance(instance(receiverMesh));
        pixel(target.render(), 48, 32, 0, 0, 0,
                "Uncharted instances cannot borrow a different retained instance's surface atlas");
        options.ambient = .5f;
        target.forward->updateLighting(options);
        camera(r);
        r.drawRetained(nullptr);
        auto pixels = target.render();
        require(target.forward->statistics().bakeContent == content,
                "Live ambient changes retain the published bake content fingerprints");
        require(pixels[left] < 3 && pixels[left + 2] < 3 &&
                    std::abs(int(pixels[left + 1]) - int(first[left + 1])) < 3,
                "Dark surface texels replace ambient fill rather than falling through to probes");
        options.ambient = 0;
        target.forward->updateLighting(options);
        light.radiance = {0, 0, 1};
        r.updateLight(environment, light);
        ++info.revision;
        r.setBakeScene(&info);
        until(REMIXAPI_BAKE_TRACING_EXT);
        pixels = until(REMIXAPI_BAKE_READY_EXT);
        require(target.forward->statistics().bakeContent.surfaces != content.surfaces,
                "Rebaking different environment lighting changes the surface payload fingerprint");
        require(pixels[right + 2] > 80 && pixels[right + 1] < 3 && pixels[left + 2] < pixels[right + 2] * .3f,
                "Replacement surface atlas publishes only the matching completed bake generation");
        light.radiance = {0, 1, 0};
        r.updateLight(environment, light);
        ++info.revision;
        r.setBakeScene(&info);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        bool surfaceBatch = false;
        do {
          camera(r);
          r.drawRetained(nullptr);
          target.render();
          const auto &bake = target.forward->statistics().baking;
          require(bake.state != REMIXAPI_BAKE_FAILED_EXT, "Replacement surface preparation succeeded");
          surfaceBatch = std::strstr(bake.status, "Baking surface lightmaps") != nullptr;
          if (!surfaceBatch) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (!surfaceBatch && std::chrono::steady_clock::now() < deadline);
        require(surfaceBatch, "Map unload fixture reached in-flight surface tracing");
        r.setBakeScene(nullptr);
        for (uint32_t i = 0; i < remix::gpuFrameCount + 2; ++i) {
          camera(r);
          r.drawRetained(nullptr);
          pixel(target.render(), 48, 32, 0, 0, 0, "Unloaded surface jobs cannot reactivate an obsolete atlas");
        }
        require(target.forward->statistics().baking.state == REMIXAPI_BAKE_WAITING_EXT,
                "Cancelled surface batches release their owners after their fences");
        require(target.forward->statistics().bakeContent == remix::forward::bake::ContentHashes{},
                "Unloaded bake content fingerprints clear with the published volume");
        light.radiance = {0, 0, 1};
        r.updateLight(environment, light);
        ++info.revision;
        r.setBakeScene(&info);
        until(REMIXAPI_BAKE_READY_EXT);
        options.baking.enabled = false;
        target.forward->updateLighting(options);
        camera(r);
        r.drawRetained(nullptr);
        pixel(target.render(), 48, 32, 0, 0, 0, "Disabling an active surface atlas removes its lighting immediately");
        require(!target.forward->statistics().baking.active, "Disabled surface volume is no longer active");
        options.baking.enabled = true;
        target.forward->updateLighting(options);
        until(REMIXAPI_BAKE_READY_EXT);
        auto moved = receiver;
        identity.instanceId = 9504;
        identity.rasterVisible = 1;
        moved.transform.matrix[0][3] = .1f;
        r.updateRetained(retained, moved);
        camera(r);
        r.drawRetained(nullptr);
        pixel(target.render(), 48, 32, 0, 0, 0, "Moved geometry rejects stale surface chart bindings");
        r.setBakeScene(nullptr);
        for (uint32_t i = 0; i < remix::gpuFrameCount + 1; ++i) target.render();
        std::printf("%s surface lightmap %ux MSAA, SSR %s: shaded green %u, exposed green %u; replacement, cancellation and stale bindings passed\n",
                      split ? "Stitched curved" : curved ? "Curved" : "Planar", samples, ssr ? "on" : "off", unsigned(first[left + 1]), unsigned(first[right + 1]));
      }
  }
  void viewModelLightingTests(bool probeTest = false, bool reflectionTest = false, bool authoringTest = false) {
    for (bool ssr : {false, true})
      for (bool local : {false, true})
        for (uint32_t samples : {1u, 2u, 4u}) {
          if (probeTest && (!local || ssr || samples == 2)) continue;
          Target target(64, 64, probeTest);
          auto &r = target.raster;
          remix::forward::Options options;
          options.samples = samples;
          options.ssr = ssr;
          options.baking.enabled = probeTest;
          options.baking.aggregateDiffuse = true;
          options.baking.maxProbes = 128;
          options.baking.spacing = .5f;
          options.baking.samples = options.baking.batch = 64;
          options.baking.bounces = 1;
          options.baking.reflectionProbes = reflectionTest ? 1 : 0;
          options.baking.reflectionResolution = 16;
          options.baking.lightmapTexels = 1024;
          options.baking.lightmapPageSize = 32;
          options.baking.diskCache = authoringTest;
          if (authoringTest) options.authoredReflectionFallback = false; // exercise authored-only mode before fallback
          options.baking.denoiseLightmaps = false;
          options.ambient = 0;
          options.shadowResolution = 256;
          options.shadowDistance = 20;
          options.localShadowLights = 1;
          options.localShadowResolution = 256;
          options.localShadowFade = 0;
          options.sceneScale = 1;
          target.enableForward(options);
          auto matte = [&](uint64_t hash, remixapi_Float3D color, float metallic = 0, float roughness = 1,
                           void *extension = nullptr) {
            remixapi_MaterialInfoOpaqueEXT opaque{};
            opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
            opaque.pNext = extension;
            opaque.albedoConstant = color;
            opaque.opacityConstant = 1;
            opaque.roughnessConstant = roughness;
            opaque.metallicConstant = metallic;
            opaque.alphaTestType = 7;
            remixapi_MaterialInfo info{};
            info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
            info.hash = hash;
            info.pNext = &opaque;
            remixapi_MaterialHandle handle;
            r.createMaterial(info, handle);
            return handle;
          };
          const auto wallMesh = quad(r, 13000, matte(13001, {.5f, .5f, .5f}));
          const auto weaponMesh = quad(r, 13002, reflectionTest ? matte(13003, {1, 1, 1}, 1, .04f)
                                                               : matte(13003, {1, .05f, .05f}));
          const auto overlayMesh = quad(r, 13006, matte(13007, {0, 0, 1}));
          auto wall = instance(wallMesh, 7.5f); // quad vertices contribute another .5
          wall.transform.matrix[0][0] = wall.transform.matrix[1][1] = 20;
          wall.doubleSided = 1;
          auto weapon = instance(weaponMesh, 1.5f);
          weapon.transform.matrix[0][0] = weapon.transform.matrix[1][1] = .5f;
          weapon.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL;
          weapon.doubleSided = 1;
          remixapi_InstanceIdentityEXT hidden{};
          hidden.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
          hidden.instanceId = 13005;
          hidden.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
          hidden.rasterVisible = 0;
          auto blocker = instance(wallMesh, .5f);
          blocker.transform.matrix[0][0] = blocker.transform.matrix[1][1] = .3f;
          blocker.doubleSided = 1;
          blocker.pNext = &hidden;
          remixapi_LightInfoDistantEXT distant{};
          distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
          distant.direction = {0, 0, 1};
          remixapi_LightInfoSphereEXT sphere{};
          sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
          sphere.radius = .2f;
          remixapi_LightInfo info{};
          info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
          info.pNext = &distant;
          info.hash = 13004;
          info.radiance = {4, 4, 4};
          if (local) {
            info.pNext = &sphere;
            info.radiance = {100, 100, 100};
          }
          remixapi_LightHandle sun;
          r.createLight(info, sun);
          remixapi_InstanceIdentityEXT staticIdentity{};
          staticIdentity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
          staticIdentity.instanceId = 13008;
          staticIdentity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
          staticIdentity.rasterVisible = 1;
          if (probeTest) {
            wall.pNext = &staticIdentity;
            uint64_t handles[] = {r.createRetained(wall), 0};
            if (reflectionTest) {
              remixapi_MaterialInfoOpaqueEXT opaque{};
              opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
              opaque.albedoConstant = {0, 0, 0};
              opaque.opacityConstant = 1;
              opaque.roughnessConstant = 1;
              opaque.alphaTestType = 7;
              remixapi_MaterialInfo glow{};
              glow.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
              glow.pNext = &opaque;
              glow.hash = 13010;
              glow.emissiveIntensity = 2;
              glow.emissiveColorConstant = {1, 0, 0};
              remixapi_MaterialHandle material;
              r.createMaterial(glow, material);
              auto emitter = instance(quad(r, 13011, material), -.5f);
              emitter.doubleSided = 1;
              emitter.transform.matrix[0][0] = emitter.transform.matrix[1][1] = 4;
              auto identity = staticIdentity;
              identity.instanceId = 13009;
              emitter.pNext = &identity;
              handles[1] = r.createRetained(emitter);
            }
            remixapi_BakeSceneBoundsInfoEXT request{};
            request.scene = {sizeof(request), REMIXAPI_BAKE_SCENE_BOUNDS_VERSION_EXT, 130, 1, 1,
                REMIXAPI_BAKE_SCENE_READY_EXT, reflectionTest ? 2u : 1u, reflectionTest ? 0u : 1u,
                handles, reflectionTest ? nullptr : &sun};
            for (uint32_t c = 0; c < 3; ++c) {
              request.minimum[c] = c == 2 ? 1.f : -.6f;
              request.maximum[c] = c == 2 ? 3.f : .6f;
            }
            r.setBakeScene(&request.scene);
          }
          auto cameras = [&] {
            remixapi_CameraInfoParameterizedEXT parameters{};
            parameters.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
            parameters.aspect = 1;
            parameters.fovYInDegrees = 90;
            parameters.nearPlane = .05f;
            parameters.farPlane = 100;
            parameters.right = {1, 0, 0};
            parameters.up = {0, 1, 0};
            parameters.forward = {0, 0, 1};
            remixapi_CameraInfo camera{};
            camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
            camera.pNext = &parameters;
            for (auto type : {REMIXAPI_CAMERA_TYPE_WORLD, REMIXAPI_CAMERA_TYPE_VIEW_MODEL}) {
              camera.type = type;
              r.setupCamera(camera);
            }
          };
          auto render = [&](bool shadow, bool behindWall, bool drawWeapon, bool lit,
                             bool selfShadow = false, bool compatibilityOverlay = false) {
            cameras();
            auto backdrop = wall;
            if (behindWall) backdrop.transform.matrix[2][3] = .5f;
            r.drawInstance(backdrop);
            if (drawWeapon) r.drawInstance(weapon);
            if (shadow) r.drawInstance(blocker);
            if (selfShadow) {
              auto selfCaster = blocker;
              selfCaster.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL;
              r.drawInstance(selfCaster);
            }
            if (compatibilityOverlay) {
              auto rasterOnly = hidden;
              rasterOnly.classification = REMIXAPI_INSTANCE_RASTER_ONLY_EXT;
              rasterOnly.rasterVisible = 1;
              auto overlay = weapon;
              overlay.mesh = overlayMesh;
              overlay.pNext = &rasterOnly;
              overlay.transform.matrix[0][0] = overlay.transform.matrix[1][1] = .2f;
              r.drawInstance(overlay);
            }
            if (lit) r.drawLight(sun);
            return target.render();
          };
          const size_t centre = (32 * 64 + 32) * 4;
          if (probeTest) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            do {
              render(false, false, true, false);
              require(target.forward->statistics().baking.state != REMIXAPI_BAKE_FAILED_EXT,
                      target.forward->statistics().baking.status);
            } while (!target.forward->statistics().baking.active && std::chrono::steady_clock::now() < deadline);
            require(target.forward->statistics().baking.active, "Viewmodel probe fixture completes its bake");
            const auto snapshot = r.bakeScene();
            const auto probeWeapon = render(false, false, true, false);
            weapon.categoryFlags = 0;
            const auto probeWorld = render(false, false, true, false);
            weapon.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL;
            require(probeWeapon[centre] > 40 && probeWeapon[centre] > 1.4f * probeWeapon[centre+1],
                    "Baked probes light the viewmodel with no live lights or ambient fallback");
            for (uint32_t c = 0; c < 3; ++c)
              require(std::abs(int(probeWeapon[centre+c]) - int(probeWorld[centre+c])) <= 2,
                      "Viewmodel and main-world receivers sample identical baked probe lighting");
            require(r.bakeScene() == snapshot && target.forward->statistics().baking.active,
                    "Viewmodel ownership changes preserve active map lighting and its bake snapshot");
            if (reflectionTest) {
              const auto sharp = probeWeapon[centre];
              weapon.mesh = quad(r, 13012, matte(13013, {0, 0, 0}, 0, .04f));
              const auto dielectric = render(false, false, true, false);
              weapon.mesh = quad(r, 13014, matte(13015, {1, 1, 1}, 1, 1));
              const auto rough = render(false, false, true, false);
              require(target.forward->statistics().reflectionProbes.parameters[0] > 0,
                      "Viewmodel environment fixture publishes a baked cubemap");
              require(sharp > 1.4f * dielectric[centre] && dielectric[centre] > 3,
                      "Viewmodel environment reflections obey metallic and dielectric Fresnel weights");
              require(rough[centre] < sharp && rough[centre] > 3,
                      "Viewmodel roughness uses the filtered cubemap mip chain");
              std::printf("Viewmodel environment %ux: metal %u, dielectric %u, rough %u\n",
                          samples, sharp, dielectric[centre], rough[centre]);
            }
            if (authoringTest) {
              weapon.mesh = weaponMesh;
              // Observe steady ownership after every frame slot has retired the
              // initial bake publication, before comparing later edit cleanup.
              for (uint32_t frame = 0; frame < 8; ++frame) render(false, false, true, false);
              const auto baseline = target.forward->statistics();
              const auto probe = baseline.reflectionProbes.probes[0];
              remix::forward::probes::Capture inputCapture;
              inputCapture.resolution = baseline.reflectionProbes.parameters[1];
              inputCapture.worldToLocal = probe.worldToLocal;
              for (uint32_t c = 0; c < 3; ++c) inputCapture.position[c] = probe.position[c];
              inputCapture.bounds.include({probe.minimum[0], probe.minimum[1], probe.minimum[2]});
              inputCapture.bounds.include({probe.maximum[0], probe.maximum[1], probe.maximum[2]});
              const auto inputRays = remix::forward::probes::rays(inputCapture);
              require(baseline.reflectionProbes.parameters[0] == 1 && baseline.bakeContent.captureInputs ==
                          (remix::scene::fastHash(inputRays.data(), inputRays.size() * sizeof(inputRays[0])) | 1),
                      "Published automatic capture provenance preserves exact ray bytes after work-buffer release");
              remixapi_ReflectionVolumeEXT authored[2]{};
              authored[0].id = 71;
              for (uint32_t c = 0; c < 3; ++c) {
                authored[0].minimum[c] = probe.minimum[c];
                authored[0].maximum[c] = probe.maximum[c];
                authored[0].capture[c] = probe.position[c];
                authored[0].worldToLocal[c][c] = 1;
              }
              authored[1] = authored[0];
              authored[1].id = 72;
              authored[1].priority = 1;
              remixapi_ReflectionVolumesInfoEXT request{sizeof(request), REMIXAPI_REFLECTION_VOLUMES_VERSION_EXT,
                  snapshot->sceneId, 1, 2, 0, authored};
              auto waitForEdit = [&] {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                std::vector<uint8_t> image;
                do {
                  image = render(false, false, true, false);
                  const auto state = target.forward->statistics().authoredReflections;
                  if (state.active && !state.pending && state.revision == request.revision) return image;
                  std::this_thread::sleep_for(std::chrono::milliseconds(1));
                } while (std::chrono::steady_clock::now() < deadline);
                require(false, "Authored reflection publication completes without restarting the world bake");
                return image;
              };
              r.setReflectionVolumes(&request);
              const auto firstEdit = waitForEdit();
              require(target.forward->statistics().reflectionIdentities[0] == 72 &&
                          target.forward->statistics().reflectionIdentities[1] == 71 &&
                          target.forward->statistics().authoredReflections.publishedSceneId == snapshot->sceneId &&
                          target.forward->statistics().authoredReflections.publishedRevision == request.revision,
                      "Published authored identities follow priority-sorted GPU layers and their own request");
              const auto publishedSnapshot = target.forward->reflectionAuthoring()->snapshot(target.forward->bakeController()->volume());
              require(publishedSnapshot.image && publishedSnapshot.baseline.composed &&
                          publishedSnapshot.baseline.authoredSceneId == snapshot->sceneId &&
                          publishedSnapshot.baseline.authoredRevision == request.revision &&
                          publishedSnapshot.identities[0] == 72 && publishedSnapshot.identities[1] == 71 &&
                          publishedSnapshot.baseline.volumes[0].protectedCapture &&
                          publishedSnapshot.image->header().parameters[0] == publishedSnapshot.baseline.volumes.size(),
                      "Retained reflection snapshot couples actual image layers, authored roles and published identity");
              require(firstEdit[centre] > 40 && target.forward->statistics().authoredReflections.rays == 6*16*16,
                      "Authored capture publishes visible viewmodel reflections through the production path");
              require(target.forward->statistics().authoredReflections.captureScratchBytes == 0,
                      "Published captures release transport scratch while preserving completed ray counters");
              require(target.forward->statistics().authoredReflections.reused == 1 &&
                          target.forward->statistics().reflectionProbes.parameters[0] == 2 &&
                          target.forward->statistics().reflectionProbes.probes[0].minimum[3] == 1,
                      "New same-origin volumes trace once while retaining independent priority records");
              request.volumeCount = 1; ++request.revision;
              r.setReflectionVolumes(&request);
              waitForEdit();
              authored[0].id = 171;
              ++request.revision;
              r.setReflectionVolumes(&request);
              waitForEdit();
              require(target.forward->statistics().reflectionIdentities[0] == 171,
                      "Replacing a logical ID updates published identity without retracing unchanged radiance");
              require(target.forward->statistics().authoredReflections.reused == 1 &&
                          target.forward->statistics().authoredReflections.rays == 0,
                      "Replacing an authoring ID preserves identical world-space capture lighting");
              authored[1] = authored[0]; authored[1].id = 72;
              request.volumeCount = 2; ++request.revision;
              r.setReflectionVolumes(&request);
              waitForEdit();
              require(target.forward->statistics().authoredReflections.reused == 2 &&
                          target.forward->statistics().authoredReflections.rays == 0,
                      "A new volume at a published capture origin reuses its complete mip chain");
              authored[1].capture[0] += .01f;
              request.volumeCount = 2; ++request.revision;
              r.setReflectionVolumes(&request);
              waitForEdit();
              require(target.forward->statistics().authoredReflections.reused == 1 &&
                          target.forward->statistics().authoredReflections.sceneReused &&
                          target.forward->statistics().authoredReflections.rays == 6*16*16,
                      "Adding a capture reuses unchanged cubemaps and traces only the new origin");
              authored[0].priority = 4;
              for (auto &volume : authored) {
                const float xmin = volume.minimum[0], xmax = volume.maximum[0];
                volume.minimum[0] = volume.minimum[1]; volume.maximum[0] = volume.maximum[1];
                volume.minimum[1] = -xmax; volume.maximum[1] = -xmin;
                volume.worldToLocal[0][0] = volume.worldToLocal[1][1] = 0;
                volume.worldToLocal[0][1] = 1; volume.worldToLocal[1][0] = -1;
              }
              ++request.revision;
              r.setReflectionVolumes(&request);
              const auto rotated = waitForEdit();
              require(target.forward->statistics().authoredReflections.reused == 2 &&
                          target.forward->statistics().authoredReflections.rays == 0 &&
                          std::abs(int(rotated[centre])-int(firstEdit[centre])) <= 2,
                      "Bounds rotation and priority edits reuse filtered radiance without tracing");
              std::array<remixapi_ReflectionSamplingEXT, 2> sampling{};
              for (uint32_t i = 0; i < sampling.size(); ++i) {
                for (uint32_t c = 0; c < 3; ++c) {
                  sampling[i].minimum[c] = authored[i].minimum[c] - .25f;
                  sampling[i].maximum[c] = authored[i].maximum[c] + .25f;
                }
                sampling[i].blendDistance = .125f;
              }
              remixapi_ReflectionVolumesSamplingInfoEXT extended{request, sampling.data()};
              extended.base.structSize = sizeof(extended);
              extended.base.version = REMIXAPI_REFLECTION_VOLUMES_SAMPLING_VERSION_EXT;
              extended.base.revision = ++request.revision;
              r.setReflectionVolumes(&extended.base);
              waitForEdit();
              const auto &separate = target.forward->statistics().reflectionProbes.probes[0];
              require(separate.minimum[0] == authored[0].minimum[0] &&
                          separate.influenceMinimum[0] == sampling[0].minimum[0] && separate.maximum[3] == 8 &&
                          target.forward->statistics().authoredReflections.rays == 0 &&
                          target.forward->statistics().authoredReflections.reused == 2 && r.bakeScene() == snapshot,
                      "Independent influence and feather edits reach GPU metadata while reusing both captures");
              ++request.revision;
              r.setReflectionVolumes(&request);
              waitForEdit();
              const float originalCaptureX = authored[1].capture[0];
              authored[1].capture[0] += .01f;
              ++request.revision;
              r.setReflectionVolumes(&request);
              const auto cancellationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
              do {
                require(render(false, false, true, false)[centre] > 40,
                        "Pending capture edits retain the previously published reflection set");
                const auto pending = target.forward->statistics().authoredReflections;
                if (pending.pending && pending.rays > 0) {
                  require(pending.captureScratchBytes >= uint64_t(pending.rays) * sizeof(remix::forward::bake::Point),
                          "Pending capture transport retains its work list until readbacks retire");
                  require(pending.requested && pending.active && !pending.failed,
                          "Pending reflection edits expose the retained active set and request readiness separately");
                  const auto retained = target.forward->reflectionAuthoring()->snapshot(target.forward->bakeController()->volume());
                  require(retained.baseline.authoredRevision == pending.publishedRevision &&
                              retained.baseline.authoredRevision < request.revision,
                          "Snapshot uses the sampled authored revision while a newer edit is pending");
                  require(pending.publishedSceneId == snapshot->sceneId &&
                              pending.publishedRevision < request.revision,
                          "Pending edits retain the publication identity of the image actually sampled");
                  break;
                }
              } while (std::chrono::steady_clock::now() < cancellationDeadline);
              require(target.forward->statistics().authoredReflections.pending,
                      "Cancellation fixture reaches a submitted reflection edit");
              const auto pendingAuthoring = target.forward->reflectionAuthoring();
              const auto activeHeader = pendingAuthoring->header();
              target.enableForward(options, true);
              require(target.forward->reflectionAuthoring() == pendingAuthoring &&
                          pendingAuthoring->header() == activeHeader && pendingAuthoring->statistics().pending,
                      "Target replacement retains pending transport and the last published image together");
              authored[1].capture[0] = originalCaptureX;
              ++request.revision;
              r.setReflectionVolumes(&request);
              waitForEdit();
              require(target.forward->statistics().authoredReflections.reused == 2 &&
                          target.forward->statistics().authoredReflections.rays == 0,
                      "A superseding edit cancels old work while reusing the last complete captures");
              const auto after = target.forward->statistics();
              require(r.bakeScene() == snapshot && after.baking.active &&
                          after.baking.activeHash == baseline.baking.activeHash &&
                          after.bakeContent.grid == baseline.bakeContent.grid &&
                          after.bakeContent.surfaces == baseline.bakeContent.surfaces,
                      "Authored reflection edits preserve completed diffuse probes and surface lightmaps");
              options.authoredReflectionFallback = true;
              target.forward->updateLighting(options);
              waitForEdit();
              require(target.forward->statistics().authoredReflections.rays == 0 &&
                          target.forward->statistics().authoredReflections.reused == 2 &&
                          target.forward->statistics().baking.activeHash == baseline.baking.activeHash,
                      "Enabling automatic fallback reuses authored radiance without rebaking diffuse lighting");
              // Shrink both influence boxes away from the receiver. Automatic
              // capture radiance must now light it outside authored coverage.
              auto fallbackVolumes = std::array{authored[0], authored[1]};
              for (auto &volume : fallbackVolumes)
                for (uint32_t c = 0; c < 3; ++c) {
                  float capture = volume.worldToLocal[c][3];
                  for (uint32_t axis = 0; axis < 3; ++axis)
                    capture += volume.worldToLocal[c][axis] * volume.capture[axis];
                  volume.minimum[c] = capture - .01f;
                  volume.maximum[c] = capture + .01f;
                }
              request.volumes = fallbackVolumes.data(); ++request.revision;
              r.setReflectionVolumes(&request);
              const auto fallbackImage = waitForEdit();
              require(std::count_if(target.forward->statistics().reflectionIdentities.begin(),
                          target.forward->statistics().reflectionIdentities.end(),
                          [](uint64_t id) { return id != 0; }) == 2,
                      "Composed automatic layers remain unbound while preserving both authored identities");
              require(target.forward->statistics().authoredReflections.automatic > 0 &&
                          target.forward->statistics().authoredReflections.rays == 0 && fallbackImage[centre] > 40,
                      "Automatic image layers supply reflections outside edited authored bounds without retracing");
              options.authoredReflectionFallback = false;
              target.forward->updateLighting(options);
              waitForEdit();
              const auto unbound = render(false, false, true, false);
              remixapi_MaterialReflectionBindingEXT receiverBinding{
                  REMIXAPI_STRUCT_TYPE_MATERIAL_REFLECTION_BINDING_EXT, nullptr, snapshot->sceneId, authored[0].id};
              const auto boundMesh = quad(r, 13100, matte(13101, {1, 1, 1}, 1, .04f, &receiverBinding));
              weapon.mesh = boundMesh;
              const auto bound = render(false, false, true, false);
              require(bound[centre] > unbound[centre] + 8,
                      "Viewmodel receiver binding selects authored radiance outside influence bounds");
              weapon.categoryFlags = 0;
              const auto boundWorld = render(false, false, true, false);
              require(target.forward->statistics().boundReflectionDraws > 0,
                      "Reflection diagnostics count resolved receiver draws separately from static shadow commands");
              // Target recreation (MSAA/output changes) keeps CPU bindings and diffuse
              // ownership while the new renderer republishes its authored captures.
              const auto retainedBaking = target.forward->bakeController();
              const auto retainedReflections = target.forward->reflectionAuthoring();
              const auto publishedHeader = retainedReflections->header();
              bool incompatibleRejected = false;
              try {
                remix::forward::Renderer incompatible;
                incompatible.initialize(target.physical, target.device, target.pass, target.extent, options,
                    false, target.pipelines.get(), target.hardwareRay, {}, target.queueFamily + 1, {}, false,
                    retainedReflections);
              } catch (const std::invalid_argument &) { incompatibleRejected = true; }
              require(incompatibleRejected,
                      "Target recreation rejects capture ownership from a different queue family before GPU preparation");
              options.samples = samples == 1 ? 4 : 1;
              target.enableForward(options, true);
              require(target.forward->reflectionAuthoring()->header() == publishedHeader &&
                          !target.forward->reflectionAuthoring()->statistics().pending,
                      "Changing MSAA retains the published capture image without scheduling new transport");
              const auto rebuiltWorld = waitForEdit();
              require(target.forward->bakeController() == retainedBaking &&
                          target.forward->reflectionAuthoring() == retainedReflections && r.bakeScene() == snapshot &&
                          target.forward->statistics().baking.activeHash == baseline.baking.activeHash &&
                          target.forward->statistics().boundReflectionDraws > 0 &&
                          std::abs(int(rebuiltWorld[centre]) - int(boundWorld[centre])) <= 2,
                      "Target recreation restores bound receiver radiance while preserving the diffuse bake");
              weapon.mesh = weaponMesh;
              const auto unboundWorld = render(false, false, true, false);
              require(boundWorld[centre] > unboundWorld[centre] + 8,
                      "World reflection guides preserve the explicit receiver assignment through MSAA resolve");
              weapon.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL;
              receiverBinding.sceneId ^= 1;
              weapon.mesh = quad(r, 13102, matte(13103, {1, 1, 1}, 1, .04f, &receiverBinding));
              require(render(false, false, true, false)[centre] == unbound[centre],
                      "A binding from another scene uses ordinary selection rather than a stale image layer");
              require(target.forward->statistics().unresolvedReflectionDraws > 0 &&
                          target.forward->statistics().boundReflectionDraws == 0,
                      "Missing-scene receiver diagnostics distinguish assignments from resolved captures");
              options.samples = samples;
              target.enableForward(options, true);
              waitForEdit();
              weapon.mesh = weaponMesh;
              options.authoredReflectionFallback = true;
              target.forward->updateLighting(options);
              request.volumes = authored; ++request.revision;
              r.setReflectionVolumes(&request);
              waitForEdit();
              options.authoredReflectionFallback = false;
              target.forward->updateLighting(options);
              waitForEdit();
              require(target.forward->statistics().reflectionProbes.parameters[0] == 2 &&
                          target.forward->statistics().authoredReflections.rays == 0,
                      "Disabling fallback restores the authored-only set without retracing");
              options.authoredReflectionFallback = true;
              target.forward->updateLighting(options);
              waitForEdit();
              // Only the last probe can influence the weapon. This exercises
              // the upper mask word through authored publication and shading.
              std::array<remixapi_ReflectionVolumeEXT, 64> complete{};
              for (uint32_t i = 0; i < 63; ++i) {
                auto &volume = complete[i];
                volume.id = 1000 + i;
                for (uint32_t c = 0; c < 3; ++c) {
                  volume.minimum[c] = 100 + float(i * 2);
                  volume.maximum[c] = volume.minimum[c] + 1;
                  volume.capture[c] = volume.minimum[c] + .5f;
                  volume.worldToLocal[c][c] = 1;
                }
              }
              complete.back() = authored[0];
              complete.back().priority = -1; // force its published image to layer 63
              request.volumeCount = uint32_t(complete.size());
              request.volumes = complete.data(); ++request.revision;
              r.setReflectionVolumes(&request);
              const auto lastProbe = waitForEdit();
              require(target.forward->statistics().reflectionProbes.parameters[0] == 64 &&
                          std::abs(int(lastProbe[centre]) - int(firstEdit[centre])) <= 2,
                      "The 64th rotated probe preserves reflections while unrelated volumes are excluded");
              weapon.categoryFlags = 0;
              const auto lastWorld = render(false, false, true, false);
              receiverBinding.sceneId = snapshot->sceneId;
              receiverBinding.volumeId = complete.back().id;
              weapon.mesh = quad(r, 13104, matte(13105, {1, 1, 1}, 1, .04f, &receiverBinding));
              const auto lastBoundWorld = render(false, false, true, false);
              require(target.forward->statistics().reflectionIdentities[63] == receiverBinding.volumeId &&
                          lastBoundWorld[centre] > 5 &&
                          std::abs(int(lastBoundWorld[centre]) - int(lastWorld[centre])) <= 2,
                      "The highest receiver selector survives RGBA8 guides and MSAA resolution without aliasing layers");
              weapon.mesh = weaponMesh;
              weapon.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL;
              std::printf("Viewmodel last-probe image %ux: %llu\n", samples,
                          static_cast<unsigned long long>(remix::scene::fastHash(lastProbe.data(), lastProbe.size())));
              request.volumeCount = 0; ++request.revision;
              r.setReflectionVolumes(&request);
              require(waitForEdit()[centre] < 5, "An empty authoring document suppresses local cubemaps");
              r.setReflectionVolumes(nullptr);
              require(render(false, false, true, false)[centre] > 40,
                      "Restoring automatic placement immediately restores the original reflection buffer");
              for (uint32_t frame = 0; frame < 8; ++frame) render(false, false, true, false);
              const auto restored = target.forward->statistics();
              require(restored.targetBytes == baseline.targetBytes &&
                          restored.accelerationBytes == baseline.accelerationBytes,
                      "Retiring authored edits releases their images and capture acceleration after frame fences");
              std::printf("Authored cleanup %ux: image bytes %llu -> %llu, acceleration bytes %llu -> %llu\n",
                          samples, static_cast<unsigned long long>(baseline.targetBytes),
                          static_cast<unsigned long long>(restored.targetBytes),
                          static_cast<unsigned long long>(baseline.accelerationBytes),
                          static_cast<unsigned long long>(restored.accelerationBytes));
              remix::forward::bake::cache::persistence().waitIdle();
              options.baking.enabled = false;
              target.forward->updateLighting(options);
              render(false, false, true, false);
              require(!target.forward->statistics().baking.active, "Cache reload fixture retires the active bake");
              options.baking.enabled = true;
              target.forward->updateLighting(options);
              const auto cacheDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
              do {
                render(false, false, true, false);
                if (target.forward->statistics().baking.active) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
              } while (std::chrono::steady_clock::now() < cacheDeadline);
              const auto cached = target.forward->statistics();
              require(cached.baking.active && std::strstr(cached.baking.status, "Cached lighting active") &&
                          cached.bakeContent.captureInputs == baseline.bakeContent.captureInputs,
                      "Warm automatic cubemap cache preserves exact capture provenance after scratch release");
            }
            std::printf("Viewmodel probes %ux: overlay %u, world %u, analytic lights %u\n",
                        samples, probeWeapon[centre], probeWorld[centre], target.forward->statistics().lights);
            continue;
          }
          const auto bright = render(false, false, true, true);
          const auto dark = render(true, false, true, true);
          require(bright[centre] > bright[centre + 1] * 1.4f && bright[centre] > 40,
                  "Viewmodel uses lit PBR shading rather than compatibility colour");
          require(dark[centre] < bright[centre] * .6f,
                  "World dynamic casters shadow the isolated viewmodel receiver");
          const auto selfShadowed = render(false, false, true, true, true);
          require(selfShadowed[centre] < bright[centre] * .6f,
                  "Isolated sun or local maps supply viewmodel self shadows");
          // Keep the physically occluded weapon visible through ambient light;
          // overlay ordering is independent of whether the wall blocks its sun.
          options.ambient = .2f;
          target.forward->updateLighting(options);
          const auto overlay = render(false, true, true, true);
          require(overlay[centre] > overlay[centre + 1] * 1.4f,
                  "Viewmodel remains over world geometry even when physically behind it");
          options.ambient = 0;
          target.forward->updateLighting(options);
          const auto unlit = render(false, false, true, false);
          require(unlit[centre] < 5 && unlit[centre + 1] < 5,
                  "Viewmodel does not remain fullbright without lighting");
          const auto fallback = render(false, true, true, true, false, true);
          require(fallback[centre+2] > 200 && fallback[centre] < 5,
                  "Raster-only viewmodel attachments retain their overlay layer after camera correction");
          const auto world = render(false, false, false, true);
          require(std::abs(int(world[centre]) - int(world[centre+1])) < 3,
                  "Removing the weapon cannot leave a stale overlay in a reused frame slot");
          const auto weaponAgain = render(false, false, true, true);
          const auto isolated = render(false, false, true, true, true);
          for (uint32_t y = 0; y < 64; ++y)
            for (uint32_t x = 0; x < 64; ++x) {
              if (x >= 23 && x <= 40 && y >= 23 && y <= 40) continue;
              const size_t pixel = (y * 64 + x) * 4;
              for (uint32_t c = 0; c < 3; ++c)
                require(std::abs(int(world[pixel + c]) - int(weaponAgain[pixel + c])) <= 1,
                        "Viewmodel cannot cast shadows or alter world reflection guides");
              for (uint32_t c = 0; c < 3; ++c)
                require(std::abs(int(world[pixel + c]) - int(isolated[pixel + c])) <= 1,
                        "Self-shadow casters affect only viewmodel receivers");
            }
          std::printf("Viewmodel %ux SSR %u local %u: lit %u, world-shadowed %u, overlay %u\n",
                      samples, unsigned(ssr), unsigned(local), bright[centre], dark[centre], overlay[centre]);
        }
  }
  void terrainLightingTests(bool decal = false) {
    for (bool ssr : {false, true})
      for (uint32_t samples : {1u, 2u, 4u}) {
        Target target(64, 64, true);
        auto &r = target.raster;
        remix::forward::Options options;
        options.baking.lightmapFitBudget = false; // Controlled-density lighting fixtures.
        options.samples = samples;
        options.ambient = 0;
        options.ssr = ssr;
        options.shadowResolution = 128;
        options.baking.maxProbes = 64;
        options.baking.samples = 128;
        options.baking.bounces = 1;
        options.baking.batch = 128;
        options.baking.reflectionProbes = 0;
        options.baking.lightmapPageSize = 32;
        options.baking.diskCache = false;
        target.enableForward(options);
        const auto baseMesh = quad(r, 12000, material(r, 12001, 1, 1, 1));
        const auto layerMesh = quad(r, 12002, material(r, 12003, 1, 0, 0), 0x80ffffff);
        const auto shadeMesh = quad(r, 12004, material(r, 12005, 0, 0, 0));
        remixapi_InstanceIdentityEXT baseIdentity{};
        baseIdentity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
        baseIdentity.instanceId = 12006;
        baseIdentity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
        baseIdentity.rasterVisible = 1;
        auto base = instance(baseMesh);
        base.pNext = &baseIdentity;
        const auto baseHandle = r.createRetained(base);
        remixapi_InstanceInfoBlendEXT blend{};
        blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
        blend.alphaTestCompareOp = 7;
        blend.alphaBlendEnabled = 1;
        blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = 6;
        blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = 7;
        blend.writeMask = 15;
        blend.textureAlphaOperation = 1;
        blend.textureAlphaArg1Source = 2;
        auto layerIdentity = baseIdentity;
        layerIdentity.instanceId = 12007;
        layerIdentity.pNext = &blend;
        auto layer = instance(layerMesh, -.001f);
        layer.pNext = &layerIdentity;
        layer.categoryFlags = (decal ? 0 : REMIXAPI_INSTANCE_CATEGORY_BIT_TERRAIN) | REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC;
        const auto layerHandle = r.createRetained(layer);
        require(target.forward->owns(r.retained(layerHandle)->records[0]),
                "Tagged terrain paint belongs to lit Forward+ rather than unlit compatibility rendering");
        auto shadeIdentity = baseIdentity;
        shadeIdentity.instanceId = 12008;
        shadeIdentity.rasterVisible = 0;
        auto shade = instance(shadeMesh, -.4f);
        shade.pNext = &shadeIdentity;
        shade.doubleSided = 1;
        shade.transform.matrix[0][0] = .5f;
        shade.transform.matrix[0][3] = -.4f;
        shade.transform.matrix[1][1] = 3;
        const uint64_t instances[] = {baseHandle, layerHandle, r.createRetained(shade)};
        remixapi_LightInfoDistantEXT distant{};
        distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
        distant.direction = {0, 0, 1};
        remixapi_LightInfo light{};
        light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
        light.pNext = &distant;
        light.hash = 12009;
        light.radiance = {1, 1, 1};
        remixapi_LightHandle sun;
        r.createLight(light, sun);
        const float uv[] = {0, 0, 8, 8, 8, 0, 0, 0, 0, 8, 8, 8};
        const remixapi_BakeSurfaceChartEXT charts[] = {
            {baseHandle, 0, 0, 0, 6, 9, 9, uv}, {layerHandle, 0, 0, 0, 6, 9, 9, uv}};
        remixapi_BakeSceneSurfacesInfoEXT request{};
        request.bounds.scene = {sizeof(request), REMIXAPI_BAKE_SCENE_SURFACES_VERSION_EXT, 120, 1, 1,
                                REMIXAPI_BAKE_SCENE_READY_EXT, 3, 1, instances, &sun};
        for (uint32_t c = 0; c < 3; ++c) {
          request.bounds.minimum[c] = 10;
          request.bounds.maximum[c] = 11;
        }
        request.chartRevision = 1;
        request.chartCount = decal ? 1 : 2;
        request.charts = charts;
        r.setBakeScene(&request.bounds.scene);
        auto render = [&](bool paint) {
          camera(r);
          if (paint) r.drawRetained(nullptr);
          else r.drawInstance(base);
          r.drawLight(sun);
          return target.render();
        };
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        do {
          render(true);
          require(target.forward->statistics().baking.state != REMIXAPI_BAKE_FAILED_EXT,
                  "Terrain lightmap bake succeeds");
          if (target.forward->statistics().baking.active) break;
        } while (std::chrono::steady_clock::now() < deadline);
        require(target.forward->statistics().baking.active, "Terrain lightmaps publish");
        const auto plain = render(false), painted = render(true);
        const size_t left = (32 * 64 + 16) * 4, right = (32 * 64 + 48) * 4;
        require(plain[left] < 3 && painted[left] < 3,
                "Terrain paint respects the baked shadow instead of glowing over its receiver");
        require(plain[right] > 30 && std::abs(int(plain[right]) - int(painted[right])) <= 3,
                "Identical terrain colour channels retain the receiver's lighting across blend boundaries");
        require(painted[right + 1] > plain[right + 1] * .35f &&
                    painted[right + 1] < plain[right + 1] * .75f,
                "Vertex coverage blends different terrain albedos in HDR without making the layer opaque");
        std::printf("Terrain %ux MSAA, SSR %u: shadow %u, lit base %u, painted RGB %u %u %u\n",
                    samples, unsigned(ssr), painted[left], plain[right], painted[right],
                    painted[right + 1], painted[right + 2]);
      }
  }
  void compactOcclusionTests(bool saturated = false, bool partialOverflow = false) {
    for (uint32_t samples : {1u, 2u, 4u}) {
      Target target(64, 64, true);
      auto &r = target.raster;
      remix::forward::Options options;
      options.baking.lightmapFitBudget = false; // Controlled-density lighting fixtures.
      options.samples = samples;
      options.prism = false; // Visibility assertions inspect linear channels before display mixing.
      options.ambient = 0;
      options.shadows = options.localShadows = options.ssr = false;
      options.baking.maxProbes = 64;
      options.baking.samples = 64;
      options.baking.bounces = 1;
      options.baking.batch = 128;
      options.baking.reflectionProbes = 0;
      options.baking.lightmapPageSize = 32;
      if (saturated)
        options.baking.diskCache = false; // each MSAA case must execute the overflow audit
      target.enableForward(options);
      auto receiverMaterial = material(r, 9701, 1, 1, 1);
      if (saturated) {
        remixapi_MaterialInfoOpaqueEXT metal{};
        metal.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
        metal.albedoConstant = {1, 1, 1};
        metal.opacityConstant = 1;
        metal.roughnessConstant = .45f;
        metal.metallicConstant = 1;
        metal.alphaTestType = 7;
        remixapi_MaterialInfo info{};
        info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
        info.pNext = &metal;
        info.hash = 9799;
        r.createMaterial(info, receiverMaterial);
      }
      const auto receiverMesh = quad(r, 9700, receiverMaterial);
      const auto blockerMaterial = material(r, 9703, 0, 0, 0);
      remixapi_HardcodedVertex blockerVertices[3]{};
      const float blockerPositions[3][3] = {{-.7f, -.55f, .5f}, {.55f, .7f, .5f}, {.55f, -.55f, .5f}};
      for (uint32_t i = 0; i < 3; ++i) {
        std::copy_n(blockerPositions[i], 3, blockerVertices[i].position);
        blockerVertices[i].color = 0xffffffff;
      }
      const uint32_t blockerIndices[] = {0, 1, 2};
      remixapi_MeshInfoSurfaceTriangles blockerSurface{};
      blockerSurface.vertices_values = blockerVertices;
      blockerSurface.vertices_count = 3;
      blockerSurface.indices_values = blockerIndices;
      blockerSurface.indices_count = 3;
      blockerSurface.material = blockerMaterial;
      remixapi_MeshInfo blockerInfo{};
      blockerInfo.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
      blockerInfo.hash = 9702;
      blockerInfo.surfaces_values = &blockerSurface;
      blockerInfo.surfaces_count = 1;
      remixapi_MeshHandle blockerMesh;
      r.createMesh(blockerInfo, blockerMesh);
      remixapi_InstanceIdentityEXT identity{};
      identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      identity.instanceId = 9704;
      identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
      identity.rasterVisible = 1;
      auto receiver = instance(receiverMesh);
      receiver.pNext = &identity;
      const auto receiverHandle = r.createRetained(receiver);
      identity.instanceId = 9705;
      identity.rasterVisible = 0;
      auto blocker = instance(blockerMesh, -.45f);
      blocker.pNext = &identity;
      blocker.doubleSided = 1;
      const uint64_t instances[] = {receiverHandle, r.createRetained(blocker)};
      // More than32 blocked lights exercises every RGBA byte and a bitset word
      // seam. No realtime shadow map can hide a broken static-occlusion lookup.
      std::vector<remixapi_LightHandle> lights;
      remixapi_LightInfoSphereEXT sphere{};
      sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
      sphere.position = {0, 0, -.4f};
      sphere.radius = .1f;
      remixapi_LightInfo light{};
      light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
      light.pNext = &sphere;
      light.radiance = {200, 0, 0};
      for (uint32_t i = 0; i < 36; ++i) {
        light.hash = 9710 + i;
        if (i == 35) {
          sphere.position.x = 2.2f;
          // Keep the whole emitter outside the blocker silhouette at the
          // chart's far corner; equal area*radiance preserves its brightness.
          sphere.radius = .05f;
          light.radiance = {0, 800, 0};
        }
        remixapi_LightHandle handle;
        r.createLight(light, handle);
        lights.push_back(handle);
      }
      if (saturated) {
        // Large green emitters hold all four partial-visibility slots. Red
        // behind the blocker must still be rejected, including metallic specular.
        sphere.position = {0, 0, -.4f};
        sphere.radius = .5f;
        light.radiance = {0, 5000, 0};
        for (uint32_t i = 0; i < 4; ++i) {
          light.hash = 9800 + i;
          remixapi_LightHandle handle;
          r.createLight(light, handle);
          lights.push_back(handle);
        }
      }
      const float uv[] = {0, 0, 8, 8, 8, 0, 0, 0, 0, 8, 8, 8};
      const remixapi_BakeSurfaceChartEXT charts[] = {
          {receiverHandle, 0, 0, 0, partialOverflow ? 6u : 3u, 9, 9, uv},
          {receiverHandle, 0, 0, 3, 3, 9, 9, uv + 6}};
      remixapi_BakeSceneSurfacesInfoEXT request{};
      request.bounds.scene = {sizeof(request), REMIXAPI_BAKE_SCENE_SURFACES_VERSION_EXT, 97, 1, 1,
                              REMIXAPI_BAKE_SCENE_READY_EXT, 2, uint32_t(lights.size()), instances, lights.data()};
      for (uint32_t c = 0; c < 3; ++c) {
        request.bounds.minimum[c] = 10;
        request.bounds.maximum[c] = 11;
      }
      request.chartRevision = 1;
      request.chartCount = partialOverflow ? 1u : 2u;
      request.charts = charts;
      r.setBakeScene(&request.bounds.scene);
      auto render = [&](bool charted = true) {
        camera(r);
        for (auto lamp : lights) r.drawLight(lamp);
        if (charted) r.drawRetained(nullptr);
        else r.drawInstance(instance(receiverMesh));
        return target.render();
      };
      std::vector<uint8_t> pixels;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
      do {
        pixels = render();
        const auto &bake = target.forward->statistics().baking;
        require(bake.state != REMIXAPI_BAKE_FAILED_EXT, "Compact static occlusion bake succeeds");
        if (bake.active) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while (std::chrono::steady_clock::now() < deadline);
      require(target.forward->statistics().baking.active, "Compact static occlusion publishes");
      const size_t center = (48 * 64 + 48) * 4;
      const size_t neighbouringChart = (16 * 64 + 16) * 4;
      std::printf("Compact occlusion %ux MSAA: RGB %u %u %u; %s\n", samples,
                  unsigned(pixels[center]), unsigned(pixels[center + 1]), unsigned(pixels[center + 2]),
                  target.forward->statistics().baking.status);
      if (saturated) {
        require(pixels[center] < 3 && pixels[center + 1] > 30 && pixels[center + 2] < 3,
                "Hidden overflow lights cannot produce metallic specular after four penumbra slots fill");
        if (partialOverflow) {
          require(pixels[neighbouringChart] > 30,
                  "An overflow mask preserves the exposed part of the same connected chart");
          require(target.forward->bakeController()->volume()->occlusionParameters[3] != 0,
                  "Partial overflow publishes sparse visibility masks");
          std::printf("Partial overflow exposed red %u; blocked red %u\n",
                      unsigned(pixels[neighbouringChart]), unsigned(pixels[center]));
          auto single = [&] {
            camera(r);
            r.drawLight(lights[0]);
            r.drawRetained(nullptr);
            return target.render();
          };
          sphere.position = {0, 0, -.4f};
          sphere.radius = .1f;
          light.hash = 9710;
          light.radiance = {0, 0, 7000}; // match the energy of the 35 red test lights
          r.updateLight(lights[0], light);
          pixels = single();
          std::printf("Sparse colour edit: blocked blue %u, exposed blue %u\n",
                      unsigned(pixels[center + 2]), unsigned(pixels[neighbouringChart + 2]));
          require(pixels[center + 2] < 3 && pixels[neighbouringChart + 2] > 30,
                  "A light colour edit retains sparse static visibility with the correct light identity");
          sphere.position.z = -.8f;
          r.updateLight(lights[0], light);
          pixels = single();
          std::printf("Sparse moved light: blocked-site blue %u\n", unsigned(pixels[center + 2]));
          require(pixels[center + 2] > 30, "A moved light rejects its old sparse visibility mask");
          r.setBakeScene(nullptr);
          for (uint32_t frame = 0; frame <= remix::gpuFrameCount; ++frame)
            pixels = single();
          require(!target.forward->statistics().baking.active && pixels[center + 2] > 30,
                  "Unload releases sparse visibility with its published bake volume");
        } else {
          require(std::strstr(target.forward->statistics().baking.status, "35 fully blocked"),
                  "All hidden red overflow emitters retain chart-specific compact visibility");
        }
        continue;
      }
      require(pixels[center] < 3 && pixels[center + 1] > 30 && pixels[center + 2] < 3 &&
                  std::strstr(target.forward->statistics().baking.status, "35 fully blocked") &&
                  std::strstr(target.forward->statistics().baking.status, "2 fully clear"),
              "Blocked overflow lights stay dark while promoted clear visibility uses compact metadata");
      pixel(pixels, 48, 48, 0, 53, 0,
            "Compacting a clear light preserves its original baked unit direct diffuse irradiance");
      require(pixels[neighbouringChart] > 30,
              "A partially lit neighbouring chart cannot inherit another chart's compact occlusion");
      light.hash = 9710;
      light.radiance = {0, 0, 200};
      sphere.radius = .1f;
      sphere.position = {0, 0, -.4f};
      r.updateLight(lights[0], light);
      pixels = render();
      require(pixels[center] < 3 && pixels[center + 2] < 3 && pixels[center + 1] > 30,
              "A colour edit cannot remove valid static occlusion");
      sphere.position.x = 2.2f;
      r.updateLight(lights[0], light);
      pixels = render();
      require(pixels[center + 2] > 30, "A moved light rejects its old compact occlusion");
      pixels = render(false);
      require(pixels[center] > 30, "Uncharted instances cannot borrow another chart's negative visibility");
      r.setBakeScene(nullptr);
      for (uint32_t i = 0; i < remix::gpuFrameCount + 1; ++i) pixels = render();
      require(!target.forward->statistics().baking.active && pixels[center] > 30,
              "Unload clears compact occlusion with the rest of the published volume");
    }
  }
  void clearVisibilityTests() {
    for (uint32_t samples : {1u, 2u, 4u}) {
      Target target(64, 64, true);
      auto &r = target.raster;
      remix::forward::Options options;
      options.baking.lightmapFitBudget = false; // Controlled-density lighting fixtures.
      options.samples = samples;
      options.ambient = 0;
      options.shadows = options.ssr = false;
      options.localShadows = true;
      options.localShadowLights = 1;
      options.localShadowResolution = 512;
      options.localShadowFade = 0; // Spatial fixtures use immediate selection; elapsed transitions are tested separately.
      options.baking.samples = 256;
      options.baking.bounces = 1;
      options.baking.batch = 128;
      options.baking.maxProbes = 64;
      options.baking.reflectionProbes = 0;
      options.baking.lightmapPageSize = 32;
      target.enableForward(options);
      const auto mesh = quad(r, 9800, material(r, 9801, 1, 1, 1));
      remixapi_InstanceIdentityEXT identity{};
      identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      identity.instanceId = 9802;
      identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
      identity.rasterVisible = 1;
      auto receiver = instance(mesh);
      receiver.pNext = &identity;
      const auto retained = r.createRetained(receiver);
      remixapi_LightInfoSphereEXT sphere{};
      sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
      sphere.position = {0, 0, -.8f};
      sphere.radius = .3f;
      remixapi_LightInfo light{};
      light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
      light.pNext = &sphere;
      light.hash = 9803;
      light.radiance = {8, 0, 0};
      remixapi_LightHandle lamp;
      r.createLight(light, lamp);
      const float uv[] = {0, 0, 8, 8, 8, 0, 0, 0, 0, 8, 8, 8};
      const remixapi_BakeSurfaceChartEXT chart{retained, 0, 0, 0, 6, 9, 9, uv};
      remixapi_BakeSceneSurfacesInfoEXT request{};
      request.bounds.scene = {sizeof(request), REMIXAPI_BAKE_SCENE_SURFACES_VERSION_EXT, 98, 1, 1,
                              REMIXAPI_BAKE_SCENE_READY_EXT, 1, 1, &retained, &lamp};
      for (uint32_t c = 0; c < 3; ++c) {
        request.bounds.minimum[c] = 10;
        request.bounds.maximum[c] = 11;
      }
      request.chartRevision = request.chartCount = 1;
      request.charts = &chart;
      r.setBakeScene(&request.bounds.scene);
      auto render = [&] {
        camera(r);
        r.drawRetained(nullptr);
        r.drawLight(lamp);
        return target.render();
      };
      std::vector<uint8_t> pixels;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
      do {
        pixels = render();
        const auto &bake = target.forward->statistics().baking;
        require(bake.state != REMIXAPI_BAKE_FAILED_EXT, "Clear visibility bake succeeds");
        if (bake.active) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while (std::chrono::steady_clock::now() < deadline);
      const auto &bake = target.forward->statistics().baking;
      require(bake.active && std::strstr(bake.status, "1 fully clear"),
              "A wholly visible emitter publishes compact clear visibility");
      const auto hash = bake.activeHash;
      const size_t center = (32 * 64 + 48) * 4, adjacent = (27 * 64 + 48) * 4;
      const auto lit = pixels[center];
      require(lit > 80, "A clear light retains its baked direct illumination");
      auto moving = instance(quad(r, 9804, material(r, 9805, 0, 0, 0)), -.4f);
      moving.doubleSided = 1;
      moving.transform.matrix[0][0] = .15f;
      moving.transform.matrix[0][3] = .35f;
      moving.transform.matrix[1][1] = .04f;
      identity.instanceId = 9806;
      moving.pNext = &identity;
      const auto actor = r.createRetained(moving);
      pixels = render();
      std::printf("Compact clear %ux MSAA: lit %u, moving %u, adjacent %u\n", samples,
                  unsigned(lit), unsigned(pixels[center]), unsigned(pixels[adjacent]));
      require(pixels[center] < lit / 3 && pixels[adjacent] > lit / 2 &&
                  target.forward->statistics().baking.activeHash == hash,
              "A realtime caster cuts a narrow shadow into compact clear baked lighting without rebaking");
      light.radiance = {0, 0, 8};
      r.updateLight(lamp, light);
      pixels = render();
      require(pixels[center + 2] < lit / 3 && pixels[adjacent] < 3 && pixels[adjacent + 2] > lit / 2,
              "Live colour edits preserve the clear light's baked ownership and dynamic shadow");
      moving.transform.matrix[1][3] = .4f;
      r.updateRetained(actor, moving);
      pixels = render();
      require(std::abs(int(pixels[center + 2]) - int(lit)) < 4 &&
                  target.forward->statistics().staticShadowUpdates == 0,
              "Moving the caster restores compact clear lighting without rebuilding static maps");
      r.destroyRetained(&actor, 1);
      require(errors.load() == 0, "Compact clear visibility and realtime shadows are validation clean");
    }
  }
  void sunLightingTests() {
    for (uint32_t samples : {1u, 2u, 4u}) {
      Target target(64, 64, true);
      auto &r = target.raster;
      remix::forward::Options options;
      options.baking.lightmapFitBudget = false; // Controlled-density lighting fixtures.
      // This fixture measures shadow visibility in linear output, independent
      // of the shared display curve's compression of saturated primaries.
      options.prism = false;
      options.exposure = 2;
      options.samples = samples;
      options.rtGI = samples == 2;
      options.rtGISamples = options.rtGIBounces = 1;
      options.ambient = 0;
      options.ssr = options.localShadows = false;
      options.shadowResolution = 512;
      options.shadowDistance = 3;
      options.baking.samples = 256;
      options.baking.bounces = 1;
      options.baking.batch = 128;
      options.baking.maxProbes = 64;
      options.baking.reflectionProbes = 0;
      options.baking.lightmapPageSize = 32;
      options.baking.diskCache = false; // exercise cancellation in the sun dispatch itself
      target.enableForward(options);
      const auto receiverMesh = quad(r, 9900, material(r, 9901, 1, 1, 1));
      const auto blockerMesh = quad(r, 9902, material(r, 9903, 0, 0, 0));
      remixapi_InstanceIdentityEXT identity{};
      identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      identity.instanceId = 9904;
      identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
      identity.rasterVisible = 1;
      auto receiver = instance(receiverMesh);
      receiver.pNext = &identity;
      const auto receiverHandle = r.createRetained(receiver);
      auto blocker = instance(blockerMesh, -.4f);
      blocker.doubleSided = 1;
      blocker.transform.matrix[0][0] = .5f;
      blocker.transform.matrix[0][3] = -.4f;
      blocker.transform.matrix[1][1] = 3;
      identity.instanceId = 9905;
      identity.rasterVisible = 0;
      blocker.pNext = &identity;
      const uint64_t instances[] = {receiverHandle, r.createRetained(blocker)};
      remixapi_LightInfoDistantEXT distant{};
      distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
      distant.direction = {0, 0, 1};
      distant.angularDiameterDegrees = 40;
      remixapi_LightInfo light{};
      light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
      light.pNext = &distant;
      light.hash = 9906;
      light.radiance = {1, 0, 0};
      remixapi_LightHandle sun;
      r.createLight(light, sun);
      const float uv[] = {0, 0, 8, 8, 8, 0, 0, 0, 0, 8, 8, 8};
      const remixapi_BakeSurfaceChartEXT chart{receiverHandle, 0, 0, 0, 6, 9, 9, uv};
      remixapi_BakeSceneSurfacesInfoEXT request{};
      auto &base = request.bounds.scene;
      base = {sizeof(request), REMIXAPI_BAKE_SCENE_SURFACES_VERSION_EXT, 99, 1, 1,
              REMIXAPI_BAKE_SCENE_READY_EXT, 2, 1, instances, &sun};
      for (uint32_t c = 0; c < 3; ++c) {
        request.bounds.minimum[c] = 10;
        request.bounds.maximum[c] = 11;
      }
      request.chartRevision = request.chartCount = 1;
      request.charts = &chart;
      r.setBakeScene(&base);
      std::optional<remixapi_InstanceInfo> probeCaster;
      auto render = [&](bool charted = true) {
        camera(r);
        if (charted) r.drawRetained(nullptr);
        else r.drawInstance(instance(receiverMesh));
        if (probeCaster) r.drawInstance(*probeCaster);
        r.drawLight(sun);
        return target.render();
      };
      auto until = [&](bool ready) {
        std::vector<uint8_t> pixels;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        do {
          pixels = render();
          const auto &bake = target.forward->statistics().baking;
          require(bake.state != REMIXAPI_BAKE_FAILED_EXT, "Mixed sunlight bake succeeds");
          if ((ready && bake.active && bake.state == REMIXAPI_BAKE_READY_EXT) || (!ready && std::strstr(bake.status, "Baking static sunlight")))
            return pixels;
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        require(false, "Mixed sunlight reaches the requested bake phase");
        return pixels;
      };
      // Cancel the new dispatch mode while its owning frame submissions exist.
      until(false);
      r.setBakeScene(nullptr);
      for (uint32_t i = 0; i < remix::gpuFrameCount + 2; ++i) render();
      require(!target.forward->statistics().baking.active, "Cancelled sunlight cannot publish a stale volume");
      options.baking.diskCache = true;
      target.forward->updateLighting(options);
      ++base.revision;
      r.setBakeScene(&base);
      auto pixels = until(true);
      const size_t left = (32 * 64 + 16) * 4, edge = (32 * 64 + 32) * 4,
                   right = (32 * 64 + 48) * 4, adjacent = (27 * 64 + 48) * 4;
      const auto lit = pixels[right];
      pixels = render();
      require(target.forward->statistics().staticSunUpdates == 0 &&
                  target.forward->statistics().staticCasterPreparations == 0 &&
                  target.forward->statistics().capturedCasters == 2 &&
                  target.forward->statistics().liveCasters == 0,
              "Captured world geometry stays out of moving sun maps and unchanged static cascades are cached");
      std::printf("Mixed sun %ux: blocked %u, penumbra %u, lit %u\n", samples,
                  unsigned(pixels[left]), unsigned(pixels[edge]), unsigned(lit));
      require(pixels[left] < 3 && lit > 80 && pixels[edge] > 15 && pixels[edge] < lit - 10,
              "A coarse sunlight atlas retains soft static penumbrae independently of static cascade depth");
      const auto hash = target.forward->statistics().baking.activeHash;
      auto moving = instance(blockerMesh, -.4f);
      moving.doubleSided = 1;
      moving.transform.matrix[0][0] = .15f;
      moving.transform.matrix[0][3] = .4f;
      moving.transform.matrix[1][1] = .04f;
      identity.instanceId = 9910;
      moving.pNext = &identity;
      const auto actor = r.createRetained(moving);
      pixels = render();
      std::printf("Mixed sun moving %ux: covered %u, adjacent %u\n", samples,
                  unsigned(pixels[right]), unsigned(pixels[adjacent]));
      require(pixels[right] < lit / 3 && pixels[adjacent] > lit / 2 && pixels[left] < 3,
              "A realtime sun caster cuts a narrow shadow into baked static lighting at independent resolution");
      require(target.forward->statistics().liveCasters == 1 && target.forward->statistics().capturedCasters == 2,
              "Only the new actor enters moving sun maps alongside the captured world");
      require(target.forward->statistics().staticCasterPreparations == 0,
              "A live actor inside the frozen scene bounds needs no frozen caster preparation");
      moving.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_BODY;
      r.updateRetained(actor, moving);
      pixels = render();
      require(pixels[right] > lit / 2 && target.forward->statistics().liveCasters == 0 &&
                  target.forward->statistics().baking.activeHash == hash,
              "Reflection-only players cast no moving sun shadow and do not restart the world bake");
      moving.categoryFlags = 0;
      r.updateRetained(actor, moving);
      pixels = render();
      require(pixels[right] < lit / 3 && target.forward->statistics().liveCasters == 1,
              "Returning a reflection-only player to ordinary geometry restores its moving sun shadow");
      // Skybox-scale depth bounds must not turn a small comparison bias into
      // metres of missing shadow around a gameplay-sized moving caster.
      auto remoteCaster = instance(blockerMesh, 400000);
      remoteCaster.doubleSided = 1;
      remoteCaster.transform.matrix[0][3] = 1000000;
      identity.instanceId = 9911;
      remoteCaster.pNext = &identity;
      const auto remote = r.createRetained(remoteCaster);
      pixels = render();
      require(pixels[right] < lit / 3 && pixels[adjacent] > lit / 2 && pixels[left] < 3,
              "Skybox-scale cascade depth preserves small dynamic shadows and the baked static mask");
      r.destroyRetained(&remote, 1);
      pixels = render();
      light.radiance = {0, 0, 1};
      r.updateLight(sun, light);
      pixels = render();
      require(pixels[right + 2] < lit / 3 && pixels[adjacent] < 3 && pixels[adjacent + 2] > lit / 2 &&
                  target.forward->statistics().baking.activeHash == hash,
              "Current sun colour updates diffuse and specular without rebaking static visibility");
      moving.transform.matrix[1][3] = .4f;
      r.updateRetained(actor, moving);
      pixels = render();
      require(std::abs(int(pixels[right + 2]) - int(lit)) < 4,
              "Moving the sun caster restores its old pixel without shadow trails");
      r.destroyRetained(&actor, 1);
      options.baking.directDiffuse = false;
      target.forward->updateLighting(options);
      pixels = until(true);
      require(pixels[left + 2] < 3 && pixels[edge + 2] > 15 && pixels[edge + 2] < pixels[right + 2] - 10,
              "Visibility-only sunlight still keeps the baked soft static mask");
      const auto soft = pixels[edge + 2];
      distant.angularDiameterDegrees = 0;
      r.updateLight(sun, light);
      pixels = render();
      require(std::abs(int(pixels[edge + 2]) - int(soft)) > 10,
              "Changing sun size rejects the baked penumbra and restores complete realtime shadows");
      distant.angularDiameterDegrees = 40;
      r.updateLight(sun, light);
      pixels = render();
      require(std::abs(int(pixels[edge + 2]) - int(soft)) < 4, "Restoring sun shape reuses its valid bake");
      distant.direction = {-.8944272f, 0, .4472136f};
      r.updateLight(sun, light);
      pixels = render();
      std::printf("Moved sun %ux: left %u, edge %u, right %u\n", samples,
                  unsigned(pixels[left + 2]), unsigned(pixels[edge + 2]), unsigned(pixels[right + 2]));
      require(pixels[left + 2] > 30,
              "Moving the sun rejects its baked shadow and uses the current complete cascade geometry");
      distant.direction = {0, 0, 1};
      r.updateLight(sun, light);
      pixels = render();
      require(pixels[left + 2] < 3 && std::abs(int(pixels[edge + 2]) - int(soft)) < 4,
              "Restoring sun direction recovers the matching soft lightmap without rebaking");
      options.shadows = false;
      target.forward->updateLighting(options);
      pixels = render();
      require(pixels[left + 2] > 80, "Disabling sun shadows also bypasses cached static sunlight occlusion");
      options.shadows = true;
      target.forward->updateLighting(options);
      pixels = render();
      require(pixels[left + 2] < 3, "Re-enabling sun shadows restores the matching baked mask");
      r.destroyRetained(&instances[1], 1);
      pixels = render(false);
      require(pixels[left + 2] < 3 && pixels[right + 2] > 80,
              "Uncharted sun receivers still see complete frozen casters outside live submissions");
      // Put actual probes around the receiver, then move the realtime cascade
      // range entirely in front of it. Occlusion on the uncharted draw can now
      // come only from baked probe data, never either shadow-map family.
      const auto fallbackDistance = options.shadowDistance;
      options.baking.spacing = .125f;
      options.baking.maxProbes = 8192;
      options.baking.diskCache = false;
      target.forward->updateLighting(options);
      // Recreate the frozen occluder removed for the fallback test above.
      const uint64_t probeInstances[] = {receiverHandle, r.createRetained(blocker)};
      base.instances = probeInstances;
      ++base.revision;
      for (uint32_t c = 0; c < 3; ++c) {
        request.bounds.minimum[c] = c == 2 ? .25f : -1.f;
        request.bounds.maximum[c] = c == 2 ? .55f : 1.f;
      }
      r.setBakeScene(&base);
      until(true);
      options.shadowDistance = .11f;
      target.forward->updateLighting(options);
      pixels = render(false);
      std::printf("Probe sunlight %ux: blocked %u, clear %u\n", samples,
                  unsigned(pixels[left + 2]), unsigned(pixels[right + 2]));
      require(pixels[left + 2] < 15 && pixels[right + 2] > 80,
              "Dynamic receivers use baked static sunlight outside all realtime cascades");
      distant.direction = {-.8944272f, 0, .4472136f};
      r.updateLight(sun, light);
      pixels = render(false);
      require(pixels[left + 2] > 30,
              "Changed sun rejects probe visibility even when the cascade fallback is out of range");
      distant.direction = {0, 0, 1};
      r.updateLight(sun, light);
      options.shadowDistance = fallbackDistance;
      target.forward->updateLighting(options);
      identity.instanceId = 9912;
      probeCaster = moving;
      probeCaster->transform.matrix[1][3] = 0;
      pixels = render(false);
      require(pixels[right + 2] < 30 && pixels[adjacent + 2] > 80 && pixels[left + 2] < 15,
              "Moving-only sunlight shadows combine with probe static visibility on dynamic receivers");
      probeCaster.reset();
      pixels = render(false);
      require(pixels[right + 2] > 80 && pixels[left + 2] < 15,
              "Removing the moving caster restores probe-lit receivers without shadow trails");
      r.destroyRetained(&probeInstances[1], 1);
      r.setBakeScene(nullptr);
      for (uint32_t i = 0; i < remix::gpuFrameCount + 1; ++i) pixels = render(false);
      require(pixels[left + 2] > 80 && !target.forward->statistics().baking.active,
              "Unload removes frozen sun masks and frozen depth casters immediately");
      require(errors.load() == 0, "Mixed sunlight is Vulkan validation clean");
    }
  }
#include "styled_indirect.h"
#include "local_shadow_plane.h"
#include "sun_depth_lifecycle.h"
#include "auto_exposure.h"
  void bakingTests() {
    Target target(64, 64, true);
    auto &r = target.raster;
    camera(r);
    remix::forward::Options options;
    options.baking.lightmapFitBudget = false; // Controlled-density lighting fixtures.
    options.ambient = 0;
    options.shadows = false;
    options.ssr = false;
    options.shadowResolution = 128;
    options.baking.maxProbes = 64;
    options.baking.spacing = 2;
    options.baking.samples = 256;
    options.baking.bounces = 1;
    options.baking.batch = 1;
    options.baking.gpuBudgetMs = 0;
    options.baking.diskCache = false; // this fixture checks every preparation/tracing transition
    options.baking.reflectionResolution = 8;
    target.enableForward(options);
    const auto white = material(r, 9000, 1, 1, 1);
    const auto mesh = quad(r, 9001, white);
    r.drawInstance(instance(mesh));
    pixel(target.render(), 32, 32, 0, 0, 0, "World is dark before static lighting is published");
    remixapi_LightInfoDomeEXT dome{};
    dome.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
    for (uint32_t c = 0; c < 3; ++c) dome.transform.matrix[c][c] = 1;
    remixapi_LightInfo light{};
    light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    light.pNext = &dome;
    light.hash = 9002;
    light.radiance = {1, .05f, .05f};
    remixapi_LightHandle lamp;
    r.createLight(light, lamp);
    remixapi_BakeSceneBoundsInfoEXT bounded{};
    auto &info = bounded.scene;
    info.structSize = sizeof(bounded);
    info.version = REMIXAPI_BAKE_SCENE_BOUNDS_VERSION_EXT;
    info.sceneId = info.revision = 1;
    info.sceneScale = 1;
    info.state = REMIXAPI_BAKE_SCENE_READY_EXT;
    info.lightCount = 1;
    info.lights = &lamp;
    for (uint32_t c = 0; c < 3; ++c) {
      bounded.minimum[c] = -1;
      bounded.maximum[c] = 1;
    }
    r.setBakeScene(&info);
    const auto oldHash = r.bakeScene()->contentHash;
    auto until = [&](uint32_t state) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
      std::vector<uint8_t> pixels;
      do {
        camera(r);
        r.drawInstance(instance(mesh));
        pixels = target.render();
        const auto &statistics = target.forward->statistics().baking;
        if (statistics.state == state) return pixels;
        if (statistics.state == REMIXAPI_BAKE_FAILED_EXT) std::fprintf(stderr, "%s\n", statistics.status);
        require(statistics.state != REMIXAPI_BAKE_FAILED_EXT, "Static lighting preparation succeeded");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while (std::chrono::steady_clock::now() < deadline);
      const auto &last = target.forward->statistics().baking;
      std::fprintf(stderr, "Static lighting timeout: requested %u, actual %u, active %u; %s\n",
                   state, last.state, uint32_t(last.active), last.status);
      require(false, "Static lighting reached its requested state before timeout");
      return pixels;
    };
    until(REMIXAPI_BAKE_TRACING_EXT);
    {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (!target.forward->statistics().baking.completed && std::chrono::steady_clock::now() < deadline) {
        camera(r);
        r.drawInstance(instance(mesh));
        target.render();
      }
      const auto before = target.forward->statistics().baking;
      require(before.completed > 0 && before.completed < before.probes && !before.active,
              "Target replacement fixture has completed and pending bake batches");
      const auto controller = target.forward->bakeController();
      options.baking.batch = 2;
      target.forward->updateLighting(options);
      camera(r);
      r.drawInstance(instance(mesh));
      target.render();
      const auto &rescheduled = target.forward->statistics().baking;
      require(rescheduled.requestedHash == oldHash && rescheduled.completed >= before.completed &&
                  rescheduled.state == REMIXAPI_BAKE_TRACING_EXT,
              "Changing fixed work mid-bake preserves completed samples and the existing job");
      options.samples = 1;
      target.enableForward(options, true);
      camera(r);
      r.drawInstance(instance(mesh));
      target.render();
      const auto &after = target.forward->statistics().baking;
      require(target.forward->bakeController() == controller && after.requestedHash == oldHash &&
                  after.completed >= before.completed && after.state == REMIXAPI_BAKE_TRACING_EXT,
              "Replacement targets consume retained fenced batches and continue the same unfinished bake");
      const auto completedBeforeAutomatic = after.completed;
      options.baking.gpuBudgetMs = 2;
      target.forward->updateLighting(options);
      camera(r);
      r.drawInstance(instance(mesh));
      target.render();
      require(target.forward->bakeController() == controller &&
                  target.forward->statistics().baking.completed >= completedBeforeAutomatic,
              "Enabling measured work on an unfinished bake preserves its completed samples");
    }
    until(REMIXAPI_BAKE_READY_EXT);
    require(target.forward->statistics().baking.activeHash == oldHash,
            "The retained unfinished bake reaches publication after target replacement");
    // Start another generation so the existing replacement test still cancels
    // in-flight work rather than merely replacing a completed volume.
    light.radiance = {.05f, .05f, 1};
    r.updateLight(lamp, light);
    r.setBakeScene(&info);
    until(REMIXAPI_BAKE_TRACING_EXT);
    light.radiance = {.05f, 1, .05f};
    r.updateLight(lamp, light);
    r.setBakeScene(&info);
    const auto newHash = r.bakeScene()->contentHash;
    require(newHash != oldHash, "Replaced environment has a new content identity");
    auto pixels = until(REMIXAPI_BAKE_READY_EXT);
    const auto &statistics = target.forward->statistics().baking;
    require(statistics.active && statistics.activeHash == newHash && statistics.completed == statistics.probes,
            "Only the completed replacement scene is activated");
    const size_t center = (32 * 64 + 32) * 4;
    std::printf("Baked surface RGB: %u %u %u; completed %u probes\n", unsigned(pixels[center]),
                unsigned(pixels[center + 1]), unsigned(pixels[center + 2]), statistics.completed);
    require(pixels[center + 1] > 80 && pixels[center + 1] > 3 * pixels[center],
            "Baked environment irradiance reaches the Forward+ surface shader");
    remixapi_MaterialInfoOpaqueEXT metallic{};
    metallic.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    metallic.albedoConstant = {1, 1, 1};
    metallic.opacityConstant = 1;
    metallic.metallicConstant = 1;
    metallic.roughnessConstant = .8f;
    metallic.alphaTestType = 7;
    remixapi_MaterialInfo metalInfo{};
    metalInfo.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    metalInfo.hash = 9003;
    metalInfo.pNext = &metallic;
    remixapi_MaterialHandle metal;
    r.createMaterial(metalInfo, metal);
    const auto metalMesh = quad(r, 9004, metal);
    camera(r);
    r.drawInstance(instance(metalMesh));
    pixels = target.render();
    require(pixels[center + 1] > 80 && pixels[center + 1] > 3 * pixels[center],
            "A rough metallic surface receives cubemap specular with SSR disabled and zero diffuse fill");
    std::printf("Probe-only rough metal RGB: %u %u %u\n", unsigned(pixels[center]),
                unsigned(pixels[center + 1]), unsigned(pixels[center + 2]));
    const auto controller = target.forward->bakeController();
    const auto published = controller->volume();
    options.baking.batch = 512;
    options.baking.gpuBudgetMs = 0;
    target.forward->updateLighting(options);
    for (uint32_t i = 0; i < remix::gpuFrameCount + 1; ++i) {
      camera(r);
      r.drawInstance(instance(metalMesh));
      target.render();
    }
    require(controller->volume() == published && target.forward->statistics().baking.active,
            "Execution-only edits preserve the published volume across completed frame slots");
    for (uint32_t samples : {1u, 2u, 4u}) {
      options.samples = samples;
      options.ssr = !options.ssr;
      target.enableForward(options, true);
      camera(r);
      r.drawInstance(instance(metalMesh));
      const auto reused = target.render();
      require(target.forward->bakeController() == controller && controller->volume() == published &&
                  target.forward->statistics().baking.active &&
                  target.forward->statistics().baking.state == REMIXAPI_BAKE_READY_EXT,
              "MSAA/reflection target replacements retain the published map bake without retracing");
      require(reused[center + 1] > 80 && reused[center + 1] > 3 * reused[center],
              "Retained probe lighting reaches the first frame of each target replacement");
    }
    options.baking.enabled = false;
    target.forward->updateLighting(options);
    camera(r);
    r.drawInstance(instance(mesh));
    pixel(target.render(), 32, 32, 0, 0, 0, "Disabling baking removes its contribution immediately");
    require(!target.forward->statistics().baking.active, "Disabled volume is no longer active");
    options.baking.enabled = true;
    target.forward->updateLighting(options);
    until(REMIXAPI_BAKE_PREPARING_EXT);
    r.setBakeScene(nullptr);
    camera(r);
    r.drawInstance(instance(mesh));
    pixel(target.render(), 32, 32, 0, 0, 0, "Map unload cancels preparation and clears baked lighting");
    for (uint32_t i = 0; i < remix::gpuFrameCount + 2; ++i) target.render();
    require(target.forward->statistics().baking.state == REMIXAPI_BAKE_WAITING_EXT,
            "Unloaded map waits for an explicit complete scene");
    std::puts("Forward+ bake publication, replacement, pixel lighting, disable and unload passed");
  }
  void pixel(const std::vector<uint8_t> &image, uint32_t x, uint32_t y,
             const std::array<float, 3> &expected, const char *reason) {
    pixel(image, x, y, int(std::lround(expected[0] * 255)), int(std::lround(expected[1] * 255)),
          int(std::lround(expected[2] * 255)), reason);
  }
  void skyTests() {
    for (uint32_t samples : {1u, 4u}) {
      Target target;
      auto &r = target.raster;
      camera(r);
      remix::forward::Options options;
      options.baking.lightmapFitBudget = false; // Controlled-density lighting fixtures.
      options.samples = samples;
      options.shadows = false;
      options.ambient = 0;
      options.baking.enabled = false;
      target.enableForward(options);
      remixapi_LightInfoDomeEXT dome{};
      dome.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
      for (uint32_t c = 0; c < 3; ++c) dome.transform.matrix[c][c] = 1;
      remixapi_LightInfo light{};
      light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
      light.pNext = &dome;
      light.hash = 0x900001;
      light.radiance = {1, 0, 0};
      remixapi_LightHandle lamp;
      r.createLight(light, lamp);
      auto submit = [&] { r.drawLight(lamp); };
      submit();
      pixel(target.render(), 32, 32, prismDisplay({1, 0, 0}), "Forward+ displays a radiance dome without world geometry");
      const auto green = quad(r, 0x900002, material(r, 0x900003, 0, 1, 0));
      auto sky = instance(green);
      sky.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_SKY;
      submit();
      r.drawInstance(sky);
      pixel(target.render(), 32, 32, 0, 255, 0, "Raster sky geometry covers the radiance background");
      const auto black = quad(r, 0x900004, material(r, 0x900005, 0, 0, 0));
      submit();
      r.drawInstance(instance(black));
      const auto image = target.render();
      pixel(image, 32, 32, 0, 0, 0, "Opaque world covers the radiance background");
      pixel(image, 0, 0, prismDisplay({1, 0, 0}), "Uncovered world samples retain the radiance background");
      if (target.forward->statistics().samples > 1) {
        bool partial = false;
        for (uint32_t x = 0; x < 64; ++x) {
          const auto value = image[(32 * 64 + x) * 4];
          partial |= value > 5 && value < 200;
        }
        require(partial, "MSAA world edges blend partial coverage over the sky background");
      }
      pixel(target.render(), 0, 0, 0, 0, 0, "Absent dome restores the host clear background");
      // Opposite hemispheres expose direction, rotation and translation errors
      // that a constant-colour environment would conceal.
      std::array<uint8_t, 8 * 4 * 4> panorama{};
      for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
          panorama[(y * 8 + x) * 4 + (x < 4 ? 0 : 2)] = 255;
          panorama[(y * 8 + x) * 4 + 3] = 255;
        }
      remixapi_TextureInfo texture{};
      texture.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
      texture.hash = 0x900006;
      texture.width = 8;
      texture.height = 4;
      texture.depth = texture.mipLevels = 1;
      texture.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
      texture.data = panorama.data();
      texture.dataSize = panorama.size();
      remixapi_TextureHandle imageHandle;
      r.createTexture(texture, imageHandle);
#if defined(_WIN32)
      dome.colorTexture = L"0x900006";
#else
      dome.colorTexture = "0x900006";
#endif
      light.radiance = {1, 1, 1};
      r.createLight(light, lamp);
      remixapi_CameraInfoParameterizedEXT params{};
      params.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
      params.position = {5, 7, -9};
      params.forward = {0, 0, 1};
      params.right = {1, 0, 0};
      params.up = {0, 1, 0};
      params.aspect = 1;
      params.fovYInDegrees = 60;
      params.nearPlane = .1f;
      params.farPlane = 10;
      remixapi_CameraInfo view{};
      view.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
      view.pNext = &params;
      r.setupCamera(view);
      submit();
      pixel(target.render(), 32, 32, prismDisplay({0, 0, 1}), "Dome panorama follows the world view direction");
      params.position = {-200, 30, 80};
      r.setupCamera(view);
      submit();
      pixel(target.render(), 32, 32, prismDisplay({0, 0, 1}), "Camera translation does not move an infinite sky");
      params.forward = {0, 0, -1};
      params.right = {-1, 0, 0};
      r.setupCamera(view);
      submit();
      pixel(target.render(), 32, 32, prismDisplay({1, 0, 0}), "Camera rotation selects the opposite dome hemisphere");
      dome.transform.matrix[0][0] = dome.transform.matrix[2][2] = -1;
      r.createLight(light, lamp);
      submit();
      pixel(target.render(), 32, 32, prismDisplay({0, 0, 1}), "Dome transform rotates the panoramic background");
      // A horizontal horizon must agree with world geometry when pitching.
      // Longitude-only colours above cannot detect a reversed vertical axis.
      std::array<uint8_t, 8 * 64 * 4> horizon{};
      for (uint32_t y = 0; y < 64; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
          horizon[(y * 8 + x) * 4 + (y < 32 ? 1 : 2)] = 255;
          horizon[(y * 8 + x) * 4 + 3] = 255;
        }
      texture.height = 64;
      texture.data = horizon.data();
      texture.dataSize = horizon.size();
      r.createTexture(texture, imageHandle);
      dome.transform.matrix[0][0] = dome.transform.matrix[2][2] = 1;
      r.createLight(light, lamp);
      for (float pitch : {0.f, -.2617994f, .2617994f}) {
        params.right = {1, 0, 0};
        params.forward = {0, std::sin(pitch), std::cos(pitch)};
        params.up = {0, std::cos(pitch), -std::sin(pitch)};
        r.setupCamera(view);
        submit();
        const auto pitched = target.render();
        const int horizonRow = int(32 + 32 * std::tan(pitch) / std::tan(.5235988f));
        const size_t above = size_t((horizonRow - 12) * 64 + 32) * 4;
        const size_t below = size_t((horizonRow + 12) * 64 + 32) * 4;
        require(int(pitched[above + 1]) - int(pitched[above + 2]) > 100 &&
                    int(pitched[below + 2]) - int(pitched[below + 1]) > 100,
                "Sky north/south and pitched horizon agree with the world camera projection");
      }
    }
    std::puts("Forward+ sky, world/raster coverage, absence and MSAA passed");
  }
  void cameraBackfaceTests() {
    for (uint32_t samples : {1u, 2u, 4u}) for (bool viewModel : {false, true}) {
      Target target;
      auto &r = target.raster;
      remix::forward::Options options;
      options.baking.lightmapFitBudget = false; // Controlled-density lighting fixtures.
      options.samples = samples;
      options.ambient = 1;
      options.shadows = false;
      options.ssr = false;
      target.enableForward(options);
      const auto mesh = quad(r, 12900, material(r, 12901, 1, 1, 1));
      remixapi_InstanceInfoCameraCullingEXT culling{};
      culling.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_CAMERA_CULLING_EXT;
      culling.flags = REMIXAPI_INSTANCE_CAMERA_CULL_BACK_FACES_EXT;
      auto info = instance(mesh);
      if (viewModel) info.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL;
      info.doubleSided = 1; // Light transport must remain double-sided.
      info.pNext = &culling;
      const auto render = [&] {
        camera(r);
        if (viewModel) {
          // Viewmodel correction requires perspective cameras. Identity
          // projection leaves this as a compatibility-raster draw instead.
          remixapi_CameraInfoParameterizedEXT parameters{};
          parameters.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
          parameters.position = {0, 0, -2};
          parameters.forward = {0, 0, 1};
          parameters.right = {1, 0, 0};
          parameters.up = {0, 1, 0};
          parameters.aspect = 1;
          parameters.fovYInDegrees = 90;
          parameters.nearPlane = .1f;
          parameters.farPlane = 10;
          remixapi_CameraInfo overlay{};
          overlay.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
          overlay.pNext = &parameters;
          for (const auto type : {REMIXAPI_CAMERA_TYPE_WORLD, REMIXAPI_CAMERA_TYPE_VIEW_MODEL}) {
            overlay.type = type;
            r.setupCamera(overlay);
          }
        }
        r.drawInstance(info);
        return target.render();
      };
      const size_t centre = 4 * (32 * 64 + 32);
      require(render()[centre] > 100, "Camera backface policy keeps the front face");
      info.transform.matrix[0][0] = -1;
      pixel(render(), 32, 32, 0, 0, 0, "Camera backface policy hides reversed winding");
      culling.flags = 0; // Source $nocull materials omit the camera flag.
      require(render()[centre] > 100, "Explicit two-sided camera material remains visible");
      const auto primaryCategory = info.categoryFlags;
      for (const auto category : {REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_MODEL,
                                  REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_BODY}) {
        info.categoryFlags = primaryCategory | category;
        pixel(render(), 32, 32, 0, 0, 0,
              "Reflection-only players remain absent from world and viewmodel camera passes");
        info.categoryFlags = primaryCategory;
        require(render()[centre] > 100,
                "Changing a reflection-only player back to primary geometry restores camera ownership");
      }
    }
  }
#include "forward_skinning.h"
#include "forward_depth.h"
  void forwardTests(const std::filesystem::path &directory) {
    cameraBackfaceTests();
    forwardDepthPrepassTests();
    Target target;
    auto &r = target.raster;
    camera(r);
    remix::forward::Options options;
    options.baking.lightmapFitBudget = false; // Controlled-density lighting fixtures.
    options.ambient = 0;
    options.shadowResolution = 128;
    options.shadows = false;
    target.enableForward(options);
    const auto white = material(r, 8000, 1, 1, 1);
    const auto mesh = quad(r, 8001, white);
    r.drawInstance(instance(mesh));
    pixel(target.render(), 32, 32, 0, 0, 0, "Forward world has no fabricated direct light");

    remixapi_MaterialInfoSelfIlluminationEXT self{};
    self.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_SELF_ILLUMINATION_EXT;
    self.maskScale = 1;
    remixapi_MaterialInfoOpaqueEXT glowing{};
    glowing.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    glowing.pNext = &self;
    glowing.albedoConstant = {.5f, .5f, .5f};
    glowing.opacityConstant = 1;
    glowing.alphaTestType = 7;
    remixapi_MaterialInfo glowInfo{};
    glowInfo.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    glowInfo.pNext = &glowing;
    glowInfo.hash = 8050;
    glowInfo.emissiveIntensity = 2;
    glowInfo.emissiveColorConstant = {1, 0, 0};
    remixapi_MaterialHandle glow, referenceGlow;
    r.createMaterial(glowInfo, glow);
    glowing.pNext = nullptr;
    glowing.albedoConstant = {0, 0, 0};
    glowInfo.hash = 8051;
    glowInfo.emissiveIntensity = 1;
    r.createMaterial(glowInfo, referenceGlow);
    const auto glowingMesh = quad(r, 8052, glow), referenceMesh = quad(r, 8053, referenceGlow);
    r.drawInstance(instance(referenceMesh));
    const auto referenceEmission = target.render();
    r.drawInstance(instance(glowingMesh));
    const auto selfEmission = target.render();
    const size_t emissionPixel = 4 * (32 * 64 + 32);
    for (uint32_t c = 0; c < 3; ++c)
      require(std::abs(int(referenceEmission[emissionPixel + c]) - int(selfEmission[emissionPixel + c])) <= 1,
              "Forward self illumination multiplies surface colour by emission strength");
    options.ambient = 1;
    target.forward->updateLighting(options);
    r.drawInstance(instance(glowingMesh));
    const auto ambientEmission = target.render();
    for (uint32_t c = 0; c < 3; ++c)
      require(std::abs(int(ambientEmission[emissionPixel + c]) - int(selfEmission[emissionPixel + c])) <= 1,
              "Fully self-illuminated forward surfaces do not reflect diffuse ambient fill");
    options.ambient = 0;
    target.forward->updateLighting(options);

    remixapi_LightInfoSphereEXT sphere{};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.radius = .2f;
    remixapi_LightInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.hash = 8002;
    info.pNext = &sphere;
    info.radiance = {2, 0, 0};
    remixapi_LightHandle light;
    r.createLight(info, light);
    r.drawLight(light);
    r.drawInstance(instance(mesh));
    auto pixels = target.render();
    const auto center = 4 * (32 * 64 + 32);
    require(pixels[center] > 20 && pixels[center + 1] < 2, "Clustered local light changes the shaded pixels");
    require(target.forward->statistics().lights == 1, "Forward light upload");
    r.drawLight(light);
    r.drawInstance(instance(mesh));
    ui(r, 0, 0xffff0000);
    pixel(target.render(), 32, 32, 0, 0, 255, "Compatibility HUD remains above lit world");
    const bool previousLocalShadows = options.localShadows;
    options.localShadows = false; // Compare list traversal without time-dependent shadow selection fades.
    target.forward->updateLighting(options);
    std::vector<remixapi_LightHandle> crowdedLights;
    for (uint32_t i = 0; i < 65; ++i) {
      info.hash = 8100 + i;
      info.radiance = i == 64 ? remixapi_Float3D{2, 0, 0} : remixapi_Float3D{0, 0, .002f};
      remixapi_LightHandle member;
      r.createLight(info, member);
      r.drawLight(member);
      crowdedLights.push_back(member);
    }
    r.drawInstance(instance(mesh));
    pixels = target.render();
    require(target.forward->statistics().lights == 65 && pixels[center] > 20,
            "Cluster overflow retains the last light instead of truncating illumination");
    require(target.forward->statistics().clusterLightCapacity == 128, "Dense fixture expands its light list");
    const auto adaptivePixels = pixels;
    options.clusterLights = 64;
    target.forward->updateLighting(options);
    for (auto member : crowdedLights) r.drawLight(member);
    r.drawInstance(instance(mesh));
    require(target.render() == adaptivePixels && target.forward->statistics().clusterLightCapacity == 64,
            "Adaptive lists reproduce the complete legacy overflow image without losing late lights");
    options.clusterLights = 0;
    target.forward->updateLighting(options);
    // Keep 65 uploaded lights but only the last one inside the camera clusters.
    // This exercises an index beyond the first 64 lights through the stored list,
    // independently of the full-array overflow fallback above.
    for (uint32_t i = 0; i < 65; ++i) {
      info.hash = 8100 + i;
      info.radiance = i == 64 ? remixapi_Float3D{2, 0, 0} : remixapi_Float3D{0, 0, .002f};
      sphere.position = i == 64 ? remixapi_Float3D{} : remixapi_Float3D{100, 0, 0};
      r.createLight(info, crowdedLights[i]);
      r.drawLight(crowdedLights[i]);
    }
    r.drawInstance(instance(mesh));
    pixels = target.render();
    require(target.forward->statistics().lights == 65 && pixels[center] > 20 && pixels[center + 2] < 2,
            "A sparse cluster retains an eligible light beyond the first 64 uploaded lights");
    for (auto member : crowdedLights)
      r.destroyLight(member);
    options.localShadows = previousLocalShadows;
    target.forward->updateLighting(options);

    remixapi_LightInfoDistantEXT distant{};
    distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
    distant.direction = {.624695f, 0, .780869f};
    info.hash = 8003;
    info.pNext = &distant;
    info.radiance = {3, 3, 3};
    remixapi_LightHandle sun;
    r.createLight(info, sun);
    auto receiver = instance(mesh, .3f);
    auto occluder = instance(mesh, -.2f);
    remixapi_InstanceInfoCameraCullingEXT casterCulling{};
    casterCulling.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_CAMERA_CULLING_EXT;
    casterCulling.flags = REMIXAPI_INSTANCE_CAMERA_CULL_BACK_FACES_EXT;
    occluder.pNext = &casterCulling;
    occluder.doubleSided = 1;
    occluder.transform.matrix[0][0] = -.2f;
    occluder.transform.matrix[1][1] = .2f;
    occluder.transform.matrix[0][3] = -.2f;
    auto shadowScene = [&] {
      r.drawLight(sun);
      r.drawInstance(receiver);
      r.drawInstance(occluder);
      return target.render();
    };
    const auto unshadowed = shadowScene();
    options.shadows = true;
    target.enableForward(options);
    const auto shadowed = shadowScene();
    const size_t shadowPixel = 4 * (32 * 64 + 38);
    require(int(unshadowed[shadowPixel]) - int(shadowed[shadowPixel]) > 30,
            "Sun shadow occludes a visible receiver behind the caster");
    // Entirely outside the camera's XY and near plane, but still on the sun
    // ray reaching the same receiver. Camera culling must not remove this
    // object from the independently culled cascade submissions.
    occluder.transform.matrix[0][3] = -1.3f;
    occluder.transform.matrix[2][3] = -1.575f;
    const auto offscreenShadow = shadowScene();
    require(int(unshadowed[shadowPixel]) - int(offscreenShadow[shadowPixel]) > 30,
            "A camera-culled offscreen caster still shadows a visible receiver");

    remixapi_HardcodedVertex vertices[3]{};
    const float positions[][3] = {{-.73f, -.8f, .5f}, {.81f, .77f, .5f}, {.81f, -.8f, .5f}};
    for (int i = 0; i < 3; ++i) {
      std::copy_n(positions[i], 3, vertices[i].position);
      vertices[i].color = 0xffffffff;
    }
    const uint32_t indices[] = {0, 1, 2};
    remixapi_MeshInfoSurfaceTriangles surface{};
    surface.vertices_values = vertices;
    surface.vertices_count = 3;
    surface.indices_values = indices;
    surface.indices_count = 3;
    surface.material = white;
    remixapi_MeshInfo triangleInfo{};
    triangleInfo.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    triangleInfo.hash = 8004;
    triangleInfo.surfaces_values = &surface;
    triangleInfo.surfaces_count = 1;
    remixapi_MeshHandle triangle;
    r.createMesh(triangleInfo, triangle);
    options.shadows = false;
    options.ambient = 1;
    auto coverage = [&](uint32_t samples) {
      options.samples = samples;
      target.enableForward(options);
      r.drawInstance(instance(triangle));
      return target.render();
    };
    const auto single = coverage(1);
    pixels = coverage(4);
    auto partialPixels = [](const auto &image) {
      size_t count = 0;
      for (size_t i = 0; i < image.size(); i += 4)
        count += image[i] > 2 && image[i] < 190;
      return count;
    };
    if (target.forward->statistics().samples > 1)
      require(partialPixels(pixels) > partialPixels(single) + 10,
              "MSAA produces partial geometric edge coverage");
    std::ofstream out(directory / "forward-msaa.ppm", std::ios::binary);
    out << "P6\n64 64\n255\n";
    for (size_t i = 0; i < pixels.size(); i += 4)
      out.write(reinterpret_cast<const char *>(pixels.data() + i), 3);
    std::puts(
        "Forward+ GPU readback: local lights, sun shadows, MSAA, frame slots and compatibility HUD passed");
  }
  void reflectionTests(const std::filesystem::path &directory, bool baked = false, bool hardware = false,
                       bool environment = false, uint32_t environmentSamples = 4, bool viewModel = false) {
    // Odd dimensions exercise hierarchy tails as well as half-resolution SSR.
    Target target(hardware ? 191 : 95, hardware ? 127 : 79, baked || hardware);
    auto &r = target.raster;
    remixapi_LightHandle environmentLight = nullptr;
    auto makeMaterial = [&](uint64_t hash, float roughness, float metallic, float emission,
                            bool cutout = false) {
      remixapi_MaterialInfoOpaqueEXT opaque{};
      opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
      opaque.albedoConstant = {1, 1, 1};
      // Isolate source replacement: a white emitter also receives blue baked
      // diffuse light, which would correctly appear in the red SSR hit.
      if (baked && emission > 0) opaque.albedoConstant = {0, 0, 0};
      opaque.opacityConstant = cutout ? 0 : 1;
      opaque.alphaTestType = cutout ? 4 : 7;
      opaque.alphaReferenceValue = 128;
      if (hardware && cutout) { opaque.opacityConstant = 1; opaque.alphaTestType = 0; }
      opaque.roughnessConstant = roughness;
      opaque.metallicConstant = metallic;
      remixapi_MaterialInfo info{};
      info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
      info.pNext = &opaque;
      info.hash = hash;
      info.emissiveIntensity = emission;
      info.emissiveColorConstant = {1, 0, 0};
      remixapi_MaterialHandle result;
      r.createMaterial(info, result);
      return result;
    };
    auto makeQuad = [&](uint64_t hash, remixapi_MaterialHandle material, const float positions[4][3],
                        const float normal[3]) {
      remixapi_HardcodedVertex vertices[4]{};
      for (int i = 0; i < 4; ++i) {
        std::copy_n(positions[i], 3, vertices[i].position);
        std::copy_n(normal, 3, vertices[i].normal);
        vertices[i].color = 0xffffffff;
      }
      const uint32_t indices[] = {0, 1, 2, 0, 2, 3};
      remixapi_MeshInfoSurfaceTriangles surface{};
      surface.vertices_values = vertices;
      surface.vertices_count = 4;
      surface.indices_values = indices;
      surface.indices_count = 6;
      surface.material = material;
      remixapi_MeshInfo info{};
      info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
      info.hash = hash;
      info.surfaces_values = &surface;
      info.surfaces_count = 1;
      remixapi_MeshHandle result;
      r.createMesh(info, result);
      return result;
    };
    const float floorPositions[][3] = {{-3, 0, 0}, {3, 0, 0}, {3, 0, 5}, {-3, 0, 5}},
                floorNormal[] = {0, 1, 0};
    const float wallPositions[][3] = {{-.8f, .15f, 3}, {.8f, .15f, 3}, {.8f, 1.8f, 3}, {-.8f, 1.8f, 3}},
                wallNormal[] = {0, 0, -1};
    const auto floorMaterial = makeMaterial(9000, .04f, 1, 0);
    const auto roughMaterial = makeMaterial(9001, .95f, 1, 0);
    const auto wallMaterial = makeMaterial(9002, .5f, 0, hardware ? .3f : 4);
    const auto cutoutMaterial = makeMaterial(9003, .5f, 0, 4, true);
    const auto floor = makeQuad(9010, floorMaterial, floorPositions, floorNormal);
    const auto roughFloor = makeQuad(9011, roughMaterial, floorPositions, floorNormal);
    const auto wall = makeQuad(9012, wallMaterial, wallPositions, wallNormal);
    const auto cutoutWall = makeQuad(9013, cutoutMaterial, wallPositions, wallNormal);
    remixapi_MaterialInfoTranslucentEXT waterOptics{};
    waterOptics.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
    waterOptics.refractiveIndex = 1.333f;
    waterOptics.transmittanceColor = {1, 1, 1};
    waterOptics.transmittanceMeasurementDistance = 1;
    remixapi_MaterialInfo waterInfo{};
    waterInfo.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    waterInfo.hash = 9014;
    waterInfo.pNext = &waterOptics;
    remixapi_MaterialHandle waterMaterial;
    r.createMaterial(waterInfo, waterMaterial);
    const auto occludedWater = makeQuad(9015, waterMaterial, wallPositions, wallNormal);
    waterOptics.transmittanceColor = {0, 0, 0};
    waterInfo.hash = 9016;
    r.createMaterial(waterInfo, waterMaterial);
    const auto waterFloor = makeQuad(9017, waterMaterial, floorPositions, floorNormal);
    const auto waterRayEmitter = makeQuad(9019, makeMaterial(9018, .5f, 0, 4), wallPositions, wallNormal);
    const auto waterRayRejected = makeQuad(9021, makeMaterial(9020, .5f, 0, 4, true), wallPositions, wallNormal);
    const auto waterRayBlack = makeQuad(9023, makeMaterial(9022, .5f, 0, 0), wallPositions, wallNormal);
    waterOptics.transmittanceColor = {1, 1, 1};
    waterOptics.thinWallThickness_hasvalue = 1;
    waterOptics.thinWallThickness_value = .1f;
    waterInfo.hash = 9024;
    r.createMaterial(waterInfo, waterMaterial);
    const auto sheetFloor = makeQuad(9025, waterMaterial, floorPositions, floorNormal);
    float waterRoughness = .04f;
    bool composeWater = false;
    remixapi_MeshHandle reflectionBackdrop = nullptr;
    uint32_t emitterCategory = 0;
    auto drawScene = [&](bool rough = false, bool showWall = true, bool cutout = false,
                         bool hidden = false, float offset = 0.f, remixapi_MeshHandle receiver = nullptr,
                         remixapi_MeshHandle emitter = nullptr) {
      remixapi_CameraInfoParameterizedEXT parameters{};
      parameters.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
      parameters.position = {0, 1.5f, -3};
      parameters.forward = {0, -.2f, .9797959f};
      parameters.up = {0, .9797959f, .2f};
      parameters.right = {1, 0, 0};
      parameters.aspect = float(target.extent.width) / target.extent.height;
      parameters.fovYInDegrees = 60;
      parameters.nearPlane = .05f;
      parameters.farPlane = 30;
      remixapi_CameraInfo camera{};
      camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
      camera.pNext = &parameters;
      r.setupCamera(camera);
      if (viewModel) {
        camera.type = REMIXAPI_CAMERA_TYPE_VIEW_MODEL;
        r.setupCamera(camera);
      }
      if (environmentLight)
        r.drawLight(environmentLight);
      auto reflectionReceiver = instance(receiver ? receiver : rough ? roughFloor : floor);
      if (viewModel) reflectionReceiver.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL;
      r.drawInstance(reflectionReceiver);
      // Exercise the resolved material, where USD replacements can author
      // roughness beyond the legacy translucent API's fixed smooth value.
      if (receiver == waterFloor || receiver == sheetFloor)
        r.draws().back().material->roughness = waterRoughness;
      if (composeWater) {
        auto liquid = instance(occludedWater, .01f);
        liquid.transform.matrix[0][0] = liquid.transform.matrix[1][1] = .5f;
        liquid.transform.matrix[1][3] = .5f;
        r.drawInstance(liquid);
      }
      if (showWall) {
        auto draw = instance(emitter ? emitter : cutout ? cutoutWall : wall);
        draw.categoryFlags = emitterCategory;
        remixapi_InstanceIdentityEXT identity{};
        identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
        identity.instanceId = 9400;
        identity.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
        identity.rasterVisible = !hidden;
        if (hardware) { draw.pNext = &identity; draw.doubleSided = 1; }
        draw.transform.matrix[0][3] = offset;
        r.drawInstance(draw);
        if (reflectionBackdrop)
          r.drawInstance(instance(reflectionBackdrop));
      }
      return target.render();
    };
    auto save = [&](const char *name, const std::vector<uint8_t> &image) {
      std::ofstream file(directory / name, std::ios::binary);
      file << "P6\n" << target.extent.width << " " << target.extent.height << "\n255\n";
      for (size_t i = 0; i < image.size(); i += 4)
        file.write(reinterpret_cast<const char *>(image.data() + i), 3);
    };
    remix::forward::Options options;
    options.baking.lightmapFitBudget = false; // Controlled-density lighting fixtures.
    options.ambient = 0;
    options.shadows = false;
    options.shadowResolution = 64;
    options.ssr = false;
    if (baked) {
      options.baking.maxProbes = 512;
      options.baking.samples = 64;
      options.baking.bounces = 1;
      options.baking.reflectionResolution = 8;
      remixapi_LightInfoDomeEXT dome{};
      dome.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
      for (uint32_t c = 0; c < 3; ++c) dome.transform.matrix[c][c] = 1;
      remixapi_LightInfo light{};
      light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
      light.hash = 9100;
      light.pNext = &dome;
      light.radiance = {0, 0, 1};
      remixapi_LightHandle lamp;
      r.createLight(light, lamp);
      if (environment) {
        environmentLight = lamp;
        options.samples = environmentSamples;
        options.baking.reflectionProbes = 0;
        options.ssr = true;
        options.ssrSteps = 0; // isolate fallback without screen hits
      }
      remixapi_BakeSceneBoundsInfoEXT bounds{};
      bounds.scene.structSize = sizeof(bounds);
      bounds.scene.version = REMIXAPI_BAKE_SCENE_BOUNDS_VERSION_EXT;
      bounds.scene.sceneId = bounds.scene.revision = 2;
      bounds.scene.sceneScale = 1;
      bounds.scene.state = REMIXAPI_BAKE_SCENE_READY_EXT;
      bounds.scene.lightCount = 1;
      bounds.scene.lights = &lamp;
      const float minimum[] = {-4, -1, -4}, maximum[] = {4, 3, 6};
      std::copy_n(minimum, 3, bounds.minimum);
      std::copy_n(maximum, 3, bounds.maximum);
      r.setBakeScene(&bounds.scene);
    }
    target.enableForward(options);
    const auto without = drawScene(false, !environment);
    save("ssr-disabled.ppm", without);
    if (environment) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
      while (!target.forward->statistics().baking.active && std::chrono::steady_clock::now() < deadline) {
        drawScene(false, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      require(target.forward->statistics().baking.active, "Sky fallback waits for published visibility");
      auto addedBlue = [&](const std::vector<uint8_t> &image) {
        size_t count = 0;
        for (size_t i = 0; i < image.size(); i += 4)
          // Shared Prism compresses saturated blue into small red/green
          // components. Test blue dominance rather than a pure RGB primary.
          count += without[i + 2] < 8 && image[i + 2] > 40 &&
                   image[i + 2] > 4 * image[i] && image[i + 2] > 4 * image[i + 1];
        return count;
      };
      const auto lit = drawScene(false, false);
      save("sky-reflection.ppm", lit);
      require(addedBlue(lit) > 50, "Uncovered metallic receivers reflect the visible live sky");
      require(target.forward->statistics().reflectionProbes.parameters[0] == 0,
              "Sky fallback works with no fitted reflection cubemaps");
      require(addedBlue(drawScene(true, false)) > 50, "Rough receivers use filtered sky radiance");
      std::array<uint8_t, 64 * 16 * 4> panorama{};
      for (uint32_t y = 0; y < 16; ++y)
        for (uint32_t x = 0; x < 64; ++x) {
          const auto at = (y * 64 + x) * 4;
          if (x >= 47 && x <= 49)
            panorama[at] = panorama[at + 1] = panorama[at + 2] = 255;
          panorama[at + 3] = 255;
        }
      remixapi_TextureInfo texture{};
      texture.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
      texture.hash = 9101;
      texture.width = 64; texture.height = 16;
      texture.depth = texture.mipLevels = 1;
      texture.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
      texture.data = panorama.data(); texture.dataSize = panorama.size();
      remixapi_TextureHandle imageHandle;
      r.createTexture(texture, imageHandle);
      remixapi_LightInfoDomeEXT dome{};
      dome.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
      for (uint32_t c = 0; c < 3; ++c) dome.transform.matrix[c][c] = 1;
#if defined(_WIN32)
      dome.colorTexture = L"0x238d";
#else
      dome.colorTexture = "0x238d";
#endif
      remixapi_LightInfo light{};
      light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
      light.pNext = &dome; light.hash = 9100; light.radiance = {1, 1, 1};
      r.createLight(light, environmentLight);
      auto peak = [&](const std::vector<uint8_t> &image) {
        uint8_t result = 0;
        for (size_t i = 0; i < image.size(); i += 4)
          if (without[i] < 8 && without[i + 2] < 8)
            result = std::max(result, image[i]);
        return result;
      };
      const auto sharp = peak(drawScene(false, false)), rough = peak(drawScene(true, false));
      std::printf("Sky reflection %ux: narrow-source peak %u sharp, %u rough\n", environmentSamples, sharp, rough);
      require(sharp > 100 && rough + 15 < sharp, "GGX sky filtering spreads a narrow bright source");
      const auto previousEnvironment = environmentLight;
      environmentLight = nullptr;
      require(peak(drawScene(false, false)) < 3, "Removing the live dome leaves no stale sky reflection");
      environmentLight = previousEnvironment;
      require(peak(drawScene(false, false)) > 100, "Restoring the live dome recovers sky reflection immediately");
      options.baking.enabled = false;
      target.forward->updateLighting(options);
      require(peak(drawScene(false, false)) < 3, "Missing visibility suppresses unverified sky reflections");
      std::puts("Visibility-gated sky reflection fallback and roughness passed");
      return;
    }
    auto reflectedPixels = [&](const auto &image) {
      size_t count = 0;
      for (uint32_t y = target.extent.height / 2; y < target.extent.height; ++y)
        for (uint32_t x = 0; x < target.extent.width; ++x) {
          const auto at = (y * target.extent.width + x) * 4;
          // The shared display transform adds green/blue to bright red.
          // Require a new red-dominant reflection rather than a pure primary.
          count += image[at] > without[at] + 25 && image[at] > 2 * image[at + 1] &&
                   image[at] > 2 * image[at + 2];
        }
      return count;
    };
    if (viewModel) {
      options.baking.enabled = false;
      options.baking.reflectionProbes = 0;
      options.water = false; // SSR guide synchronization must not depend on water.
      for (uint32_t samples : {1u, 2u, 4u}) {
        options.samples = samples;
        options.ssr = true;
        target.enableForward(options);
        const auto reflected = drawScene();
        save("viewmodel-ssr.ppm", reflected);
        std::printf("Viewmodel SSR %ux: %zu reflected pixels\n", samples, reflectedPixels(reflected));
        require(reflectedPixels(reflected) > 12, "Viewmodels trace the completed world SSR guides");
        require(reflectedPixels(drawScene(true)) < 3, "Rough viewmodels reject sharp screen transport");
        require(reflectedPixels(drawScene(false, false)) < 3, "Removed world emitters leave no stale viewmodel SSR");
        options.ssr = false;
        target.enableForward(options);
        require(reflectedPixels(drawScene()) < 3, "Disabling SSR removes viewmodel screen transport");
      }
      require(errors.load() == 0, "Viewmodel SSR is Vulkan validation clean");
      return;
    }
    if (hardware) {
      options.rtReflections = true; options.ssr = false;
      options.baking.enabled = false; options.baking.reflectionProbes = 0;
      options.localShadows = false; options.localShadowLights = 0;
      options.rtReflectionBounces = 1;
      for (uint32_t samples : {1u, 2u, 4u})
        for (uint32_t scale : {1u, 2u, 4u}) {
          options.samples = samples; options.ssrScale = scale;
          target.enableForward(options);
          const auto reflected = drawScene(false, true, false, true);
          std::printf("Hardware reflection %ux, scale %u: %zu offscreen pixels (%s)\n", samples, scale,
                      reflectedPixels(reflected), target.forward->statistics().rayReflections.status);
          require(target.forward->statistics().rayReflections.active && reflectedPixels(reflected) > 12,
                  "Hardware rays reflect a hidden dynamic emitter with SSR and baking disabled");
          drawScene(false, true, false, true);
          require(target.forward->statistics().rayReflections.sceneBuilds == 0,
                  "An unchanged live scene reuses its acceleration structures");
          for (const auto category : {REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_MODEL,
                                      REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_BODY}) {
            emitterCategory = category;
            require(reflectedPixels(drawScene(false, true, false, false)) > 12 &&
                        target.forward->statistics().rayReflections.sceneBuilds == 1,
                    "Changing a live player category refreshes transport and retains its reflection");
            require(reflectedPixels(drawScene(false, true, false, false)) > 12 &&
                        target.forward->statistics().rayReflections.sceneBuilds == 0,
                    "Unchanged reflection-only players reuse their live acceleration structures");
            require(reflectedPixels(drawScene(false, true, false, false, 50)) < 3,
                    "Moving a reflection-only player removes its old reflection immediately");
            require(reflectedPixels(drawScene(false, true, false, false)) > 12,
                    "Returning a reflection-only player restores its current reflection");
            emitterCategory = 0;
            require(reflectedPixels(drawScene(false, true, false, true)) > 12 &&
                        target.forward->statistics().rayReflections.sceneBuilds == 1,
                    "Removing the live reflection-only policy refreshes transport without dropping geometry");
          }
          emitterCategory = 0;
          require(reflectedPixels(drawScene(true, true, false, true)) < 3,
                  "Hardware reflection roughness cutoff rejects unsupported sharp transport");
          require(reflectedPixels(drawScene(false, false)) < 3,
                  "Removed dynamic geometry cannot leave stale hardware reflections");
          require(reflectedPixels(drawScene(false, true, true, true)) < 3,
                  "Hardware candidate traversal honors rejected material alpha");
          require(reflectedPixels(drawScene(false, true, false, true, 50)) < 3,
                  "Moving a hidden dynamic emitter updates its ray-query placement");
          drawScene(false, true, false, true);
          options.rtReflectionDistance = .1f; target.forward->updateLighting(options);
          require(reflectedPixels(drawScene(false, true, false, true)) < 3,
                  "Hardware rays obey a live distance edit");
          require(target.forward->statistics().rayReflections.sceneBuilds == 0,
                  "A ray distance edit does not rebuild unchanged acceleration structures");
          options.rtReflectionDistance = 50; target.forward->updateLighting(options);
          require(reflectedPixels(drawScene(false, true, false, true)) > 12,
                  "Restored ray range recovers the current reflection without history");
          save("hardware-offscreen.ppm", reflected);
        }
      options.water = true;
      options.rtGI = true;
      options.rtGISamples = options.rtGIBounces = 1;
      options.rtGIDistance = .1f;
      for (uint32_t samples : {1u, 2u, 4u}) {
        options.samples = samples;
        target.enableForward(options);
        auto waterRayPixels = [&](const auto &image) {
          size_t hits = 0;
          for (uint32_t y = target.extent.height / 2; y < target.extent.height; ++y)
            for (uint32_t x = 0; x < target.extent.width; ++x) {
              const auto at = (y * target.extent.width + x) * 4;
              hits += image[at] > 8 && image[at + 1] < 5 && image[at + 2] < 5;
            }
          return hits;
        };
        const auto reflected = drawScene(false, true, false, true, 0, waterFloor, waterRayEmitter);
        std::printf("Hardware water reflection %ux: %zu hidden-emitter pixels\n", samples, waterRayPixels(reflected));
        require(waterRayPixels(reflected) > 12,
                "Water hardware continuation reflects hidden current geometry with SSR disabled");
        require(waterRayPixels(drawScene(false, true, false, true, 0, sheetFloor, waterRayEmitter)) > 12,
                "Finite sheets share hardware reflection of hidden current geometry with SSR disabled");
        require(waterRayPixels(drawScene(false, true, false, true, 0, sheetFloor, waterRayRejected)) < 3,
                "Finite-sheet hardware reflections reject alpha-tested candidates");
        require(waterRayPixels(drawScene(false, true, false, true, 0, waterFloor, waterRayRejected)) < 3,
                "Water hardware traversal rejects alpha-tested candidates");
        options.ssr = true;
        target.enableForward(options);
        const auto combinedWater = drawScene(false, true, false, false, 0, waterFloor, waterRayEmitter);
        bool overbright = false;
        for (uint32_t y = target.extent.height / 2; y < target.extent.height; ++y)
          for (uint32_t x = 0; x < target.extent.width; ++x) {
            const auto at = (y * target.extent.width + x) * 4;
            if (reflected[at] > 8) overbright |= combinedWater[at] > reflected[at] + 5;
          }
        require(waterRayPixels(combinedWater) > 12 && !overbright,
                "Water SSR and hardware continuation share one reflection lobe without double brightness");
        options.ssr = false;
        target.enableForward(options);
        require(waterRayPixels(drawScene(false, false, false, true, 0, waterFloor)) < 3,
                "Removing current geometry clears its water hardware reflection");
        require(waterRayPixels(drawScene(false, true, false, true, 50, waterFloor, waterRayEmitter)) < 3,
                "Moving a hidden emitter updates water hardware reflection placement");
        options.rtReflectionDistance = .1f;
        target.forward->updateLighting(options);
        require(waterRayPixels(drawScene(false, true, false, true, 0, waterFloor, waterRayEmitter)) < 3,
                "Water hardware rays obey the reflection range independently of the GI range");
        options.rtReflectionDistance = 50;
        options.ssrRoughness = .03f;
        target.forward->updateLighting(options);
        require(waterRayPixels(drawScene(false, true, false, true, 0, waterFloor, waterRayEmitter)) < 3,
                "Water hardware rays obey the shared roughness cutoff");
        options.ssrRoughness = .6f;
        target.forward->updateLighting(options);
        require(waterRayPixels(drawScene(false, true, false, true, 0, waterFloor, waterRayEmitter)) > 12,
                "Restored water ray settings recover reflections without temporal history");
        save("hardware-water.ppm", reflected);
      }
      options.water = options.rtGI = false;
      options.ssr = true;
      target.enableForward(options);
      const auto combined = drawScene();
      const auto hardwareOnly = drawScene(false, true, false, true);
      size_t overbright = 0;
      for (uint32_t y = target.extent.height / 2; y < target.extent.height; ++y)
        for (uint32_t x = 0; x < target.extent.width; ++x) {
          const size_t at = (y * target.extent.width + x) * 4;
          overbright += combined[at] > 140;
        }
      require(reflectedPixels(combined) > 12 && reflectedPixels(hardwareOnly) > 12 && !overbright,
              "SSR and hardware radiance share one reflection lobe without double brightness");
      remixapi_LightInfoDomeEXT sky{};
      sky.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
      for (uint32_t c = 0; c < 3; ++c) sky.transform.matrix[c][c] = 1;
      remixapi_LightInfo skyInfo{};
      skyInfo.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO; skyInfo.pNext = &sky;
      skyInfo.hash = 9480; skyInfo.radiance = {0, 0, 1};
      remixapi_LightHandle skyLight; r.createLight(skyInfo, skyLight);
      remixapi_BakeSceneBoundsInfoEXT authored{};
      authored.scene = {sizeof(authored), REMIXAPI_BAKE_SCENE_BOUNDS_VERSION_EXT, 9481, 1, 1,
                        REMIXAPI_BAKE_SCENE_READY_EXT, 0, 1, nullptr, &skyLight};
      const float minimum[] = {-4, -1, -4}, maximum[] = {4, 3, 6};
      std::copy_n(minimum, 3, authored.minimum); std::copy_n(maximum, 3, authored.maximum);
      r.setBakeScene(&authored.scene);
      options.baking.enabled = true; options.baking.maxProbes = 64; options.baking.samples = 64;
      options.baking.bounces = 1; options.baking.batch = 64;
      options.baking.reflectionProbes = 4; options.baking.reflectionResolution = 8;
      options.ssr = false;
      target.enableForward(options);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
      std::vector<uint8_t> blended;
      do {
        blended = drawScene(false, true, false, true);
        if (target.forward->statistics().baking.active) break;
        require(target.forward->statistics().baking.state != REMIXAPI_BAKE_FAILED_EXT,
                "Hardware/probe fixture bake succeeded");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while (std::chrono::steady_clock::now() < deadline);
      require(target.forward->statistics().baking.active, "Hardware/probe fallback published");
      require(target.forward->statistics().bakeContent.captureRadiance != 0,
              "Published cubemaps fingerprint raw RGB capture samples before filtering");
      require(target.forward->statistics().bakeContent.captureInputs != 0,
              "Published cubemaps fingerprint the submitted capture origins and ray directions");
      size_t replaced = 0, fallbackPixels = 0;
      for (uint32_t y = target.extent.height / 2; y < target.extent.height; ++y)
        for (uint32_t x = 0; x < target.extent.width; ++x) {
          const size_t at = (y * target.extent.width + x) * 4;
          replaced += blended[at] > 40 && blended[at + 2] < 20;
          fallbackPixels += blended[at] < 10 && blended[at + 2] > 100;
        }
      std::printf("Hardware/probe blend: %zu replaced, %zu fallback pixels\n", replaced, fallbackPixels);
      require(replaced > 12 && fallbackPixels > 12,
              "Hardware hits replace the blue cubemap while misses retain probe radiance");
      options.water = true;
      target.enableForward(options);
      std::vector<uint8_t> waterBlend;
      const auto waterDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
      do {
        waterBlend = drawScene(false, true, false, true, 0, waterFloor, waterRayEmitter);
        require(target.forward->statistics().baking.state != REMIXAPI_BAKE_FAILED_EXT,
                "Water hardware/probe fixture bake succeeded");
        if (target.forward->statistics().baking.active) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while (std::chrono::steady_clock::now() < waterDeadline);
      require(target.forward->statistics().baking.active, "Water hardware reflection probes published");
      const auto blackWater = drawScene(false, true, false, true, 0, waterFloor, waterRayBlack);
      size_t waterReplaced = 0, waterFallback = 0, blackHits = 0;
      for (uint32_t y = target.extent.height / 2; y < target.extent.height; ++y)
        for (uint32_t x = 0; x < target.extent.width; ++x) {
          const auto at = (y * target.extent.width + x) * 4;
          const bool hit = waterBlend[at] > 20 && waterBlend[at + 2] < 8;
          waterReplaced += hit;
          waterFallback += waterBlend[at] < 3 && waterBlend[at + 2] > 8;
          blackHits += hit && blackWater[at] < 3 && blackWater[at + 2] < 3;
        }
      std::printf("Hardware water/probe blend: %zu replaced, %zu fallback, %zu valid black pixels\n",
                  waterReplaced, waterFallback, blackHits);
      require(waterReplaced > 12 && waterFallback > 12 && blackHits == waterReplaced,
              "Water ray hits replace probes, misses retain them, and valid black hits suppress fallback");
      save("hardware-water-probes.ppm", waterBlend);
      options.water = false;
      target.enableForward(options);
      remixapi_InstanceIdentityEXT wallIdentity{};
      wallIdentity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      wallIdentity.instanceId = 9482; wallIdentity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
      wallIdentity.rasterVisible = 0;
      auto authoredWall = instance(wall); authoredWall.doubleSided = 1; authoredWall.pNext = &wallIdentity;
      const auto authoredHandle = r.createRetained(authoredWall);
      authored.scene.instanceCount = 1; authored.scene.instances = &authoredHandle; ++authored.scene.revision;
      r.setBakeScene(&authored.scene);
      options.baking.enabled = false; target.forward->updateLighting(options);
      require(reflectedPixels(drawScene(false, false)) > 12,
              "Complete authored geometry reflects even when absent from current draws");
      r.setBakeScene(nullptr);
      require(reflectedPixels(drawScene(false, false)) < 3,
              "Unloading the authored scene removes its hardware reflection geometry");
      {
        Target fallback;
        auto &ordinary = fallback.raster;
        options.ambient = 1;
        fallback.enableForward(options);
        camera(ordinary);
        ordinary.drawInstance(instance(quad(ordinary, 9490, material(ordinary, 9491, 1, 1, 1))));
        const auto image = fallback.render();
        require(image[(32 * 64 + 32) * 4] > 150 &&
                    !fallback.forward->statistics().rayReflections.available &&
                    !fallback.forward->statistics().rayReflections.active,
                "Requesting optional rays on an ordinary device preserves the Forward+ fallback");
      }
      require(errors.load() == 0, "Hardware reflection passes are Vulkan validation clean");
      return;
    }
    options.ssr = true;
    for (uint32_t samples : {1u, 2u, 4u}) {
      options.samples = samples;
      for (uint32_t scale : {1u, 2u, 4u}) {
        options.ssrScale = scale;
        target.enableForward(options);
        if (baked) {
          const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
          do {
            drawScene();
            require(target.forward->statistics().baking.state != REMIXAPI_BAKE_FAILED_EXT,
                    "Reflection blend bake succeeded");
            if (target.forward->statistics().baking.active) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          } while (std::chrono::steady_clock::now() < deadline);
          require(target.forward->statistics().baking.active, "Reflection blend bake completed");
        }
        const auto reflected = drawScene();
        if (baked) {
          size_t replaced = 0, fallback = 0;
          for (uint32_t y = target.extent.height / 2; y < target.extent.height; ++y)
            for (uint32_t x = 0; x < target.extent.width; ++x) {
              const size_t at = (y * target.extent.width + x) * 4;
              replaced += reflected[at] > 200 && reflected[at + 2] < 60;
              fallback += reflected[at] < 10 && reflected[at + 2] > 150;
            }
          std::printf("SSR/probe blend %ux, scale %u: %zu replaced, %zu fallback pixels\n",
                      samples, scale, replaced, fallback);
          save("probe-blended.ppm", reflected);
          require(replaced > 4 && fallback > 4,
                  "SSR replaces probe radiance on hits while misses retain the blue cubemap fallback");
        }
        if (scale == 2)
          save(samples == 1 ? "ssr-1x.ppm" : samples == 2 ? "ssr-2x.ppm" : "ssr-4x.ppm", reflected);
        std::printf("SSR %ux, scale %u: %zu reflected pixels\n", samples, scale, reflectedPixels(reflected));
        require(reflectedPixels(reflected) > 12,
                "SSR reflects the visible emissive wall in the metallic floor");
        if (!baked) {
          options.water = true;
          target.enableForward(options);
          composeWater = true;
          const auto composed = drawScene();
          require(target.forward->statistics().draws == 3,
                  "An occluded measured surface still exercises late water composition");
          for (size_t at = 0; at < reflected.size(); ++at)
            require(std::abs(int(composed[at]) - int(reflected[at])) <= 2,
                    "Late water composition preserves opaque SSR energy and coverage exactly once");
          if (samples == 4 && scale == 1) {
            for (const uint32_t view : {1u, 5u, 6u}) {
              options.debugView = view;
              target.enableForward(options);
              composeWater = false;
              const auto diagnostic = drawScene();
              composeWater = true;
              const auto withWater = drawScene();
              require(target.forward->statistics().draws == 3,
                      "Diagnostic regression exercises occluded water composition");
              for (size_t at = 0; at < diagnostic.size(); ++at)
                require(std::abs(int(withWater[at]) - int(diagnostic[at])) <= 2,
                        "Occluded water added reflections to an opaque lighting diagnostic");
            }
            options.debugView = 0;
            target.enableForward(options);
          }
          composeWater = false;
          const auto waterReflection = drawScene(false, true, false, false, 0, waterFloor);
          options.ssrSteps = 0;
          target.forward->updateLighting(options);
          const auto waterWithout = drawScene(false, true, false, false, 0, waterFloor);
          auto waterHits = [&](const auto &image) {
            size_t hits = 0;
            for (uint32_t y = target.extent.height / 2; y < target.extent.height; ++y)
              for (uint32_t x = 0; x < target.extent.width; ++x) {
                const auto at = (y * target.extent.width + x) * 4;
                hits += image[at] > waterWithout[at] + 8 && image[at + 1] < 8;
              }
            return hits;
          };
          std::printf("Water SSR %ux, scale %u: %zu reflected pixels\n", samples, scale,
                      waterHits(waterReflection));
          require(waterHits(waterReflection) > 12,
                  "A measured water surface reflects a current opaque emitter through shared SSR");
          options.ssrSteps = 128;
          target.forward->updateLighting(options);
          const auto sheetReflection = drawScene(false, true, false, false, 0, sheetFloor);
          options.ssrSteps = 0;
          target.forward->updateLighting(options);
          const auto sheetWithout = drawScene(false, true, false, false, 0, sheetFloor);
          size_t sheetHits = 0;
          for (uint32_t y = target.extent.height / 2; y < target.extent.height; ++y)
            for (uint32_t x = 0; x < target.extent.width; ++x) {
              const auto at = (y * target.extent.width + x) * 4;
              sheetHits += sheetReflection[at] > sheetWithout[at] + 8 && sheetReflection[at + 1] < 8;
            }
          require(sheetHits > 12,
                  "A finite dielectric sheet reflects current opaque geometry through shared SSR");
          options.ssrSteps = 128;
          options.ssrRoughness = .03f;
          target.forward->updateLighting(options);
          require(waterHits(drawScene(false, true, false, false, 0, waterFloor)) < 3,
                  "Water reflection tracing obeys the shared roughness cutoff");
          options.ssrRoughness = .6f;
          target.forward->updateLighting(options);
          require(waterHits(drawScene(false, false, false, false, 0, waterFloor)) < 3,
                  "Removed opaque geometry leaves no stale water reflection");
          if (scale == 2) save("water-ssr.ppm", waterReflection);
          options.water = false;
          target.enableForward(options);
        }
        require(reflectedPixels(drawScene(true)) < 3, "Roughness cutoff removes sharp screen reflections");
        require(reflectedPixels(drawScene(false, false)) < 3,
                "Removed geometry leaves no stale reflection radiance");
        require(reflectedPixels(drawScene(false, true, true)) < 3,
                "Alpha-tested surfaces do not write false reflection hits");
        if (baked && samples == 4 && scale == 4) {
          options.water = true;
          target.enableForward(options);
          const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
          std::vector<uint8_t> waterBlend;
          do {
            waterBlend = drawScene(false, true, false, false, 0, waterFloor);
            require(target.forward->statistics().baking.state != REMIXAPI_BAKE_FAILED_EXT,
                    "Water reflection fallback bake succeeded");
            if (target.forward->statistics().baking.active) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          } while (std::chrono::steady_clock::now() < deadline);
          require(target.forward->statistics().baking.active, "Water reflection probes published");
          size_t replaced = 0, fallback = 0;
          for (uint32_t y = target.extent.height / 2; y < target.extent.height; ++y)
            for (uint32_t x = 0; x < target.extent.width; ++x) {
              const auto at = (y * target.extent.width + x) * 4;
              replaced += waterBlend[at] > 20 && waterBlend[at + 2] < 8;
              fallback += waterBlend[at] < 3 && waterBlend[at + 2] > 8;
            }
          std::printf("Water SSR/probe blend: %zu replaced, %zu fallback pixels\n", replaced, fallback);
          require(replaced > 4 && fallback > 4,
                  "Water screen hits replace probe radiance while misses retain the baked cubemap");
          save("water-probe-blended.ppm", waterBlend);
        }
      }
    }
    if (!baked) {
      const auto blurredMaterial = makeMaterial(9500, .45f, 1, 0);
      const auto blurredFloor = makeQuad(9501, blurredMaterial, floorPositions, floorNormal);
      const float narrowPositions[][3] = {{-.12f, .15f, 3}, {.12f, .15f, 3},
                                          {.12f, 1.8f, 3}, {-.12f, 1.8f, 3}};
      const auto narrowMaterial = makeMaterial(9502, .5f, 0, .5f);
      const auto narrowWall = makeQuad(9503, narrowMaterial, narrowPositions, wallNormal);
      const auto waterEmitter = makeQuad(9511, makeMaterial(9510, .5f, 0, 1), narrowPositions, wallNormal);
      const auto blackMaterial = makeMaterial(9504, .5f, 0, 0);
      const float backdropPositions[][3] = {{-.8f, .15f, 3.02f}, {.8f, .15f, 3.02f},
                                            {.8f, 1.8f, 3.02f}, {-.8f, 1.8f, 3.02f}};
      reflectionBackdrop = makeQuad(9505, blackMaterial, backdropPositions, wallNormal);
      struct Highlight {
        double energy = 0, width = 0;
        uint8_t peak = 0;
      };
      auto highlight = [&](const std::vector<uint8_t> &image) {
        Highlight result;
        for (uint32_t y = target.extent.height / 2; y < target.extent.height; ++y)
          for (uint32_t x = 0; x < target.extent.width; ++x) {
            const size_t at = (y * target.extent.width + x) * 4;
            if (without[at] > 8) continue; // exclude the directly visible emitter
            const auto red = image[at];
            const double offset = double(x) - double(target.extent.width - 1) * .5;
            result.energy += red;
            result.width += red * offset * offset;
            result.peak = std::max(result.peak, red);
          }
        result.width /= std::max(result.energy, 1.);
        return result;
      };
      options.ssrScale = 1;
      for (uint32_t samples : {1u, 2u, 4u}) {
        options.samples = samples;
        target.enableForward(options);
        const auto sharpImage = drawScene(false, true, false, false, 0, floor, narrowWall);
        const auto roughImage = drawScene(false, true, false, false, 0, blurredFloor, narrowWall);
        const auto sharp = highlight(sharpImage), rough = highlight(roughImage);
        std::printf("SSR colour footprint %ux: peak %u/%u, width %.4f/%.4f, energy %.0f/%.0f sharp/rough\n",
                    samples, sharp.peak, rough.peak, sharp.width, rough.width, sharp.energy, rough.energy);
        require(sharp.energy > 100 && rough.energy > 100 && rough.peak + 5 < sharp.peak &&
                    rough.width > sharp.width * 1.2,
                "Roughness spreads a narrow reflected emitter and lowers its peak without losing the reflection");
        save(samples == 1 ? "cone-sharp-1x.ppm" : samples == 2 ? "cone-sharp-2x.ppm" : "cone-sharp-4x.ppm", sharpImage);
        save(samples == 1 ? "cone-rough-1x.ppm" : samples == 2 ? "cone-rough-2x.ppm" : "cone-rough-4x.ppm", roughImage);
        options.water = true;
        target.enableForward(options);
        waterRoughness = .04f;
        const auto sharpWaterImage = drawScene(false, true, false, false, 0, waterFloor, waterEmitter);
        waterRoughness = .45f;
        const auto roughWaterImage = drawScene(false, true, false, false, 0, waterFloor, waterEmitter);
        const auto sharpWater = highlight(sharpWaterImage), roughWater = highlight(roughWaterImage);
        std::printf("Water SSR colour footprint %ux: peak %u/%u, width %.4f/%.4f, energy %.0f/%.0f sharp/rough\n",
                    samples, sharpWater.peak, roughWater.peak, sharpWater.width, roughWater.width,
                    sharpWater.energy, roughWater.energy);
        require(sharpWater.energy > 100 && roughWater.energy > 100 && roughWater.peak + 3 < sharpWater.peak &&
                    roughWater.width > sharpWater.width * 1.2,
                "Resolved water roughness spreads a narrow reflected emitter and lowers its peak");
        save("water-cone-sharp.ppm", sharpWaterImage);
        save("water-cone-rough.ppm", roughWaterImage);
        options.water = false;
        target.enableForward(options);
      }
    }
    std::puts("SSR GPU readback: reflected object, odd dimensions, roughness, MSAA, removal and alpha "
              "coverage passed");
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
    pixel(target.render(), 32, 32, 55, 55, 55, "UI display-color decoding into linear target");
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
    const auto greyMaterial = material(r, 100, .21404114f, .21404114f, .21404114f);
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
#include "forward_sheet.h"
  void waterTests() {
    for (uint32_t samples : {1u, 2u, 4u}) {
      Target target;
      auto &r = target.raster;
      remix::forward::Options options;
      options.baking.lightmapFitBudget = false; // Controlled-density lighting fixtures.
      options.water = true;
      options.samples = samples;
      options.rtReflections = true; // deliberately unavailable on this core device
      options.ambient = 1;
      options.shadows = options.localShadows = options.ssr = false;
      options.baking.enabled = false;
      options.baking.reflectionProbes = 0;
      target.enableForward(options);
      const auto background = quad(r, 0xe100, material(r, 0xe101, 1, .1f, .05f));
      remixapi_MaterialInfoTranslucentEXT optical{};
      optical.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
      optical.refractiveIndex = 1;
      optical.transmittanceColor = {.15f, .6f, .8f};
      optical.transmittanceMeasurementDistance = 1;
      remixapi_MaterialInfo info{};
      info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
      info.hash = 0xe102;
      info.pNext = &optical;
      remixapi_MaterialHandle measured;
      r.createMaterial(info, measured);
      const auto liquid = quad(r, 0xe103, measured);
      const auto foreground = quad(r, 0xe104, material(r, 0xe105, 0, 0, 1));
      const auto centre = (32 * 64 + 32) * 4;
      auto draw = [&](bool water, bool front) {
        camera(r);
        r.drawInstance(instance(background, .3f));
        if (water) {
          auto surface = instance(liquid, -.2f);
          surface.transform.matrix[0][0] = surface.transform.matrix[1][1] = .6f;
          r.drawInstance(surface);
        }
        if (front) r.drawInstance(instance(foreground, -.4f));
        return target.render();
      };
      const auto dry = draw(false, false);
      const auto wet = draw(true, false);
      const float passed[] = {std::sqrt(.15f), std::sqrt(.6f), std::sqrt(.8f)};
      const float tint[] = {.15f, .6f, .8f}, color[] = {1, .1f, .05f};
      const float body = 1 - (passed[0] * .2126f + passed[1] * .7152f + passed[2] * .0722f);
      std::array<float, 3> wetHdr;
      for (uint32_t c = 0; c < 3; ++c) wetHdr[c] = color[c] * passed[c] + tint[c] * body;
      const auto wetDisplay = prismDisplay(wetHdr);
      for (uint32_t c = 0; c < 3; ++c) {
        const float expected = wetDisplay[c] * 255;
        std::printf("Water %ux channel %u: %u expected %.2f\n", samples, c, unsigned(wet[centre+c]), expected);
        require(std::abs(float(wet[centre+c]) - expected) < 4,
                "Live water consumes opaque HDR, measured path length and lit body colour before tone mapping");
      }
      require(target.forward->statistics().draws == 2,
              "Water geometry is submitted by the active Forward+ renderer");
      const auto front = draw(true, true);
      const auto foregroundDisplay = prismDisplay({0, 0, 1});
      require(std::abs(front[centre] - foregroundDisplay[0] * 255) < 3 &&
                  std::abs(front[centre+1] - foregroundDisplay[1] * 255) < 3 &&
                  std::abs(front[centre+2] - foregroundDisplay[2] * 255) < 3,
              "Foreground opaque geometry remains in front of measured water");
      for (uint32_t slot = 0; slot < remix::gpuFrameCount + 2; ++slot) {
        const auto reused = draw(true, false);
        for (uint32_t c = 0; c < 3; ++c)
          require(std::abs(int(reused[centre+c]) - int(wet[centre+c])) <= 1,
                  "Water targets and descriptor slots can be reused without stale data");
      }
      if (samples > 1) {
        uint32_t partial = 0;
        for (uint32_t y = 10; y < 54; ++y)
          for (uint32_t x = 10; x < 54; ++x) {
            const auto at = (y*64+x)*4;
            if (wet[at] > wet[centre] + 2 && wet[at] + 2 < dry[at]) ++partial;
          }
        require(partial > 0, "Water edges retain partial MSAA coverage over opaque HDR");
      }
      std::array<float, 3> environment{};
      auto submerged = [&](bool surface) {
        remixapi_CameraEnvironmentEXT sky{};
        sky.sType = REMIXAPI_STRUCT_TYPE_CAMERA_ENVIRONMENT_EXT;
        sky.enabled = 1;
        sky.radiance = {environment[0], environment[1], environment[2]};
        remixapi_CameraMediumEXT medium{};
        medium.sType = REMIXAPI_STRUCT_TYPE_CAMERA_MEDIUM_EXT;
        medium.pNext = &sky;
        medium.inside = 1;
        medium.transmittanceColor = {.25f, .5f, .75f};
        medium.transmittanceMeasurementDistance = 1;
        remixapi_CameraInfo view{};
        view.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
        view.pNext = &medium;
        for (uint32_t i = 0; i < 4; ++i) view.view[i][i] = view.projection[i][i] = 1;
        r.setupCamera(view);
        r.drawInstance(instance(background, .3f));
        if (surface) r.drawInstance(instance(liquid, -.2f));
        return target.render();
      };
      for (bool exit : {false, true}) {
        const auto underwater = submerged(exit);
        const float distance = exit ? .3f : .8f;
        const float medium[] = {.25f, .5f, .75f};
        std::array<float, 3> fogHdr;
        for (uint32_t c = 0; c < 3; ++c) fogHdr[c] = color[c] * std::pow(medium[c], distance);
        const auto fogDisplay = prismDisplay(fogHdr);
        for (uint32_t c = 0; c < 3; ++c)
          require(std::abs(float(underwater[centre+c]) - fogDisplay[c] * 255) < 4,
                  "Camera medium attenuates opaque geometry and water exits once at the nearest depth");
      }
      environment = {.2f, .4f, .6f};
      for (bool exit : {false, true}) {
        const auto underwater = submerged(exit);
        const float distance = exit ? .3f : .8f;
        const float medium[] = {.25f, .5f, .75f};
        std::array<float, 3> fogHdr;
        for (uint32_t c = 0; c < 3; ++c) {
          const float passed = std::pow(medium[c], distance);
          fogHdr[c] = passed * color[c] + (1 - passed) * medium[c] * environment[c];
        }
        const auto fogDisplay = prismDisplay(fogHdr);
        for (uint32_t c = 0; c < 3; ++c) {
          require(std::abs(float(underwater[centre+c]) - fogDisplay[c] * 255) < 4,
                  "Underwater body colour uses uniform camera environment without a sky dome");
        }
      }
      options.water = false;
      target.enableForward(options);
      draw(true, false);
      require(target.forward->statistics().draws == 1,
              "Disabling the water pass returns its draw to compatibility rendering");
    }
  }
  void waterShorelineTests(const std::filesystem::path &directory) {
    for (uint32_t samples : {1u, 2u, 4u}) {
      Target target;
      auto &r = target.raster;
      remix::forward::Options options;
      options.samples = samples;
      options.ambient = 1;
      options.shadows = options.localShadows = options.ssr = false;
      options.baking.enabled = false;
      target.enableForward(options);
      remixapi_HardcodedVertex vertices[4]{};
      // One red plane crosses the water at the centre of pixel 32. Its optical
      // path is effectively zero there, even when only some MSAA samples are wet.
      for (uint32_t i = 0; i < 4; ++i) {
        const float x = i == 1 || i == 2 ? .8f : -.8f;
        vertices[i].position[0] = x;
        vertices[i].position[1] = i >= 2 ? .8f : -.8f;
        vertices[i].position[2] = .3f + .2f * (x - 1.f / 64);
        vertices[i].color = 0xffffffff;
      }
      const uint32_t indices[] = {0, 2, 1, 0, 3, 2};
      remixapi_MeshInfoSurfaceTriangles surface{};
      surface.vertices_values = vertices;
      surface.vertices_count = 4;
      surface.indices_values = indices;
      surface.indices_count = 6;
      surface.material = material(r, 0xe200, 1, .1f, .05f);
      remixapi_MeshInfo mesh{};
      mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
      mesh.hash = 0xe201;
      mesh.surfaces_values = &surface;
      mesh.surfaces_count = 1;
      remixapi_MeshHandle ground;
      r.createMesh(mesh, ground);
      remixapi_MaterialInfoTranslucentEXT optical{};
      optical.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
      optical.refractiveIndex = 1;
      optical.transmittanceColor = {.15f, .6f, .8f};
      optical.transmittanceMeasurementDistance = 1;
      remixapi_MaterialInfo info{};
      info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
      info.hash = 0xe202;
      info.pNext = &optical;
      remixapi_MaterialHandle water;
      r.createMaterial(info, water);
      const auto liquid = quad(r, 0xe203, water);
      camera(r);
      r.drawInstance(instance(ground));
      const auto dry = target.render();
      camera(r);
      r.drawInstance(instance(ground));
      r.drawInstance(instance(liquid, -.2f));
      const auto wet = target.render();
      std::ofstream image(directory / ("shoreline-" + std::to_string(samples) + ".ppm"), std::ios::binary);
      image << "P6\n64 64\n255\n";
      for (size_t i = 0; i < wet.size(); i += 4) image.write(reinterpret_cast<const char *>(wet.data() + i), 3);
      const auto centre = (32 * 64 + 32) * 4;
      std::printf("Shoreline %ux: dry %u %u %u; wet %u %u %u\n", samples,
                  dry[centre], dry[centre+1], dry[centre+2], wet[centre], wet[centre+1], wet[centre+2]);
      VkPhysicalDeviceProperties properties{};
      vkGetPhysicalDeviceProperties(target.physical, &properties);
      if (samples == 1 || properties.limits.standardSampleLocations)
        for (uint32_t c = 0; c < 3; ++c)
          require(std::abs(int(wet[centre+c]) - int(dry[centre+c])) <= 2,
                  "A zero-depth shoreline stays transparent with mixed foreground/background MSAA samples");
    }
  }
  void waterEnvironmentTests(bool automatic = false, bool sheet = false) {
    for (uint32_t samples : {1u, 2u, 4u}) {
      Target target(64, 64, true); // The visibility bake requires hardware ray queries.
      auto &r = target.raster;
      remix::forward::Options options;
      options.samples = samples;
      options.water = true;
      options.autoExposure = automatic;
      options.autoExposureMinEv = options.autoExposureMaxEv = 0; // Keep analytic water references at unity.
      options.shadows = options.localShadows = options.ssr = false;
      options.baking.enabled = false;
      options.baking.reflectionProbes = 0;
      const auto backdrop = quad(r, 0xe109, material(r, 0xe10a, 0, 0, 0));
      const auto centre = (32 * 64 + 32) * 4;
      remixapi_MaterialInfoTranslucentEXT optical{};
      optical.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
      optical.transmittanceMeasurementDistance = 1;
      optical.refractiveIndex = 1.5f;
      optical.transmittanceColor = {1, 1, 1};
      optical.thinWallThickness_hasvalue = sheet;
      optical.thinWallThickness_value = sheet ? .25f : 0;
      const float reflectance = sheet ? 1.f / 13 : .04f;
      remixapi_MaterialInfo info{};
      info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
      info.pNext = &optical;
      info.hash = 0xe106;
      remixapi_MaterialHandle measured;
      r.createMaterial(info, measured);
      const auto reflectiveLiquid = quad(r, 0xe107, measured);
      options.ambient = 0;
      target.enableForward(options);
      const std::array<float, 3> environment{2, 4, 6};
      remixapi_CameraEnvironmentEXT sky{};
      sky.sType = REMIXAPI_STRUCT_TYPE_CAMERA_ENVIRONMENT_EXT;
      sky.enabled = 1;
      sky.radiance = {environment[0], environment[1], environment[2]};
      remixapi_CameraInfo view{};
      view.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
      view.pNext = &sky;
      for (uint32_t i = 0; i < 4; ++i) view.view[i][i] = view.projection[i][i] = 1;
      remixapi_LightHandle environmentLight = nullptr;
      auto reflectSky = [&] {
        r.setupCamera(view);
        if (environmentLight) r.drawLight(environmentLight);
        r.drawInstance(instance(reflectiveLiquid, -.2f));
        // Keep the black backdrop inside the identity projection's clip
        // volume, isolating reflection from transmission of the visible sky.
        r.drawInstance(instance(backdrop, .3f));
        return target.render();
      };
      const auto unknown = reflectSky();
      require(unknown[centre] < 3 && unknown[centre+1] < 3 && unknown[centre+2] < 3,
              "Uniform environment reflections remain suppressed without baked visibility");
      remixapi_BakeSceneBoundsInfoEXT bounds{};
      bounds.scene.structSize = sizeof(bounds);
      bounds.scene.version = REMIXAPI_BAKE_SCENE_BOUNDS_VERSION_EXT;
      bounds.scene.sceneId = bounds.scene.revision = 0xe108;
      bounds.scene.sceneScale = 1;
      bounds.scene.state = REMIXAPI_BAKE_SCENE_READY_EXT;
      std::fill_n(bounds.minimum, 3, -1.f);
      std::fill_n(bounds.maximum, 3, 1.f);
      r.setBakeScene(&bounds.scene);
      options.baking.enabled = true;
      options.baking.diskCache = false;
      options.baking.maxProbes = 128;
      options.baking.samples = 16;
      options.baking.bounces = 1;
      target.enableForward(options);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
      while (!target.forward->statistics().baking.active && std::chrono::steady_clock::now() < deadline) {
        reflectSky();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      require(target.forward->statistics().baking.active, "Water sky fallback waits for published visibility");
      const auto reflected = reflectSky();
      const auto reflectedDisplay = prismDisplay({reflectance * environment[0], reflectance * environment[1], reflectance * environment[2]});
      for (uint32_t c = 0; c < 3; ++c) {
        std::printf("Water sky %ux channel %u: %u expected %.2f\n", samples, c, unsigned(reflected[centre+c]),
                    reflectedDisplay[c] * 255);
        require(std::abs(float(reflected[centre+c]) - reflectedDisplay[c] * 255) < 3,
                "Uncovered water reflections use uniform camera environment with IOR Fresnel weighting");
      }
      remixapi_LightInfoDomeEXT dome{};
      dome.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
      for (uint32_t c = 0; c < 3; ++c) dome.transform.matrix[c][c] = 1;
      remixapi_LightInfo domeLight{};
      domeLight.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
      domeLight.pNext = &dome;
      domeLight.hash = 0xe10b;
      domeLight.radiance = {6, 2, 1};
      r.createLight(domeLight, environmentLight);
      const auto authored = reflectSky();
      const auto authoredDisplay = prismDisplay({reflectance * 6, reflectance * 2, reflectance});
      for (uint32_t c = 0; c < 3; ++c)
        require(std::abs(float(authored[centre+c]) - authoredDisplay[c] * 255) < 3,
                "An authored dome replaces uniform water environment instead of adding it twice");
      environmentLight = nullptr;
      const auto restored = reflectSky();
      require(restored == reflected, "Removing a live dome restores identical uniform water reflections");
    }
  }
} // namespace
int main(int argc, char **argv) try {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  require(argc >= 2, "Supply scratch directory");
  const std::filesystem::path directory(argv[1]);
  std::filesystem::create_directories(directory);
  if (argc>2 && std::strcmp(argv[2],"forward")==0) forwardTests(directory);
  else if (argc>2 && std::strcmp(argv[2],"forward-skinning")==0) forwardSkinningTests();
  else if (argc>2 && std::strcmp(argv[2],"lighting-views")==0) lightingViewTests();
  else if (argc>2 && std::strcmp(argv[2],"no-oidn")==0) {
    require(remix::forward::lightmap::denoiserVersion() == 0,
            "Unavailable-denoiser test links the actual module without OIDN support");
    surfaceLightmapTests();
  }
  else if (argc>2 && std::strcmp(argv[2],"water")==0) waterTests();
  else if (argc>2 && std::strcmp(argv[2],"sheet")==0) sheetTests();
  else if (argc>2 && std::strcmp(argv[2],"sheet-environment")==0) waterEnvironmentTests(false, true);
  else if (argc>2 && std::strcmp(argv[2],"water-environment")==0) waterEnvironmentTests();
  else if (argc>2 && std::strcmp(argv[2],"water-exposure")==0) waterEnvironmentTests(true);
  else if (argc>2 && std::strcmp(argv[2],"water-shoreline")==0) waterShorelineTests(directory);
  else if (argc>2 && std::strcmp(argv[2],"sky")==0) skyTests();
  else if (argc>2 && std::strcmp(argv[2],"live-gi")==0) liveGITests();
  else if (argc>2 && std::strcmp(argv[2],"hardware-reflections")==0) reflectionTests(directory, false, true);
  else if (argc>2 && std::strcmp(argv[2],"reflections")==0) reflectionTests(directory);
  else if (argc>2 && std::strcmp(argv[2],"baking")==0) bakingTests();
  else if (argc>2 && std::strcmp(argv[2],"lightmaps")==0) { surfaceLightmapTests(); surfaceLightmapTests(true); surfaceLightmapTests(true, true); }
  else if (argc>2 && std::strcmp(argv[2],"local-shadows")==0) mixedLightingTests(true);
  else if (argc>2 && std::strcmp(argv[2],"baked-direct")==0) mixedLightingTests(true, true);
  else if (argc>2 && std::strcmp(argv[2],"aggregate-direct")==0) mixedLightingTests(true, true, false, true);
  else if (argc>2 && std::strcmp(argv[2],"indexed-directionals")==0) indexedDirectionalTests();
  else if (argc>2 && std::strcmp(argv[2],"selected-directionals")==0) {
    selectedDirectionalTests();
    finiteConeLightingTests(true);
  }
  else if (argc>2 && std::strcmp(argv[2],"finite-cone")==0) finiteConeLightingTests();
  else if (argc>2 && std::strcmp(argv[2],"styled-indirect")==0) styledIndirectTests();
  else if (argc>2 && std::strcmp(argv[2],"styled-direct")==0) styledIndirectTests(true);
  else if (argc>2 && std::strcmp(argv[2],"styled-combined")==0) {
    const auto one = styledIndirectTests(true, true);
    const auto seven = styledIndirectTests(true, true, 7);
    for (uint32_t i = 0; i < one.size(); ++i)
      require(std::abs(int(one[i]) - int(seven[i])) <= 2,
              "Splitting the same authored power across seven overlapping emitters preserves grouped energy");
  }
  else if (argc>2 && std::strcmp(argv[2],"local-shadow-plane")==0) localShadowPlaneTests();
  else if (argc>2 && std::strcmp(argv[2],"auto-exposure")==0) autoExposureTests();
  else if (argc>2 && std::strcmp(argv[2],"styled-mixed")==0) mixedLightingTests(true, true, true);
  else if (argc>2 && std::strcmp(argv[2],"mixed")==0) mixedLightingTests();
  else if (argc>2 && std::strcmp(argv[2],"compact-occlusion")==0) compactOcclusionTests();
  else if (argc>2 && std::strcmp(argv[2],"overflow-specular")==0) compactOcclusionTests(true);
  else if (argc>2 && std::strcmp(argv[2],"overflow-masks")==0) compactOcclusionTests(true, true);
  else if (argc>2 && std::strcmp(argv[2],"viewmodel-lighting")==0) viewModelLightingTests();
  else if (argc>2 && std::strcmp(argv[2],"viewmodel-probes")==0) viewModelLightingTests(true);
  else if (argc>2 && std::strcmp(argv[2],"authored-reflections")==0) viewModelLightingTests(true, true, true);
  else if (argc>2 && std::strcmp(argv[2],"viewmodel-ssr")==0) reflectionTests(directory, false, false, false, 4, true);
  else if (argc>2 && std::strcmp(argv[2],"viewmodel-environment")==0) viewModelLightingTests(true, true);
  else if (argc>2 && std::strcmp(argv[2],"terrain-lighting")==0) terrainLightingTests();
  else if (argc>2 && std::strcmp(argv[2],"decal-lighting")==0) terrainLightingTests(true);
  else if (argc>2 && std::strcmp(argv[2],"clear-visibility")==0) clearVisibilityTests();
  else if (argc>2 && std::strcmp(argv[2],"mixed-sun")==0) sunLightingTests();
  else if (argc>2 && std::strcmp(argv[2],"sun-depth-lifecycle")==0) sunDepthLifecycleTests();
  else if (argc>2 && std::strcmp(argv[2],"probe-blend")==0) reflectionTests(directory, true);
  else if (argc>2 && std::strcmp(argv[2],"sky-reflections")==0) {
    reflectionTests(directory, true, false, true, 1);
    reflectionTests(directory, true, false, true, 4);
  }
  else gpuTests(directory);
  require(!errors.load(), "Vulkan validation errors");
} catch (VkResult error) {
  std::fprintf(stderr, "Raster GPU fixture failed with VkResult %d\n", int(error));
  return 1;
} catch (remixapi_ErrorCode error) {
  std::fprintf(stderr, "Raster GPU fixture failed with Remix error %d\n", int(error));
  return 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "Raster GPU fixture failed: %s\n", error.what());
  return 1;
}
