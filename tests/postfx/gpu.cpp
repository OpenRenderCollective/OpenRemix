#include "postfx/meter.h"
#include "postfx/renderer.h"
#include "pathtrace/options.h"
#include "runtime/pipelines.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
using namespace remix;
using namespace remix::postfx;
void require(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}
void check(VkResult r) {
  if (r) {
    std::fprintf(stderr, "Vulkan %d\n", r);
    std::exit(1);
  }
}
void barrier(VkCommandBuffer cmd) {
  VkMemoryBarrier b{
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, nullptr,
                       0, nullptr);
}
struct Fixture {
  VkInstance instance{};
  VkPhysicalDevice physical{};
  VkDevice device{};
  VkQueue queue{};
  uint32_t family{};
  VkCommandPool pool{};
  VkCommandBuffer cmd{};
  VkPhysicalDeviceMemoryProperties memory{};
  std::unique_ptr<pipeline::Service> pipelines;
  Fixture() {
    VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                          .pApplicationName = "Postfx deterministic GPU test",
                          .apiVersion = VK_API_VERSION_1_2};
    const char *layer = "VK_LAYER_KHRONOS_validation";
    bool validation = std::getenv("REMIX_VULKAN_VALIDATION");
    VkInstanceCreateInfo ci{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                            .pApplicationInfo = &app,
                            .enabledLayerCount = uint32_t(validation),
                            .ppEnabledLayerNames = &layer};
    check(vkCreateInstance(&ci, nullptr, &instance));
    uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    require(count, "No GPU");
    std::vector<VkPhysicalDevice> devices(count);
    check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    physical = devices[0];
    for (auto candidate : devices) {
      VkPhysicalDeviceProperties props;
      vkGetPhysicalDeviceProperties(candidate, &props);
      if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        physical = candidate;
        break;
      }
    }
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    while (!(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT))
      ++family;
    float priority = 1;
    VkDeviceQueueCreateInfo qi{.sType =
                                   VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                               .queueFamilyIndex = family,
                               .queueCount = 1,
                               .pQueuePriorities = &priority};
    VkPhysicalDeviceFeatures features{};
    features.shaderStorageImageExtendedFormats = VK_TRUE;
    VkDeviceCreateInfo di{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                          .queueCreateInfoCount = 1,
                          .pQueueCreateInfos = &qi,
                          .pEnabledFeatures = &features};
    check(vkCreateDevice(physical, &di, nullptr, &device));
    vkGetDeviceQueue(device, family, 0, &queue);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    std::printf("GPU: %s\n", properties.deviceName);
    pipelines = std::make_unique<pipeline::Service>(device, properties, false);
    VkCommandPoolCreateInfo pi{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = family};
    check(vkCreateCommandPool(device, &pi, nullptr, &pool));
    VkCommandBufferAllocateInfo ai{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1};
    check(vkAllocateCommandBuffers(device, &ai, &cmd));
  }
  ~Fixture() {
    pipelines.reset();
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
  }
  uint32_t type(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
      if ((bits & (1 << i)) &&
          (memory.memoryTypes[i].propertyFlags & flags) == flags)
        return i;
    require(false, "Memory type");
    return 0;
  }
  void begin() {
    check(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo b{.sType =
                                   VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(cmd, &b));
    barrier(cmd);
  }
  void end() {
    check(vkEndCommandBuffer(cmd));
    VkSubmitInfo s{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                   .commandBufferCount = 1,
                   .pCommandBuffers = &cmd};
    check(vkQueueSubmit(queue, 1, &s, {}));
    check(vkQueueWaitIdle(queue));
  }
  std::unique_ptr<Renderer> renderer(VkExtent2D size, bool bloom = true,
                                     std::shared_ptr<State> state = {}, bool meter = true,
                                     MeterMode mode = MeterMode::MaterialNormalized) {
    auto r = std::make_unique<Renderer>();
    static unsigned id = 0;
    auto ticket = pipelines->prepare("fixture-" + std::to_string(++id), [&] {
      r->initialize(physical, device, family, size, size, meter, bloom, state, mode);
    });
    while (!ticket->ready())
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ticket->check();
    return r;
  }
};
struct Buffer {
  Fixture &f;
  VkBuffer handle{};
  VkDeviceMemory memory{};
  void *data{};
  Buffer(Fixture &f, size_t size) : f(f) {
    VkBufferCreateInfo i{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                         .size = size,
                         .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    check(vkCreateBuffer(f.device, &i, nullptr, &handle));
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(f.device, handle, &r);
    VkMemoryAllocateInfo a{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = r.size,
        .memoryTypeIndex =
            f.type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    check(vkAllocateMemory(f.device, &a, nullptr, &memory));
    check(vkBindBufferMemory(f.device, handle, memory, 0));
    check(vkMapMemory(f.device, memory, 0, size, 0, &data));
  }
  ~Buffer() {
    vkUnmapMemory(f.device, memory);
    vkDestroyBuffer(f.device, handle, nullptr);
    vkFreeMemory(f.device, memory, nullptr);
  }
};
struct Texture {
  Fixture &f;
  VkImage image{};
  VkImageView view{};
  VkDeviceMemory memory{};
  VkExtent2D size;
  Texture(Fixture &f, VkExtent2D size) : f(f), size(size) {
    VkImageCreateInfo i{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                        .imageType = VK_IMAGE_TYPE_2D,
                        .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                        .extent = {size.width, size.height, 1},
                        .mipLevels = 1,
                        .arrayLayers = 1,
                        .samples = VK_SAMPLE_COUNT_1_BIT,
                        .tiling = VK_IMAGE_TILING_OPTIMAL,
                        .usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    check(vkCreateImage(f.device, &i, nullptr, &image));
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(f.device, image, &r);
    VkMemoryAllocateInfo a{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = r.size,
        .memoryTypeIndex =
            f.type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    check(vkAllocateMemory(f.device, &a, nullptr, &memory));
    check(vkBindImageMemory(f.device, image, memory, 0));
    VkImageViewCreateInfo v{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = i.format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    check(vkCreateImageView(f.device, &v, nullptr, &view));
    f.begin();
    VkImageMemoryBarrier b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                           .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                           .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                           .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                           .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                           .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                           .image = image,
                           .subresourceRange = v.subresourceRange};
    vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    f.end();
  }
  ~Texture() {
    vkDestroyImageView(f.device, view, nullptr);
    vkDestroyImage(f.device, image, nullptr);
    vkFreeMemory(f.device, memory, nullptr);
  }
  void upload(const std::vector<std::array<float, 4>> &pixels) {
    Buffer b(f, pixels.size() * 16);
    std::memcpy(b.data, pixels.data(), pixels.size() * 16);
    f.begin();
    VkBufferImageCopy copy{
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {size.width, size.height, 1}};
    vkCmdCopyBufferToImage(f.cmd, b.handle, image, VK_IMAGE_LAYOUT_GENERAL, 1,
                           &copy);
    barrier(f.cmd);
    f.end();
  }
};
float half(uint16_t h) {
  float value = std::ldexp(float(h & 1023), -24);
  unsigned e = (h >> 10) & 31;
  if (e)
    value = e == 31 ? INFINITY
                    : std::ldexp(1.f + float(h & 1023) / 1024, int(e) - 15);
  return h & 0x8000 ? -value : value;
}
std::vector<float> readBloom(Fixture &f, Renderer &r, VkExtent2D size) {
  uint32_t w = std::max(1u, size.width / 2), h = std::max(1u, size.height / 2);
  Buffer b(f, w * h * 8);
  f.begin();
  VkBufferImageCopy copy{
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageExtent = {w, h, 1}};
  vkCmdCopyImageToBuffer(f.cmd, r.bloomOutput().image, VK_IMAGE_LAYOUT_GENERAL,
                         b.handle, 1, &copy);
  barrier(f.cmd);
  f.end();
  std::vector<float> result(w * h);
  for (size_t i = 0; i < result.size(); ++i)
    result[i] = half(static_cast<uint16_t *>(b.data)[i * 4]);
  return result;
}
int main() {
  Fixture f;
  {
    const VkExtent2D size{32, 24};
    auto r = f.renderer(size, false, {}, true, MeterMode::SceneRadiance);
    require(!r->materialGuide() && !r->needsGuide(), "Radiance metering allocates no material guide");
    Texture hdr(f, size), reflected(f, size);
    TraceOptions options;
    options.bloom = false;
    options.autoExposureTarget = .18f;
    options.autoExposureCentre = 0;
    options.autoExposureMinEv = -8;
    options.autoExposureMaxEv = 4;
    uint64_t epoch = 0;
    auto run = [&](float light, float alpha, float specular, bool reset, float seconds = .1f) {
      hdr.upload(std::vector<std::array<float, 4>>(size.width * size.height, {light, light, light, alpha}));
      reflected.upload(std::vector<std::array<float, 4>>(size.width * size.height, {specular, specular, specular, 1}));
      r->selectFrame(0);
      f.begin();
      r->begin(f.cmd);
      if (reset) ++epoch;
      r->record(f.cmd, {hdr.image, hdr.view}, postfx::settings(options), {1, epoch}, true, false, seconds, 0,
                 {VK_IMAGE_LAYOUT_GENERAL, {{}, reflected.view, VK_IMAGE_LAYOUT_GENERAL}});
      f.end();
      r->selectFrame(0);
      return r->statistics();
    };
    require(std::abs(run(.18f, 1, 0, true).exposure - 1) < .0001f, "Neutral scene radiance meters to unity");
    require(std::abs(run(.09f, .5f, 0, true).exposure - 1) < .0001f, "MSAA coverage is unpremultiplied before metering");
    require(std::abs(run(.18f, 1, .18f, true).exposure - .5f) < .0001f, "Composited reflection radiance participates");
    const auto bright = run(2, 1, 0, true);
    const auto dark = run(.02f, 1, 0, false);
    const float expected = bright.exposure + (9 - bright.exposure) * adaptation(.1f, options.autoExposureSpeed);
    require(std::abs(dark.exposure - expected) < .001f && dark.exposure < dark.target,
            "Radiance adaptation is time based and approaches the new target without snapping");
    const auto empty = run(10000, 0, 0, false);
    require(std::abs(empty.exposure - dark.exposure) < .0001f && !empty.adapting,
            "An empty background retains exposure instead of metering its clear color");
    const auto resumed = run(.18f, 1, 0, false);
    require(resumed.exposure > 1 && resumed.exposure < empty.exposure,
            "Returning from an empty background resumes adaptation without resetting it");
    std::printf("Radiance meter: neutral/coverage/reflections, adaptation %.6f -> %.6f, memory %llu bytes\n",
                bright.exposure, dark.exposure, (unsigned long long)r->statistics().memoryBytes);
  }
  for (VkExtent2D size : {VkExtent2D{1, 1}, VkExtent2D{3, 7},
                          VkExtent2D{65, 49}, VkExtent2D{128, 128}}) {
    auto r = f.renderer(size);
    Texture hdr(f, size);
    TraceOptions o;
    o.autoExposureMinEv = -2;
    o.autoExposureMaxEv = 2; // This analytic fixture expects the explicit 4x night cap.
    std::vector<std::array<float, 4>> pixels(size.width * size.height);
    uint64_t epoch = 0;
    auto run = [&](float albedo, bool changed = true, bool reset = true,
                   float seconds = 1.f / 60) {
      r->selectFrame(0);
      f.begin();
      r->begin(f.cmd);
      const VkClearColorValue color{{albedo, albedo, albedo, 1}};
      const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                          1};
      vkCmdClearColorImage(f.cmd, r->materialInput().image,
                           VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
      barrier(f.cmd);
      if (reset)
        ++epoch;
      r->record(f.cmd, {hdr.image, hdr.view}, postfx::settings(o), {1, epoch}, changed, false,
                seconds);
      f.end();
      r->selectFrame(0);
      return r->statistics();
    };
    for (float albedo : {.0625f, .25f, 1.f}) {
      std::fill(pixels.begin(), pixels.end(),
                std::array<float, 4>{albedo, albedo, albedo, 1});
      hdr.upload(pixels);
      auto s = run(albedo);
      require(std::abs(s.exposure - .2f / .18f) < .001f,
              "GPU material compensation");
    }
    for (unsigned channel = 0; channel < 3; ++channel) {
      std::fill(pixels.begin(), pixels.end(), std::array<float, 4>{0, 0, 0, 1});
      for (auto &p : pixels)
        p[channel] = 3;
      hdr.upload(pixels);
      require(std::abs(run(1).exposure - .2f / .18f) < .001f,
              "GPU equal RGB metering");
    }
    std::fill(pixels.begin(), pixels.end(), std::array<float, 4>{NAN, 0, 0, 1});
    hdr.upload(pixels);
    require(std::abs(run(1).exposure - .2f / .18f) < .001f,
            "GPU invalid frame retention");
    std::fill(pixels.begin(), pixels.end(), std::array<float, 4>{0, 0, 0, 1});
    hdr.upload(pixels);
    require(run(1).exposure == 4, "GPU night cap");
    std::fill(pixels.begin(), pixels.end(), std::array<float, 4>{1, 1, 1, 1});
    hdr.upload(pixels);
    for (unsigned i = 0; i < 120; ++i)
      run(1, false, false);
    auto s = r->statistics();
    float expected = .2f / .18f + (4 - .2f / .18f) * std::exp(-4.f);
    require(std::abs(s.exposure - expected) < .001f,
            "GPU cached-frame convergence");
    if (size.width > 4) {
      o.autoExposureCentre = .75f;
      std::fill(pixels.begin(), pixels.end(),
                std::array<float, 4>{.1, .1, .1, 1});
      pixels[pixels.size() / 2] = {10000, 10000, 10000, 1};
      hdr.upload(pixels);
      Meter meter;
      for (unsigned y = 0; y < size.height; ++y)
        for (unsigned x = 0; x < size.width; ++x) {
          auto p = pixels[y * size.width + x];
          meter.sample({p[0], p[1], p[2]}, {1, 1, 1}, x, y,
                       {size.width, size.height}, o.autoExposureCentre);
        }
      Exposure reference;
      reference.resolve(meter, .2, -16, 2, 1);
      require(std::abs(run(1).exposure - reference.current) < .002f,
              "GPU geometric mean/center weight");
      std::fill(pixels.begin(), pixels.end(), std::array<float, 4>{0, 0, 0, 1});
      pixels[(size.height / 2) * size.width + size.width / 2] = {100, 100, 100,
                                                                 1};
      hdr.upload(pixels);
      run(1);
      auto impulse = readBloom(f, *r, size);
      double energy = 0;
      for (float value : impulse) {
        require(std::isfinite(value) && value >= 0, "Invalid bloom impulse");
        energy += value;
      }
      require(energy > 0 &&
                  *std::max_element(impulse.begin(), impulse.end()) < 100,
              "Bloom did not spread impulse");
      auto centroid = [&](const std::vector<float> &image) {
        double weighted = 0, total = 0;
        const unsigned width = std::max(size.width / 2, 1u);
        for (size_t i = 0; i < image.size(); ++i) {
          weighted += (i % width) * image[i];
          total += image[i];
        }
        return weighted / total;
      };
      pixels[(size.height / 2) * size.width + size.width / 2] = {0, 0, 0, 1};
      pixels[(size.height / 2) * size.width + 3 * size.width / 4] = {100, 100, 100, 1};
      hdr.upload(pixels);
      run(1);
      auto moved = readBloom(f, *r, size);
      require(centroid(moved) > centroid(impulse) + size.width / 32.,
              "Bloom did not follow a moving highlight");
      std::fill(pixels.begin(), pixels.end(), std::array<float, 4>{0, 0, 0, 1});
      pixels[(size.height / 2) * size.width] = {100, 100, 100, 1};
      hdr.upload(pixels);
      run(1);
      auto edgeImpulse = readBloom(f, *r, size);
      double edgeEnergy = 0;
      for (float v : edgeImpulse) edgeEnergy += v;
      require(edgeEnergy > 0 && edgeEnergy < energy && centroid(edgeImpulse) < centroid(impulse),
              "Screen-edge bloom failed black-border energy loss");
      const auto bloomFrames = r->statistics().bloomFrames;
      run(1, false, false);
      require(r->statistics().bloomFrames == bloomFrames,
              "Cached bloom recomputed");
      o.bloomRadius = .5;
      run(1, false, false);
      require(r->statistics().bloomFrames == bloomFrames + 1,
              "Spread edit did not invalidate bloom");
      std::fill(pixels.begin(), pixels.end(), std::array<float, 4>{1, 1, 1, 1});
      hdr.upload(pixels);
      run(1);
      auto flat = readBloom(f, *r, size);
      require(flat.front() < flat[flat.size() / 2 + size.width / 4],
              "Black border downsampling missing");
    }
    const float frozen = r->statistics().exposure;
    r->freeze();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    o.autoExposureTarget = .6f;
    run(1, false, false, -1);
    require(std::abs(r->statistics().exposure - frozen) < .001f,
            "Loading time leaked into resumed exposure adaptation");
    auto shared = r->state();
    float before = r->statistics().exposure;
    auto replacement = f.renderer(size, true, shared);
    replacement->selectFrame(0);
    f.begin();
    replacement->begin(f.cmd);
    const VkClearColorValue white{{1, 1, 1, 1}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(f.cmd, replacement->materialInput().image,
                         VK_IMAGE_LAYOUT_GENERAL, &white, 1, &range);
    barrier(f.cmd);
    o.autoExposureTarget = .1;
    replacement->record(f.cmd, {hdr.image, hdr.view}, postfx::settings(o), {1, epoch}, true,
                        false, 0);
    f.end();
    replacement->selectFrame(0);
    require(std::abs(replacement->statistics().exposure - before) < .001f,
            "Replacement discarded GPU exposure state");
    auto fallback = f.renderer({1, 1}, false, shared, false);
    fallback->selectFrame(0);
    f.begin();
    fallback->begin(f.cmd);
    fallback->record(f.cmd, {hdr.image, hdr.view}, postfx::settings(o), {1, ++epoch}, false, false, 0);
    f.end();
    fallback->selectFrame(0);
    require(std::abs(fallback->statistics().exposure - before) < .001f,
            "Resize fallback discarded shared exposure");
    replacement->selectFrame(0);
    f.begin();
    replacement->record(f.cmd, {hdr.image, hdr.view}, postfx::settings(o), {1, epoch}, false, false, 0);
    f.end();
    replacement->selectFrame(0);
    require(std::abs(replacement->statistics().exposure - replacement->statistics().target) < .001f,
            "Resize fallback consumed camera reset before a guide was ready");
    std::printf("%ux%u passed; %.3f ms; %llu bytes\n", size.width, size.height,
                r->statistics().milliseconds,
                (unsigned long long)r->statistics().memoryBytes);
  }
  double footprint[2]{};
  unsigned resolution = 0;
  for (VkExtent2D size : {VkExtent2D{1920, 1080}, VkExtent2D{3840, 2160}}) {
    auto r = f.renderer(size);
    Texture hdr(f, size);
    TraceOptions o;
    std::vector<std::array<float, 4>> pixels(size.width * size.height,
                                             {0, 0, 0, 1});
    pixels[(size.height / 2) * size.width + size.width / 2] = {16384, 16384,
                                                               16384, 1};
    hdr.upload(pixels);
    auto run = [&](bool changed) {
      r->selectFrame(0);
      f.begin();
      r->begin(f.cmd);
      const VkClearColorValue white{{1, 1, 1, 1}};
      const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                          1};
      vkCmdClearColorImage(f.cmd, r->materialInput().image,
                           VK_IMAGE_LAYOUT_GENERAL, &white, 1, &range);
      barrier(f.cmd);
      r->record(f.cmd, {hdr.image, hdr.view}, postfx::settings(o), {2, 0}, changed, false,
                1.f / 60);
      f.end();
      r->selectFrame(0);
    };
    run(true);
    auto result = readBloom(f, *r, size);
    double energy = 0, moment = 0;
    unsigned w = size.width / 2, h = size.height / 2;
    for (unsigned y = 0; y < h; ++y)
      for (unsigned x = 0; x < w; ++x) {
        double value = result[y * w + x];
        double dx = (x + .5 - w * .5) / h, dy = (y + .5 - h * .5) / h;
        energy += value;
        moment += value * (dx * dx + dy * dy);
      }
    footprint[resolution++] = std::sqrt(moment / energy);
    double enabled = 0, disabled = 0, cached = 0;
    for (unsigned i = 0; i < 30; ++i) {
      run(true);
      enabled += r->statistics().milliseconds;
    }
    for (unsigned i = 0; i < 30; ++i) {
      run(false);
      cached += r->statistics().milliseconds;
    }
    o.autoExposure = o.bloom = false;
    for (unsigned i = 0; i < 30; ++i) {
      run(false);
      disabled += r->statistics().milliseconds;
    }
    std::printf("BENCH %ux%u enabled=%.4fms cached=%.4fms disabled=%.4fms "
                "memory=%llu footprint=%.6f\n",
                size.width, size.height, enabled / 30, cached / 30,
                disabled / 30, (unsigned long long)r->statistics().memoryBytes,
                footprint[resolution - 1]);
  }
  require(std::abs(footprint[0] / footprint[1] - 1) < .2,
          "Bloom normalized footprint changed with resolution");
  std::puts("Deterministic GPU post-processing tests passed");
}
