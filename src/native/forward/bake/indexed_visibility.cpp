#include "forward/bake/indexed_visibility.h"
#include <cmath>
#include <stdexcept>

namespace remix::forward::bake {
  bool Transport::hasVisibilityReceivers() const { return m_gpu && bool(m_gpu->indexedVisibility); }
  uint64_t Transport::visibilityReceiverBytes() const {
    return hasVisibilityReceivers() ? m_gpu->indexedVisibility->receivers.size() * sizeof(Point) : 0;
  }
  bool Transport::prepareVisibilityReceivers(std::span<const Point> points, uint64_t byteBudget) {
    auto &g = *m_gpu;
    if (g.prepared || g.indexedVisibility)
      throw std::logic_error("Visibility receivers must be installed once before transport preparation");
    if (points.empty() || points.size() > UINT32_MAX || points.size_bytes() > byteBudget ||
        !g.options.parallel || !g.options.cpuReadback || g.options.deviceLocalBatches)
      return false;
    for (const auto &point : points)
      for (const auto &values : {point.position, point.normal})
        for (float value : values)
          if (!std::isfinite(value)) throw std::invalid_argument("Non-finite visibility receiver");
    try {
      auto next = std::make_unique<Gpu::IndexedVisibility>();
      next->receivers.assign(points.begin(), points.end());
      g.indexedVisibility = std::move(next);
      return true;
    } catch (const std::bad_alloc &) {
      return false;
    }
  }
  void Transport::recordIndexedVisibility(VkCommandBuffer command, uint32_t index,
                                          std::span<const VisibilityTask> tasks, uint32_t samples) {
    auto &g = *m_gpu;
    if (!g.prepared || !g.indexedVisibility || g.slots.at(index).lighting)
      throw std::logic_error("Indexed visibility requires prepared immutable receiver and lighting tables");
    if (tasks.empty() || tasks.size() > g.options.batchPoints || samples < 1 || samples > 8192 ||
        uint64_t(tasks.size()) * samples > g.options.rayBudget)
      throw std::invalid_argument("Indexed visibility exceeds its point or sample limits");
    const auto &receivers = g.indexedVisibility->receivers;
    for (const auto &task : tasks)
      if (task.point >= receivers.size() || task.light >= g.source->lights.size() || task.light >= (1u << 24))
        throw std::invalid_argument("Invalid indexed visibility receiver or light");
    g.reserveSlot(index, uint32_t(tasks.size()), sizeof(float));
    auto &points = *g.slots.at(index).points;
    // reserveSlot guarantees mapped host input with room for one Point per task.
    // Write each complete point before setting its light; never read upload memory.
    auto *packed = static_cast<Point *>(points.mapped);
    for (size_t i = 0; i < tasks.size(); ++i) {
      packed[i] = receivers[tasks[i].point];
      packed[i].position[3] = float(tasks[i].light);
    }
    points.flush();
    dispatch(command, index, uint32_t(tasks.size()), samples, 1, 0, Mode::Visibility);
  }
}
