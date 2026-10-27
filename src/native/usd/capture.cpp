#include "usd/capture.h"
#include "usd/capture_scene.h"
#include "usd/mods.h"
#include "platform/utf8_path.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <future>
#include <mutex>
#include <string_view>
#include <tuple>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace remix::usd {
  namespace {
    using State = CaptureStatus::State;
    // Calls come from the API thread (hooks and the boundary) and the panel
    // (requestCapture, captureStatus). `recording` lets the hooks return
    // without the lock on every other frame.
    struct Capture {
      std::mutex lock;
      std::atomic<bool> recording{false};
      bool requested = false;
      State state = State::Idle;
      std::vector<CaptureScene::Instance> instances;
      std::vector<uint64_t> lights;
      std::future<std::pair<std::string, std::string>> writing; // stage path, or an error
      std::string directory, path, message, name;
    } capture;

    uint64_t key(const void *handle) { return uint64_t(reinterpret_cast<uintptr_t>(handle)); }
    std::string utf8(const std::filesystem::path &path) {
      auto text = path.u8string();
      return {text.begin(), text.end()};
    }

    std::filesystem::path directory() {
      const char *configured = std::getenv("OPENREMIX_CAPTURE_DIRECTORY");
      return configured ? utf8Path(configured) : std::filesystem::current_path() / "rtx-remix/captures";
    }

    std::string executable() {
#if defined(_WIN32)
      std::wstring path(32768, L'\0');
      const auto size = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
      path.resize(size);
      return utf8(std::filesystem::path(path).filename());
#elif defined(__linux__)
      char path[4096];
      const auto size = readlink("/proc/self/exe", path, sizeof path - 1);
      return size > 0 ? std::filesystem::path(std::string(path, size_t(size))).filename().string() : std::string();
#else
      return {};
#endif
    }

    std::string timestamp() {
      const auto now = std::time(nullptr);
      std::tm local{};
#if defined(_WIN32)
      localtime_s(&local, &now);
#else
      localtime_r(&now, &local);
#endif
      char text[32];
      std::strftime(text, sizeof text, "%Y-%m-%d_%H-%M-%S", &local);
      return text;
    }

    // A file-safe stage name: the chosen one, else capture_<date>_<time>; the
    // time is appended when a capture of the chosen name exists.
    std::string stageName(const std::string &chosen) {
      std::string name;
      for (unsigned char c : chosen)
        name += c < 32 || std::string_view("\\/:*?\"<>|").find(char(c)) != std::string_view::npos ? '_' : char(c);
      while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
        name.pop_back();
      if (name.empty())
        return "capture_" + timestamp();
      std::error_code error;
      if (std::filesystem::exists(directory() / utf8Path(name + ".usda"), error))
        name += "_" + timestamp();
      return name;
    }

    bool skipped(const remixapi_InstanceInfo &info) {
      // As in drawInstance: halo sources and masks outline or cover what the
      // scene draws, and are not part of it. A CUT instance is.
      const auto *mask = static_cast<const remixapi_InstanceInfoMaskEXT *>(
          scene::extension(info.pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MASK_EXT));
      return scene::extension(info.pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_HALO_EXT) ||
             (mask && mask->role != REMIXAPI_INSTANCE_MASK_CUT_EXT);
    }

    void record(const scene::InstanceDescription &d, const std::vector<scene::Matrix> &locals,
                const float *origin) {
      CaptureScene::Instance instance;
      instance.mesh = d.mesh;
      instance.materialOverride = d.materialOverride;
      instance.categoryFlags = d.categoryFlags;
      instance.doubleSided = d.doubleSided;
      instance.blend = d.blend;
      if (d.hasAlphaState) {
        instance.alphaTest = d.alphaTest;
        instance.alphaCompare = d.alphaCompare;
        instance.alphaReference = d.alphaReference;
      }
      if (d.bones)
        instance.bones = *d.bones;
      const size_t count = d.instanced ? locals.size() : 1;
      for (size_t i = 0; i < count; ++i) {
        instance.model = d.instanced ? scene::multiply(d.model, locals[i]) : d.model;
        if (origin)
          for (int c = 0; c < 3; ++c)
            instance.model[12 + c] -= origin[c];
        capture.instances.push_back(instance);
      }
    }

    // Resolves the recorded handles against the resources as they are at the
    // frame's end, copying what the writer reads.
    class Resolver {
    public:
      Resolver(const NativeResources &resources, CaptureScene &scene) : resources_(resources), scene_(scene) {}

      bool mesh(uint64_t hash) {
        if (scene_.meshes.count(hash))
          return true;
        const auto mesh = resources_.findMesh(hash);
        if (!mesh) {
          scene_.warnings.push_back("Mesh " + hex(hash) + " was destroyed before the frame ended");
          return false;
        }
        CaptureScene::Mesh out;
        for (const auto &surface : mesh->surfaces) {
          out.surfaces.push_back(*surface);
          out.materials.push_back(material(key(surface->material)));
        }
        scene_.meshes.emplace(hash, std::move(out));
        return true;
      }

      uint64_t material(uint64_t hash) {
        if (!hash || scene_.materials.count(hash))
          return hash;
        const auto material = resources_.findMaterial(hash);
        if (!material)
          return 0;
        // Field by field: a render thread may be filling the emission cache.
        CaptureScene::Material out;
        const auto &m = *material;
        out.translucent = m.transmission;
        std::copy_n(m.tint, 4, out.tint);
        out.roughness = m.roughness;
        out.metallic = m.metallic;
        std::copy_n(m.emission, 3, out.emission);
        out.refractiveIndex = m.refractiveIndex;
        out.transmittanceDistance = m.transmittanceDistance;
        out.compare = m.compare;
        out.normalEncoding = m.normalEncoding;
        out.reference = m.reference;
        out.drawAlpha = m.drawAlpha;
        out.filter = m.filter;
        out.wrapU = m.wrapU;
        out.wrapV = m.wrapV;
        out.albedo = texture(m.texture);
        out.normal = texture(m.normalTexture);
        out.roughnessMap = texture(m.roughnessTexture);
        out.metallicMap = texture(m.metallicTexture);
        out.emissive = texture(m.emissionTexture);
        scene_.materials.emplace(hash, out);
        return hash;
      }

      uint64_t texture(const std::shared_ptr<scene::Texture> &texture) {
        if (!texture || resources_.isDefaultTexture(texture.get()))
          return 0;
        const auto hash = texture->getHash();
        if (!hash || scene_.textures.count(hash))
          return hash;
        scene::Pixels pixels;
        {
          // A dynamic texture's texels change under this lock.
          std::unique_lock<std::mutex> guard;
          if (texture->updates)
            guard = std::unique_lock(texture->updates->lock);
          pixels.width = texture->pixels.width;
          pixels.height = texture->pixels.height;
          pixels.srgb = texture->pixels.srgb;
          pixels.twoChannel = texture->pixels.twoChannel;
          pixels.mips = texture->pixels.mips;
          pixels.hdrMips = texture->pixels.hdrMips;
        }
        if (const auto &source = texture->pixels.compressed) {
          // A deep copy: the upload payload is released on the presenting
          // thread unless scene::retainCompressedUploads keeps it.
          auto c = std::make_shared<scene::CompressedPixels>();
          c->format = source->format;
          c->block = source->block;
          c->bc1Alpha = source->bc1Alpha;
          c->levels = source->levels;
          c->hash = source->hash;
          c->previewWidth = source->previewWidth;
          c->previewHeight = source->previewHeight;
          c->preview = source->preview;
          c->hdrPreview = source->hdrPreview;
          if (scene::retainCompressedUploads.load(std::memory_order_relaxed))
            c->upload = source->upload;
          if (c->upload.empty())
            ++degraded_;
          pixels.compressed = std::move(c);
        }
        scene_.textures.emplace(hash, std::move(pixels));
        return hash;
      }

      size_t degraded() const { return degraded_; }

      static std::string hex(uint64_t value) {
        char text[17];
        std::snprintf(text, sizeof text, "%016llX", static_cast<unsigned long long>(value));
        return text;
      }

    private:
      const NativeResources &resources_;
      CaptureScene &scene_;
      size_t degraded_ = 0;
    };

    void camera(const NativeResources &resources, CaptureScene::Camera &out) {
      if (!resources.cameraValid())
        return;
      // The host's camera, not the free camera. The projection's Y row is
      // stored negated for Vulkan (setupCamera).
      const auto &p = resources.projection(0);
      const auto toWorld = scene::inverse(resources.authoredView(0));
      const float y = std::abs(p[5]), x = std::abs(p[0]);
      if (!(y > 0) || !(x > 0) || !std::isfinite(y) || !std::isfinite(x))
        return;
      out.fovY = 2 * std::atan(1 / y);
      out.aspect = y / x;
      // A left-handed view looks down +Z (clip w = +z), a right-handed one -Z.
      const bool leftHanded = p[11] > 0;
      const float n = leftHanded ? -p[14] / p[10] : p[14] / p[10],
                  f = leftHanded ? p[14] / (1 - p[10]) : p[14] / (p[10] + 1);
      if (std::isfinite(n) && std::isfinite(f) && n > 0 && f > n) {
        out.nearPlane = n;
        out.farPlane = f;
      }
      // Rows: right, up, back (USD cameras look down -Z), position.
      for (int c = 0; c < 4; ++c) {
        const float sign = c == 2 && leftHanded ? -1.f : 1.f;
        for (int r = 0; r < 3; ++r)
          out.toWorld[c * 4 + r] = sign * toWorld[c * 4 + r];
        out.toWorld[c * 4 + 3] = c == 3 ? 1 : 0;
      }
      out.valid = true;
    }
  } // namespace

  void setCaptureName(const std::string &name) {
    std::lock_guard lock(capture.lock);
    capture.name = name;
  }

  void requestCapture() {
    std::lock_guard lock(capture.lock);
    capture.requested = true;
    if (capture.state != State::Recording && capture.state != State::Writing)
      capture.state = State::Requested;
  }

  void captureInstance(const remixapi_InstanceInfo &info) {
    if (!capture.recording.load(std::memory_order_relaxed) || skipped(info))
      return;
    scene::InstanceDescription d;
    try {
      scene::describeInstance(info, d);
    } catch (...) {
      return; // DrawInstance refuses it as well.
    }
    std::vector<scene::Matrix> locals;
    for (uint32_t i = 0; d.instanced && i < d.instanceCount; ++i)
      locals.push_back(scene::transform(d.instanceValues[i]));
    std::lock_guard lock(capture.lock);
    record(d, locals, nullptr);
  }

  void captureLight(remixapi_LightHandle handle) {
    if (!capture.recording.load(std::memory_order_relaxed))
      return;
    std::lock_guard lock(capture.lock);
    capture.lights.push_back(key(handle));
  }

  void captureSceneInstances(const NativeResources &resources, const float *origin) {
    if (!capture.recording.load(std::memory_order_relaxed))
      return;
    std::lock_guard lock(capture.lock);
    for (const auto &entry : resources.retainedInstances()) {
      const auto &d = entry.description;
      if (entry.visible && !d.halo && !d.maskSource && !d.maskGroup)
        record(d, entry.locals, origin);
    }
  }

  void captureBoundary(const NativeResources &resources) {
    std::unique_lock lock(capture.lock);
    if (capture.writing.valid() && capture.writing.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      auto [path, error] = capture.writing.get();
      capture.state = error.empty() ? State::Complete : State::Failed;
      capture.path = path;
      if (!error.empty())
        capture.message = error;
    }
    if (capture.recording.exchange(false)) {
      auto scene = std::make_shared<CaptureScene>();
      scene->name = stageName(capture.name);
      scene->executable = executable();
      std::tie(scene->metersPerUnit, scene->upAxis) = modUnits();
      Resolver resolve(resources, *scene);
      for (auto &instance : capture.instances) {
        if (!resolve.mesh(instance.mesh))
          continue;
        instance.materialOverride = resolve.material(instance.materialOverride);
        scene->instances.push_back(std::move(instance));
      }
      for (auto hash : capture.lights)
        if (const auto *light = resources.lightSource(hash))
          scene->lights.emplace(hash, CaptureScene::Light{*light, resolve.texture(light->cookie)});
      camera(resources, scene->camera);
      if (resolve.degraded())
        scene->warnings.push_back(std::to_string(resolve.degraded()) +
                                  " compressed textures were written from their preview level: enable "
                                  "\"Retain texture data for captures\" before the textures load");
      capture.instances.clear();
      capture.lights.clear();
      capture.state = State::Writing;
      capture.message.clear();
      for (const auto &warning : scene->warnings)
        capture.message += (capture.message.empty() ? "" : "\n") + warning;
      capture.directory = utf8(directory());
      try {
        capture.writing = std::async(std::launch::async, [scene, target = directory()] {
          try {
            return std::pair{utf8(writeCapture(*scene, target)), std::string()};
          } catch (const std::exception &e) {
            return std::pair{std::string(), std::string(e.what())};
          }
        });
      } catch (...) {
        capture.state = State::Failed;
        capture.message = "Could not start the capture writer";
      }
    }
    if (capture.requested && capture.state != State::Writing) {
      capture.requested = false;
      capture.state = State::Recording;
      capture.recording = true;
    }
  }

  CaptureStatus captureStatus() {
    std::lock_guard lock(capture.lock);
    CaptureStatus status;
    status.state = capture.state;
    status.directory = capture.directory.empty() ? utf8(directory()) : capture.directory;
    status.path = capture.path;
    status.message = capture.message;
    status.name = capture.name;
    return status;
  }
} // namespace remix::usd
