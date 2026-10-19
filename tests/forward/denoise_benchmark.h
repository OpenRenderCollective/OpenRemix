#pragma once
#include "forward/lightmap/denoise_filter.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <vector>

#ifdef REMIX_OIDN
namespace remix::forward::test {
  // Optional CPU scheduling benchmark. Identical packets and output digests
  // distinguish scheduling changes from changes to filtering or chart context.
  inline void denoiseBenchmark() {
    using namespace lightmap;
    using namespace lightmap::denoising;
    for (uint32_t side : {32u, 64u}) {
      constexpr uint32_t fields = 9, charts = 512, queued = 8;
      const size_t plane = size_t(side) * side * 3;
      std::vector<float> original(plane * fields);
      for (uint32_t band = 0; band < fields; ++band)
        for (size_t pixel = 0; pixel < plane; ++pixel)
          original[band * plane + pixel] =
              (band ? -.2f : 1.f) + .15f * std::sin(float(pixel * 17 + band * 31) * .17f);
      std::vector<std::vector<float>> packets(queued, original);
      DenoiseStatistics statistics;
      Filter filter(statistics, original.size() * sizeof(float), HostTransfers::Pageable,
                    FilterDispatch::Ordinary, FilterDevice::Cpu);
      filter.initialize({});
      if (statistics.deviceType != 1)
        throw std::runtime_error("CPU denoise benchmark requires an available CPU device");
      // Prepare the network once, outside the measured window.
      filter.run(packets.front(), side, side, 0, side, side);
      const auto start = std::chrono::steady_clock::now();
      uint64_t digest = 1469598103934665603ull;
      for (uint32_t chart = 0; chart < charts; chart += queued) {
        for (auto &packet : packets) {
          std::copy(original.begin(), original.end(), packet.begin());
          filter.enqueue(packet, side, side, 0, side, side);
        }
        filter.finish();
        for (const auto &packet : packets) {
          filter.validate(packet);
          for (float value : packet) {
            digest ^= std::bit_cast<uint32_t>(value);
            digest *= 1099511628211ull;
          }
        }
      }
      const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      std::printf("CPU denoise benchmark: side=%u charts=%u seconds=%.6f monitored=%u digest=%llu\n", side,
                  charts, seconds, statistics.monitoredExecutions, static_cast<unsigned long long>(digest));
      std::fflush(stdout);
    }
  }
} // namespace remix::forward::test
#endif
