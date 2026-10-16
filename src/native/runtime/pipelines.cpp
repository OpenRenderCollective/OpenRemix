#include "runtime/pipelines.h"
#include "runtime/pipeline_cache.h"
#include "platform/utf8_path.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <thread>

namespace remix::pipeline {
namespace {
// Keys are length-delimited values. Pointer values and structure padding never
// enter a key. Nested objects contribute their definitions, not Vulkan handles.
struct Key {
  std::string bytes;
  template <class T> void add(T value) {
    static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
    bytes.append(reinterpret_cast<const char *>(&value), sizeof(value));
  }
  void blob(const void *data, size_t size) {
    add(uint64_t(size));
    if (size)
      bytes.append(static_cast<const char *>(data), size);
  }
  void add(const std::string &value) { blob(value.data(), value.size()); }
};
std::mutex registryMutex;
std::map<VkDevice, Service::Impl *> registry;
std::mutex nameMutex;
std::map<const void *, std::string> codeNames;
std::string codeName(const void *code, size_t bytes) {
  std::lock_guard lock(nameMutex);
  const auto found = codeNames.find(code);
  return found != codeNames.end() ? found->second
                                   : std::to_string(bytes) + "-byte module";
}
Service::Impl &service(VkDevice device) {
  std::lock_guard lock(registryMutex);
  return *registry.at(device);
}
void require(bool valid) {
  if (!valid)
    throw VK_ERROR_FEATURE_NOT_PRESENT;
}
struct Stage {
  VkPipelineShaderStageCreateInfo info{};
  std::string name;
  std::vector<VkSpecializationMapEntry> entries;
  std::vector<uint8_t> data;
  VkSpecializationInfo specialization{};
  VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup{
      .sType =
          VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
  bool hasSubgroup = false;
  explicit Stage(const VkPipelineShaderStageCreateInfo &source)
      : info(source), name(source.pName) {
    if (source.pNext) {
      auto *next = static_cast<
          const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *>(
          source.pNext);
      require(next->sType == subgroup.sType && !next->pNext);
      subgroup = *next;
      hasSubgroup = true;
    }
    if (source.pSpecializationInfo) {
      const auto &s = *source.pSpecializationInfo;
      if (s.mapEntryCount)
        entries.assign(s.pMapEntries, s.pMapEntries + s.mapEntryCount);
      if (s.dataSize)
        data.assign(static_cast<const uint8_t *>(s.pData),
                    static_cast<const uint8_t *>(s.pData) + s.dataSize);
      for (const auto &entry : entries)
        require(entry.offset <= data.size() &&
                entry.size <= data.size() - entry.offset);
    }
  }
  VkPipelineShaderStageCreateInfo bind() {
    specialization = {uint32_t(entries.size()), entries.data(), data.size(),
                      data.data()};
    info.pName = name.c_str();
    info.pSpecializationInfo = entries.empty() ? nullptr : &specialization;
    info.pNext = hasSubgroup ? &subgroup : nullptr;
    return info;
  }
};
struct Compute {
  VkComputePipelineCreateInfo info;
  Stage stage;
  explicit Compute(const VkComputePipelineCreateInfo &source)
      : info(source), stage(source.stage) {
    require(!source.pNext && !source.basePipelineHandle &&
            !(source.flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT));
  }
};
struct Graphics {
  VkGraphicsPipelineCreateInfo info;
  std::vector<Stage> stages;
  std::vector<VkPipelineShaderStageCreateInfo> boundStages;
  VkPipelineVertexInputStateCreateInfo vertex{};
  std::vector<VkVertexInputBindingDescription> bindings;
  std::vector<VkVertexInputAttributeDescription> attributes;
  VkPipelineInputAssemblyStateCreateInfo assembly{};
  VkPipelineViewportStateCreateInfo viewport{};
  VkPipelineRasterizationStateCreateInfo raster{};
  VkPipelineMultisampleStateCreateInfo samples{};
  std::vector<VkSampleMask> masks;
  VkPipelineDepthStencilStateCreateInfo depth{};
  VkPipelineColorBlendStateCreateInfo blend{};
  std::vector<VkPipelineColorBlendAttachmentState> attachments;
  VkPipelineDynamicStateCreateInfo dynamic{};
  std::vector<VkDynamicState> states;
  explicit Graphics(const VkGraphicsPipelineCreateInfo &s) : info(s) {
    require(!s.pNext && !s.pTessellationState && !s.basePipelineHandle &&
            !(s.flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT));
    for (uint32_t i = 0; i < s.stageCount; ++i)
      stages.emplace_back(s.pStages[i]);
    require(s.pVertexInputState && s.pInputAssemblyState && s.pViewportState &&
            s.pRasterizationState && s.pMultisampleState &&
            s.pDepthStencilState && s.pColorBlendState && s.pDynamicState);
    vertex = *s.pVertexInputState;
    assembly = *s.pInputAssemblyState;
    viewport = *s.pViewportState;
    raster = *s.pRasterizationState;
    samples = *s.pMultisampleState;
    depth = *s.pDepthStencilState;
    blend = *s.pColorBlendState;
    dynamic = *s.pDynamicState;
    require(!vertex.pNext && !assembly.pNext && !viewport.pNext &&
            !raster.pNext && !samples.pNext && !depth.pNext && !blend.pNext &&
            !dynamic.pNext);
    if (vertex.vertexBindingDescriptionCount)
      bindings.assign(vertex.pVertexBindingDescriptions,
                      vertex.pVertexBindingDescriptions +
                          vertex.vertexBindingDescriptionCount);
    if (vertex.vertexAttributeDescriptionCount)
      attributes.assign(vertex.pVertexAttributeDescriptions,
                        vertex.pVertexAttributeDescriptions +
                            vertex.vertexAttributeDescriptionCount);
    if (blend.attachmentCount)
      attachments.assign(blend.pAttachments,
                         blend.pAttachments + blend.attachmentCount);
    if (dynamic.dynamicStateCount)
      states.assign(dynamic.pDynamicStates,
                    dynamic.pDynamicStates + dynamic.dynamicStateCount);
    require(std::find(states.begin(), states.end(),
                      VK_DYNAMIC_STATE_VIEWPORT) != states.end() &&
            std::find(states.begin(), states.end(), VK_DYNAMIC_STATE_SCISSOR) !=
                states.end());
    if (samples.pSampleMask)
      masks.assign(samples.pSampleMask,
                   samples.pSampleMask +
                       (uint32_t(samples.rasterizationSamples) + 31) / 32);
  }
  void bind() {
    for (auto &stage : stages)
      boundStages.push_back(stage.bind());
    vertex.pVertexBindingDescriptions = bindings.data();
    vertex.pVertexAttributeDescriptions = attributes.data();
    viewport.pViewports = nullptr;
    viewport.pScissors = nullptr;
    samples.pSampleMask = masks.empty() ? nullptr : masks.data();
    blend.pAttachments = attachments.data();
    dynamic.pDynamicStates = states.data();
    info.pStages = boundStages.data();
    info.pVertexInputState = &vertex;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &samples;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
  }
};
} // namespace
struct Service::Impl {
  VkDevice device;
  VkPipelineCache cache{};
  CacheIdentity identity;
  std::filesystem::path path;
  mutable std::mutex mutex;
  Statistics stats;
  // Immutable objects are retained until the compiler and GPU have stopped.
  struct Object {
    uint64_t handle;
    std::function<void()> destroy;
  };
  std::map<std::string, Object> objects;
  std::map<std::pair<uint32_t, uint64_t>, std::string> definitions;
  std::map<VkRenderPass, std::string> compatiblePasses;
  struct Pipeline {
    VkPipeline handle{};
    PreparationQueue::Ticket job;
    // Set once the handle is created: the job then still saves the cache
    // (about 150 ms for 100 MiB), which a frame does not wait for.
    std::atomic<bool> compiled{false};
  };
  std::map<std::string, std::shared_ptr<Pipeline>> pipelines;
  // Dev report (guarded by mutex): compiles and saves, milliseconds since
  // the service started.
  const std::chrono::steady_clock::time_point born =
      std::chrono::steady_clock::now();
  std::map<uint64_t, std::string> moduleNames;
  struct Record {
    double queued = 0, started = 0, finished = 0;
    bool hit = false, failed = false;
    std::string name;
  };
  std::vector<Record> records;
  std::vector<std::pair<double, std::string>> events;
  double since() const {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - born)
        .count();
  }
  // A stage's module name and its non-zero specialization constants.
  std::string stageName(const Stage &stage) {
    auto name = moduleNames.count(uint64_t(stage.info.module))
                    ? moduleNames[uint64_t(stage.info.module)]
                    : std::string("module");
    std::string constants;
    for (const auto &entry : stage.entries) {
      uint64_t value = 0;
      std::memcpy(&value, stage.data.data() + entry.offset,
                  std::min<size_t>(entry.size, sizeof(value)));
      if (value)
        constants += (constants.empty() ? "" : ",") +
                     std::to_string(entry.constantID) +
                     (value == 1 ? "" : "=" + std::to_string(value));
    }
    return constants.empty() ? name : name + "[" + constants + "]";
  }
  // Cache writes are coalesced: a save covers every compile finished before it.
  std::mutex saveMutex;
  std::atomic<uint64_t> saveRequests{0};
  uint64_t savedRequest = 0; // guarded by saveMutex
  PreparationQueue queue;
  explicit Impl(VkDevice value) : device(value), queue(compilerThreads()) {}
  // Independent pipelines compile concurrently; drivers compile each one on
  // the calling thread. REMIX_PIPELINE_THREADS overrides the worker count.
  static unsigned compilerThreads() {
    if (const char *value = std::getenv("REMIX_PIPELINE_THREADS"); value && *value)
      return std::clamp(unsigned(std::strtoul(value, nullptr, 10)), 1u, 64u);
    return std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
  }
  std::string definition(uint32_t type, uint64_t handle) {
    return definitions.at({type, handle}); // caller holds mutex
  }
  template <class Handle, class Create, class Destroy, class Info>
  VkResult intern(uint32_t type, Key key, const Info *info, Handle *out,
                  Create create, Destroy destroy) {
    std::lock_guard lock(mutex);
    key.add(type);
    if (auto found = objects.find(key.bytes); found != objects.end()) {
      *out = Handle(found->second.handle);
      return VK_SUCCESS;
    }
    auto result = create(device, info, nullptr, out);
    if (result != VK_SUCCESS)
      return result;
    const auto value = *out;
    try {
      definitions.emplace(std::make_pair(type, uint64_t(value)), key.bytes);
      objects.emplace(std::move(key.bytes),
                      Object{uint64_t(value),
                             [=, this] { destroy(device, value, nullptr); }});
    } catch (...) {
      definitions.erase({type, uint64_t(value)});
      destroy(device, value, nullptr);
      *out = {};
      throw;
    }
    return VK_SUCCESS;
  }
  void stageKey(Key &key, const Stage &stage) {
    key.add(stage.info.flags);
    key.add(stage.info.stage);
    key.add(
        definition(VK_OBJECT_TYPE_SHADER_MODULE, uint64_t(stage.info.module)));
    key.add(stage.name);
    key.add(stage.hasSubgroup);
    if (stage.hasSubgroup)
      key.add(stage.subgroup.requiredSubgroupSize);
    auto entries = stage.entries;
    std::sort(entries.begin(), entries.end(),
              [](auto a, auto b) { return a.constantID < b.constantID; });
    key.add(uint32_t(entries.size()));
    for (auto entry : entries) {
      key.add(entry.constantID);
      key.blob(stage.data.data() + entry.offset, entry.size);
    }
  }
  // Schedules a compile (or finds the existing one) without waiting, so a
  // batch of pipelines is queued before any of it is awaited.
  std::shared_ptr<Pipeline>
  enqueue(std::string key, std::string name,
          std::function<VkResult(VkPipeline *,
                                 VkPipelineCreationFeedbackCreateInfo *)>
              compile,
          PreparationQueue::Kind kind = PreparationQueue::Kind::Parallel) {
    queue.checkCancelled();
    std::shared_ptr<Pipeline> result;
    {
      std::lock_guard lock(mutex);
      auto found = pipelines.find(key);
      if (found != pipelines.end()) {
        result = found->second;
        if (result->compiled)
          ++stats.reused;
        else
          ++stats.deduplicated;
        // Wanted now: a prewarmed compile joins the ordinary ones.
        if (kind != PreparationQueue::Kind::Background)
          queue.promote(result->job);
      } else {
        result = std::make_shared<Pipeline>();
        ++stats.requested;
        const size_t record = records.size();
        records.push_back({.queued = since(), .name = std::move(name)});
        result->job = queue.request(key, [this, result, record,
                                          compile = std::move(compile)] {
          const auto start = std::chrono::steady_clock::now();
          {
            std::lock_guard lock(mutex);
            records[record].started = since();
          }
          VkPipelineCreationFeedback feedback{};
          VkPipelineCreationFeedbackCreateInfo info{
              .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
              .pPipelineCreationFeedback = &feedback};
          const auto status =
              compile(&result->handle, stats.feedback ? &info : nullptr);
          {
            std::lock_guard lock(mutex);
            ++stats.completed;
            stats.milliseconds += std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - start)
                                      .count();
            // With the count, so a caller that sees it completed can use it.
            if (status == VK_SUCCESS) {
              ++stats.created;
              result->compiled = true;
            } else
              ++stats.failed;
            auto &entry = records[record];
            entry.finished = since();
            entry.failed = status != VK_SUCCESS;
            if (feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) {
              stats.driverNanoseconds += feedback.duration;
              entry.hit = feedback.flags &
                          VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT;
              stats.cacheHits += entry.hit;
            }
          }
          if (status != VK_SUCCESS) {
            if (result->handle)
              vkDestroyPipeline(device, result->handle, nullptr);
            result->handle = {};
            throw status;
          }
          save();
        }, kind);
        pipelines.emplace(std::move(key), result);
      }
    }
    return result;
  }
  // Workers compile a missing pipeline inline or wait for another worker's
  // compile of it; other threads get VK_NOT_READY until it is ready.
  VkResult resolve(const std::shared_ptr<Pipeline> &result, VkPipeline *out) {
    *out = {};
    if (queue.onWorker())
      queue.finishOnWorker(result->job);
    if (result->compiled) {
      *out = result->handle;
      return VK_SUCCESS;
    }
    if (!result->job->ready())
      return VK_NOT_READY;
    try {
      result->job->check();
    } catch (VkResult error) {
      return error;
    }
    *out = result->handle;
    return VK_SUCCESS;
  }
  void save() noexcept {
    if (!cache)
      return;
    const uint64_t request = ++saveRequests;
    std::lock_guard saving(saveMutex);
    if (savedRequest >= request)
      return; // A later save already included this compile.
    savedRequest = saveRequests.load();
    const double started = since();
    try {
      size_t size = 0;
      if (vkGetPipelineCacheData(device, cache, &size, nullptr) != VK_SUCCESS ||
          size > 256ull * 1024 * 1024)
        return;
      std::vector<uint8_t> bytes(size);
      if (vkGetPipelineCacheData(device, cache, &size, bytes.data()) !=
          VK_SUCCESS)
        return;
      bytes.resize(size);
      const bool ok = writeCache(path, identity, bytes);
      std::lock_guard lock(mutex);
      stats.cacheWriteFailed = !ok;
      char line[160];
      std::snprintf(line, sizeof(line),
                    "cache save of %.1f MiB took %.0f ms%s", size / 1048576.0,
                    since() - started, ok ? "" : " (write failed)");
      events.emplace_back(started, line);
    } catch (...) {
      std::lock_guard lock(mutex);
      stats.cacheWriteFailed = true;
    }
  }
};
Service::Service(VkDevice device, const VkPhysicalDeviceProperties &p,
                 bool feedback)
    : impl(std::make_unique<Impl>(device)) {
  auto &g = *impl;
  g.stats.feedback = feedback;
  g.identity = {p.vendorID, p.deviceID, p.driverVersion};
  std::copy_n(p.pipelineCacheUUID, 16, g.identity.uuid.begin());
  std::error_code directoryError;
  const auto workingDirectory = std::filesystem::current_path(directoryError);
  const char *directory = std::getenv("REMIX_CONFIG_DIR");
  const auto configDirectory =
      directory && *directory
          ? utf8Path(directory)
          : (directoryError ? std::filesystem::path(".") : workingDirectory) /
                "openremix";
  g.path = configDirectory / "cache" / "vulkan" /
           (std::to_string(p.vendorID) + "-" + std::to_string(p.deviceID) +
            "-" + std::to_string(p.driverVersion) + ".bin");
  auto data = readCache(g.path, g.identity);
  VkPipelineCacheCreateInfo ci{.sType =
                                   VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                               .initialDataSize = data.size(),
                               .pInitialData = data.data()};
  auto status = vkCreatePipelineCache(device, &ci, nullptr, &g.cache);
  g.stats.cacheLoaded = status == VK_SUCCESS && !data.empty();
  if (status != VK_SUCCESS) {
    ci.initialDataSize = 0;
    ci.pInitialData = nullptr;
    vkCreatePipelineCache(device, &ci, nullptr, &g.cache);
  }
  std::lock_guard lock(registryMutex);
  registry.emplace(device, &g);
}
Service::~Service() {
  stop();
  auto &g = *impl;
  g.save();
  for (auto &[key, p] : g.pipelines)
    if (p->handle)
      vkDestroyPipeline(g.device, p->handle, nullptr);
  // Layouts referencing immutable samplers are destroyed before the samplers.
  for (auto type :
       {VK_OBJECT_TYPE_PIPELINE_LAYOUT, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
        VK_OBJECT_TYPE_SHADER_MODULE, VK_OBJECT_TYPE_RENDER_PASS,
        VK_OBJECT_TYPE_SAMPLER})
    for (auto &[id, key] : g.definitions)
      if (id.first == uint32_t(type))
        g.objects.at(key).destroy();
  if (g.cache)
    vkDestroyPipelineCache(g.device, g.cache, nullptr);
  std::lock_guard lock(registryMutex);
  registry.erase(g.device);
}
void Service::stop() { impl->queue.stop(); }
void Service::cancel(const PreparationQueue::Ticket &job) {
  impl->queue.cancel(job);
}
PreparationQueue::Ticket Service::prepare(const std::string &key,
                                          std::function<void()> work) {
  return impl->queue.request("configuration:" + key, std::move(work));
}
Statistics Service::statistics() const {
  std::lock_guard lock(impl->mutex);
  return impl->stats;
}
void Service::note(const std::string &event) {
  std::lock_guard lock(impl->mutex);
  impl->events.emplace_back(impl->since(), event);
}
std::string Service::report() const {
  auto &g = *impl;
  std::lock_guard lock(g.mutex);
  const auto &s = g.stats;
  char line[512];
  std::snprintf(line, sizeof(line),
                "# pipelines requested %llu, created %llu, failed %llu, reused "
                "%llu, deduplicated %llu, cache hits %llu; compile time %.0f ms "
                "summed over %u workers; cache %s\n",
                (unsigned long long)s.requested, (unsigned long long)s.created,
                (unsigned long long)s.failed, (unsigned long long)s.reused,
                (unsigned long long)s.deduplicated,
                (unsigned long long)s.cacheHits, s.milliseconds,
                Impl::compilerThreads(),
                s.cacheLoaded ? "loaded" : "not loaded");
  std::string text = line;
  text += "# queued started finished compile(ms) hit name, and events "
          "(milliseconds since the service started)\n";
  // Compiles and events merged in time order.
  std::vector<std::pair<double, std::string>> lines = g.events;
  for (const auto &r : g.records) {
    if (r.finished)
      std::snprintf(line, sizeof(line), "%9.0f %9.0f %9.0f %8.0f %s %s%s",
                    r.queued, r.started, r.finished, r.finished - r.started,
                    r.hit ? "hit " : "miss", r.name.c_str(),
                    r.failed ? " FAILED" : "");
    else
      std::snprintf(line, sizeof(line), "%9.0f %9s %9s %8s %s %s", r.queued,
                    r.started ? "running" : "queued", "", "", "    ",
                    r.name.c_str());
    lines.emplace_back(r.queued, line);
  }
  std::stable_sort(lines.begin(), lines.end(),
                   [](auto &a, auto &b) { return a.first < b.first; });
  for (const auto &[time, entry] : lines) {
    if (entry.empty() || entry[0] != ' ') {
      std::snprintf(line, sizeof(line), "%9.0f # ", time);
      text += line;
    }
    text += entry + "\n";
  }
  return text;
}
void nameCode(const void *code, const char *name) {
  std::lock_guard lock(nameMutex);
  codeNames[code] = name;
}

VKAPI_ATTR VkResult VKAPI_CALL
createShaderModule(VkDevice d, const VkShaderModuleCreateInfo *i,
                   const VkAllocationCallbacks *a, VkShaderModule *out) {
  require(!a && !i->pNext);
  Key k;
  k.add(i->flags);
  k.blob(i->pCode, i->codeSize);
  auto &g = service(d);
  const auto result =
      g.intern(VK_OBJECT_TYPE_SHADER_MODULE, std::move(k), i, out,
               vkCreateShaderModule, vkDestroyShaderModule);
  if (result == VK_SUCCESS) {
    auto name = codeName(i->pCode, i->codeSize);
    std::lock_guard lock(g.mutex);
    g.moduleNames.emplace(uint64_t(*out), std::move(name));
  }
  return result;
}
VKAPI_ATTR VkResult VKAPI_CALL createSampler(VkDevice d,
                                             const VkSamplerCreateInfo *i,
                                             const VkAllocationCallbacks *a,
                                             VkSampler *out) {
  require(!a && !i->pNext);
  Key k;
  k.add(i->flags);
  k.add(i->magFilter);
  k.add(i->minFilter);
  k.add(i->mipmapMode);
  k.add(i->addressModeU);
  k.add(i->addressModeV);
  k.add(i->addressModeW);
  k.add(i->mipLodBias);
  k.add(i->anisotropyEnable);
  k.add(i->maxAnisotropy);
  k.add(i->compareEnable);
  k.add(i->compareOp);
  k.add(i->minLod);
  k.add(i->maxLod);
  k.add(i->borderColor);
  k.add(i->unnormalizedCoordinates);
  return service(d).intern(VK_OBJECT_TYPE_SAMPLER, std::move(k), i, out,
                           vkCreateSampler, vkDestroySampler);
}
VKAPI_ATTR VkResult VKAPI_CALL createDescriptorSetLayout(
    VkDevice d, const VkDescriptorSetLayoutCreateInfo *i,
    const VkAllocationCallbacks *a, VkDescriptorSetLayout *out) {
  require(!a && !i->pNext);
  auto &g = service(d);
  Key k;
  k.add(i->flags);
  k.add(i->bindingCount);
  std::vector<VkDescriptorSetLayoutBinding> bindings;
  if (i->bindingCount)
    bindings.assign(i->pBindings, i->pBindings + i->bindingCount);
  std::sort(bindings.begin(), bindings.end(),
            [](auto a, auto b) { return a.binding < b.binding; });
  {
    std::lock_guard lock(g.mutex);
    for (const auto &b : bindings) {
      k.add(b.binding);
      k.add(b.descriptorType);
      k.add(b.descriptorCount);
      k.add(b.stageFlags);
      k.add(bool(b.pImmutableSamplers));
      if (b.pImmutableSamplers)
        for (uint32_t n = 0; n < b.descriptorCount; ++n)
          k.add(g.definition(VK_OBJECT_TYPE_SAMPLER,
                             uint64_t(b.pImmutableSamplers[n])));
    }
  }
  return g.intern(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, std::move(k), i, out,
                  vkCreateDescriptorSetLayout, vkDestroyDescriptorSetLayout);
}
VKAPI_ATTR VkResult VKAPI_CALL
createPipelineLayout(VkDevice d, const VkPipelineLayoutCreateInfo *i,
                     const VkAllocationCallbacks *a, VkPipelineLayout *out) {
  require(!a && !i->pNext);
  auto &g = service(d);
  Key k;
  k.add(i->flags);
  k.add(i->setLayoutCount);
  {
    std::lock_guard lock(g.mutex);
    for (uint32_t n = 0; n < i->setLayoutCount; ++n)
      k.add(g.definition(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
                         uint64_t(i->pSetLayouts[n])));
  }
  k.add(i->pushConstantRangeCount);
  for (uint32_t n = 0; n < i->pushConstantRangeCount; ++n) {
    const auto &r = i->pPushConstantRanges[n];
    k.add(r.stageFlags);
    k.add(r.offset);
    k.add(r.size);
  }
  return g.intern(VK_OBJECT_TYPE_PIPELINE_LAYOUT, std::move(k), i, out,
                  vkCreatePipelineLayout, vkDestroyPipelineLayout);
}
VKAPI_ATTR VkResult VKAPI_CALL createRenderPass(VkDevice d,
                                                const VkRenderPassCreateInfo *i,
                                                const VkAllocationCallbacks *a,
                                                VkRenderPass *out) {
  require(!a && !i->pNext);
  Key k;
  k.add(i->flags);
  k.add(i->attachmentCount);
  for (uint32_t n = 0; n < i->attachmentCount; ++n) {
    const auto &r = i->pAttachments[n];
    k.add(r.flags);
    k.add(r.format);
    k.add(r.samples);
    k.add(r.loadOp);
    k.add(r.storeOp);
    k.add(r.stencilLoadOp);
    k.add(r.stencilStoreOp);
    k.add(r.initialLayout);
    k.add(r.finalLayout);
  }
  k.add(i->subpassCount);
  auto references = [&](const VkAttachmentReference *p, uint32_t count) {
    k.add(bool(p));
    if (p)
      for (uint32_t n = 0; n < count; ++n) {
        k.add(p[n].attachment);
        k.add(p[n].layout);
      }
  };
  for (uint32_t n = 0; n < i->subpassCount; ++n) {
    const auto &r = i->pSubpasses[n];
    k.add(r.flags);
    k.add(r.pipelineBindPoint);
    k.add(r.inputAttachmentCount);
    k.add(r.colorAttachmentCount);
    references(r.pInputAttachments, r.inputAttachmentCount);
    references(r.pColorAttachments, r.colorAttachmentCount);
    references(r.pResolveAttachments, r.colorAttachmentCount);
    references(r.pDepthStencilAttachment, 1);
    k.add(r.preserveAttachmentCount);
    for (uint32_t j = 0; j < r.preserveAttachmentCount; ++j)
      k.add(r.pPreserveAttachments[j]);
  }
  k.add(i->dependencyCount);
  for (uint32_t n = 0; n < i->dependencyCount; ++n) {
    const auto &r = i->pDependencies[n];
    k.add(r.srcSubpass);
    k.add(r.dstSubpass);
    k.add(r.srcStageMask);
    k.add(r.dstStageMask);
    k.add(r.srcAccessMask);
    k.add(r.dstAccessMask);
    k.add(r.dependencyFlags);
  }
  auto &g = service(d);
  const auto result = g.intern(VK_OBJECT_TYPE_RENDER_PASS, std::move(k), i, out,
                               vkCreateRenderPass, vkDestroyRenderPass);
  if (result != VK_SUCCESS)
    return result;
  // Vulkan compatibility excludes load/store ops, image layouts, dependencies
  // and preserve attachments. Capture formats/samples at each referenced slot.
  Key compatible;
  compatible.add(i->flags);
  compatible.add(i->subpassCount);
  auto reference = [&](const VkAttachmentReference *r) {
    const bool used = r && r->attachment != VK_ATTACHMENT_UNUSED;
    compatible.add(used);
    if (used) {
      require(r->attachment < i->attachmentCount);
      compatible.add(i->pAttachments[r->attachment].format);
      compatible.add(i->pAttachments[r->attachment].samples);
    }
  };
  for (uint32_t n = 0; n < i->subpassCount; ++n) {
    const auto &s = i->pSubpasses[n];
    compatible.add(s.flags);
    compatible.add(s.pipelineBindPoint);
    compatible.add(s.inputAttachmentCount);
    compatible.add(s.colorAttachmentCount);
    for (uint32_t j = 0; j < s.inputAttachmentCount; ++j)
      reference(s.pInputAttachments + j);
    for (uint32_t j = 0; j < s.colorAttachmentCount; ++j) {
      reference(s.pColorAttachments + j);
      if (i->subpassCount > 1)
        reference(s.pResolveAttachments ? s.pResolveAttachments + j : nullptr);
    }
    reference(s.pDepthStencilAttachment);
  }
  std::lock_guard lock(g.mutex);
  g.compatiblePasses[*out] = std::move(compatible.bytes);
  return result;
}
#define RETAIN(Name, Handle)                                                   \
  VKAPI_ATTR void VKAPI_CALL destroy##Name(VkDevice, Handle,                   \
                                           const VkAllocationCallbacks *) {}
RETAIN(ShaderModule, VkShaderModule)
RETAIN(Sampler, VkSampler)
RETAIN(DescriptorSetLayout, VkDescriptorSetLayout)
RETAIN(PipelineLayout, VkPipelineLayout)
RETAIN(RenderPass, VkRenderPass)
RETAIN(Pipeline, VkPipeline)
#undef RETAIN
namespace {
// Every pipeline of a call is queued before any is awaited: workers compile
// the rest of the batch while the caller compiles or waits for one of them.
VkResult resolveBatch(Service::Impl &g,
                      const std::vector<std::shared_ptr<Service::Impl::Pipeline>> &batch,
                      VkPipeline *out) {
  VkResult result = VK_SUCCESS;
  for (size_t n = 0; n < batch.size(); ++n)
    if (const auto r = g.resolve(batch[n], out + n); r != VK_SUCCESS)
      result = r;
  return result;
}
} // namespace
namespace {
std::shared_ptr<Service::Impl::Pipeline>
enqueueCompute(Service::Impl &g, const VkComputePipelineCreateInfo &info,
               PreparationQueue::Kind kind = PreparationQueue::Kind::Parallel) {
  auto recipe = std::make_shared<Compute>(info);
  Key k;
  k.add(uint32_t(VK_PIPELINE_BIND_POINT_COMPUTE));
  k.add(recipe->info.flags);
  {
    std::lock_guard lock(g.mutex);
    g.stageKey(k, recipe->stage);
    k.add(g.definition(VK_OBJECT_TYPE_PIPELINE_LAYOUT,
                       uint64_t(recipe->info.layout)));
  }
  std::string name;
  {
    std::lock_guard lock(g.mutex);
    name = g.stageName(recipe->stage);
  }
  return g.enqueue(
      std::move(k.bytes), std::move(name),
      [recipe, &g](VkPipeline *p,
                   VkPipelineCreationFeedbackCreateInfo *feedback) {
        recipe->info.stage = recipe->stage.bind();
        recipe->info.pNext = feedback;
        return vkCreateComputePipelines(g.device, g.cache, 1, &recipe->info,
                                        nullptr, p);
      },
      kind);
}
} // namespace
VKAPI_ATTR VkResult VKAPI_CALL
createComputePipelines(VkDevice d, VkPipelineCache, uint32_t count,
                       const VkComputePipelineCreateInfo *infos,
                       const VkAllocationCallbacks *a, VkPipeline *out) {
  require(!a);
  std::fill_n(out, count, VK_NULL_HANDLE);
  auto &g = service(d);
  std::vector<std::shared_ptr<Service::Impl::Pipeline>> batch;
  batch.reserve(count);
  for (uint32_t n = 0; n < count; ++n)
    batch.push_back(enqueueCompute(g, infos[n]));
  return resolveBatch(g, batch, out);
}
VKAPI_ATTR void VKAPI_CALL
prefetchComputePipelines(VkDevice d, uint32_t count,
                         const VkComputePipelineCreateInfo *infos, bool background) {
  auto &g = service(d);
  for (uint32_t n = 0; n < count; ++n)
    enqueueCompute(g, infos[n],
                   background ? PreparationQueue::Kind::Background : PreparationQueue::Kind::Parallel);
}
VKAPI_ATTR VkResult VKAPI_CALL
createGraphicsPipelines(VkDevice d, VkPipelineCache, uint32_t count,
                        const VkGraphicsPipelineCreateInfo *infos,
                        const VkAllocationCallbacks *a, VkPipeline *out) {
  require(!a);
  std::fill_n(out, count, VK_NULL_HANDLE);
  auto &g = service(d);
  std::vector<std::shared_ptr<Service::Impl::Pipeline>> batch;
  batch.reserve(count);
  for (uint32_t n = 0; n < count; ++n) {
    auto r = std::make_shared<Graphics>(infos[n]);
    Key k;
    k.add(uint32_t(VK_PIPELINE_BIND_POINT_GRAPHICS));
    k.add(r->info.flags);
    k.add(r->info.stageCount);
    k.add(r->info.subpass);
    {
      std::lock_guard lock(g.mutex);
      for (const auto &s : r->stages)
        g.stageKey(k, s);
      k.add(g.definition(VK_OBJECT_TYPE_PIPELINE_LAYOUT,
                         uint64_t(r->info.layout)));
      k.add(g.compatiblePasses.at(r->info.renderPass));
    }
    k.add(r->vertex.flags);
    k.add(r->vertex.vertexBindingDescriptionCount);
    k.add(r->vertex.vertexAttributeDescriptionCount);
    for (const auto &b : r->bindings) {
      k.add(b.binding);
      k.add(b.stride);
      k.add(b.inputRate);
    }
    for (const auto &a : r->attributes) {
      k.add(a.location);
      k.add(a.binding);
      k.add(a.format);
      k.add(a.offset);
    }
    k.add(r->assembly.flags);
    k.add(r->assembly.topology);
    k.add(r->assembly.primitiveRestartEnable);
    k.add(r->viewport.flags);
    k.add(r->viewport.viewportCount);
    k.add(r->viewport.scissorCount);
    k.add(r->raster.flags);
    k.add(r->raster.depthClampEnable);
    k.add(r->raster.rasterizerDiscardEnable);
    k.add(r->raster.polygonMode);
    k.add(r->raster.cullMode);
    k.add(r->raster.frontFace);
    k.add(r->raster.depthBiasEnable);
    k.add(r->raster.depthBiasConstantFactor);
    k.add(r->raster.depthBiasClamp);
    k.add(r->raster.depthBiasSlopeFactor);
    k.add(r->raster.lineWidth);
    k.add(r->samples.flags);
    k.add(r->samples.rasterizationSamples);
    k.add(r->samples.sampleShadingEnable);
    k.add(r->samples.minSampleShading);
    k.add(r->samples.alphaToCoverageEnable);
    k.add(r->samples.alphaToOneEnable);
    k.add(r->depth.flags);
    k.add(r->depth.depthTestEnable);
    k.add(r->depth.depthWriteEnable);
    k.add(r->depth.depthCompareOp);
    k.add(r->depth.depthBoundsTestEnable);
    k.add(r->depth.stencilTestEnable);
    k.add(r->depth.minDepthBounds);
    k.add(r->depth.maxDepthBounds);
    k.add(r->depth.front.failOp);
    k.add(r->depth.front.passOp);
    k.add(r->depth.front.depthFailOp);
    k.add(r->depth.front.compareOp);
    k.add(r->depth.front.compareMask);
    k.add(r->depth.front.writeMask);
    k.add(r->depth.front.reference);
    k.add(r->depth.back.failOp);
    k.add(r->depth.back.passOp);
    k.add(r->depth.back.depthFailOp);
    k.add(r->depth.back.compareOp);
    k.add(r->depth.back.compareMask);
    k.add(r->depth.back.writeMask);
    k.add(r->depth.back.reference);
    k.add(r->blend.flags);
    k.add(r->blend.logicOpEnable);
    k.add(r->blend.logicOp);
    k.add(r->blend.attachmentCount);
    k.add(r->dynamic.flags);
    k.add(r->dynamic.dynamicStateCount);
    k.add(uint32_t(r->masks.size()));
    for (auto m : r->masks)
      k.add(m);
    for (auto v : r->blend.blendConstants)
      k.add(v);
    for (auto v : r->states)
      k.add(v);
    for (const auto &b : r->attachments) {
      k.add(b.blendEnable);
      k.add(b.srcColorBlendFactor);
      k.add(b.dstColorBlendFactor);
      k.add(b.colorBlendOp);
      k.add(b.srcAlphaBlendFactor);
      k.add(b.dstAlphaBlendFactor);
      k.add(b.alphaBlendOp);
      k.add(b.colorWriteMask);
    }
    std::string name;
    {
      std::lock_guard lock(g.mutex);
      for (const auto &s : r->stages)
        name += (name.empty() ? "" : "+") + g.stageName(s);
    }
    batch.push_back(g.enqueue(
        std::move(k.bytes), std::move(name),
        [r, &g](VkPipeline *p, VkPipelineCreationFeedbackCreateInfo *feedback) {
          r->bind();
          r->info.pNext = feedback;
          return vkCreateGraphicsPipelines(g.device, g.cache, 1, &r->info,
                                           nullptr, p);
        }));
  }
  return resolveBatch(g, batch, out);
}
} // namespace remix::pipeline
