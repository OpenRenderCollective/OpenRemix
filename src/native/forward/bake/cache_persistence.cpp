#include "forward/bake/cache_persistence.h"
#include "forward/bake/local_reflection_cache.h"
#include <chrono>
#include <cstdio>

namespace remix::forward::bake::cache {
namespace {
View view(const Snapshot &s) {
  return {s.grid,
          s.layout,
          s.mixed,
          s.reflectionCaptures(),
          s.reflectionData(),
          s.pixels,
          s.surfaceSamples,
          s.directDiffuse,
          s.sun,
          s.styles,
          s.localProbes ? &*s.localProbes : nullptr,
          s.directionalAttempted,
          s.directionals,
          s.directionalBudget,
          s.localReflections.get(),
          s.localReflectionsOnly,
          s.reflectionKey,
          bool(s.reflectionOwner) && s.reflectionKey != 0};
}
template <typename T> uint64_t storage(const std::vector<T> &v) {
  return uint64_t(v.capacity()) * sizeof(T);
}
} // namespace
uint64_t Persistence::retainedCharge(const Snapshot &s) {
  uint64_t bytes =
      sizeof(Snapshot) + storage(s.grid.probes) + storage(s.grid.tasks) +
      storage(s.grid.points) + storage(s.grid.lookup) +
      storage(s.grid.origins) + storage(s.grid.depth.records) +
      storage(s.layout.charts) + storage(s.layout.mappings) +
      storage(s.pixels) + storage(s.styles) + storage(s.layout.points) +
      storage(s.layout.pointCharts) + storage(s.layout.addresses) +
      storage(s.layout.receiverSources) + storage(s.layout.receiverSamples) +
      storage(s.layout.footprints) + storage(s.layout.sampleCenters) +
      storage(s.layout.owners) + storage(s.mixed.charts) +
      storage(s.mixed.directCharts) + storage(s.mixed.tasks) +
      storage(s.mixed.remaining) + storage(s.mixed.next) + storage(s.captures) +
      storage(s.specular.texels) + storage(s.directionals);
  for (const auto &remaining : s.mixed.remaining)
    bytes += storage(remaining);
  for (const auto &mapping : s.layout.mappings)
    bytes += storage(mapping.texcoords);
  for (const auto &capture : s.captures)
    bytes += storage(capture.radiance);
  if (s.reflectionOwner) {
    const auto &reflection = *s.reflectionOwner;
    bytes += sizeof(probes::CapturePayload) + 64 +
             storage(reflection.captures) + storage(reflection.data.texels);
    for (const auto &capture : reflection.captures)
      bytes += storage(capture.radiance);
  }
  // Private compressed containers expose logical storage. Charge twice their
  // size plus per-entry metadata to cover vector growth and identity tables.
  bytes += 2 * (s.mixed.occlusion.words().size_bytes() +
                s.mixed.masks.bytes().size_bytes() +
                s.mixed.masks.entries().size_bytes()) +
           s.mixed.masks.entries().size() * 64;
  bytes += storage(s.mixed.lights.directLighting.lights) +
           s.mixed.lights.identities.size() * 64;
  if (s.localProbes)
    bytes += 2 * (s.localProbes->offsets().size_bytes() +
                  s.localProbes->entries().size_bytes() +
                  s.localProbes->direct().size_bytes());
  if (s.localReflections)
    bytes += local_cache::retainedBytes(*s.localReflections);
  // The captured source is shared with the published runtime, not duplicated.
  return bytes;
}
Persistence::Persistence(uint64_t byteBudget, Writer write)
    : cache_io::Persistence<Snapshot>(
          byteBudget, retainedCharge,
          [write = std::move(write)](const auto &file, uint64_t key,
                                     const Snapshot &snapshot) {
            const auto data = view(snapshot);
            if (write)
              return write(file, key, data);
            if (data.localReflectionsOnly)
              return data.localReflections &&
                     local_cache::write(local_cache::path(file), key,
                                        *data.localReflections);
            const bool saved = cache::write(file, key, data);
            if (saved && data.localReflections)
              (void)local_cache::write(local_cache::path(file), key,
                                       *data.localReflections);
            return saved;
          },
          "Forward bake") {}
Persistence &persistence() {
  static Persistence service;
  return service;
}
} // namespace remix::forward::bake::cache
