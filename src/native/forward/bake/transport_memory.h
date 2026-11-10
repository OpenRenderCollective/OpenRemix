#pragma once
#include "forward/bake/transport.h"
#include <array>
#include <vector>

namespace remix::forward::bake {
// One collection per renderer snapshot, shared across provider controllers.
// Returns only allocations not already counted, without retaining GPU owners.
class TransportMemory {
public:
  Transport::Memory::Bytes add(const Transport &transport) { return add(transport.memory()); }
  Transport::Memory::Bytes add(const Transport::Memory &memory) {
    if (!memory.sceneIdentity && !memory.consumerIdentity) return {};
    bool sceneSeen = !memory.sceneIdentity, consumerSeen = !memory.consumerIdentity;
    auto inspect = [&](const Entry &entry) {
      sceneSeen |= entry.scene == memory.sceneIdentity;
      consumerSeen |= entry.consumer == memory.consumerIdentity;
    };
    for (size_t i = 0; i < m_count; ++i) inspect(m_inline[i]);
    for (const auto &entry : m_overflow) inspect(entry);
    if (sceneSeen && consumerSeen) return {};
    const Entry entry{memory.sceneIdentity, memory.consumerIdentity};
    if (m_count < m_inline.size()) m_inline[m_count++] = entry;
    else m_overflow.push_back(entry);
    auto result = consumerSeen ? Transport::Memory::Bytes{} : memory.consumer;
    if (!sceneSeen) {
      result.buffers += memory.scene.buffers;
      result.images += memory.scene.images;
      result.acceleration += memory.scene.acceleration;
    }
    return result;
  }
private:
  struct Entry { const void *scene, *consumer; };
  // Normal jobs and frame slots need no heap allocation. A cancellation burst
  // may retain more owners; overflow remains correct rather than truncating.
  std::array<Entry, 8> m_inline;
  size_t m_count = 0;
  std::vector<Entry> m_overflow;
};
}
