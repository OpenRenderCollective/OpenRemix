#include "forward/lightmap/denoise_graph.h"
#if defined(REMIX_OIDN) && defined(_WIN32)
#include <OpenImageDenoise/oidn.h>
#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <stdexcept>

namespace remix::forward::lightmap::denoising {
  struct CudaGraph::State {
    // Stable CUDA driver ABI, loaded from the installed driver. No toolkit or
    // redistributable CUDA runtime is added to the renderer's dependencies.
    using Context = struct CUctx_st *;
    using Stream = cudaStream_t;
    using Graph = struct CUgraph_st *;
    using Executable = struct CUgraphExec_st *;
    struct Uuid { char bytes[16]; };
    HMODULE library = nullptr;
    Context context = nullptr;
    Stream stream = nullptr;
    Executable graph = nullptr;
    int ordinal = -1;
    bool disabled = false;
    int (WINAPI *init)(unsigned) = nullptr;
    int (WINAPI *deviceCount)(int *) = nullptr;
    int (WINAPI *deviceUuid)(Uuid *, int) = nullptr;
    int (WINAPI *retainContext)(Context *, int) = nullptr;
    int (WINAPI *releaseContext)(int) = nullptr;
    int (WINAPI *pushContext)(Context) = nullptr;
    int (WINAPI *popContext)(Context *) = nullptr;
    int (WINAPI *createStream)(Stream *, unsigned) = nullptr;
    int (WINAPI *destroyStream)(Stream) = nullptr;
    int (WINAPI *syncStream)(Stream) = nullptr;
    int (WINAPI *beginCapture)(Stream, int) = nullptr;
    int (WINAPI *endCapture)(Stream, Graph *) = nullptr;
    int (WINAPI *graphNodes)(Graph, void **, size_t *) = nullptr;
    int (WINAPI *instantiate)(Executable *, Graph, void *, char *, size_t) = nullptr;
    int (WINAPI *destroyGraph)(Graph) = nullptr;
    int (WINAPI *destroyExecutable)(Executable) = nullptr;
    int (WINAPI *launchGraph)(Executable, Stream) = nullptr;
    int (WINAPI *copyDevice)(uint64_t, uint64_t, size_t, Stream) = nullptr;
    struct Current {
      State &state;
      bool valid;
      explicit Current(State &state) : state(state), valid(state.pushContext(state.context) == 0) {}
      ~Current() {
        if (valid) {
          Context previous = nullptr;
          state.popContext(&previous);
        }
      }
    };
    ~State() {
      if (context) {
        {
          Current current(*this);
          if (current.valid) {
            if (stream) syncStream(stream);
            if (graph) destroyExecutable(graph);
            if (stream) destroyStream(stream);
          }
        }
        releaseContext(ordinal);
      }
      if (library) FreeLibrary(library);
    }
    template <typename T> bool load(T &function, const char *name) {
      function = reinterpret_cast<T>(GetProcAddress(library, name));
      return function != nullptr;
    }
    bool initialize() {
      // Preserve explicit OIDN device selection, including CPU/reference runs.
      if (std::getenv("OIDN_DEFAULT_DEVICE") || oidnGetNumPhysicalDevices() < 1 ||
          oidnGetPhysicalDeviceInt(0, "type") != OIDN_DEVICE_TYPE_CUDA ||
          !oidnGetPhysicalDeviceBool(0, "uuidSupported"))
        return false;
      size_t uuidBytes = 0;
      const void *uuid = oidnGetPhysicalDeviceData(0, "uuid", &uuidBytes);
      if (!uuid || uuidBytes != sizeof(Uuid)) return false;
      library = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!library || !load(init, "cuInit") || !load(deviceCount, "cuDeviceGetCount") ||
          !load(deviceUuid, "cuDeviceGetUuid") ||
          !load(retainContext, "cuDevicePrimaryCtxRetain") ||
          !load(releaseContext, "cuDevicePrimaryCtxRelease_v2") ||
          !load(pushContext, "cuCtxPushCurrent_v2") || !load(popContext, "cuCtxPopCurrent_v2") ||
          !load(createStream, "cuStreamCreate") || !load(destroyStream, "cuStreamDestroy_v2") ||
          !load(syncStream, "cuStreamSynchronize") ||
          !load(beginCapture, "cuStreamBeginCapture_v2") || !load(endCapture, "cuStreamEndCapture") ||
          !load(graphNodes, "cuGraphGetNodes") || !load(instantiate, "cuGraphInstantiate_v2") ||
          !load(destroyGraph, "cuGraphDestroy") ||
          !load(destroyExecutable, "cuGraphExecDestroy") || !load(launchGraph, "cuGraphLaunch") ||
          !load(copyDevice, "cuMemcpyDtoDAsync_v2") || init(0))
        return false;
      int count = 0;
      if (deviceCount(&count)) return false;
      for (int i = 0; i < count; ++i) {
        Uuid candidate{};
        if (!deviceUuid(&candidate, i) && std::memcmp(candidate.bytes, uuid, sizeof(candidate)) == 0) {
          ordinal = i;
          break;
        }
      }
      if (ordinal < 0 || !oidnIsCUDADeviceSupported(ordinal) || retainContext(&context, ordinal)) return false;
      Current current(*this);
      return current.valid && createStream(&stream, 1 /* nonblocking */) == 0;
    }
  };
  CudaGraph::CudaGraph() : m_state(std::make_unique<State>()) {}
  CudaGraph::~CudaGraph() = default;
  OIDNDeviceImpl *CudaGraph::createDevice() {
    if (!m_state->initialize()) {
      oidnGetDeviceError(nullptr, nullptr); // optional interop must not poison ordinary creation
      disable();
      return nullptr;
    }
    auto device = oidnNewCUDADevice(&m_state->ordinal, &m_state->stream, 1);
    const auto error = oidnGetDeviceError(device, nullptr);
    if (!device || error != OIDN_ERROR_NONE) {
      if (device) oidnReleaseDevice(device);
      disable();
      return nullptr;
    }
    return device;
  }
  bool CudaGraph::enabled() const { return m_state->stream && !m_state->disabled; }
  bool CudaGraph::ready() const { return enabled() && m_state->graph; }
  void CudaGraph::invalidate() noexcept {
    if (!m_state->graph) return;
    State::Current current(*m_state);
    if (current.valid) m_state->destroyExecutable(m_state->graph);
    m_state->graph = nullptr;
  }
  void CudaGraph::disable() noexcept {
    invalidate();
    m_state->disabled = true;
  }
  bool CudaGraph::capture(const std::function<void()> &record) {
    if (!enabled()) return false;
    State::Current current(*m_state);
    if (!current.valid || m_state->beginCapture(m_state->stream, 1 /* thread local */)) {
      disable();
      return false;
    }
    State::Graph captured = nullptr;
    try {
      record();
    } catch (...) {
      m_state->endCapture(m_state->stream, &captured);
      if (captured) m_state->destroyGraph(captured);
      m_state->disabled = true;
      throw;
    }
    const int ended = m_state->endCapture(m_state->stream, &captured);
    size_t nodes = 0;
    const int inspected = ended || !captured ? -1 : m_state->graphNodes(captured, nullptr, &nodes);
    // An ignored interoperability stream must never become a successful no-op.
    const int instantiated = inspected || !nodes ? -1 :
        m_state->instantiate(&m_state->graph, captured, nullptr, nullptr, 0);
    if (captured) m_state->destroyGraph(captured);
    if (ended || instantiated) {
      disable();
      return false;
    }
    return true;
  }
  void CudaGraph::launch() {
    State::Current current(*m_state);
    if (!ready() || !current.valid || m_state->launchGraph(m_state->graph, m_state->stream))
      throw std::runtime_error("Cannot submit CUDA denoising graph");
  }
  bool CudaGraph::copyDevice(void *destination, const void *source, size_t bytes) {
    if (!enabled()) return false;
    State::Current current(*m_state);
    return current.valid &&
        m_state->copyDevice(uint64_t(reinterpret_cast<uintptr_t>(destination)),
                            uint64_t(reinterpret_cast<uintptr_t>(source)), bytes, m_state->stream) == 0;
  }
} // namespace remix::forward::lightmap::denoising
#else
namespace remix::forward::lightmap::denoising {
  struct CudaGraph::State {};
  CudaGraph::CudaGraph() = default;
  CudaGraph::~CudaGraph() = default;
  OIDNDeviceImpl *CudaGraph::createDevice() { return nullptr; }
  bool CudaGraph::enabled() const { return false; }
  bool CudaGraph::ready() const { return false; }
  bool CudaGraph::capture(const std::function<void()> &) { return false; }
  void CudaGraph::launch() {}
  bool CudaGraph::copyDevice(void *, const void *, size_t) { return false; }
  void CudaGraph::invalidate() noexcept {}
  void CudaGraph::disable() noexcept {}
} // namespace remix::forward::lightmap::denoising
#endif
