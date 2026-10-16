#include "forward/ray_scene.h"
#include "forward/resources.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace remix::forward {
  namespace {
    constexpr VkBufferUsageFlags inputUsage =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    constexpr const char *extensions[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                                          VK_KHR_RAY_QUERY_EXTENSION_NAME,
                                          VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
    template <class T> T function(VkDevice device, const char *name) {
      const auto result = reinterpret_cast<T>(vkGetDeviceProcAddr(device, name));
      if (!result)
        throw VK_ERROR_EXTENSION_NOT_PRESENT;
      return result;
    }
    VkDeviceAddress aligned(VkDeviceAddress value, VkDeviceSize alignment) {
      return (value + alignment - 1) & ~(alignment - 1);
    }
    void buildBarrier(VkCommandBuffer command, VkPipelineStageFlags destination, VkAccessFlags access) {
      const VkMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                    .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                                    .dstAccessMask = access};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, destination, 0, 1,
                           &barrier, 0, nullptr, 0, nullptr);
    }
  } // namespace
  struct RayScene::Gpu {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkCreateAccelerationStructureKHR create = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroy = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR sizes = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR accelerationAddress = nullptr;
    PFN_vkGetBufferDeviceAddress bufferAddress = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR build = nullptr;
    struct Acceleration {
      VkDevice device = VK_NULL_HANDLE;
      PFN_vkDestroyAccelerationStructureKHR destroy = nullptr;
      VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
      VkDeviceAddress address = 0;
      std::unique_ptr<Buffer> storage;
      ~Acceleration() {
        if (handle)
          destroy(device, handle, nullptr);
      }
    };
    std::unique_ptr<Acceleration> bottom, top;
    std::unique_ptr<Buffer> positions, indices, instances, scratch;
    VkAccelerationStructureGeometryKHR triangles{}, placements{};
    VkAccelerationStructureBuildSizesInfoKHR bottomSize{}, topSize{};
    uint32_t triangleCount = 0;
    VkDeviceAddress scratchAddress = 0;
    uint64_t scratchAlignment = 1;
    void createBottom(Geometry geometry);
    void createTop();
    void createScratch();

    std::unique_ptr<Buffer> buffer(VkDeviceSize bytes, VkBufferUsageFlags usage,
                                   Buffer::Memory placement = Buffer::Memory::Host) {
      auto result = std::make_unique<Buffer>();
      result->create(physical, device, bytes, usage, placement);
      return result;
    }
    VkDeviceAddress address(const Buffer &buffer) const {
      const VkBufferDeviceAddressInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                                           .buffer = buffer.handle};
      return bufferAddress(device, &info);
    }
    VkAccelerationStructureBuildGeometryInfoKHR description(bool topLevel) const {
      return {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
              .type = topLevel ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
                               : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
              .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
              .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
              .geometryCount = 1,
              .pGeometries = topLevel ? &placements : &triangles};
    }
    std::unique_ptr<Acceleration> allocate(VkDeviceSize bytes, VkAccelerationStructureTypeKHR type) {
      auto result = std::make_unique<Acceleration>();
      result->device = device;
      result->destroy = destroy;
      result->storage = buffer(bytes,
                               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                               Buffer::Memory::Device);
      const VkAccelerationStructureCreateInfoKHR info{
          .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
          .buffer = result->storage->handle,
          .size = bytes,
          .type = type};
      check(create(device, &info, nullptr, &result->handle));
      const VkAccelerationStructureDeviceAddressInfoKHR query{
          .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
          .accelerationStructure = result->handle};
      result->address = accelerationAddress(device, &query);
      return result;
    }
  };
  RayScene::RayScene() = default;
  RayScene::~RayScene() = default;

  bool RayScene::supported(VkPhysicalDevice physical) {
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_2)
      return false;
    uint32_t count = 0;
    check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr));
    std::vector<VkExtensionProperties> available(count);
    check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, available.data()));
    for (const char *name : extensions)
      if (std::none_of(available.begin(), available.end(),
                       [&](const auto &value) { return std::strcmp(name, value.extensionName) == 0; }))
        return false;
    VkPhysicalDeviceRayQueryFeaturesKHR ray{.sType =
                                                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, .pNext = &ray};
    VkPhysicalDeviceVulkan12Features vulkan12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                              .pNext = &acceleration};
    VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                       .pNext = &vulkan12};
    vkGetPhysicalDeviceFeatures2(physical, &features);
    return ray.rayQuery && acceleration.accelerationStructure && vulkan12.bufferDeviceAddress;
  }
  void RayScene::initialize(VkPhysicalDevice physical, VkDevice device, Geometry geometry) {
    if (geometry.indices.size() % 3 || geometry.positions.size() > UINT32_MAX ||
        geometry.indices.size() / 3 > UINT32_MAX)
      throw std::invalid_argument("Ray geometry must contain addressable triangle triples");
    for (const auto &position : geometry.positions)
      for (float value : position)
        if (!std::isfinite(value))
          throw std::invalid_argument("Ray geometry contains a non-finite position");
    for (uint32_t index : geometry.indices)
      if (index >= geometry.positions.size())
        throw std::invalid_argument("Ray geometry index is out of bounds");
    auto next = std::make_unique<Gpu>();
    auto &g = *next;
    g.physical = physical;
    g.device = device;
    g.create = function<PFN_vkCreateAccelerationStructureKHR>(device, "vkCreateAccelerationStructureKHR");
    g.destroy = function<PFN_vkDestroyAccelerationStructureKHR>(device, "vkDestroyAccelerationStructureKHR");
    g.sizes = function<PFN_vkGetAccelerationStructureBuildSizesKHR>(
        device, "vkGetAccelerationStructureBuildSizesKHR");
    g.accelerationAddress = function<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
        device, "vkGetAccelerationStructureDeviceAddressKHR");
    g.bufferAddress = function<PFN_vkGetBufferDeviceAddress>(device, "vkGetBufferDeviceAddress");
    g.build =
        function<PFN_vkCmdBuildAccelerationStructuresKHR>(device, "vkCmdBuildAccelerationStructuresKHR");
    VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                           .pNext = &acceleration};
    vkGetPhysicalDeviceProperties2(physical, &properties);
    g.triangleCount = uint32_t(geometry.indices.size() / 3);
    if (g.triangleCount > acceleration.maxPrimitiveCount || acceleration.maxInstanceCount < 1)
      throw VK_ERROR_FEATURE_NOT_PRESENT;
    g.scratchAlignment = acceleration.minAccelerationStructureScratchOffsetAlignment;
    g.createBottom(geometry);
    g.createTop();
    g.createScratch();
    m_gpu = std::move(next);
  }
  void RayScene::Gpu::createBottom(Geometry geometry) {
    if (triangleCount) {
      positions = buffer(geometry.positions.size_bytes(), inputUsage);
      indices = buffer(geometry.indices.size_bytes(), inputUsage);
      positions->write(geometry.positions.data(), geometry.positions.size_bytes());
      indices->write(geometry.indices.data(), geometry.indices.size_bytes());
      // Candidates remain non-opaque. The querying shader owns alpha coverage
      // and sidedness policy; an opaque-only BVH would erase holes in fences.
      triangles = {
          .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
          .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
          .geometry = {
              .triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                            .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                            .vertexData = {.deviceAddress = address(*positions)},
                            .vertexStride = sizeof(Position),
                            .maxVertex = uint32_t(geometry.positions.size() - 1),
                            .indexType = VK_INDEX_TYPE_UINT32,
                            .indexData = {.deviceAddress = address(*indices)}}}};
      bottomSize.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
      const auto buildInfo = description(false);
      sizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &triangleCount, &bottomSize);
      bottom =
          allocate(bottomSize.accelerationStructureSize, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
    }
  }
  void RayScene::Gpu::createTop() {
    VkAccelerationStructureInstanceKHR instance{};
    instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = 1;
    instance.mask = 0xff;
    instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    instance.accelerationStructureReference = bottom ? bottom->address : 0;
    instances = buffer(sizeof(instance), inputUsage);
    instances->write(&instance, sizeof(instance));
    placements = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry = {
            .instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                          .data = {.deviceAddress = address(*instances)}}}};
    const uint32_t instanceCount = bottom ? 1 : 0;
    topSize.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    const auto buildInfo = description(true);
    sizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &instanceCount, &topSize);
    top = allocate(topSize.accelerationStructureSize, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR);
  }
  void RayScene::Gpu::createScratch() {
    const auto scratchBytes = std::max(bottomSize.buildScratchSize, topSize.buildScratchSize);
    scratch = buffer(scratchBytes + scratchAlignment - 1,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                     Buffer::Memory::Device);
    scratchAddress = aligned(address(*scratch), scratchAlignment);
  }
  void RayScene::recordBuild(VkCommandBuffer command, VkPipelineStageFlags queryStages) {
    auto &g = *m_gpu;
    if (!g.scratch)
      throw std::logic_error("Ray scene build inputs have already been released");
    // Rebuilding is legal after earlier queries on this same queue. Serialize
    // their reads before overwriting either structure or the shared scratch.
    const VkMemoryBarrier incoming{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                   .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                   .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                                                    VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &incoming, 0, nullptr,
                         0, nullptr);
    auto record = [&](bool topLevel, VkAccelerationStructureKHR destination, uint32_t count) {
      auto description = g.description(topLevel);
      description.dstAccelerationStructure = destination;
      description.scratchData.deviceAddress = g.scratchAddress;
      const VkAccelerationStructureBuildRangeInfoKHR range{.primitiveCount = count};
      const auto *ranges = &range;
      g.build(command, 1, &description, &ranges);
    };
    if (g.bottom) {
      record(false, g.bottom->handle, g.triangleCount);
      buildBarrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                   VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                       VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    }
    record(true, g.top->handle, g.bottom ? 1 : 0);
    buildBarrier(command, queryStages,
                 VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
  }
  void RayScene::releaseBuildInputs() {
    m_gpu->positions.reset();
    m_gpu->indices.reset();
    m_gpu->instances.reset();
    m_gpu->scratch.reset();
  }
  VkAccelerationStructureKHR RayScene::handle() const {
    return m_gpu->top->handle;
  }
  uint64_t RayScene::accelerationBytes() const {
    return m_gpu->top->storage->allocationBytes +
           (m_gpu->bottom ? m_gpu->bottom->storage->allocationBytes : 0);
  }
  uint64_t RayScene::buildBytes() const {
    uint64_t result = 0;
    for (const auto *buffer :
         {m_gpu->positions.get(), m_gpu->indices.get(), m_gpu->instances.get(), m_gpu->scratch.get()})
      if (buffer)
        result += buffer->allocationBytes;
    return result;
  }
} // namespace remix::forward
