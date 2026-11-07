#include "api/state.h"
#include <remix/remix_usd_ext.h>
#if REMIX_HAS_USD
#include "usd/mods.h"
#include "usd/submission.h"
#include <atomic>
#include <cstring>
#include <future>
#include <map>
#include <optional>
namespace {
  struct Document {
    remix::usd::Options options;
    remix::usd::Snapshot snapshot;
    std::unique_ptr<remix::usd::Submission> submission;
    // Failures are returned in-band (see usd/mods.cpp) rather than through
    // the future's exception_ptr.
    std::future<std::pair<remix::usd::Snapshot, std::optional<std::string>>> pending;
    uint64_t generation = 1;
    std::string diagnostic;
  };
  void publish(Document &d, remix::NativeResources &resources, uint64_t handle) {
    try {
      auto [snapshot, error] = d.pending.get();
      if (error)
        throw std::runtime_error(*error);
      resources.invalidateTexturePaths();
      auto submission =
          std::make_unique<remix::usd::Submission>(resources, snapshot, (handle << 32) + (d.generation + 1));
      d.snapshot = std::move(snapshot);
      d.submission = std::move(submission);
      ++d.generation;
      d.diagnostic.clear();
    } catch (const std::exception &e) {
      d.diagnostic = e.what();
    } catch (...) {
      d.diagnostic = "USD replacement resource creation failed; retaining previous scene";
    }
  }
  std::mutex documentsMutex;
  std::map<uint64_t, std::shared_ptr<Document>> documents;
  std::atomic<uint64_t> nextDocument{1};
  thread_local std::string lastLoadDiagnostic;
  std::shared_ptr<Document> document(uint64_t handle) {
    auto it = documents.find(handle);
    return it == documents.end() ? nullptr : it->second;
  }
} // namespace
#endif
extern "C" {
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_LoadUSDSceneEXT(const remixapi_USDLoadOptionsEXT *options,
                                                                   remixapi_USDSceneHandleEXT *handle) {
  if (handle)
    *handle = 0;
  if (!options || !handle || options->structSize != sizeof(*options) || options->version != 1 || !options->path)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
#if REMIX_HAS_USD
  try {
    lastLoadDiagnostic.clear();
    auto d = std::make_shared<Document>();
    d->options = {options->path, options->hostMetersPerUnit, options->hostUpAxis, true};
    d->snapshot = remix::usd::load(d->options);
    std::lock_guard lock(documentsMutex);
    *handle = nextDocument++;
    documents[*handle] = std::move(d);
    return REMIXAPI_ERROR_CODE_SUCCESS;
  } catch (const std::exception &e) {
    lastLoadDiagnostic = e.what();
    std::fprintf(stderr, "USD load: %s\n", e.what());
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
#else
  return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
#endif
}
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_DrawUSDSceneEXT(remixapi_USDSceneHandleEXT handle,
                                                                   const remixapi_USDDrawOptionsEXT *options) {
  if (!options || options->structSize != sizeof(*options) || options->version != 1)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
#if REMIX_HAS_USD
  std::lock_guard documentsLock(documentsMutex);
  auto d = document(handle);
  if (!d)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  return remix::api::resourceCall([&](auto &resources) {
    // Publish only before this scene's first draw at a frame boundary.
    if (resources.pendingDrawCount() == 0 && d->pending.valid() &&
        d->pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      publish(*d, resources, handle);
    }
    if (!d->submission)
      d->submission = std::make_unique<remix::usd::Submission>(resources, d->snapshot, (handle << 32) + d->generation);
    d->submission->draw(options->useStageCamera, options->aspectRatio);
  });
#else
  return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
#endif
}
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_ReloadUSDSceneEXT(remixapi_USDSceneHandleEXT handle) {
#if REMIX_HAS_USD
  std::lock_guard lock(documentsMutex);
  auto d = document(handle);
  if (!d)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  if (d->pending.valid())
    return REMIXAPI_ERROR_CODE_ALREADY_EXISTS;
  try {
    d->pending = std::async(std::launch::async, [options = d->options] {
      try {
        return std::pair{remix::usd::load(options), std::optional<std::string>{}};
      } catch (const std::exception &e) {
        return std::pair{remix::usd::Snapshot{}, std::optional<std::string>{e.what()}};
      }
    });
    return REMIXAPI_ERROR_CODE_SUCCESS;
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
#else
  return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
#endif
}
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_DestroyUSDSceneEXT(remixapi_USDSceneHandleEXT handle) {
#if REMIX_HAS_USD
  std::shared_ptr<Document> removed;
  {
    std::lock_guard lock(documentsMutex);
    auto d = document(handle);
    if (!d)
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    std::lock_guard apiLock(remix::api::g_apiMutex);
    d->submission.reset();
    removed = std::move(d);
    documents.erase(handle);
  }
  // Async parser joins outside the API mutex.
  return REMIXAPI_ERROR_CODE_SUCCESS;
#else
  return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
#endif
}
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetUSDDiagnosticsEXT(remixapi_USDSceneHandleEXT handle,
                                                                        remixapi_USDDiagnosticsEXT *out) {
  if (!out || out->structSize != sizeof(*out))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
#if REMIX_HAS_USD
  if (!handle) {
    *out = {.structSize = sizeof(*out), .version = 1};
    std::strncpy(out->message, lastLoadDiagnostic.c_str(), sizeof(out->message) - 1);
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }
  std::lock_guard lock(documentsMutex);
  auto d = document(handle);
  if (!d)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  const auto &s = d->snapshot.scene;
  *out = {.structSize = sizeof(*out),
          .version = 1,
          .meshes = s["meshes"].size(),
          .materials = s["materials"].size(),
          .lights = s["lights"].size(),
          .cameras = s["cameras"].size(),
          .generation = d->generation,
          .metersPerUnit = s["meters_per_unit"].get<double>(),
          .upAxis = s["up_axis"] == "Y" ? 1u : 2u,
          .warnings = uint32_t(s["warnings"].size())};
  auto message = d->diagnostic;
  for (const auto &w : s["warnings"]) {
    if (!message.empty())
      message += '\n';
    message += w.get<std::string>();
    if (message.size() > sizeof(out->message))
      break;
  }
  std::strncpy(out->message, message.c_str(), sizeof(out->message) - 1);
  return REMIXAPI_ERROR_CODE_SUCCESS;
#else
  return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
#endif
}
}

extern "C" REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_ConfigureUSDModsEXT(const remixapi_USDModsOptionsEXT *options) {
  if (!options || options->structSize != sizeof(*options) || options->version != 1)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
#if REMIX_HAS_USD
  std::lock_guard lock(remix::api::g_apiMutex);
  return remix::usd::configureMods(options->directory, options->hostMetersPerUnit, options->hostUpAxis, options->watch);
#else
  return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
#endif
}
extern "C" REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetUSDModsDiagnosticEXT(char *out, uint32_t capacity) {
  if (!out || capacity < 1)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
#if REMIX_HAS_USD
  std::lock_guard lock(remix::api::g_apiMutex);
  std::strncpy(out, remix::usd::modDiagnostic(), capacity - 1);
  out[capacity - 1] = 0;
  return REMIXAPI_ERROR_CODE_SUCCESS;
#else
  out[0] = 0;
  return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
#endif
}

namespace remix::api {
  void publishUsdReloads() {
#if REMIX_HAS_USD
    std::lock_guard documentsLock(documentsMutex);
    resourceCall([&](auto &resources) {
      for (auto &[handle, d] : documents)
        if (d->pending.valid() && d->pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
          publish(*d, resources, handle);
    });
#endif
  }
  void releaseUsdSubmissions() {
#if REMIX_HAS_USD
    std::scoped_lock lock(documentsMutex, g_apiMutex);
    for (auto &[id, document] : documents)
      document->submission.reset();
#endif
  }
} // namespace remix::api
