#include "forward/lightmap/denoise.h"
#include "forward/lightmap/denoise_chart.h"
#include "forward/lightmap/denoise_graph.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#ifdef REMIX_OIDN
#include <OpenImageDenoise/oidn.h>
static_assert(OIDN_VERSION >= 20500, "Lightmap denoising requires Open Image Denoise 2.5 or newer");
#endif

namespace remix::forward::lightmap {
  uint32_t denoiserVersion() {
#ifdef REMIX_OIDN
    return OIDN_VERSION;
#else
    return 0;
#endif
  }
#ifdef REMIX_OIDN
  namespace {
    // Match OIDN 2.5's 16-pixel U-Net input padding, allowing neighbouring chart
    // sizes to share committed shapes. Unknown versions retain exact chart sizes.
    constexpr uint32_t imageAlignment = OIDN_VERSION >= 20500 && OIDN_VERSION < 20600 ? 16 : 1;
    auto imageShape(const Allocation &chart) {
      return std::pair{denoising::alignedExtent(chart.width, imageAlignment),
                        denoising::alignedExtent(chart.height, imageAlignment)};
    }
    void checkCancellation(std::stop_token cancelled) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Lightmap denoising superseded");
    }
    class Duration {
    public:
      explicit Duration(double &seconds) : m_seconds(seconds) {}
      ~Duration() {
        m_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
      }
      Duration(const Duration &) = delete;
      Duration &operator=(const Duration &) = delete;

    private:
      double &m_seconds;
      std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
    };
    struct Filter {
      struct Model {
        OIDNFilter handle = nullptr;
        uint32_t width = 0, height = 0;
        OIDNBuffer image = nullptr;
        size_t offset = 0;
        bool monitored = false;
      };
      denoising::CudaGraph graph;
      uint32_t graphMask = 0;
      FilterDispatch dispatch;
      OIDNDevice device = nullptr;
      std::array<Model, denoising::coefficientCount> models{}; // one immutable image binding per SH band
      OIDNBuffer buffer = nullptr;
      OIDNBuffer staging = nullptr;
      float *stagingData = nullptr;
      size_t stagingCapacity = 0, stagingUsed = 0; // float elements, not bytes
      struct Transfer {
        std::span<float> destination;
        size_t offset; // float elements in the pinned arena
      };
      std::vector<Transfer> pendingTransfers;
      HostTransfers hostTransfers;
      size_t capacity = 0;
      size_t requiredCapacity = 0;
      DenoiseStatistics &statistics;
      std::stop_token cancellation;
      bool inFlight = false;
      void retire() noexcept {
        if (inFlight) {
          const Duration duration(statistics.waitSeconds);
          oidnSyncDevice(device);
          inFlight = false;
          ++statistics.synchronizations;
        }
      }
      void finish() {
        retire();
        check();
        const Duration duration(statistics.stagingSeconds);
        for (const auto &transfer : pendingTransfers)
          std::copy_n(stagingData + transfer.offset, transfer.destination.size(), transfer.destination.data());
        pendingTransfers.clear();
        stagingUsed = 0;
      }
      Filter(DenoiseStatistics &statistics, size_t requiredCapacity, HostTransfers hostTransfers, FilterDispatch dispatch)
          : dispatch(dispatch), hostTransfers(hostTransfers), requiredCapacity(requiredCapacity), statistics(statistics) {}
      ~Filter() {
        // Even cancellation/errors must retire queued work before its inputs,
        // callbacks, filters or buffers can be released.
        retire();
        graph.invalidate();
        for (const auto &model : models)
          if (model.handle)
            oidnReleaseFilter(model.handle);
        if (buffer)
          oidnReleaseBuffer(buffer);
        if (staging)
          oidnReleaseBuffer(staging);
        if (device)
          oidnReleaseDevice(device);
      }
      void check() {
        const char *message = nullptr;
        if (oidnGetDeviceError(device, &message) != OIDN_ERROR_NONE)
          throw std::runtime_error(std::string("OIDN lightmap: ") + (message ? message : "unknown error"));
      }
      void initialize(std::stop_token &cancelled) {
        const Duration duration(statistics.initializationSeconds);
        cancellation = cancelled;
        if (dispatch == FilterDispatch::Automatic) device = graph.createDevice();
        if (!device) device = oidnNewDevice(OIDN_DEVICE_TYPE_DEFAULT);
        check();
        if (!device)
          throw std::runtime_error("No OIDN lightmap device");
        oidnCommitDevice(device);
        check();
        for (uint32_t i = 0; i < models.size(); ++i) {
          auto &model = models[i];
          model.handle = oidnNewFilter(device, "RTLightmap");
          check();
          oidnSetFilterInt(model.handle, "quality", OIDN_QUALITY_HIGH);
          oidnSetFilterInt(model.handle, "maxMemoryMB", 256);
          // Fixed across charts, avoiding per-chart auto-exposure differences.
          oidnSetFilterFloat(model.handle, "inputScale", 1);
          oidnSetFilterBool(model.handle, "directional", i != 0);
          check();
        }
      }
      float *transferData(std::span<float> images) {
        if (hostTransfers == HostTransfers::Pageable || statistics.deviceType == OIDN_DEVICE_TYPE_CPU ||
            images.size_bytes() > denoising::queueBudgetBytes)
          return images.data();
        if (!staging) {
          const Duration duration(statistics.stagingSeconds);
          stagingCapacity = denoising::queueBudgetBytes / sizeof(float);
          staging = oidnNewBufferWithStorage(device, stagingCapacity * sizeof(float), OIDN_STORAGE_HOST);
          const char *message = nullptr;
          const auto error = oidnGetDeviceError(device, &message);
          if (!staging && error == OIDN_ERROR_OUT_OF_MEMORY) {
            // Staging is an optimization. Keep the existing transfer path if
            // additional pinned memory is unavailable.
            hostTransfers = HostTransfers::Pageable;
            return images.data();
          }
          if (error != OIDN_ERROR_NONE)
            throw std::runtime_error(std::string("OIDN host staging: ") + (message ? message : "unknown error"));
          stagingData = static_cast<float *>(oidnGetBufferData(staging));
          check();
          if (!stagingData)
            throw std::runtime_error("OIDN host staging is not accessible");
          statistics.stagingBufferBytes = stagingCapacity * sizeof(float);
        }
        if (images.size() > stagingCapacity)
          throw std::logic_error("OIDN packet exceeds its host staging budget");
        if (images.size() > stagingCapacity - stagingUsed)
          finish();
        const Duration duration(statistics.stagingSeconds);
        auto *data = stagingData + stagingUsed;
        std::copy(images.begin(), images.end(), data);
        // Keep the destination until the readback retires. Callers already
        // retain its chart before enqueueing, including exception unwinding.
        pendingTransfers.push_back({images, stagingUsed});
        stagingUsed += images.size();
        return data;
      }
      void enqueue(std::span<float> images, uint32_t width, uint32_t height, uint32_t first,
                    uint32_t contentWidth, uint32_t contentHeight) {
        checkCancellation(cancellation);
        const size_t plane = size_t(width) * height * 3;
        const uint32_t count = uint32_t(images.size() / plane);
        std::array<bool, denoising::coefficientCount> active{};
        bool any = false;
        for (uint32_t slot = 0; slot < count; ++slot) {
          const auto image = images.subspan(size_t(slot) * plane, plane);
          // Preserve constant fields exactly, avoiding both neural bias and work.
          bool constant = true;
          // Ignore synthetic network padding when deciding whether real lighting
          // is constant. Such fields remain exact and never enter the network.
          for (uint32_t y = 0; y < contentHeight && constant; ++y)
            for (uint32_t x = 0; x < contentWidth && constant; ++x)
              for (uint32_t c = 0; c < 3 && constant; ++c)
                constant = std::abs(image[(size_t(y) * width + x) * 3 + c] - image[c]) <= 1e-7f;
          active[slot] = !constant;
          statistics.constantFields += uint32_t(constant);
          any |= !constant;
        }
        if (!any)
          return;
        const auto bytes = images.size() * sizeof(float);
        if (bytes > capacity) {
          finish();
          graph.invalidate();
          graphMask = 0;
          if (buffer)
            oidnReleaseBuffer(buffer);
          capacity = std::max(bytes, requiredCapacity);
          buffer = oidnNewBufferWithStorage(device, capacity, OIDN_STORAGE_DEVICE);
          check();
          statistics.imageBufferBytes = capacity;
          // A new allocation may reuse a released handle's value.
          for (auto &model : models)
            model.image = nullptr;
        }
        // Commit every binding before queuing anything. Recommitting while an
        // earlier field is running would introduce another implicit device wait.
        for (uint32_t slot = 0; slot < count; ++slot) {
          if (!active[slot])
            continue;
          checkCancellation(cancellation);
          auto &model = models[first + slot];
          const size_t offset = size_t(slot) * plane * sizeof(float);
          if (model.image == buffer && model.width == width && model.height == height &&
              model.offset == offset)
            continue;
          finish(); // retire prior uses before changing a committed binding
          graph.invalidate();
          graphMask = 0;
          const Duration duration(statistics.preparationSeconds);
          // OIDN warns that progress callbacks add significant GPU synchronization
          // overhead. Small fields already have bounded cancellation between bands.
          model.monitored =
              statistics.deviceType == OIDN_DEVICE_TYPE_CPU || uint64_t(width) * height >= (1u << 20);
          OIDNProgressMonitorFunction monitor = nullptr;
          if (model.monitored)
            monitor = [](void *user, double) {
              return !static_cast<std::stop_token *>(user)->stop_requested();
            };
          oidnSetFilterProgressMonitorFunction(model.handle, monitor,
                                               model.monitored ? &cancellation : nullptr);
          oidnSetFilterImage(model.handle, "color", buffer, OIDN_FORMAT_FLOAT3, width, height, offset, 0, 0);
          oidnSetFilterImage(model.handle, "output", buffer, OIDN_FORMAT_FLOAT3, width, height, offset, 0, 0);
          oidnCommitFilter(model.handle);
          check();
          model.image = buffer;
          model.width = width;
          model.height = height;
          model.offset = offset;
          ++statistics.preparations;
        }
        uint32_t mask = 0;
        bool monitored = false;
        for (uint32_t slot = 0; slot < count; ++slot)
          if (active[slot]) {
            mask |= 1u << (first + slot);
            monitored |= models[first + slot].monitored;
          }
        if (graph.enabled() && !monitored && (!graph.ready() || graphMask != mask)) {
          // Captured kernels keep OIDN scratch/image pointers. Finish old work
          // and capture only after every binding in this packet is committed.
          // Transfers stay outside capture and always use the same stream.
          finish();
          graph.invalidate();
          const bool captured = graph.capture([&] {
            for (uint32_t slot = 0; slot < count; ++slot)
              if (active[slot]) oidnExecuteFilterAsync(models[first + slot].handle);
          });
          const auto captureError = oidnGetDeviceError(device, nullptr);
          if (!captured || captureError != OIDN_ERROR_NONE) {
            graph.disable();
            ++statistics.graphFallbacks;
          } else {
            graphMask = mask;
            ++statistics.graphCaptures;
          }
        }
        auto *host = transferData(images);
        inFlight = true;
        {
          const Duration duration(statistics.uploadSeconds);
          oidnWriteBufferAsync(buffer, 0, bytes, host);
        }
        check();
        if (graph.ready() && !monitored && graphMask == mask) {
          const Duration duration(statistics.executionSeconds);
          graph.launch();
          ++statistics.graphLaunches;
          statistics.executions += uint32_t(std::count(active.begin(), active.begin() + count, true));
        } else {
          for (uint32_t slot = 0; slot < count; ++slot) {
            if (!active[slot]) continue;
            checkCancellation(cancellation);
            {
              const Duration duration(statistics.executionSeconds);
              oidnExecuteFilterAsync(models[first + slot].handle);
            }
            check();
            ++statistics.executions;
            statistics.monitoredExecutions += uint32_t(models[first + slot].monitored);
          }
        }
        ++statistics.batches;
        {
          const Duration duration(statistics.readbackSeconds);
          oidnReadBufferAsync(buffer, 0, bytes, host);
        }
        check();
        statistics.transferBytes += 2 * bytes;
      }
      void validate(std::span<const float> images) {
        for (const auto value : images)
          if (!std::isfinite(value))
            throw std::runtime_error("OIDN returned nonfinite lightmap data");
      }
      void run(std::span<float> images, uint32_t width, uint32_t height, uint32_t first,
                uint32_t contentWidth, uint32_t contentHeight) {
        try {
          enqueue(images, width, height, first, contentWidth, contentHeight);
          finish();
          validate(images);
        } catch (...) {
          retire(); // caller-owned host arrays must outlive even failed async copies
          throw;
        }
      }
    };
  } // namespace
#endif
  DenoiseStatistics denoise(const Layout &layout, std::span<bake::Result> results, std::stop_token cancelled,
                            std::function<void(uint32_t, uint32_t)> progress, HostTransfers transfers, FilterDispatch dispatch) {
#ifndef REMIX_OIDN
    throw std::runtime_error("Lightmap denoising requires a build configured with oidn_root");
#else
    const auto start = std::chrono::steady_clock::now();
    if (results.size() != layout.points.size() || layout.pointCharts.size() != results.size() ||
        layout.addresses.size() != results.size() ||
        layout.owners.size() != uint64_t(layout.pageSize) * layout.pageSize * layout.pages)
      throw std::invalid_argument("Incomplete lightmap denoising layout");
    DenoiseStatistics statistics;
    if (results.empty())
      return statistics;
    checkCancellation(cancelled);
    std::vector<uint32_t> order(layout.charts.size());
    std::iota(order.begin(), order.end(), 0u);
    size_t largestImage = 0;
    for (const auto &a : layout.charts) {
      if (!a.width || !a.height || a.page >= layout.pages || uint64_t(a.x) + a.width > layout.pageSize ||
          uint64_t(a.y) + a.height > layout.pageSize)
        throw std::invalid_argument("Invalid denoising allocation");
      const auto [width, height] = imageShape(a);
      const size_t fieldBytes = size_t(width) * height * 3 * sizeof(float);
      largestImage = std::max(largestImage, fieldBytes * denoising::batchFields(fieldBytes));
    }
    // Chart-local donors make charts independent. Group equal image shapes so
    // the same committed network can serve all of them without re-preparation.
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
      const auto &first = layout.charts[a], &second = layout.charts[b];
      return imageShape(first) > imageShape(second);
    });
    struct PendingChart {
      std::unique_ptr<denoising::Chart> image;
      std::span<float> fields;
    };
    // Declaration order matters: Filter retires copies before pending host
    // arrays are destroyed, including on cancellation or an exception.
    std::vector<PendingChart> pending;
    Filter filter(statistics, largestImage, transfers, dispatch);
    filter.initialize(cancelled);
    statistics.deviceType = oidnGetDeviceInt(filter.device, "type");
    filter.check();
    std::vector<std::vector<uint32_t>> chartPoints(layout.charts.size());
    for (uint32_t i = 0; i < results.size(); ++i) {
      if (layout.pointCharts[i] >= chartPoints.size())
        throw std::invalid_argument("Invalid denoising chart");
      chartPoints[layout.pointCharts[i]].push_back(i);
    }
    const auto total = uint32_t(std::count_if(chartPoints.begin(), chartPoints.end(),
                                              [](const auto &points) { return !points.empty(); }));
    if (progress)
      progress(0, total);
    uint64_t pendingBytes = 0;
    auto flush = [&] {
      filter.finish();
      for (auto &chart : pending) {
        const Duration duration(statistics.chartSeconds);
        filter.validate(chart.fields);
        chart.image->apply(0, denoising::coefficientCount, chart.fields);
        chart.image->finish();
        ++statistics.charts;
        if (progress)
          progress(statistics.charts, total);
      }
      pending.clear();
      pendingBytes = 0;
    };
    uint32_t pendingWidth = 0, pendingHeight = 0;
    for (const auto chart : order) {
      checkCancellation(cancelled);
      if (chartPoints[chart].empty())
        continue;
      const auto &a = layout.charts[chart];
      const auto [width, height] = imageShape(a);
      const auto fieldBytes = uint64_t(width) * height * 3 * sizeof(float);
      const auto batch = denoising::batchFields(size_t(fieldBytes));
      const auto chartBytes = fieldBytes * (2 + batch);
      if (!pending.empty() && (width != pendingWidth || height != pendingHeight ||
                               pendingBytes + chartBytes > denoising::queueBudgetBytes ||
                               pending.size() >= denoising::queuedChartLimit))
        flush();
      std::unique_ptr<denoising::Chart> image;
      {
        const Duration duration(statistics.chartSeconds);
        image = std::make_unique<denoising::Chart>(layout, chart, results, chartPoints[chart], cancelled,
                                                  imageAlignment);
      }
      statistics.texels += uint64_t(a.width) * a.height;
      statistics.peakHostBytes = std::max(statistics.peakHostBytes, pendingBytes + image->hostBytes());
      if (batch == denoising::coefficientCount) {
        pendingWidth = width;
        pendingHeight = height;
        std::span<float> fields;
        {
          const Duration duration(statistics.chartSeconds);
          fields = image->pack(0, batch);
        }
        pendingBytes += image->hostBytes();
        pending.push_back({std::move(image), fields}); // retain arrays before any async copy
        filter.enqueue(fields, width, height, 0, a.width, a.height);
        continue;
      }
      flush();
      for (uint32_t first = 0; first < denoising::coefficientCount; first += batch) {
        checkCancellation(cancelled);
        const auto count = std::min(batch, denoising::coefficientCount - first);
        std::span<float> fields;
        {
          const Duration duration(statistics.chartSeconds);
          fields = image->pack(first, count);
        }
        filter.run(fields, width, height, first, a.width, a.height);
        {
          const Duration duration(statistics.chartSeconds);
          image->apply(first, count, fields);
        }
      }
      {
        const Duration duration(statistics.chartSeconds);
        image->finish();
      }
      ++statistics.charts;
      if (progress)
        progress(statistics.charts, total);
    }
    flush();
    statistics.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return statistics;
#endif
  }
} // namespace remix::forward::lightmap
