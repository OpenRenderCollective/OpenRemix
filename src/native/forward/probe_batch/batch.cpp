#include "forward/probe_batch/batch.h"
#include "forward/bake/controller.h"
#include "forward/probe_batch/shaders/embedded.h"
#include "runtime/pipelines.h"
#include <algorithm>
#include <array>
#include <atomic>

namespace remix::forward::probe_batch {
  struct Resources {
    VkDevice device;
    VkDescriptorSetLayout set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::atomic<uint64_t> bufferBytes{0};
    explicit Resources(VkDevice target) : device(target) {}
    ~Resources() {
      if (pipeline) pipeline::destroyPipeline(device, pipeline, nullptr);
      if (layout) pipeline::destroyPipelineLayout(device, layout, nullptr);
      if (set) pipeline::destroyDescriptorSetLayout(device, set, nullptr);
    }
    void initialize() {
      const VkDescriptorSetLayoutBinding bindings[] = {
          {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
          {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
          {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
      const VkDescriptorSetLayoutCreateInfo setInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
          .bindingCount = 3, .pBindings = bindings};
      check(pipeline::createDescriptorSetLayout(device, &setInfo, nullptr, &set));
      const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
          .setLayoutCount = 1, .pSetLayouts = &set};
      check(pipeline::createPipelineLayout(device, &layoutInfo, nullptr, &layout));
      pipeline::nameCode(gatherShader, "forward.probe-visibility-gather");
      const auto module = shaderModule(device, gatherShader, sizeof(gatherShader));
      const VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
          .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"},
          .layout = layout};
      const auto result = pipeline::createComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
      pipeline::destroyShaderModule(device, module, nullptr);
      check(result);
    }
  };
  Field::~Field() {
    if (m_completed) vkDestroyEvent(m_resources->device, m_completed, nullptr);
    if (m_pool) vkDestroyDescriptorPool(m_resources->device, m_pool, nullptr);
    if (m_bufferBytes) m_resources->bufferBytes.fetch_sub(m_bufferBytes, std::memory_order_relaxed);
  }
  Cache::Cache(VkPhysicalDevice physical, VkDevice device, uint64_t byteBudget)
      : m_physical(physical), m_device(device) {
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    m_storageRange = properties.limits.maxStorageBufferRange;
    m_byteBudget = std::min({byteBudget, maximumFieldBytes, m_storageRange});
  }
  void Cache::prepare() {
    if (m_resources) return;
    auto next = std::make_shared<Resources>(m_device);
    next->initialize();
    m_resources = std::move(next);
  }
  uint64_t Cache::bufferBytes() const {
    return m_resources ? m_resources->bufferBytes.load(std::memory_order_relaxed) : 0;
  }
  std::shared_ptr<const Field> Cache::record(VkCommandBuffer command,
      std::shared_ptr<const bake::Volume> source, std::span<const uint32_t> capturedLights) {
    const uint32_t probes = source ? source->irradianceHeader.dimensions[3] : 0;
    const uint32_t groups = uint32_t((capturedLights.size() + 3) / 4);
    const uint64_t elements = uint64_t(probes) * groups;
    const uint64_t bytes = 16 + elements * 16;
    if (!source || !source->data || probes == 0 || capturedLights.empty() ||
        capturedLights.size() > maximumLights || bytes > m_byteBudget ||
        source->data->device != m_device ||
        source->data->capacity < sizeof(bake::GridHeader) + uint64_t(probes) * sizeof(bake::Result) ||
        source->data->capacity > m_storageRange) {
      reset();
      return {};
    }
    // A published volume is immutable. Selection changes retire cache owners;
    // submitted frames retain their own copies until their fences complete.
    if (!m_fields.empty() && (m_fields.front()->m_source != source ||
        !std::equal(capturedLights.begin(), capturedLights.end(),
                    m_fields.front()->m_lights.begin(), m_fields.front()->m_lights.end()))) reset();
    for (const auto &field : m_fields) {
      const auto status = vkGetEventStatus(m_device, field->m_completed);
      if (status == VK_EVENT_SET) {
        auto completed = field;
        m_fields.assign(1, completed); // Other submissions retain their own owners.
        return completed;
      }
      if (status != VK_EVENT_RESET) throw status;
    }
    if (!m_resources) return {};
    auto next = std::shared_ptr<Field>(new Field);
    next->m_resources = m_resources;
    next->m_source = std::move(source);
    next->m_lights.assign(capturedLights.begin(), capturedLights.end());
    next->m_probes = probes;
    next->m_groups = groups;
    std::array<uint32_t, 4 + maximumLights> parameters{};
    parameters[0] = probes;
    parameters[1] = uint32_t(capturedLights.size());
    parameters[2] = groups;
    parameters[3] = 0x56504252u;
    std::copy(capturedLights.begin(), capturedLights.end(), parameters.begin() + 4);
    next->m_parameters.create(m_physical, m_device, sizeof(parameters), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    next->m_bufferBytes += next->m_parameters.allocationBytes;
    m_resources->bufferBytes.fetch_add(next->m_parameters.allocationBytes, std::memory_order_relaxed);
    next->m_parameters.write(parameters.data(), sizeof(parameters));
    next->m_values.create(m_physical, m_device, bytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Buffer::Memory::Device);
    next->m_bufferBytes += next->m_values.allocationBytes;
    m_resources->bufferBytes.fetch_add(next->m_values.allocationBytes, std::memory_order_relaxed);
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
    const VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size};
    check(vkCreateDescriptorPool(m_device, &pool, nullptr, &next->m_pool));
    const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = next->m_pool, .descriptorSetCount = 1, .pSetLayouts = &m_resources->set};
    check(vkAllocateDescriptorSets(m_device, &allocate, &next->m_set));
    const VkDescriptorBufferInfo buffers[] = {next->m_source->data->descriptor(),
        next->m_parameters.descriptor(), next->m_values.descriptor()};
    VkWriteDescriptorSet writes[3];
    for (uint32_t i = 0; i < 3; ++i)
      writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = next->m_set,
          .dstBinding = i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &buffers[i]};
    vkUpdateDescriptorSets(m_device, 3, writes, 0, nullptr);
    const VkEventCreateInfo event{.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    check(vkCreateEvent(m_device, &event, nullptr, &next->m_completed));
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, m_resources->pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, m_resources->layout,
                            0, 1, &next->m_set, 0, nullptr);
    vkCmdDispatch(command, uint32_t((elements + 63) / 64), 1, 1);
    const VkMemoryBarrier visible{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &visible, 0, nullptr, 0, nullptr);
    vkCmdSetEvent(command, next->m_completed, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    // Keep enough candidates for the ordinary in-flight frame slots. A caller
    // abandoning a recording cannot make its unset event reusable or grow this
    // cache without bound. External submission owners remain authoritative.
    if (m_fields.size() >= gpuFrameCount + 1) m_fields.erase(m_fields.begin());
    m_fields.push_back(next);
    return next;
  }
}
