#include "runtime/pipelines.h"
#include "runtime/pipeline_cache.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <stdexcept>
#include <vector>

using namespace remix::pipeline;
using namespace std::chrono_literals;
namespace {
  void require(bool value, const char *message) {
    if (!value)
      throw std::runtime_error(message);
  }
  std::atomic<uint64_t> handles{1}, compilations{0}, destroyedPipelines{0}, destroyedObjects{0};
  std::mutex compilerMutex;
  std::condition_variable compilerWake;
  bool blocked = false, entered = false;
  bool failCompile = false;
  VkDevice fakeDevice = reinterpret_cast<VkDevice>(uintptr_t(1));
  void blockCompiler() {
    std::lock_guard lock(compilerMutex);
    blocked = true;
    entered = false;
  }
  void releaseCompiler() {
    std::lock_guard lock(compilerMutex);
    blocked = false;
    compilerWake.notify_all();
  }
  void waitCompiler() {
    std::unique_lock lock(compilerMutex);
    require(compilerWake.wait_for(lock, 5s, [] { return entered; }), "compiler did not start");
  }
  void await(const PreparationQueue::Ticket &ticket) {
    const auto limit = std::chrono::steady_clock::now() + 5s;
    while (!ticket->ready() && std::chrono::steady_clock::now() < limit)
      std::this_thread::yield();
    require(ticket->ready(), "preparation timeout");
    ticket->check();
  }
  VkResult compile(const void *next, VkPipeline *out) {
    {
      std::unique_lock lock(compilerMutex);
      entered = true;
      compilerWake.notify_all();
      compilerWake.wait(lock, [] { return !blocked; });
    }
    ++compilations;
    *out = reinterpret_cast<VkPipeline>(++handles);
    if (next) {
      auto *feedback = static_cast<const VkPipelineCreationFeedbackCreateInfo *>(next);
      *feedback->pPipelineCreationFeedback = {VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT |
                                                  VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT,
                                              123};
    }
    return failCompile ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
  }
} // namespace
// Inject a CPU-only Vulkan implementation at the service's driver boundary.
// This exercises real key construction, immutable ownership and cache requests.
#define OBJECT(Name, Handle)                                                                                           \
  extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreate##Name(VkDevice, const Vk##Name##CreateInfo *,                     \
                                                           const VkAllocationCallbacks *, Handle *out) {               \
    *out = reinterpret_cast<Handle>(++handles);                                                                        \
    return VK_SUCCESS;                                                                                                 \
  }                                                                                                                    \
  extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroy##Name(VkDevice, Handle, const VkAllocationCallbacks *) {             \
    ++destroyedObjects;                                                                                                \
  }
OBJECT(ShaderModule, VkShaderModule)
OBJECT(Sampler, VkSampler)
OBJECT(DescriptorSetLayout, VkDescriptorSetLayout)
OBJECT(PipelineLayout, VkPipelineLayout)
OBJECT(RenderPass, VkRenderPass)
OBJECT(PipelineCache, VkPipelineCache)
#undef OBJECT
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineCacheData(VkDevice, VkPipelineCache, size_t *size, void *out) {
  if (!out) {
    *size = 32;
    return VK_SUCCESS;
  }
  uint32_t header[8]{32, 1, 7, 9};
  std::memcpy(out, header, 32);
  *size = 32;
  return VK_SUCCESS;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice, VkPipelineCache, uint32_t count,
                                                                   const VkComputePipelineCreateInfo *info,
                                                                   const VkAllocationCallbacks *, VkPipeline *out) {
  require(count == 1, "compiler batches must be isolated");
  require(std::string(info->stage.pName) == "main", "entry point lifetime");
  return compile(info->pNext, out);
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t,
                                                                    const VkGraphicsPipelineCreateInfo *info,
                                                                    const VkAllocationCallbacks *, VkPipeline *out) {
  return compile(info->pNext, out);
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice, VkPipeline, const VkAllocationCallbacks *) {
  ++destroyedPipelines;
}

int main(int argc, char **argv) {
  try {
    require(argc == 2, "supply isolated cache directory");
#ifdef _WIN32
    _putenv_s("REMIX_CONFIG_DIR", argv[1]);
#else
    setenv("REMIX_CONFIG_DIR", argv[1], 1);
#endif
    {
      PreparationQueue queue;
      LatestConfiguration selection;
      const auto initial = selection.request("A");
      require(selection.request("A") == initial, "identical requests retain revision");
      const auto second = selection.request("B");
      require(!selection.accepts(initial) && selection.accepts(second), "obsolete completion cannot activate");
      const auto latestRevision = selection.request("A");
      require(latestRevision != initial && !selection.accepts(second) && !selection.accepts(initial),
              "rapid A-B-A edits reject both obsolete revisions");
      std::promise<void> entered, release;
      auto gate = release.get_future().share();
      int count = 0;
      auto first = queue.request("same", [&] {
        entered.set_value();
        gate.wait();
        ++count;
      });
      entered.get_future().wait();
      require(queue.request("same", [] { throw 1; }) == first, "pending deduplication");
      require(!first->ready(), "delayed completion");
      auto obsolete = queue.request("obsolete", [] { throw std::runtime_error("obsolete executed"); });
      queue.cancel(obsolete);
      auto latest = queue.request("latest", [&] { ++count; });
      release.set_value();
      await(first);
      await(latest);
      require(count == 2 && obsolete->state == PreparationQueue::State::Cancelled, "rapid edits coalesce queued work");
      auto failure = queue.request("failure", [] { throw std::runtime_error("injected"); });
      bool failed = false;
      try {
        await(failure);
      } catch (const std::runtime_error &) {
        failed = true;
      }
      require(failed, "failure publication");
      auto nested = queue.request("parent", [&] {
        auto child = queue.request("child", [&] { ++count; });
        queue.finishOnWorker(child);
      });
      await(nested);
      require(count == 3, "inline dependency must not deadlock or run twice");
    }
    {
      // Additional workers run Parallel jobs concurrently. A Serial job still
      // starts after every earlier job, and a worker awaits a job another
      // worker runs rather than failing it.
      PreparationQueue queue(3);
      std::mutex mutex;
      std::condition_variable changed;
      int started = 0;
      bool overlapped = true;
      auto both = [&] {
        std::unique_lock lock(mutex);
        ++started;
        changed.notify_all();
        overlapped &= changed.wait_for(lock, 5s, [&] { return started >= 2; });
      };
      auto a = queue.request("parallel-a", both, PreparationQueue::Kind::Parallel);
      auto b = queue.request("parallel-b", both, PreparationQueue::Kind::Parallel);
      bool followed = false;
      auto barrier = queue.request("serial-barrier", [&] { followed = a->ready() && b->ready(); });
      await(barrier);
      require(overlapped && followed, "parallel jobs overlap and a serial job follows them");
      std::promise<void> release, slowStarted;
      auto gate = release.get_future().share();
      std::promise<PreparationQueue::Ticket> requested;
      bool awaited = false;
      auto parent = queue.request("waiter", [&] {
        auto slow = queue.request(
            "slow",
            [&] {
              slowStarted.set_value();
              gate.wait();
            },
            PreparationQueue::Kind::Parallel);
        requested.set_value(slow);
        slowStarted.get_future().wait(); // A helper runs it.
        queue.finishOnWorker(slow);
        awaited = slow->ready();
      });
      auto slow = requested.get_future().get();
      std::this_thread::sleep_for(20ms);
      require(!parent->ready() && !slow->ready(), "worker waits for a job running elsewhere");
      release.set_value();
      await(parent);
      require(awaited, "awaited job completed before the waiter continued");
    }
    {
      const auto path = std::filesystem::path(argv[1]) / "cache-test.bin";
      CacheIdentity id{7, 9, 11};
      uint32_t header[8]{32, 1, 7, 9};
      std::vector<uint8_t> data(64);
      std::memcpy(data.data(), header, sizeof(header));
      require(writeCache(path, id, data), "cache save");
      require(readCache(path, id) == data, "cache round trip");
      auto different = id;
      ++different.driver;
      require(readCache(path, different).empty(), "driver identity");
      different = id;
      ++different.uuid[0];
      require(readCache(path, different).empty(), "cache UUID identity");
      {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(70);
        file.put('x');
      }
      require(readCache(path, id).empty(), "cache corruption");
      require(!writeCache(path / "unwritable.bin", id, data), "write failure is nonfatal");
      data[4] = 99;
      require(writeCache(path, id, data) && readCache(path, id).empty(),
              "Vulkan format validated independently of integrity");
    }
    VkPhysicalDeviceProperties properties{};
    properties.vendorID = 7;
    properties.deviceID = 9;
    properties.driverVersion = 11;
    {
      Service service(fakeDevice, properties, true);
      uint32_t code[]{0x07230203, 1, 2, 3};
      VkShaderModuleCreateInfo shaderInfo{
          .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(code), .pCode = code};
      VkShaderModule shader{}, sameShader{};
      require(createShaderModule(fakeDevice, &shaderInfo, nullptr, &shader) == VK_SUCCESS, "shader creation");
      require(createShaderModule(fakeDevice, &shaderInfo, nullptr, &sameShader) == VK_SUCCESS && shader == sameShader,
              "shader content identity");
      VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT};
      VkDescriptorSetLayoutCreateInfo setInfo{
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
      VkDescriptorSetLayout set{};
      require(createDescriptorSetLayout(fakeDevice, &setInfo, nullptr, &set) == VK_SUCCESS, "descriptor layout");
      VkPipelineLayoutCreateInfo layoutInfo{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &set};
      VkPipelineLayout layout{};
      require(createPipelineLayout(fakeDevice, &layoutInfo, nullptr, &layout) == VK_SUCCESS, "pipeline layout");
      uint32_t values[]{7, 8};
      VkSpecializationMapEntry entries[]{{4, 0, 4}, {2, 4, 4}};
      VkSpecializationInfo specialization{2, entries, sizeof(values), values};
      VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                       .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                 .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                 .module = shader,
                                                 .pName = "main",
                                                 .pSpecializationInfo = &specialization},
                                       .layout = layout};
      VkPipeline pipeline{}, repeated{};
      blockCompiler();
      require(createComputePipelines(fakeDevice, {}, 1, &info, nullptr, &pipeline) == VK_NOT_READY,
              "request is asynchronous");
      waitCompiler();
      // Simulate a rendering/input pump while the injected driver is blocked.
      unsigned presentations = 0, inputs = 0;
      const auto start = std::chrono::steady_clock::now();
      for (int i = 0; i < 100; ++i) {
        require(createComputePipelines(fakeDevice, {}, 1, &info, nullptr, &repeated) == VK_NOT_READY, "pending reuse");
        ++presentations;
        ++inputs;
      }
      require(presentations == 100 && inputs == 100 && std::chrono::steady_clock::now() - start < 1s,
              "long compiler call must not block scheduling or simulated "
              "presentation/input");
      releaseCompiler();
      await(service.prepare("barrier", [] {}));
      require(createComputePipelines(fakeDevice, {}, 1, &info, nullptr, &pipeline) == VK_SUCCESS, "completed pipeline");
      require(compilations == 1, "deduplicated creation count");
      std::swap(entries[0], entries[1]);
      require(createComputePipelines(fakeDevice, {}, 1, &info, nullptr, &repeated) == VK_SUCCESS &&
                  repeated == pipeline,
              "specialization identity is independent of entry order");
      auto get = [&](const char *key) {
        VkPipeline out{};
        await(service.prepare(key, [&] {
          require(createComputePipelines(fakeDevice, {}, 1, &info, nullptr, &out) == VK_SUCCESS, "worker compilation");
        }));
        return out;
      };
      ++values[0];
      require(get("specialization") != pipeline, "specialization values differ");
      --values[0];
      ++code[3];
      require(createShaderModule(fakeDevice, &shaderInfo, nullptr, &shader) == VK_SUCCESS, "new shader");
      info.stage.module = shader;
      require(get("shader") != pipeline, "shader bytes differ");
      info.stage.module = sameShader;
      binding.descriptorCount = 2;
      require(createDescriptorSetLayout(fakeDevice, &setInfo, nullptr, &set) == VK_SUCCESS, "new set");
      require(createPipelineLayout(fakeDevice, &layoutInfo, nullptr, &layout) == VK_SUCCESS, "new layout");
      info.layout = layout;
      require(get("layout") != pipeline, "descriptor layout definitions differ");
      failCompile = true;
      ++values[0];
      auto failed = service.prepare("failure", [&] {
        VkPipeline out{};
        const auto result = createComputePipelines(fakeDevice, {}, 1, &info, nullptr, &out);
        if (result != VK_SUCCESS)
          throw result;
      });
      bool sawFailure = false;
      try {
        await(failed);
      } catch (VkResult r) {
        sawFailure = r == VK_ERROR_OUT_OF_DEVICE_MEMORY;
      }
      require(sawFailure, "compiler failure propagates");
      failCompile = false;
      destroyPipeline(fakeDevice, pipeline, nullptr);
      destroyPipelineLayout(fakeDevice, layout, nullptr);
      require(destroyedPipelines == 1 && destroyedObjects == 0, "compiled pipelines/layouts survive resource teardown");
      const auto stats = service.statistics();
      require(stats.created == 4 && stats.failed == 1 && stats.cacheHits == 5 && stats.reused > 0 &&
                  stats.deduplicated >= 100,
              "creation feedback statistics");
      VkAttachmentDescription attachment{.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT};
      VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkSubpassDescription subpass{
          .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &color};
      VkRenderPassCreateInfo passInfo{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                      .attachmentCount = 1,
                                      .pAttachments = &attachment,
                                      .subpassCount = 1,
                                      .pSubpasses = &subpass};
      VkRenderPass pass{};
      require(createRenderPass(fakeDevice, &passInfo, nullptr, &pass) == VK_SUCCESS, "render pass");
      VkPipelineShaderStageCreateInfo stage{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                            .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                            .module = shader,
                                            .pName = "main"};
      VkPipelineVertexInputStateCreateInfo vertex{.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      VkPipelineInputAssemblyStateCreateInfo assembly{.sType =
                                                          VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
      VkPipelineViewportStateCreateInfo viewport{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
      VkPipelineRasterizationStateCreateInfo raster{.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                    .lineWidth = 1};
      VkPipelineMultisampleStateCreateInfo samples{.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                   .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
      VkPipelineDepthStencilStateCreateInfo depth{.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
      VkPipelineColorBlendAttachmentState blendAttachment{.colorWriteMask = 15};
      VkPipelineColorBlendStateCreateInfo blend{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                .attachmentCount = 1,
                                                .pAttachments = &blendAttachment};
      VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
      VkPipelineDynamicStateCreateInfo dynamic{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                               .dynamicStateCount = 2,
                                               .pDynamicStates = states};
      VkGraphicsPipelineCreateInfo graphics{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                            .stageCount = 1,
                                            .pStages = &stage,
                                            .pVertexInputState = &vertex,
                                            .pInputAssemblyState = &assembly,
                                            .pViewportState = &viewport,
                                            .pRasterizationState = &raster,
                                            .pMultisampleState = &samples,
                                            .pDepthStencilState = &depth,
                                            .pColorBlendState = &blend,
                                            .pDynamicState = &dynamic,
                                            .layout = layout,
                                            .renderPass = pass};
      auto graphicsPipeline = [&](const char *key) {
        VkPipeline out{};
        await(service.prepare(key, [&] {
          require(createGraphicsPipelines(fakeDevice, {}, 1, &graphics, nullptr, &out) == VK_SUCCESS,
                  "graphics creation");
        }));
        return out;
      };
      const auto firstGraphics = graphicsPipeline("graphics");
      attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      require(createRenderPass(fakeDevice, &passInfo, nullptr, &pass) == VK_SUCCESS, "compatible pass");
      graphics.renderPass = pass;
      require(graphicsPipeline("compatible-pass") == firstGraphics, "compatible render passes reuse a pipeline");
      blendAttachment.blendEnable = true;
      require(graphicsPipeline("blend") != firstGraphics, "graphics blend state differs");
      blendAttachment.blendEnable = false;
      depth.depthWriteEnable = true;
      require(graphicsPipeline("depth") != firstGraphics, "graphics depth state differs");
      depth.depthWriteEnable = false;
      attachment.format = VK_FORMAT_R16G16B16A16_SFLOAT;
      require(createRenderPass(fakeDevice, &passInfo, nullptr, &pass) == VK_SUCCESS, "different format");
      graphics.renderPass = pass;
      require(graphicsPipeline("format") != firstGraphics, "attachment format differs");
      blockCompiler();
      ++values[0];
      require(createComputePipelines(fakeDevice, {}, 1, &info, nullptr, &repeated) == VK_NOT_READY, "shutdown request");
      waitCompiler();
      bool queuedRan = false;
      auto queued = service.prepare("discard-at-shutdown", [&] { queuedRan = true; });
      auto stopped = std::async(std::launch::async, [&] { service.stop(); });
      require(stopped.wait_for(30ms) == std::future_status::timeout, "shutdown joins executing driver call");
      require(destroyedObjects == 0, "dependencies alive during driver call");
      releaseCompiler();
      stopped.get();
      require(!queuedRan && queued->state == PreparationQueue::State::Cancelled, "shutdown discards queued work");
    }
    require(destroyedPipelines == compilations && destroyedObjects > 0, "all handles destroyed after join");
    std::puts("Pipeline keys, deduplication, asynchronous preparation, "
              "failures, persistence and shutdown passed");
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  } catch (...) {
    std::fprintf(stderr, "Unexpected failure\n");
    return 1;
  }
}
