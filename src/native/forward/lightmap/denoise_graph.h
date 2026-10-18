#pragma once
#include <functional>
#include <memory>

struct OIDNDeviceImpl;
namespace remix::forward::lightmap::denoising {
  // Optional CUDA submission optimization. A graph contains only immutable
  // filter bindings; uploads/readbacks and chart boundaries stay independent.
  // Other devices/platforms retain ordinary OIDN execution without CUDA SDKs.
  class CudaGraph {
  public:
    CudaGraph();
    ~CudaGraph();
    OIDNDeviceImpl *createDevice(); // uncommitted; caller owns the OIDN reference
    bool enabled() const;
    bool ready() const;
    bool capture(const std::function<void()> &record);
    void launch();
    // Caller retires previous submissions before replacing filter storage or
    // bindings. Destroy graphs before releasing their referenced OIDN resources.
    void invalidate() noexcept;
    void disable() noexcept;
  private:
    struct State;
    std::unique_ptr<State> m_state;
  };
} // namespace remix::forward::lightmap::denoising
