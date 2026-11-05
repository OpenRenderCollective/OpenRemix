#include "usd/mods.h"
#include "usd/mod_cache.h"
#include "usd/submission.h"
#include <pxr/usd/sdf/layer.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <optional>
#include <utility>

namespace remix::usd {
  namespace {
    using Clock = std::chrono::steady_clock;
    using Stamps = std::map<std::string, std::pair<std::filesystem::file_time_type, uintmax_t>>;
    struct Set {
      Stamps stamps; // of the mods' files as this load ended
      std::vector<Snapshot> snapshots;
      std::vector<ModInfo> mods; // per snapshot
      // Replacements of the enabled mods (the first mod in path order wins).
      std::map<uint64_t, std::pair<size_t, Json>> meshes, lights, materials;
      std::set<std::string> dependencies;
      std::vector<PreparedMod> prepared; // What Submission takes, per snapshot.
      uint64_t geometryBytes = 0;
      uint64_t partsVersion = 0; // of the toggles index applied
      std::string undecodable;   // The first texture left out for not decoding.
      bool fromCache = false;    // Every mod came from its cache file.
      std::vector<std::future<void>> cacheWrites;
      // Parse failures travel in-band: an exception_ptr crossing threads is
      // destroyed early under mixed MSVC runtimes (Proton's builtin vcruntime).
      std::optional<std::string> error;
      bool loadError = false;
    };
    struct Scan {
      Stamps stamps;
      std::optional<std::string> error; // in-band, like Set::error
      uint64_t epoch = 0;
      Clock::duration took{};
    };
    // What of a loaded set is created on the Present thread, a little a frame.
    struct Publishing {
      Set set;
      std::vector<std::unique_ptr<Submission>> submissions;
      uint64_t generation = 0;
      size_t next = 0;
    };
    struct State {
      bool enabled = false, watch = true;
      // Developer panel toggles, applied at the next frame boundary.
      bool modsEnabled = true, reindex = false, reload = false;
      // The host configured the up axis; otherwise it follows rtx.zUp
      // (scene::sceneUpAxis), the mods loading again when it changes.
      bool hostAxis = false;
      std::map<std::string, ModParts> modParts; // by mod name; absent: defaults
      std::map<std::string, float> lightScales; // the Mods tab's, by mod name; absent: authored
      Options options;
      std::string directory, diagnostic;
      std::future<Set> pending;
      std::shared_ptr<Progress> progress;
      std::unique_ptr<Publishing> publishing;
      Clock::time_point loadStarted{};
      double loadSeconds = 0;
      bool pressure = false;
      uint64_t partsVersion = 0; // Bumped by the toggles.
      std::future<void> retiring; // A previous set going, off the Present thread.
      std::vector<std::future<void>> cacheWrites;
      std::shared_ptr<std::atomic<bool>> cacheCancel = std::make_shared<std::atomic<bool>>(false);
      Set active;
      std::set<std::string> failedDependencies;
      std::set<remixapi_LightHandle> overriddenLights;
      // Replaced instances without an identity of the host's, by the mesh they
      // replace: this frame's, and the last frame's not yet matched.
      struct PlacedInstance {
        remixapi_Transform transform;
        uint64_t id;
      };
      std::map<uint64_t, std::vector<PlacedInstance>> placed, placedBefore;
      uint64_t generation = 0;
      std::vector<std::unique_ptr<Submission>> submissions;
      Stamps stamps;
      // Watch scans run off the Present thread: walking a large mod tree takes
      // far longer than a frame (notably under Wine). A scan started before
      // the baseline was last reset is discarded.
      std::future<Scan> scan;
      uint64_t scanEpoch = 0;
      Clock::time_point next{}, changed{};
    } state;
    std::vector<std::string> paths(const std::string &directory) {
      std::vector<std::string> result;
      std::error_code error;
      if (!std::filesystem::exists(directory, error))
        return result;
      const auto options = std::filesystem::directory_options::skip_permission_denied |
                           std::filesystem::directory_options::follow_directory_symlink;
      for (auto it = std::filesystem::recursive_directory_iterator(directory, options, error);
           it != std::filesystem::recursive_directory_iterator() && !error;
           it.increment(error)) {
        std::error_code ec;
        if (it->is_directory(ec)) {
          const auto dirName = it->path().filename().string();
          if (dirName == "assets" || dirName == "deps" || dirName == ".thumbs" ||
              dirName == "materials" || dirName == "textures" || dirName == ".git") {
            it.disable_recursion_pending();
            continue;
          }
        } else if (it->is_regular_file(ec)) {
          auto name = it->path().filename().string();
          if (name == "mod.usda" || name == "mod.usdc" || name == "mod.usd")
            result.push_back(it->path().string());
        }
      }
      std::sort(result.begin(), result.end());
      return result;
    }
    std::string modName(const std::string &directory, const std::string &path) {
      const auto parent = std::filesystem::path(path).parent_path();
      auto name = parent.lexically_relative(directory).generic_string();
      return name.empty() || name == "." ? std::filesystem::path(path).filename().generic_string() : name;
    }
    // Rebuilds the replacement tables from the enabled mods' snapshots.
    void index(Set &set, const std::map<std::string, ModParts> &modParts, bool modsEnabled, uint64_t version) {
      set.partsVersion = version;
      set.meshes.clear();
      set.lights.clear();
      set.materials.clear();
      for (size_t i = 0; i < set.snapshots.size(); ++i) {
        auto &parts = set.mods[i].parts;
        auto it = modParts.find(set.mods[i].name);
        parts = it == modParts.end() ? ModParts{} : it->second;
        if (!modsEnabled || !parts.enabled)
          continue;
        // A mod leaves out what it does not apply, so the next mod replacing
        // the same asset takes over.
        auto &replacements = set.snapshots[i].scene["replacements"];
        for (const auto &entry : replacements["meshes"])
          if (parts.meshes || parts.lights ||
              (parts.materials && entry.value("preserve", false) && entry.contains("material")))
            set.meshes.try_emplace(entry["hash"].get<uint64_t>(), i, entry);
        if (parts.lightOverrides)
          for (const auto &entry : replacements["lights"])
            set.lights.try_emplace(entry["hash"].get<uint64_t>(), i, entry);
        if (parts.materials)
          for (const auto &entry : replacements["materials"])
            set.materials.try_emplace(entry["hash"].get<uint64_t>(), i, entry);
      }
    }
    // The mod materials of the active set, all at once: from now on what they
    // replace draws them. The next frame's streaming pulls in their files.
    void applyMaterials(NativeResources &resources) {
      resources.clearModMaterials();
      std::vector<std::map<std::string, std::vector<uint64_t>>> keys(state.submissions.size());
      for (const auto &[hash, entry] : state.active.materials) {
        const auto &[index, source] = entry;
        const auto &path = source["path"].get_ref<const std::string &>();
        try {
          resources.addModMaterial(hash, state.submissions[index]->materialHandle(path),
                                   source.value("authored", scene::ModParts::all));
          keys[index][path].push_back(hash);
        } catch (const std::exception &e) {
          state.diagnostic = e.what();
        }
      }
      for (size_t i = 0; i < keys.size(); ++i)
        state.submissions[i]->setMaterialKeys(std::move(keys[i]));
    }
    // Reads each file's low levels and the textures lights use whole; the rest
    // comes when drawn (Submission::service). A file that cannot be read is left
    // out where it is used, as RTX Remix leaves it.
    PreparedTextures prepareTextures(Snapshot &snapshot, Progress &progress) {
      PreparedTextures prepared;
      auto &scene = snapshot.scene;
      std::set<std::string> streamed, lit;
      for (const auto &material : scene["materials"])
        for (auto &path : materialTextureFiles(material))
          streamed.insert(std::move(path));
      for (const auto &light : scene["lights"])
        for (const char *key : {"texture", "cookie"})
          if (auto path = light.value(key, std::string()); !path.empty())
            lit.insert(std::move(path));
      for (const auto &light : scene["replacements"]["lights"])
        if (auto path = light["overrides"].value("texture", std::string()); !path.empty())
          lit.insert(std::move(path));
      const std::vector<std::string> previews(streamed.begin(), streamed.end()), whole(lit.begin(), lit.end());
      progress.enter(LoadStage::Textures, uint32_t(previews.size() + whole.size()));
      auto low = decodeTextures(previews, previewEdge, &progress.done);
      auto full = decodeTextures(whole, 0, &progress.done);
      std::set<std::string> failed;
      for (size_t i = 0; i < previews.size(); ++i)
        if (low[i])
          prepared.previews.emplace(previews[i], std::move(low[i]));
        else
          failed.insert(previews[i]);
      for (size_t i = 0; i < whole.size(); ++i)
        if (full[i])
          prepared.whole.emplace(whole[i], std::move(full[i]));
        else
          failed.insert(whole[i]);
      if (failed.empty())
        return prepared;
      auto drop = [&](Json &object, const std::string &key) {
        if (object.is_object() && object.contains(key) && object[key].is_string() &&
            failed.contains(object[key].get<std::string>()))
          object[key] = "";
      };
      for (const auto &path : failed)
        scene["warnings"].push_back("Undecodable texture " + path);
      for (auto &material : scene["materials"]) {
        std::vector<std::string> keys;
        for (auto it = material.begin(); it != material.end(); ++it)
          if (it.key().ends_with("_texture"))
            keys.push_back(it.key());
        for (const auto &key : keys)
          drop(material, key);
        if (material.contains("iridescence"))
          drop(material["iridescence"], "texture");
        if (material.contains("subsurface"))
          for (const char *key : subsurfaceTextureKeys)
            drop(material["subsurface"], key);
      }
      for (auto &light : scene["lights"]) {
        drop(light, "texture");
        drop(light, "cookie");
      }
      for (auto &light : scene["replacements"]["lights"])
        if (light.contains("overrides"))
          drop(light["overrides"], "texture");
      return prepared;
    }
    Stamps scanStamps(const std::string &directory, std::set<std::string> deps);
    struct Toggles {
      std::map<std::string, ModParts> modParts;
      bool modsEnabled = true;
      uint64_t version = 0;
    };
    Set parse(std::string directory, Options options, std::shared_ptr<Progress> progress, Toggles toggles,
              bool edited, std::shared_ptr<std::atomic<bool>> cacheCancel) {
      Set result;
      result.fromCache = true;
      const char *cacheSetting = std::getenv("OPENREMIX_USD_CACHE");
      const bool cacheAlways = cacheSetting && std::strcmp(cacheSetting, "always") == 0;
      // RTX Remix's mod load: every replacement's geometry and material now, off
      // the Present thread, and from when they are all ready each draws from its
      // first frame. Geometry stays in host memory in its compact form, not as
      // JSON and not as USD (the stage goes with this load); textures stream.
      options.preloadTextures = false;
      // One missing texture in a large mod must not cost every replacement.
      options.strictAssets = false;
      options.progress = progress.get();
      for (const auto &path : paths(directory)) {
        options.path = path;
        Snapshot snapshot;
        PreparedMeshes prepared;
        std::mutex preparing;
        const auto cacheFile = modCacheFile(path);
        if (auto cached = readModCache(cacheFile, options.meters, options.axis, *progress)) {
          snapshot = std::move(cached->snapshot);
          prepared = std::move(cached->meshes);
        } else {
          result.fromCache = false;
          const auto loadStarted = Clock::now();
          const auto fileLoadStarted = std::filesystem::file_time_type::clock::now();
          options.meshSink = [&](ComposedMesh &&mesh) {
            const auto id = mesh.id;
            try {
              prepared.add(std::move(mesh), preparing);
            } catch (...) {
              // Its replacement stays the game's mesh.
              std::lock_guard lock(preparing);
              prepared.failed.insert(id.substr(0, id.find('/', sizeof("/RootNode/meshes/") - 1)));
            }
          };
          try {
            snapshot = load(options);
          } catch (const LoadError &error) {
            result.dependencies.insert(error.dependencies.begin(), error.dependencies.end());
            throw LoadError(error.what(), std::move(result.dependencies));
          }
          // Worth a file when it saves a wait, and not at every save while a mod is
          // being edited.
          if (cacheAlways || (!edited && Clock::now() - loadStarted >= std::chrono::seconds(2)))
            if (auto write = prepareModCacheWrite(cacheFile, snapshot, prepared, options.meters, options.axis,
                                                  fileLoadStarted))
              result.cacheWrites.push_back(std::async(std::launch::async,
                                                      [write = std::move(*write), cacheCancel]() mutable {
                                                        writeModCache(std::move(write), *cacheCancel);
                                                      }));
        }
        auto textures = prepareTextures(snapshot, *progress);
        result.geometryBytes += prepared.bytes;
        result.prepared.push_back(prepareMod(snapshot, std::move(prepared), std::move(textures)));
        snapshot.scene.erase("materials");
        const auto &replacements = snapshot.scene["replacements"];
        auto count = [&](const char *group) { return replacements.contains(group) ? replacements[group].size() : 0; };
        // Lights authored under mesh replacements (game light overrides are
        // counted on their own).
        size_t attached = 0;
        for (const auto &light : snapshot.scene["lights"])
          attached += light.value("id", std::string()).starts_with("/RootNode/meshes/");
        size_t missing = 0;
        for (const auto &warning : snapshot.scene["warnings"]) {
          const auto &text = warning.get_ref<const std::string &>();
          missing += text.starts_with("Unresolved texture ") || text.starts_with("Undecodable texture ");
          if (result.undecodable.empty() && text.starts_with("Undecodable texture "))
            result.undecodable = text;
        }
        const float scale = snapshot.scene.value("light_scale", 1.0);
        result.mods.push_back({modName(directory, path), count("meshes"), attached, count("lights"),
                               count("materials"), missing, path, scale, scale});
        snapshot.scene.erase("lights");
        result.dependencies.insert(snapshot.dependencies.begin(), snapshot.dependencies.end());
        result.snapshots.push_back(std::move(snapshot));
      }
      index(result, toggles.modParts, toggles.modsEnabled, toggles.version);
      result.stamps = scanStamps(directory, result.dependencies);
      return result;
    }
    std::set<std::string> watchedDependencies() {
      auto deps = state.active.dependencies;
      deps.insert(state.failedDependencies.begin(), state.failedDependencies.end());
      return deps;
    }
    Stamps scanStamps(const std::string &directory, std::set<std::string> deps) {
      Stamps result;
      for (const auto &path : paths(directory))
        deps.insert(path);
      for (const auto &path : deps) {
        std::error_code ec;
        auto time = std::filesystem::last_write_time(path, ec);
        auto size = std::filesystem::file_size(path, ec);
        result[path] = {time, ec ? 0 : size};
      }
      return result;
    }
    Stamps stamps() {
      ++state.scanEpoch;
      return scanStamps(state.directory, watchedDependencies());
    }
    void refreshDependencies() {
      auto current = stamps();
      // Do not swallow edits made while a parser job was in flight. New
      // dependencies get one follow-up parse, then the watch baseline settles.
      if (current != state.stamps)
        state.changed = Clock::now();
      state.stamps = std::move(current);
    }
    // edited: the mods' files changed under a running game.
    void launch(bool edited = false) {
      if (!state.hostAxis)
        state.options.axis = scene::sceneUpAxis.load();
      state.progress = std::make_shared<Progress>();
      state.loadStarted = Clock::now();
      state.pending = std::async(std::launch::async, [directory = state.directory, options = state.options,
                                                      progress = state.progress,
                                                      toggles = Toggles{state.modParts, state.modsEnabled,
                                                                        state.partsVersion},
                                                      edited, cancel = state.cacheCancel] {
        try {
          return parse(directory, options, progress, toggles, edited, cancel);
        } catch (const LoadError &e) {
          Set failed;
          failed.error = e.what();
          failed.loadError = true;
          failed.dependencies = e.dependencies;
          return failed;
        } catch (const std::exception &e) {
          Set failed;
          failed.error = e.what();
          return failed;
        }
      });
    }
    float lightScale(size_t index);
    // A slice of a frame: the Present thread stays smooth while thousands of
    // materials are created.
    constexpr auto publishSlice = std::chrono::milliseconds(3);
    void activate(NativeResources &resources) {
      auto published = std::exchange(state.publishing, nullptr);
      for (auto light : state.overriddenLights)
        resources.destroyLight(light);
      state.overriddenLights.clear();
      state.submissions = std::move(published->submissions);
      // A set's tables and JSON are large: the Present thread does not free them.
      state.retiring = std::async(std::launch::async, [old = std::move(state.active)] {});
      state.active = std::move(published->set);
      state.generation = published->generation;
      // A toggle during the load is not in this set's tables.
      state.reindex = state.active.partsVersion != state.partsVersion;
      state.loadSeconds = std::chrono::duration<double>(Clock::now() - state.loadStarted).count();
      applyMaterials(resources);
      // This frame already draws them.
      for (size_t i = 0; i < state.submissions.size(); ++i)
        state.submissions[i]->setLightScale(lightScale(i));
      state.diagnostic = state.active.undecodable;
      state.failedDependencies.clear();
      // Do not swallow edits made while the load was in flight.
      ++state.scanEpoch;
      if (state.active.stamps != state.stamps)
        state.changed = Clock::now();
      state.stamps = state.active.stamps;
    }
    // What the lights mod `index` authors are scaled by.
    float lightScale(size_t index) {
      if (index >= state.active.mods.size())
        return 1;
      const auto &mod = state.active.mods[index];
      auto it = state.lightScales.find(mod.name);
      return it == state.lightScales.end() ? mod.authoredLightScale : it->second;
    }
    // The identity of a replaced instance the host gave none. It is the one of
    // the last frame's instance at the same transform, else of the nearest one
    // within 3 m (dxvk-remix's rtx.uniqueObjectDistance), so what hangs under
    // the replacement stays the same when the instance moves. Draws sharing a
    // transform share an identity, as they would a hash of it.
    // again: an instance of the mesh already drew there this frame.
    uint64_t instanceIdentity(uint64_t mesh, const remixapi_Transform &transform, bool &again) {
      const auto same = [&](const auto &placed) {
        return std::memcmp(&placed.transform, &transform, sizeof(transform)) == 0;
      };
      auto &now = state.placed[mesh];
      if (const auto found = std::find_if(now.begin(), now.end(), same); found != now.end()) {
        again = true;
        return found->id;
      }
      auto &before = state.placedBefore[mesh];
      auto match = std::find_if(before.begin(), before.end(), same);
      if (match == before.end()) {
        const float reach = 3 / float(state.options.meters > 0 ? state.options.meters : .01);
        float nearest = reach * reach;
        for (auto candidate = before.begin(); candidate != before.end(); ++candidate) {
          float distance = 0;
          for (int r = 0; r < 3; ++r) {
            const float delta = candidate->transform.matrix[r][3] - transform.matrix[r][3];
            distance += delta * delta;
          }
          if (distance <= nearest) {
            nearest = distance;
            match = candidate;
          }
        }
      }
      uint64_t id;
      if (match != before.end()) {
        id = match->id;
        before.erase(match);
      } else {
        // A hash of the transform: an instance back in view is the one it was.
        // Unless a moved instance took that identity along with it.
        id = scene::hashBytes(&transform, sizeof(transform));
        const auto taken = [&](const auto &placed) { return placed.id == id; };
        while (std::any_of(now.begin(), now.end(), taken) || std::any_of(before.begin(), before.end(), taken))
          id = scene::hashBytes(&id, sizeof(id));
      }
      now.push_back({transform, id});
      return id;
    }
    // The frame a game light's replacement lights are authored in (dxvk-remix
    // LightUtils::getLightTransform): at its position, its -Z along its
    // direction (spot and distant lights).
    remixapi_Transform gameLightTransform(const NativeResources::LightSource &source) {
      remixapi_Transform result{};
      for (int i = 0; i < 3; ++i)
        result.matrix[i][i] = 1;
      const bool distant = source.type == REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
      const bool spot = !distant && source.cone < 3.1415f;
      if (!distant)
        for (int r = 0; r < 3; ++r)
          result.matrix[r][3] = source.transform.matrix[r][3];
      if (!distant && !spot)
        return result;
      // The rotation taking (0, 0, -1) to the direction (Rodrigues).
      float to[3] = {source.direction.x, source.direction.y, source.direction.z};
      const float length = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
      if (length < 1e-6f)
        return result;
      for (float &v : to)
        v /= length;
      const float from[3] = {0, 0, -1};
      const float axis[3] = {from[1] * to[2] - from[2] * to[1], from[2] * to[0] - from[0] * to[2],
                             from[0] * to[1] - from[1] * to[0]};
      const float c = from[0] * to[0] + from[1] * to[1] + from[2] * to[2];
      if (c < -0.9999f) {
        // Opposite: half a turn about X.
        result.matrix[1][1] = result.matrix[2][2] = -1;
        return result;
      }
      const float k[3][3] = {{0, -axis[2], axis[1]}, {axis[2], 0, -axis[0]}, {-axis[1], axis[0], 0}};
      for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 3; ++col) {
          float k2 = 0;
          for (int i = 0; i < 3; ++i)
            k2 += k[r][i] * k[i][col];
          result.matrix[r][col] = (r == col ? 1.f : 0.f) + k[r][col] + k2 / (1 + c);
        }
      return result;
    }
  } // namespace
  void startMods(bool disabled) {
    state.enabled = !disabled;
    if (disabled)
      return;
    const char *directory = std::getenv("OPENREMIX_MODS_DIRECTORY");
    state.directory = directory ? directory : (std::filesystem::current_path() / "rtx-remix/mods").string();
    // Host units must be supplied for mods. Defaults match capture conventions.
    state.options.meters = .01;
    state.options.axis = scene::sceneUpAxis.load();
    state.hostAxis = false;
    state.stamps = stamps();
    launch();
  }
  void stopMods(NativeResources &resources) {
    // A cache write in flight stops where it is; its file was never in place.
    *state.cacheCancel = true;
    state.cacheWrites.clear();
    resources.clearModMaterials();
    for (auto light : state.overriddenLights)
      resources.destroyLight(light);
    state.submissions.clear();
    state = State{};
  }
  remixapi_ErrorCode configureMods(const char *directory, double meters, uint32_t axis, bool watch) {
    if (!directory || !std::isfinite(meters) || meters <= 0 || axis < 1 || axis > 2)
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    if (state.pending.valid())
      state.pending.wait();
    state.pending = {};
    state.publishing.reset();
    state.directory = directory;
    state.options.meters = meters;
    state.options.axis = axis;
    state.hostAxis = true;
    state.watch = watch;
    state.enabled = true;
    try {
      state.stamps = stamps();
      launch();
      return REMIXAPI_ERROR_CODE_SUCCESS;
    } catch (...) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
  }
  std::pair<double, uint32_t> modUnits() {
    // Captures take the host's axis, else rtx.zUp's, mods loaded or not.
    return {state.options.meters > 0 ? state.options.meters : .01,
            state.hostAxis ? state.options.axis : scene::sceneUpAxis.load()};
  }
  void modBoundary(NativeResources &resources) {
    if (!state.enabled)
      return;
    state.placedBefore.swap(state.placed);
    state.placed.clear();
    for (size_t i = 0; i < state.submissions.size(); ++i) {
      state.submissions[i]->retireAttachments();
      state.submissions[i]->service(state.pressure);
      state.submissions[i]->setLightScale(lightScale(i));
    }
    if (state.publishing) {
      try {
        const auto deadline = Clock::now() + publishSlice;
        auto &publishing = *state.publishing;
        while (publishing.next < publishing.submissions.size() &&
               publishing.submissions[publishing.next]->publish(deadline))
          ++publishing.next;
        if (publishing.next == publishing.submissions.size())
          activate(resources);
      } catch (const std::exception &e) {
        state.diagnostic = e.what();
        state.publishing.reset();
      } catch (...) {
        state.diagnostic = "Mod resources could not be created; retaining previous replacements";
        state.publishing.reset();
      }
    }
    if (state.pending.valid() &&
        state.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      try {
        auto next = state.pending.get();
        for (auto &write : next.cacheWrites)
          state.cacheWrites.push_back(std::move(write));
        next.cacheWrites.clear();
        if (next.error && next.loadError)
          throw LoadError(*next.error, std::move(next.dependencies));
        if (next.error)
          throw std::runtime_error(*next.error);
        auto publishing = std::make_unique<Publishing>();
        state.failedDependencies = next.dependencies;
        resources.invalidateTexturePaths();
        publishing->generation = state.generation + 1;
        state.progress->enter(LoadStage::Finishing, 0);
        for (size_t i = 0; i < next.prepared.size(); ++i)
          publishing->submissions.push_back(std::make_unique<Submission>(
              resources, std::move(next.prepared[i]), 0x4d4f440000000000ull + (publishing->generation << 20) + i));
        next.prepared.clear();
        publishing->set = std::move(next);
        state.publishing = std::move(publishing);
      } catch (const LoadError &e) {
        state.diagnostic = e.what();
        state.failedDependencies = e.dependencies;
        refreshDependencies();
      } catch (const std::exception &e) {
        state.diagnostic = e.what();
      } catch (...) {
        state.diagnostic = "Mod resources could not be created; retaining previous replacements";
      }
    }
    if (state.reindex) {
      state.reindex = false;
      index(state.active, state.modParts, state.modsEnabled, state.partsVersion);
      applyMaterials(resources);
    }
    if (!state.hostAxis && state.options.axis != scene::sceneUpAxis.load())
      state.reload = true;
    if (state.reload && !state.pending.valid() && !state.publishing) {
      state.reload = false;
      state.stamps = stamps();
      launch();
    }
    auto now = Clock::now();
    if (!state.watch || state.pending.valid() || state.publishing || now < state.next)
      return;
    if (!state.scan.valid()) {
      state.scan = std::async(std::launch::async, [directory = state.directory, deps = watchedDependencies(),
                                                   epoch = state.scanEpoch] {
        Scan scan{.epoch = epoch};
        const auto started = Clock::now();
        try {
          scan.stamps = scanStamps(directory, std::move(deps));
        } catch (const std::exception &e) {
          scan.error = e.what();
        }
        scan.took = Clock::now() - started;
        return scan;
      });
      return;
    }
    if (state.scan.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
      return;
    auto scan = state.scan.get();
    // A large mod's files take a while to look at: the scans leave most of the
    // time to the game.
    state.next = now + std::clamp(20 * scan.took, Clock::duration(std::chrono::milliseconds(100)),
                                  Clock::duration(std::chrono::seconds(5)));
    if (scan.error) {
      state.diagnostic = *scan.error;
      return;
    }
    if (scan.epoch == state.scanEpoch && scan.stamps != state.stamps) {
      state.stamps = std::move(scan.stamps);
      state.changed = now;
    }
    if (state.changed != Clock::time_point{} && now - state.changed >= std::chrono::milliseconds(250)) {
      state.changed = {};
      launch(true);
    }
  }
  bool modInstance(NativeResources &resources, const remixapi_InstanceInfo &info, uint64_t originalHash) {
    auto it = state.active.meshes.find(originalHash ? originalHash
                                                    : uint64_t(reinterpret_cast<uintptr_t>(info.mesh)));
    if (it == state.active.meshes.end())
      return false;
    auto &[index, entry] = it->second;
    const auto &root = entry["path"].get_ref<const std::string &>();
    // A replacement that could not be made leaves the game's mesh.
    if (!state.submissions[index]->ready(root))
      return false;
    const auto &parts = state.active.mods[index].parts;
    if (parts.meshes || parts.lights) {
      auto parent = info;
      const auto *own = static_cast<const remixapi_InstanceIdentityEXT *>(
          scene::extension(info.pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT));
      remixapi_InstanceIdentityEXT identity{};
      // A draw repeated where it already drew (a game's second pass over a
      // mesh) is the same instance: its replacement draws once (dxvk-remix
      // merges them).
      bool again = false;
      if (!own || !own->instanceId) {
        identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
        identity.pNext = info.pNext;
        // A skinned draw's bones carry where it is: instances of a mesh can
        // share their transform. The last bone is one the draw uses.
        auto placed = info.transform;
        if (const auto *bones = static_cast<const remixapi_InstanceInfoBoneTransformsEXT *>(
                scene::extension(info.pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT));
            bones && bones->boneTransforms_count)
          placed = transformed(info.transform, bones->boneTransforms_values[bones->boneTransforms_count - 1]);
        identity.instanceId = instanceIdentity(it->first, placed, again);
        parent.pNext = &identity;
      }
      if (!again)
        state.submissions[index]->draw(false, 1, &parent, root, parts.meshes, parts.lights);
    }
    if (entry.value("preserve", false)) {
      if (!parts.materials || !entry.contains("material"))
        return false;
      resources.drawInstance(info, state.submissions[index]->materialHandle(entry["material"]));
      return true;
    }
    // Without replacement geometry the original stays.
    return parts.meshes && !parts.keepOriginal;
  }
  bool modLight(NativeResources &resources, remixapi_LightHandle handle, uint64_t originalHash) {
    auto original = uint64_t(reinterpret_cast<uintptr_t>(handle));
    auto it = state.active.lights.find(originalHash ? originalHash : original);
    if (it == state.active.lights.end())
      return false;
    auto &[index, entry] = it->second;
    if (entry.value("hidden", false))
      return true;
    // Lights authored under the replacement (dxvk-remix processLight): in
    // the game light's frame, or as authored when the replacement places
    // itself (its transform authored).
    if (const auto *source = resources.lightSource(original);
        source && state.submissions[index]->ready(entry["path"])) {
      remixapi_InstanceInfo parent{};
      parent.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      if (entry["overrides"].contains("transform"))
        for (int i = 0; i < 3; ++i)
          parent.transform.matrix[i][i] = 1;
      else
        parent.transform = gameLightTransform(*source);
      remixapi_InstanceIdentityEXT identity{};
      identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      identity.instanceId = original ? original : 1;
      parent.pNext = &identity;
      state.submissions[index]->draw(false, 1, &parent, entry["path"], false, true);
    }
    const auto &o = entry["overrides"];
    const std::map<std::string, remixapi_StructType> types{
        {"SphereLight", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT},
        {"RectLight", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_RECT_EXT},
        {"DiskLight", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISK_EXT},
        {"CylinderLight", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT},
        {"DistantLight", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT},
        {"DomeLight", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT}};
    remixapi_LightInfoUSDEXT usd{};
    if (auto type = types.find(entry.value("type", std::string())); type != types.end())
      usd.lightType = type->second;
    std::map<std::string, float> values;
    auto number = [&](const char *key, bool degrees = false) -> const float * {
      if (!o.contains(key))
        return nullptr;
      auto &value = values[key];
      value = o[key].get<float>() * (degrees ? 3.14159265359f / 180 : 1);
      return &value;
    };
    usd.pIntensity = number("intensity");
    if (usd.pIntensity)
      values["intensity"] *= lightScale(index);
    usd.pExposure = number("exposure");
    usd.pRadius = number("radius");
    usd.pWidth = number("width");
    usd.pHeight = number("height");
    usd.pLength = number("length");
    usd.pAngleRadians = number("angle", true);
    usd.pConeAngleRadians = number("shaping:cone:angle", true);
    usd.pConeSoftness = number("shaping:cone:softness");
    usd.pFocus = number("shaping:focus");
    usd.pColorTemp = number("colorTemperature");
    remixapi_Float3D color{};
    if (o.contains("color")) {
      color = {o["color"][0], o["color"][1], o["color"][2]};
      usd.pColor = &color;
    }
    remixapi_Bool temperature = false;
    if (o.contains("enableColorTemperature")) {
      temperature = o["enableColorTemperature"].get<bool>();
      usd.pEnableColorTemp = &temperature;
    }
    if (o.contains("transform"))
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
          usd.transform.matrix[r][c] = o["transform"][c * 4 + r];
    auto texture = std::filesystem::path(o.value("texture", std::string()));
    const uint64_t id = original ^ 0x4d4f444c49474854ull;
    auto replacement = resources.overrideLight(handle, id ? id : 1, usd, o.contains("transform"),
                                               o.contains("texture") ? texture.c_str() : nullptr);
    state.overriddenLights.insert(replacement);
    resources.drawLight(replacement);
    return true;
  }
  const char *modDiagnostic() { return state.diagnostic.c_str(); }
  std::string modLoadText() {
    const auto progress = state.progress;
    if (!progress || !(state.pending.valid() || state.publishing))
      return {};
    const uint32_t done = progress->done, total = progress->total;
    const char *what = "Waiting";
    switch (progress->stage.load()) {
    case LoadStage::Waiting:
      break;
    case LoadStage::Opening:
      what = "Opening USD";
      break;
    case LoadStage::Meshes:
      what = "Processing meshes";
      break;
    case LoadStage::Materials:
      what = "Processing materials";
      break;
    case LoadStage::Cache:
      what = "Reading cache";
      break;
    case LoadStage::Textures:
      what = "Reading textures";
      break;
    case LoadStage::Finishing:
      what = "Finishing";
      break;
    }
    std::string text = std::string("Loading mods: ") + what;
    if (total)
      text += " (" + std::to_string(std::min(done, total)) + " / " + std::to_string(total) + ")";
    return text;
  }
  void setModMemoryPressure(bool pressure) { state.pressure = pressure; }
  ModStatus modStatus() {
    ModStatus status;
    status.active = state.enabled;
    status.enabled = state.modsEnabled;
    status.loading = state.pending.valid() || state.publishing;
    status.loadSeconds = state.loadSeconds;
    status.fromCache = state.active.fromCache && !state.active.snapshots.empty();
    for (const auto &write : state.cacheWrites)
      status.cacheWriting |= write.valid() && write.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    status.geometryBytes = state.active.geometryBytes;
    for (const auto &submission : state.submissions) {
      status.materials += submission->materialCount();
      status.materialsInFull += submission->materialsInFull();
    }
    status.directory = state.directory;
    status.diagnostic = state.diagnostic;
    status.mods = state.active.mods;
    // Pending toggles show at once.
    for (size_t i = 0; i < status.mods.size(); ++i) {
      auto &mod = status.mods[i];
      auto it = state.modParts.find(mod.name);
      mod.parts = it == state.modParts.end() ? ModParts{} : it->second;
      mod.lightScale = lightScale(i);
    }
    return status;
  }
  void setModLightScale(const std::string &name, float scale) {
    if (std::isfinite(scale) && scale >= 0)
      state.lightScales[name] = scale;
  }
  std::string saveModLightScale(const std::string &name) {
    for (size_t i = 0; i < state.active.mods.size(); ++i) {
      const auto &mod = state.active.mods[i];
      if (mod.name != name)
        continue;
      const float scale = lightScale(i);
      auto layer = pxr::SdfLayer::FindOrOpen(mod.path);
      if (!layer)
        return "Could not open " + mod.path;
      auto custom = layer->GetCustomLayerData();
      custom[lightScaleKey] = pxr::VtValue(double(scale));
      layer->SetCustomLayerData(custom);
      if (!layer->Save())
        return "Could not save " + mod.path;
      // The watch reloads the mod with it authored.
      state.active.mods[i].authoredLightScale = scale;
      state.lightScales.erase(name);
      return {};
    }
    return "No mod " + name;
  }
  void setModsEnabled(bool enabled) {
    if (state.modsEnabled == enabled)
      return;
    state.modsEnabled = enabled;
    state.reindex = true;
    ++state.partsVersion;
  }
  void setModParts(const std::string &name, const ModParts &parts) {
    auto it = state.modParts.find(name);
    if (it == state.modParts.end() ? parts == ModParts{} : it->second == parts)
      return;
    if (parts == ModParts{})
      state.modParts.erase(name);
    else
      state.modParts[name] = parts;
    state.reindex = true;
    ++state.partsVersion;
  }
  void setModEnabled(const std::string &name, bool enabled) {
    auto it = state.modParts.find(name);
    auto parts = it == state.modParts.end() ? ModParts{} : it->second;
    parts.enabled = enabled;
    setModParts(name, parts);
  }
  void reloadMods() {
    if (state.enabled)
      state.reload = true;
  }
} // namespace remix::usd
