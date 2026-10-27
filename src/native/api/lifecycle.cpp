#include "api/state.h"
#include "scene/light_bvh.h"
#include "scene/particles.h"
#if REMIX_HAS_USD
#include "usd/capture.h"
#include "usd/mods.h"
#endif
#include "api/entrypoints.h"
#include "pathtrace/volume_policy.h"
#include "platform/gpu_timings.h"
#include "runtime/profiles.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace remix::api;

extern "C" {

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_Startup(const remixapi_StartupInfo *info) {
  if (info == nullptr || info->sType != REMIXAPI_STRUCT_TYPE_STARTUP_INFO) {
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  }
  try {
    std::lock_guard lock(g_apiMutex);
    const remixapi_ErrorCode status = g_runtime.initialize(*info);
    if (status == REMIXAPI_ERROR_CODE_SUCCESS) {
      g_frameExitStages = 0;
#if REMIX_HAS_USD
      try {
        remix::usd::startMods(g_runtime.modsDisabled());
      } catch (...) {
        remix::usd::stopMods(g_runtime.raster());
        g_runtime.shutdown();
        return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
      }
#endif
    }
#if !defined(_WIN32)
    // Register after SDL startup so this runs before SDL's process-exit window cleanup.
    if (status == REMIXAPI_ERROR_CODE_SUCCESS) {
      if (std::atexit([] { remixapi_Shutdown(); }) != 0) {
#if REMIX_HAS_USD
        remix::usd::stopMods(g_runtime.raster());
#endif
        g_runtime.shutdown();
        return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
      }
    }
#endif
    return status;
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_Shutdown() {
  try {
    releaseUsdSubmissions();
    std::lock_guard lock(g_apiMutex);
#if REMIX_HAS_USD
    remix::usd::stopMods(g_runtime.raster());
#endif
    remixapi_ErrorCode status = g_runtime.shutdown();
    if (status == REMIXAPI_ERROR_CODE_SUCCESS) {
      remix::scene::particles().reset();
      g_beginSceneCallback = nullptr;
      g_endSceneCallback = nullptr;
      g_presentCallback = nullptr;
    }
    return status;
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
}

namespace {
// Dev aid for unattended testing, polled every 30 presents. Files in the
// profile directory (REMIX_CONFIG_DIR, else ./openremix):
//   config.request     "key = value" lines applied as SetConfigVariable
//   screenshot.request presented frame to screenshot.png
//   timings.request    GPU pass timings (rtx.native.gpuPassTimers) to timings.txt
//   pipelines.request  every pipeline compile and cache save to pipelines.txt
//   lighttree.request  the next frame's combined light tree to lighttree.bin
//                      (LightBvh::save)
//   sequence.request   N (default 60) consecutive captures to sequence/NNN.png,
//                      one per present while the previous one has completed
// Each request file is deleted when served.
// The host's frame pacing as Present sees it, since the last timings dump:
// the intervals between Present calls and where the calls' own time went.
// Caller's thread only (Present and the dump it serves).
struct PresentPacing {
  std::chrono::steady_clock::time_point last{};
  std::vector<float> intervals; // milliseconds
  // One call's parts: dev requests, waiting for the API mutex, the runtime's
  // Present (with the wait for the render thread), the low latency sleep,
  // USD mods and what follows the mutex.
  // interval: from this call's start to the next one's, so the call's own
  // time and the host's frame after it.
  struct Call {
    double total = 0, requests = 0, mutex = 0, present = 0, sleep = 0, mods = 0, after = 0, interval = 0;
    double wait = 0; // of present: waiting for the render thread's previous frame
    remix::RemixNativeRuntime::WaitedFrame waited; // that frame, on the render thread
    // Of the host's part of the interval: inside OpenRemix's resource calls
    // (CreateTexture, CreateMesh, DrawInstance, ...), and the longest of them.
    double api = 0, apiLongest = 0;
    const char *apiLongestName = "";
  } worst, sum, previous;
  bool hasPrevious = false;
  // The frames that took long (over 40 ms), in order, up to a limit.
  std::vector<Call> slow;
  uint64_t calls = 0;
  void add(const Call &call) {
    ++calls;
    sum.total += call.total;
    sum.requests += call.requests;
    sum.mutex += call.mutex;
    sum.present += call.present;
    sum.wait += call.wait;
    sum.api += call.api;
    sum.sleep += call.sleep;
    sum.mods += call.mods;
    sum.after += call.after;
    if (call.interval > worst.interval)
      worst = call;
    if (call.interval > 40 && slow.size() < 160)
      slow.push_back(call);
    if (intervals.size() < 65536)
      intervals.push_back(float(call.interval));
  }
  void reset() {
    intervals.clear();
    slow.clear();
    worst = sum = {};
    calls = 0;
  }
} g_pacing;

void serviceDevRequests() {
  static uint32_t calls = 0, sequenceLeft = 0, sequenceIndex = 0;
  std::error_code ec;
  static const auto directory = remix::config::Profiles::defaultDirectory();
  if (sequenceLeft) {
    char name[32];
    std::snprintf(name, sizeof(name), "%03u.png", sequenceIndex);
    const auto path = std::filesystem::absolute(directory / "sequence" / name, ec).string();
    std::lock_guard lock(g_apiMutex);
    if (g_runtime.requestScreenshot(path.c_str()) == REMIXAPI_ERROR_CODE_SUCCESS) {
      ++sequenceIndex;
      --sequenceLeft;
    }
  }
  if (++calls % 30)
    return;
  auto take = [&](const char *name, std::string *contents) {
    const auto path = directory / name;
    if (!std::filesystem::exists(path, ec))
      return false;
    if (contents) {
      std::ifstream in(path);
      std::stringstream text;
      text << in.rdbuf();
      *contents = text.str();
    }
    std::filesystem::remove(path, ec);
    return true;
  };
  std::string config;
  if (take("config.request", &config)) {
    std::istringstream lines(config);
    for (std::string line; std::getline(lines, line);) {
      const auto eq = line.find('=');
      if (eq == std::string::npos)
        continue;
      auto trim = [](std::string v) {
        const auto a = v.find_first_not_of(" \t\r"), b = v.find_last_not_of(" \t\r");
        return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
      };
      const auto key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
      if (!key.empty())
        remix::api::setConfigVariable(key.c_str(), value.c_str());
    }
  }
  if (take("screenshot.request", nullptr)) {
    const auto path = std::filesystem::absolute(directory / "screenshot.png", ec).string();
    std::lock_guard lock(g_apiMutex);
    g_runtime.requestScreenshot(path.c_str());
  }
  std::string sequence;
  if (!sequenceLeft && take("sequence.request", &sequence)) {
    std::filesystem::remove_all(directory / "sequence", ec);
    std::filesystem::create_directories(directory / "sequence", ec);
    sequenceLeft = std::clamp(std::atoi(sequence.c_str()), 0, 600);
    if (!sequenceLeft)
      sequenceLeft = 60;
    sequenceIndex = 0;
  }
  if (take("pipelines.request", nullptr)) {
    std::string text;
    {
      std::lock_guard lock(g_apiMutex);
      text = g_runtime.pipelineReport();
    }
    std::ofstream(directory / "pipelines.txt") << text;
  }
  if (take("lighttree.request", nullptr)) {
    std::lock_guard lock(remix::scene::g_lightTreeSaveMutex);
    remix::scene::g_lightTreeSavePath = std::filesystem::absolute(directory / "lighttree.bin", ec).string();
  }
  if (take("timings.request", nullptr)) {
    std::string text;
    {
      std::lock_guard lock(g_apiMutex);
      const auto &t = g_runtime.gpuPassTimings();
      char line[1024];
      if (!t.available) {
        text = "unavailable (enable rtx.native.gpuPassTimers)\n";
      } else {
        std::snprintf(line, sizeof(line), "GPU frame %.3f ms (passes %.3f, between passes %.3f); render %ux%u, output %ux%u\n",
                      t.frameMilliseconds, t.passMilliseconds, t.untimedMilliseconds, t.renderWidth, t.renderHeight,
                      t.outputWidth, t.outputHeight);
        text += line;
        for (const auto index : remix::gpu::byCost(t)) {
          const auto &pass = t.passes[index];
          std::snprintf(line, sizeof(line), "%-56s %8.3f %8.3f\n", pass.name, pass.selfMilliseconds, pass.milliseconds);
          text += line;
        }
        const auto local = g_runtime.localLightStatistics();
        std::snprintf(line, sizeof(line), "# local lights %u (active %u), light tree nodes %u, depth %u\n", local.lightCount,
                      local.activeLights, local.nodeCount, local.treeDepth);
        text += line;
        const auto cache = g_runtime.sharcStatistics();
        std::snprintf(line, sizeof(line), "# sharc update paths %u, query attempts %u, hits %u, entries %u\n", cache.updatePaths,
                      cache.queryAttempts, cache.queryHits, cache.occupiedEntries);
        text += line;
        if (const auto sparse = remix::trace::g_sparseState; sparse.requested) {
          if (sparse.launched)
            std::snprintf(line, sizeof(line), "# sparse rendering: on, %.1f%% of the pixels lit each frame\n",
                          sparse.rate * 100);
          else
            std::snprintf(line, sizeof(line), "# sparse rendering: off (%s)\n", sparse.reason);
          text += line;
        }
        if (const auto volume = remix::trace::g_volumeState; volume.requested) {
          if (volume.active)
            std::snprintf(line, sizeof(line),
                          "# volumetrics: on (%s), sight %.1f m, %u levels of %u^3 cells, march %.0f m\n", volume.source,
                          volume.visibilityMeters, volume.levels, volume.cells, volume.reachMeters);
          else
            std::snprintf(line, sizeof(line), "# volumetrics: off (%s)\n", volume.reason);
          text += line;
        }
        const auto breakdown = g_runtime.frameBreakdown();
        std::snprintf(line, sizeof(line),
                      "# raster draws %llu, outside the view (not drawn) %llu, prepare %.3f ms, record %.3f ms\n",
                      (unsigned long long)breakdown.rasterDraws, (unsigned long long)breakdown.rasterCulledDraws,
                      breakdown.rasterPrepareMilliseconds, breakdown.compositeMilliseconds);
        text += line;
        const auto raster = g_runtime.rasterProfile();
        std::snprintf(line, sizeof(line),
                      "# raster prepare %.3f ms: retire %.3f, texture refresh %.3f (%llu), order %.3f, draws %.3f "
                      "(%llu new meshes, %llu new materials)\n",
                      raster.prepareMilliseconds, raster.retireMilliseconds, raster.refreshMilliseconds,
                      (unsigned long long)raster.texturesRefreshed, raster.orderMilliseconds, raster.buildMilliseconds,
                      (unsigned long long)raster.meshesCreated, (unsigned long long)raster.materialsCreated);
        text += line;
        std::snprintf(line, sizeof(line),
                      "# raster staging: %llu buffers (%llu newly allocated), allocate+map %.3f ms, write %.3f ms, "
                      "memory flags 0x%x | skinned vertices %llu, skin upload %.3f ms, skinning %.3f ms\n",
                      (unsigned long long)raster.stagingBuffers, (unsigned long long)raster.stagingAllocated,
                      raster.stagingAllocateMilliseconds, raster.stagingWriteMilliseconds, raster.stagingMemoryFlags,
                      (unsigned long long)raster.skinnedVertices, raster.skinUploadMilliseconds,
                      raster.skinningMilliseconds);
        text += line;
        std::snprintf(line, sizeof(line),
                      "# raster retire: recycle %.3f ms (%llu buffers), materials %.3f (%llu), textures %.3f (%llu), "
                      "meshes %.3f (%llu), paths %.3f (%llu)\n",
                      raster.recycleMilliseconds, (unsigned long long)raster.recycledBuffers,
                      raster.retireMaterialsMilliseconds, (unsigned long long)raster.materialCount,
                      raster.retireTexturesMilliseconds, (unsigned long long)raster.textureCount,
                      raster.retireMeshesMilliseconds, (unsigned long long)raster.meshCount,
                      raster.retirePathsMilliseconds, (unsigned long long)raster.pathCount);
        text += line;
        {
          const auto w = g_runtime.takeWorstRasterProfile();
          std::snprintf(line, sizeof(line),
                        "# slowest raster prepare since the last dump %.2f ms: retire %.2f (recycle %.2f), order %.2f, "
                        "draws %.2f, skin upload %.2f | created: %llu pipelines %.2f ms, %llu meshes %.2f ms, %llu "
                        "textures %.2f ms, %llu materials %.2f ms | staging %llu buffers (%llu allocated) %.2f ms, "
                        "%.2f MiB | draws %llu, culled %llu, waiting for a pipeline %llu\n",
                        w.prepareMilliseconds, w.retireMilliseconds, w.recycleMilliseconds, w.orderMilliseconds,
                        w.buildMilliseconds, w.skinUploadMilliseconds, (unsigned long long)w.pipelinesCreated,
                        w.createPipelineMilliseconds, (unsigned long long)w.meshesCreated, w.createMeshMilliseconds,
                        (unsigned long long)w.texturesCreated, w.createTextureMilliseconds,
                        (unsigned long long)w.materialsCreated, w.createMaterialMilliseconds,
                        (unsigned long long)w.stagingBuffers, (unsigned long long)w.stagingAllocated,
                        w.stagingAllocateMilliseconds + w.stagingWriteMilliseconds, w.uploadBytes / 1048576.0,
                        (unsigned long long)w.draws, (unsigned long long)w.culledDraws,
                        (unsigned long long)w.unreadyDraws);
          text += line;
        }
        {
          const auto c = g_runtime.takeTextureCosts();
          std::snprintf(line, sizeof(line),
                        "# texture images created since the last dump: %llu, %.1f MiB | budget query %.2f ms, create "
                        "%.2f, allocate %.2f, bind %.2f, view %.2f, staging copy %.2f | slowest image %.2f ms\n",
                        (unsigned long long)c.images, c.bytes / 1048576.0, c.query, c.create, c.allocate, c.bind, c.view,
                        c.stage, c.slowest);
          text += line;
        }
        {
          const auto t = std::exchange(remix::trace::g_tableCosts, {});
          std::snprintf(line, sizeof(line),
                        "# slowest surface table step since the last dump %.2f ms: %.2f MiB (capacity %.2f), %llu "
                        "buffers grown %.2f ms, write %.2f, compare and copy %.2f (%.2f MiB copied)\n",
                        t.total, t.bytes / 1048576.0, t.capacity / 1048576.0, (unsigned long long)t.grown, t.grow,
                        t.write, t.compare, t.copied / 1048576.0);
          text += line;
        }
        {
          const auto a = std::exchange(remix::trace::g_accelCosts, {});
          std::snprintf(line, sizeof(line),
                        "# slowest acceleration structure step since the last dump %.2f ms: bottom level %.2f (%u "
                        "builds recorded in %.2f, compaction query pool %.2f), top level %.2f (%u instances written "
                        "in %.2f) | %u structures created in %.2f | combined top level: %s, static part %s (%u "
                        "records made), decision %.2f, records %.2f, written %.2f MiB in %.2f (static part %s), "
                        "sizes %.2f, instance buffer %.2f, structure %.2f, scratch buffer %.2f, build command %.2f\n",
                        a.total, a.bottom, a.bottomBuilds, a.record, a.queryPool, a.top, a.topInstances,
                        a.instances, a.created, a.create, a.update ? "refit" : "build",
                        a.staticChanged ? "changed" : "unchanged", a.staticFilled, a.decide, a.fill,
                        a.written / 1048576.0, a.write, a.keptPrefix ? "kept" : "rewritten", a.sizes, a.storageInstances,
                        a.storageAccel, a.storage, a.topRecord);
          text += line;
        }
        {
          const auto c = std::exchange(remix::trace::g_sceneCosts, {});
          const double n = c.frames ? double(c.frames) : 1;
          const auto &m = c.sum, &w = c.slowest;
          std::snprintf(line, sizeof(line),
                        "# scene preparation over %llu frames, mean %.2f ms: frame %.2f, prune %.2f, stage lighting "
                        "%.2f, gather lights %.2f, family trees %.2f, light tree %.2f, visibility %.2f | slowest %.2f "
                        "ms: frame %.2f, prune %.2f, stage %.2f, gather %.2f, family %.2f, tree %.2f (%u lights), "
                        "visibility %.2f\n",
                        (unsigned long long)c.frames, m.total / n, m.frame / n, m.prune / n, m.stage / n,
                        m.gather / n, m.split / n, m.tree / n, m.visibility / n, w.total, w.frame, w.prune, w.stage,
                        w.gather, w.split, w.tree, c.lights, w.visibility);
          text += line;
          const auto set = g_runtime.localLightSetStatistics();
          const auto &r = set.buildsByReason;
          std::snprintf(line, sizeof(line),
                        "# light tree since launch: builds %llu (initial %llu, first dynamic %llu, settled %llu, "
                        "retired %llu, cleared %llu; %.1f ms in all), refits %llu, dynamic part rebuilt %llu | now: static %u, "
                        "dynamic %u, unlit slots %u, of dynamic instances %u, emitting instances outside the static set %u | visibility tables %llu (last %.2f ms, "
                        "total %.1f)\n",
                        (unsigned long long)set.builds, (unsigned long long)r[1], (unsigned long long)r[2],
                        (unsigned long long)r[3], (unsigned long long)r[4], (unsigned long long)r[5],
                        set.buildTotalMilliseconds, (unsigned long long)set.refits, (unsigned long long)set.dynamicBuilds, set.staticLights,
                        set.dynamicLights, set.retiredSlots, set.dynamicInstanceLights, set.emittingInstances,
                        (unsigned long long)set.tableBuilds, set.tableMilliseconds, set.tableTotalMilliseconds);
          text += line;
          if (remix::scene::sceneTimings()) {
            const auto f = std::exchange(remix::scene::g_frameCosts, {});
            const double n = double(std::max<uint64_t>(f.frames, 1));
            std::snprintf(line, sizeof(line),
                          "# frame step over %llu frames, mean ms: static set sync %.2f, local check %.2f, ids %.2f, "
                          "instances %.2f, sort %.2f, emitters %.2f, opacity found %.2f, opacity new %.2f | draws %.0f, "
                          "emitters %.0f a frame\n",
                          (unsigned long long)f.frames, f.sync / n, f.local / n, f.ids / n, f.instances / n, f.sort / n,
                          f.emitters / n, f.find / n, f.partition / n, f.draws / n, f.emitterCount / n);
            text += line;
          }
          {
            const auto u = std::exchange(remix::trace::g_lightUploads, {});
            std::snprintf(line, sizeof(line),
                          "# light uploads since the last dump: whole %llu, changes only %llu; %.1f MiB in %llu copy regions\n",
                          (unsigned long long)u.whole, (unsigned long long)u.partial, u.bytes / 1048576.0,
                          (unsigned long long)u.regions);
            text += line;
          }
          const auto churn = remix::scene::g_lightChurn;
          remix::scene::g_lightChurn = {};
          remix::scene::g_lightChurn.unlit = churn.unlit; // a state, not a count
          std::snprintf(line, sizeof(line),
                        "# light tree dynamic part rebuilds since the last dump %llu: lights added %llu (of %llu surfaces), "
                        "stale slots dropped %llu; unlit dynamic slots now %u\n",
                        (unsigned long long)churn.rebuilds, (unsigned long long)churn.added, (unsigned long long)churn.addedSurfaces,
                        (unsigned long long)churn.removed, churn.unlit);
          text += line;
          if (remix::scene::sceneTimings()) {
            const double u = double(std::max<uint64_t>(churn.updates, 1));
            const auto &ph = churn.phases;
            std::snprintf(line, sizeof(line),
                          "# light tree update phases over %llu updates, mean ms: order %.2f, copy %.2f, moved+compare %.2f, "
                          "static match %.2f, dynamic merge %.2f, slot writes %.2f, bounds %.2f, builds %.2f, counts %.2f, "
                          "significance %.2f (snapshots %llu)\n",
                          (unsigned long long)churn.updates, ph[0] / u, ph[1] / u, ph[2] / u, ph[3] / u, ph[4] / u,
                          ph[5] / u, ph[6] / u, ph[7] / u, ph[8] / u, ph[9] / u, (unsigned long long)churn.snapshots);
            text += line;
          }
        }
        {
          const auto b = std::exchange(remix::trace::g_blockCosts, {});
          static const char *const how[] = {"spare ready", "spare still being allocated", "no spare of its type",
                                            "buffer over the block size"};
          std::snprintf(line, sizeof(line),
                        "# tracer memory blocks since the last dump: %u from a ready spare, %u waited for a spare, %u "
                        "with no spare of their type, %u for a large buffer | slowest %.2f ms (%s): a %.2f MiB %s "
                        "buffer, memory type %u, %u blocks of it before | slowest buffer binding %.2f ms (%s), "
                        "slowest block mapping %.2f, slowest buffer creation %.2f, slowest buffer destruction %.2f (%.2f MiB%s), "
                        "slowest block freed in %.2f\n",
                        b.ready, b.awaited, b.none, b.large, b.slowest, how[b.slowestHow & 3],
                        b.slowestBuffer / 1048576.0, b.slowestHost ? "host" : "device", b.slowestType,
                        b.slowestBlocks, b.slowestBind, b.bindFirst ? "the first into its block" : "not the first",
                        b.slowestMap, b.slowestCreate,
                        std::exchange(remix::trace::g_slowestBufferDestroy, 0.0),
                        remix::trace::g_slowestBufferDestroyBytes / 1048576.0,
                        remix::trace::g_slowestBufferDestroyOwn ? ", memory of its own" : "",
                        std::exchange(remix::trace::g_slowestBlockFree, 0.0));
          text += line;
        }
        if (g_pacing.calls > 1) {
          auto sorted = g_pacing.intervals;
          std::sort(sorted.begin(), sorted.end());
          const auto at = [&](double q) { return sorted[std::min(sorted.size() - 1, size_t(q * sorted.size()))]; };
          double mean = 0, deviation = 0, step = 0;
          for (const float v : sorted)
            mean += v;
          mean /= double(sorted.size());
          for (const float v : sorted)
            deviation += (v - mean) * (v - mean);
          for (size_t i = 1; i < g_pacing.intervals.size(); ++i)
            step += std::abs(g_pacing.intervals[i] - g_pacing.intervals[i - 1]);
          size_t over = 0;
          for (const float v : sorted)
            over += v > 1.5 * mean;
          std::snprintf(line, sizeof(line),
                        "# pacing over %zu frames: interval mean %.2f ms, median %.2f, 95%% %.2f, 99%% %.2f, max %.2f; "
                        "deviation %.2f, mean change frame to frame %.2f; %zu frames over 1.5x the mean\n",
                        sorted.size(), mean, at(.5), at(.95), at(.99), sorted.back(),
                        std::sqrt(deviation / double(sorted.size())), step / double(sorted.size() - 1), over);
          text += line;
          const double n = double(g_pacing.calls);
          const auto &s = g_pacing.sum;
          const auto &w = g_pacing.worst;
          std::snprintf(line, sizeof(line),
                        "# Present call mean %.2f ms: dev requests %.2f, API mutex wait %.2f, present %.2f (waiting for "
                        "the render thread %.2f), low latency sleep %.2f, mods %.2f, after %.2f | host's OpenRemix "
                        "resource calls %.2f ms a frame\n",
                        s.total / n, s.requests / n, s.mutex / n, s.present / n, s.wait / n, s.sleep / n, s.mods / n,
                        s.after / n, s.api / n);
          text += line;
          std::snprintf(line, sizeof(line),
                        "# longest interval %.2f ms, the Present call that began it %.2f: dev requests %.2f, API mutex "
                        "wait %.2f, present %.2f (waiting for the render thread %.2f), low latency sleep %.2f, mods "
                        "%.2f, after %.2f (the rest is the host)\n",
                        w.interval, w.total, w.requests, w.mutex, w.present, w.wait, w.sleep, w.mods, w.after);
          text += line;
          const auto &f = w.waited;
          std::snprintf(line, sizeof(line),
                        "# the frame that call waited for, on the render thread: %.2f ms in all, present %.2f (setup "
                        "%.2f, scene %.2f, fence %.2f, acquire %.2f, queue mutex %.2f, record %.2f, submit %.2f, queue "
                        "present %.2f), release %.2f\n",
                        f.render, f.present, f.setup, f.scene, f.fence, f.acquire, f.queueLock, f.record, f.submit,
                        f.queuePresent, f.release);
          text += line;
          // Every long frame: what its Present call waited for.
          for (const auto &c : g_pacing.slow) {
            const auto &r = c.waited;
            std::snprintf(line, sizeof(line),
                          "# slow %.1f ms: call %.1f (mutex %.1f, wait for render thread %.1f, sleep %.1f), host %.1f "
                          "(in OpenRemix calls %.1f, longest %s %.1f) | "
                          "render thread %.1f: setup %.1f scene %.1f fence %.1f acquire %.1f record %.1f queuePresent "
                          "%.1f | record: raster %.1f surfaces %.1f (material %.1f) accel %.1f table %.1f passes %.1f "
                          "composite %.1f | geometry %.1f upload %.1f allocation %.1f (%llu buffers, %llu memory)\n",
                          c.interval, c.total, c.mutex, c.wait, c.sleep, c.interval - c.total, c.api, c.apiLongestName,
                          c.apiLongest, r.render, r.setup,
                          r.scene, r.fence, r.acquire, r.record, r.queuePresent, r.raster, r.surfaces, r.material,
                          r.traceAccel, r.table, r.passes, r.composite, r.geometry, r.upload, r.allocation,
                          (unsigned long long)r.bufferAllocations, (unsigned long long)r.memoryAllocations);
            text += line;
          }
        }
        g_pacing.reset();
        if (g_runtime.latencyRequested()) {
          std::snprintf(line, sizeof(line), "# low latency: %s\n",
                        g_runtime.latencyActive() ? "on" : "off (raster backend)");
          text += line;
        }
        if (const auto caller = g_runtime.asyncCallerStatistics(); caller.active) {
          std::snprintf(line, sizeof(line),
                        "# asynchronous present, caller: call %.3f ms (wait for render thread %.3f, setup %.3f, capture "
                        "%.3f, ui %.3f)\n",
                        caller.callMilliseconds, caller.waitMilliseconds, caller.setupMilliseconds,
                        caller.captureMilliseconds, caller.uiMilliseconds);
          text += line;
        }
      }
    }
    std::ofstream(directory / "timings.txt") << text;
  }
}
} // namespace

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_Present(const remixapi_PresentInfo *info) {
  try {
    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::time_point a, Clock::time_point b) {
      return std::chrono::duration<double, std::milli>(b - a).count();
    };
    const auto entered = Clock::now();
    PresentPacing::Call call;
    {
      const double api = double(g_apiCallNanoseconds.exchange(0, std::memory_order_relaxed)) / 1e6;
      const double longest = double(g_apiLongestNanoseconds.exchange(0, std::memory_order_relaxed)) / 1e6;
      g_pacing.previous.api = api;
      g_pacing.previous.apiLongest = longest;
      g_pacing.previous.apiLongestName = g_apiLongestName.load(std::memory_order_relaxed);
    }
    if (g_pacing.hasPrevious) {
      g_pacing.previous.interval = ms(g_pacing.last, entered);
      g_pacing.add(g_pacing.previous);
    }
    g_pacing.last = entered;
    serviceDevRequests();
    const auto requested = Clock::now();
    call.requests = ms(entered, requested);
    // Closes the record on every way out of the call.
    struct Pace {
      PresentPacing::Call &call;
      Clock::time_point entered, unlocked{};
      ~Pace() {
        const auto now = Clock::now();
        call.total = std::chrono::duration<double, std::milli>(now - entered).count();
        if (unlocked != Clock::time_point{})
          call.after = std::chrono::duration<double, std::milli>(now - unlocked).count();
        g_pacing.previous = call;
        g_pacing.hasPrevious = true;
      }
    } pace{call, entered};
    PFN_remixapi_BridgeCallback presentCallback = nullptr;
    remixapi_ErrorCode status = REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    {
      struct Unlocked {
        Clock::time_point &at;
        ~Unlocked() { at = Clock::now(); }
      } unlocked{pace.unlocked}; // destroyed after the lock below: the mutex is released first
      std::lock_guard lock(g_apiMutex);
      const auto locked = Clock::now();
      call.mutex = ms(requested, locked);
      status = g_runtime.present(info);
      const auto presented = Clock::now();
      call.sleep = g_runtime.latencySleepMilliseconds();
      call.present = ms(locked, presented) - call.sleep;
      if (const auto caller = g_runtime.asyncCallerStatistics(); caller.active)
        call.wait = g_runtime.asyncCallerWait(), call.waited = g_runtime.waitedFrame();
#if REMIX_HAS_USD
      if (status == REMIXAPI_ERROR_CODE_SUCCESS) {
        remix::usd::captureBoundary(g_runtime.raster());
        remix::usd::modBoundary(g_runtime.raster());
      }
#endif
      call.mods = ms(presented, Clock::now());
#if !defined(_WIN32)
      // Validation and reconstruction SDKs initialize process-static resources on
      // first use. Async preparation can activate them after the initial raster
      // frame. Renew cleanup after each renderer's first use so our GPU wait and
      // resource release precede the SDK's process-exit destructors. The mask
      // bounds registrations per startup, independently of frame count/resizes.
      uint32_t exitStages = g_runtime.hasStartedFrame() ? 1u : 0u;
      if (g_runtime.backendCapabilities().activeBackend == 1)
        exitStages |= 2u;
      const auto reconstruction = g_runtime.reconstructionStatistics();
      if (reconstruction.frames)
        exitStages |= 4u << uint32_t(reconstruction.backend);
      if (exitStages & ~g_frameExitStages) {
        if (std::atexit([] { remixapi_Shutdown(); }) != 0) {
          g_runtime.shutdown();
          return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
        }
        g_frameExitStages |= exitStages;
      }
#endif
      presentCallback = g_presentCallback;
    }
    if (status == REMIXAPI_ERROR_CODE_SUCCESS)
      publishUsdReloads();
    static thread_local bool inCallback = false;
    if (status == REMIXAPI_ERROR_CODE_SUCCESS && presentCallback && !inCallback) {
      inCallback = true;
      try {
        presentCallback();
      } catch (...) {
        inCallback = false;
        return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
      }
      inCallback = false;
    }
    return status;
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_RegisterCallbacks(
    PFN_remixapi_BridgeCallback beginSceneCallback, PFN_remixapi_BridgeCallback endSceneCallback,
    PFN_remixapi_BridgeCallback presentCallback) {
  try {
    std::lock_guard lock(g_apiMutex);
    g_beginSceneCallback = beginSceneCallback;
    g_endSceneCallback = endSceneCallback;
    g_presentCallback = presentCallback;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
}
}
