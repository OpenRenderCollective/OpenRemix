#pragma once
#include "forward/lightmap/denoise_filter.h"
#include <algorithm>
#include <barrier>
#include <bit>
#include <cmath>
#include <cstdio>
#include <future>
#include <vector>

#ifdef REMIX_OIDN
namespace remix::forward::test {
  struct DenoiseBenchmarkResult {
    double seconds;
    uint32_t monitored;
    uint64_t digest;
    std::vector<float> output;
  };
  inline DenoiseBenchmarkResult denoiseWorker(std::span<const float> original, uint32_t side, uint32_t charts,
                                              uint32_t cpuThreads, std::barrier<> &ready) {
    using namespace lightmap;
    using namespace lightmap::denoising;
    bool arrived = false;
    try {
      constexpr uint32_t queued = 8;
      std::vector<std::vector<float>> packets(queued, std::vector<float>(original.begin(), original.end()));
      DenoiseStatistics statistics;
      Filter filter(statistics, original.size_bytes(), HostTransfers::Pageable, FilterDispatch::Ordinary,
                    FilterDevice::Cpu, cpuThreads);
      filter.initialize({});
      if (statistics.deviceType != 1)
        throw std::runtime_error("CPU denoise benchmark requires an available CPU device");
      filter.run(packets.front(), side, side, 0, side, side);
      arrived = true;
      ready.arrive_and_wait(); // every independent model is warm before timing
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
      return {std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
              statistics.monitoredExecutions, digest, packets.front()};
    } catch (...) {
      if (!arrived)
        ready.arrive_and_drop();
      throw;
    }
  }
  // Optional CPU scheduling benchmark. Identical packets and output digests
  // distinguish scheduling changes from changes to filtering or chart context.
  inline void denoiseBenchmark(uint32_t workers = 1) {
    using namespace lightmap;
    using namespace lightmap::denoising;
    if (workers != 1 && workers != 2 && workers != 4)
      throw std::invalid_argument("CPU benchmark workers must be 1, 2 or 4");
    for (uint32_t side : {32u, 64u}) {
      constexpr uint32_t fields = 9, charts = 512;
      const size_t plane = size_t(side) * side * 3;
      std::vector<float> original(plane * fields);
      for (uint32_t band = 0; band < fields; ++band)
        for (size_t pixel = 0; pixel < plane; ++pixel)
          original[band * plane + pixel] =
              (band ? -.2f : 1.f) + .15f * std::sin(float(pixel * 17 + band * 31) * .17f);
      std::barrier ready(workers);
      std::vector<std::future<DenoiseBenchmarkResult>> jobs;
      jobs.reserve(workers);
      try {
        for (uint32_t i = 0; i < workers; ++i)
          jobs.push_back(std::async(std::launch::async, [&] {
            return denoiseWorker(original, side, charts / workers, workers == 1 ? 0 : 32 / workers, ready);
          }));
      } catch (...) {
        for (size_t i = jobs.size(); i < workers; ++i)
          ready.arrive_and_drop();
        throw;
      }
      double seconds = 0;
      uint32_t monitored = 0;
      uint64_t digest = 0;
      std::vector<float> expected;
      for (auto &job : jobs) {
        auto result = job.get();
        if (!expected.empty() && (result.output != expected || result.digest != digest))
          throw std::runtime_error("Independent CPU queues produced different outputs");
        expected = std::move(result.output);
        digest = result.digest;
        seconds = std::max(seconds, result.seconds);
        monitored += result.monitored;
      }
      uint64_t packetDigest = 1469598103934665603ull;
      for (float value : expected) {
        packetDigest ^= std::bit_cast<uint32_t>(value);
        packetDigest *= 1099511628211ull;
      }
      std::printf("CPU denoise benchmark: side=%u charts=%u workers=%u seconds=%.6f monitored=%u "
                  "workerDigest=%llu packetDigest=%llu\n",
                  side, charts, workers, seconds, monitored, static_cast<unsigned long long>(digest),
                  static_cast<unsigned long long>(packetDigest));
      std::fflush(stdout);
    }
  }
} // namespace remix::forward::test
#endif
