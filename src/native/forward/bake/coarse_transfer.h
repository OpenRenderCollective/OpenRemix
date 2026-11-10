#pragma once
#include "forward/bake/grid.h"

namespace remix::forward::bake {
  // Experimental geometry-only field. Private GPU publication is optional; disk
  // caches do not contain it. Unknown connections retain ordinary reconstruction.
  class CoarseTransfer {
  public:
    struct Options {
      uint64_t maximumBytes = 64ull << 20; // owned scratch, excluding directory-validation temporaries
      uint32_t maximumRays = 2u << 20;
      float epsilon = .0001f; // scene units; caller converts from metres
    };
    // Declines the whole optional field when it cannot fit; never truncates it.
    static std::optional<CoarseTransfer> plan(const Grid &grid, Options options,
                                             std::stop_token cancelled = {});
    uint32_t rays() const { return uint32_t(m_requests.size()); }
    float maximumDistance() const { return m_maximumDistance; }
    std::vector<Point> batch(uint32_t first, uint32_t count) const;
    // Caller has observed the owning GPU slot fence. -1 clear, -2 unknown,
    // otherwise first-hit distance; finite connections exclude both endpoints.
    void consume(uint32_t first, std::span<const float> depths);
    bool complete() const { return m_received == m_requests.size(); }
    std::span<const uint32_t> words() const;
    uint64_t scratchBytes() const;
    static constexpr uint32_t magic = 0x43544652;
    // Wire: four header words (magic, root nodes, patches, word count), root
    // directory of absolute patch offsets, aligned to four words. Each patch:
    // factor, node count, absolute field offset, reserved, eight coarse record
    // IDs (+1, zero missing). Each fine node: eight visibility bytes in two
    // words, then eight known bits. Padding is zero. Fine order matches lookup.
  private:
    struct Request {
      uint32_t coarse, fine, nodeWord, corner;
      float distance;
    };
    std::vector<uint32_t> m_words;
    std::vector<Vec3> m_positions;
    std::vector<Request> m_requests;
    std::vector<uint8_t> m_completed;
    float m_epsilon = 0, m_maximumDistance = 0;
    uint32_t m_received = 0;
  };
  // Nonallocating candidate reconstruction. Malformed/missing fields leave the
  // stencil unchanged; unmatched donor connections retain unoccluded fallback.
  void applyCoarseTransfer(std::span<const uint32_t> words, const Grid &grid, ProbeStencil &stencil) noexcept;
}
