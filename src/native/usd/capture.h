#pragma once
#include <remix/remix_c.h>
#include <string>

namespace remix {
  class NativeResources;
}
namespace remix::usd {
  // Single-frame USD scene capture (docs/features/usd-capture.md): what the
  // host submits in the frame after a request, written in the Remix capture
  // layout off the API thread. The draw hooks record what was submitted
  // before mods replace it, so the capture keeps the game's own hashes.
  void requestCapture();
  // The next captures' stage name (empty: capture_<date>_<time>). Characters
  // that cannot be in a file name become '_'; an existing capture of that name
  // is kept, the new one gets the time appended.
  void setCaptureName(const std::string &name);
  // Draw hooks (API thread, under the API mutex). Cheap no-ops unless a frame
  // is being recorded.
  void captureInstance(const remixapi_InstanceInfo &info);
  void captureLight(remixapi_LightHandle handle);
  void captureSceneInstances(const NativeResources &resources, const float *origin);
  // At each Present, beside modBoundary: starts a requested recording, and
  // resolves a finished one into a capture to write.
  void captureBoundary(const NativeResources &resources);

  struct CaptureStatus {
    enum class State { Idle, Requested, Recording, Writing, Complete, Failed } state = State::Idle;
    std::string directory; // where captures are written
    std::string name;      // setCaptureName's
    std::string path;      // the last capture's stage
    std::string message;   // its warnings, or why it failed
  };
  CaptureStatus captureStatus();
} // namespace remix::usd
