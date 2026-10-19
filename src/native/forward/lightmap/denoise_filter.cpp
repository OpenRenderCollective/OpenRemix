#include "forward/lightmap/denoise_filter.h"
#include "forward/lightmap/denoise_chart.h"
#include "forward/lightmap/denoise_graph.h"
#ifdef REMIX_OIDN
#include <OpenImageDenoise/oidn.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>

namespace remix::forward::lightmap::denoising {
  struct Filter::Impl {
    struct Model {
      OIDNFilter handle = nullptr;
      uint32_t width = 0, height = 0;
      OIDNBuffer image = nullptr;
      size_t offset = 0;
      bool monitored = false;
    };
    denoising::CudaGraph graph;
    uint32_t graphMask = 0;
    std::vector<uint32_t> graphBatchMasks;
    FilterDispatch dispatch;
    FilterDevice selectedDevice;
    uint32_t cpuThreads;
    OIDNDevice device = nullptr;
    std::array<Model, denoising::coefficientCount> models{}; // one immutable image binding per SH band
    OIDNBuffer buffer = nullptr;
    OIDNBuffer staging = nullptr;
    OIDNBuffer batchBuffer = nullptr;
    void *batchData = nullptr;
    bool batchingAvailable = true;
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
    Impl(DenoiseStatistics &statistics, size_t requiredCapacity, HostTransfers hostTransfers,
         FilterDispatch dispatch, FilterDevice selectedDevice, uint32_t cpuThreads)
        : dispatch(dispatch), selectedDevice(selectedDevice), cpuThreads(cpuThreads),
          hostTransfers(hostTransfers), requiredCapacity(requiredCapacity), statistics(statistics) {}
    ~Impl() {
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
      if (batchBuffer)
        oidnReleaseBuffer(batchBuffer);
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
      if (selectedDevice == FilterDevice::Cpu) {
        device = oidnNewDevice(OIDN_DEVICE_TYPE_CPU);
        const auto error = oidnGetDeviceError(device, nullptr);
        if (error != OIDN_ERROR_NONE || !device) {
          if (device) oidnReleaseDevice(device);
          device = nullptr;
          oidnGetDeviceError(nullptr, nullptr);
        }
      } else if (dispatch == FilterDispatch::Automatic) device = graph.createDevice();
      if (!device) device = oidnNewDevice(OIDN_DEVICE_TYPE_DEFAULT);
      check();
      if (!device)
        throw std::runtime_error("No OIDN lightmap device");
      if (cpuThreads) {
        if (cpuThreads > 32)
          throw std::invalid_argument("CPU denoising thread limit outside its budget");
        if (oidnGetDeviceInt(device, "type") != OIDN_DEVICE_TYPE_CPU)
          throw std::runtime_error("CPU chart queues require a CPU denoising device");
        oidnSetDeviceInt(device, "numThreads", int(cpuThreads));
      }
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
    std::array<bool, denoising::coefficientCount> activeFields(std::span<const float> images,
        uint32_t width, uint32_t height, uint32_t contentWidth, uint32_t contentHeight) {
      const size_t plane = size_t(width) * height * 3;
      const uint32_t count = uint32_t(images.size() / plane);
      std::array<bool, denoising::coefficientCount> active{};
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
      }
      return active;
    }
    void prepareImages(size_t bytes, uint32_t width, uint32_t height, uint32_t first,
                         uint32_t count, const std::array<bool, denoising::coefficientCount> &active) {
      const size_t plane = size_t(width) * height * 3;
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
    }
    void enqueue(std::span<float> images, uint32_t width, uint32_t height, uint32_t first,
                  uint32_t contentWidth, uint32_t contentHeight) {
      if (!graphBatchMasks.empty()) {
        finish();
        graph.invalidate();
        graphBatchMasks.clear();
        graphMask = 0;
      }
      checkCancellation(cancellation);
      const size_t plane = size_t(width) * height * 3;
      const uint32_t count = uint32_t(images.size() / plane);
      const auto active = activeFields(images, width, height, contentWidth, contentHeight);
      if (std::none_of(active.begin(), active.end(), [](bool value) { return value; })) return;
      const auto bytes = images.size() * sizeof(float);
      prepareImages(bytes, width, height, first, count, active);
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
    bool canBatch(uint32_t width, uint32_t height) const {
      return batchingAvailable && graph.enabled() && hostTransfers == HostTransfers::Pinned &&
             statistics.deviceType != OIDN_DEVICE_TYPE_CPU && uint64_t(width) * height < (1u << 20);
    }
    bool fallbackBatch(uint32_t previousConstants) {
      finish();
      graph.invalidate();
      graphBatchMasks.clear();
      graphMask = 0;
      batchingAvailable = false;
      if (batchBuffer) oidnReleaseBuffer(batchBuffer);
      batchBuffer = nullptr;
      batchData = nullptr;
      statistics.batchBufferBytes = 0;
      statistics.constantFields = previousConstants;
      ++statistics.graphFallbacks;
      return false;
    }
    bool enqueueCharts(std::span<const Packet> packets, uint32_t width, uint32_t height) {
      if (packets.empty()) return true;
      if (!canBatch(width, height)) return false;
      const size_t plane = size_t(width) * height * 3;
      const size_t bytes = plane * denoising::coefficientCount * sizeof(float);
      if (bytes > denoising::queueBudgetBytes / packets.size()) return false;
      finish(); // pinned offsets and graph bindings cannot change in flight
      const auto previousConstants = statistics.constantFields;
      std::array<bool, denoising::coefficientCount> unionActive{};
      std::vector<uint32_t> masks;
      uint32_t executions = 0, batches = 0;
      for (const auto &packet : packets) {
        checkCancellation(cancellation);
        const auto active = activeFields(packet.fields, width, height, packet.contentWidth, packet.contentHeight);
        uint32_t mask = 0;
        for (uint32_t band = 0; band < active.size(); ++band)
          if (active[band]) {
            mask |= 1u << band;
            unionActive[band] = true;
            ++executions;
          }
        masks.push_back(mask);
        batches += uint32_t(mask != 0);
      }
      if (!batches) return true;
      prepareImages(bytes, width, height, 0, denoising::coefficientCount, unionActive);
      if (!batchBuffer) {
        batchBuffer = oidnNewBufferWithStorage(device, denoising::queueBudgetBytes, OIDN_STORAGE_DEVICE);
        const char *message = nullptr;
        const auto error = oidnGetDeviceError(device, &message);
        if (!batchBuffer && error == OIDN_ERROR_OUT_OF_MEMORY) return fallbackBatch(previousConstants);
        if (error != OIDN_ERROR_NONE)
          throw std::runtime_error(std::string("OIDN batch storage: ") + (message ? message : "unknown error"));
        batchData = oidnGetBufferData(batchBuffer);
        check();
        statistics.batchBufferBytes = denoising::queueBudgetBytes;
      }
      void *imageData = oidnGetBufferData(buffer);
      check();
      if (!batchData || !imageData) return fallbackBatch(previousConstants);
      std::vector<float *> host;
      for (size_t i = 0; i < packets.size(); ++i)
        host.push_back(masks[i] ? transferData(packets[i].fields) : nullptr);
      if (hostTransfers != HostTransfers::Pinned || !stagingData) return fallbackBatch(previousConstants);
      if (!graph.ready() || graphBatchMasks != masks) {
        graph.invalidate();
        bool copyFailed = false;
        const bool captured = graph.capture([&] {
          for (size_t i = 0; i < packets.size(); ++i) {
            if (!masks[i]) continue;
            checkCancellation(cancellation);
            // Each chart keeps an independent filter image and ordered result
            // copy. Host transfers surround the complete bounded graph.
            auto *packetData = static_cast<std::byte *>(batchData) + (host[i] - stagingData) * sizeof(float);
            if (!graph.copyDevice(imageData, packetData, bytes)) { copyFailed = true; return; }
            for (uint32_t band = 0; band < denoising::coefficientCount; ++band)
              if (masks[i] & (1u << band)) oidnExecuteFilterAsync(models[band].handle);
            if (!graph.copyDevice(packetData, imageData, bytes)) { copyFailed = true; return; }
          }
        });
        const auto error = oidnGetDeviceError(device, nullptr);
        if (copyFailed || !captured || error != OIDN_ERROR_NONE) {
          graph.disable();
          return fallbackBatch(previousConstants); // captured operations never executed
        }
        graphBatchMasks = masks;
        graphMask = 0;
        ++statistics.graphCaptures;
      }
      checkCancellation(cancellation);
      inFlight = true;
      {
        const Duration duration(statistics.uploadSeconds);
        oidnWriteBufferAsync(batchBuffer, 0, stagingUsed * sizeof(float), stagingData);
      }
      check();
      {
        const Duration duration(statistics.executionSeconds);
        graph.launch();
      }
      check();
      {
        const Duration duration(statistics.readbackSeconds);
        oidnReadBufferAsync(batchBuffer, 0, stagingUsed * sizeof(float), stagingData);
      }
      check();
      ++statistics.graphLaunches;
      statistics.executions += executions;
      statistics.batches += batches;
      statistics.transferBytes += 2 * bytes * batches;
      return true;
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

  Filter::Filter(DenoiseStatistics &statistics, size_t requiredCapacity, HostTransfers transfers,
                 FilterDispatch dispatch, FilterDevice device, uint32_t cpuThreads)
      : m_impl(
            std::make_unique<Impl>(statistics, requiredCapacity, transfers, dispatch, device, cpuThreads)) {}
  Filter::~Filter() = default;
  void Filter::initialize(std::stop_token cancelled) {
    m_impl->initialize(cancelled);
    m_impl->statistics.deviceType = oidnGetDeviceInt(m_impl->device, "type");
    m_impl->check();
  }
  void Filter::enqueue(std::span<float> images, uint32_t width, uint32_t height, uint32_t first,
                        uint32_t contentWidth, uint32_t contentHeight) {
    m_impl->enqueue(images, width, height, first, contentWidth, contentHeight);
  }
  bool Filter::canBatch(uint32_t width, uint32_t height) const { return m_impl->canBatch(width, height); }
  bool Filter::enqueueCharts(std::span<const Packet> packets, uint32_t width, uint32_t height) {
    return m_impl->enqueueCharts(packets, width, height);
  }
  void Filter::finish() { m_impl->finish(); }
  void Filter::validate(std::span<const float> images) { m_impl->validate(images); }
  void Filter::run(std::span<float> images, uint32_t width, uint32_t height, uint32_t first,
                    uint32_t contentWidth, uint32_t contentHeight) {
    m_impl->run(images, width, height, first, contentWidth, contentHeight);
  }
} // namespace remix::forward::lightmap::denoising
#endif
