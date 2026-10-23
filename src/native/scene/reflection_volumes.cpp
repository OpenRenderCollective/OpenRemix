#include "scene/resources.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace remix {
  void NativeResources::setMaterialReflection(uint64_t material, scene::ReflectionBinding binding) {
    if (materialReflection(material) == binding) return;
    auto next = m_materialReflections ? std::make_shared<MaterialReflections>(*m_materialReflections)
                                     : std::make_shared<MaterialReflections>();
    if (binding.volumeId) (*next)[material] = binding;
    else next->erase(material);
    m_materialReflections = next->empty() ? nullptr : std::move(next);
  }
  void NativeResources::setReflectionVolumes(const remixapi_ReflectionVolumesInfoEXT *info) {
    if (!info) { m_reflectionVolumes.reset(); return; }
    const bool extended = info->version == REMIXAPI_REFLECTION_VOLUMES_SAMPLING_VERSION_EXT;
    if ((!extended && (info->structSize != sizeof(*info) || info->version != REMIXAPI_REFLECTION_VOLUMES_VERSION_EXT)) ||
        (extended && info->structSize != sizeof(remixapi_ReflectionVolumesSamplingInfoEXT)) ||
        !info->sceneId || info->reserved || info->volumeCount > REMIXAPI_REFLECTION_VOLUMES_MAX_EXT ||
        (info->volumeCount && !info->volumes))
      throw REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    const auto sampling = extended
        ? reinterpret_cast<const remixapi_ReflectionVolumesSamplingInfoEXT *>(info)->sampling : nullptr;
    if (extended && info->volumeCount && !sampling) throw REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    auto next = std::make_shared<scene::ReflectionVolumes>();
    next->sceneId = info->sceneId;
    next->revision = info->revision;
    next->volumes.reserve(info->volumeCount);
    std::unordered_set<uint64_t> ids;
    for (uint32_t i = 0; i < info->volumeCount; ++i) {
      const auto &input = info->volumes[i];
      scene::ReflectionVolume volume;
      if (!input.id || !ids.insert(input.id).second || input.priority < -32768 || input.priority > 32767)
        throw REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      volume.id = input.id;
      volume.priority = input.priority;
      std::copy_n(input.minimum, 3, volume.minimum.begin());
      std::copy_n(input.maximum, 3, volume.maximum.begin());
      volume.influenceMinimum = volume.minimum;
      volume.influenceMaximum = volume.maximum;
      if (sampling) {
        std::copy_n(sampling[i].minimum, 3, volume.influenceMinimum.begin());
        std::copy_n(sampling[i].maximum, 3, volume.influenceMaximum.begin());
        volume.blendDistance = sampling[i].blendDistance;
        if (!std::isfinite(volume.blendDistance) || volume.blendDistance < 0)
          throw REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      }
      std::copy_n(input.capture, 3, volume.capture.begin());
      for (uint32_t row = 0; row < 3; ++row)
        std::copy_n(input.worldToLocal[row], 4, volume.worldToLocal[row].begin());
      if (!scene::validProjectionSpace(volume.worldToLocal)) throw REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      const auto origin = scene::projectionPoint(volume.worldToLocal, volume.capture);
      for (uint32_t c = 0; c < 3; ++c)
        if (!std::isfinite(volume.minimum[c]) || !std::isfinite(volume.maximum[c]) ||
            !std::isfinite(volume.influenceMinimum[c]) || !std::isfinite(volume.influenceMaximum[c]) ||
            volume.influenceMinimum[c] >= volume.influenceMaximum[c] ||
            !std::isfinite(volume.influenceMaximum[c] - volume.influenceMinimum[c]) ||
            !std::isfinite(volume.influenceMaximum[c] + volume.influenceMinimum[c]) ||
            !std::isfinite(volume.capture[c]) || !std::isfinite(origin[c]) ||
            volume.minimum[c] >= volume.maximum[c] ||
            !std::isfinite(volume.maximum[c] - volume.minimum[c]) ||
            !std::isfinite(volume.maximum[c] + volume.minimum[c]) ||
            origin[c] < volume.minimum[c] || origin[c] > volume.maximum[c])
          throw REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      next->volumes.push_back(volume);
    }
    // Unchanged editor saves must not restart capture work or replace frame ownership.
    if (m_reflectionVolumes && m_reflectionVolumes->sceneId == next->sceneId &&
        m_reflectionVolumes->volumes == next->volumes) return;
    m_reflectionVolumes = std::move(next);
  }
} // namespace remix
