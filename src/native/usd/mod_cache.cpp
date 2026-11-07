#include "usd/mod_cache.h"
#include "platform/utf8_path.h"
#include "scene/types.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace remix::usd {
  namespace {
    // Bump when what a mod's load makes changes in a way a new build's file stamp
    // would not show (it always does in a release).
    constexpr uint32_t format = 1;
    constexpr uint32_t magic = 0x434d524f; // "ORMC"
    constexpr uint32_t endMagic = 0x444e4543;
    // Keep this much of a disk free: a cache is never worth filling one.
    constexpr uint64_t spareDisk = 10ull << 30;
    struct Header {
      uint32_t magic, format;
      uint64_t tables, blob;
    };
    int64_t stampTime(std::filesystem::file_time_type time) { return int64_t(time.time_since_epoch().count()); }
    // A file's time and size, as far as it exists: an absent dependency appearing
    // changes what loads too.
    Json stamp(const std::filesystem::path &path) {
      std::error_code timeError, sizeError;
      const auto time = std::filesystem::last_write_time(path, timeError);
      const auto size = std::filesystem::file_size(path, sizeError);
      return Json::array({timeError ? int64_t(0) : stampTime(time), sizeError ? uint64_t(0) : uint64_t(size)});
    }
    // This runtime's file: another build may compose differently.
    Json runtimeStamp() {
      static const int anchor = 0;
#ifdef _WIN32
      HMODULE module{};
      std::wstring file(32768, L'\0');
      if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&anchor), &module)) {
        const auto size = GetModuleFileNameW(module, file.data(), DWORD(file.size()));
        if (size && size < file.size()) {
          file.resize(size);
          return stamp(std::filesystem::path(file));
        }
      }
#else
      Dl_info info{};
      if (dladdr(&anchor, &info) && info.dli_fname)
        return stamp(std::filesystem::path(info.dli_fname));
#endif
      return Json::array({0, 0});
    }
    Json identity(double meters, uint32_t axis) {
      return {{"format", format},
              {"vertex", sizeof(scene::Vertex)},
              {"runtime", runtimeStamp()},
              {"meters", meters},
              {"axis", axis}};
    }
    template <class T> uint64_t arrayBytes(const std::vector<T> &values) { return values.size() * sizeof(T); }
  } // namespace

  std::filesystem::path modCacheFile(const std::string &modPath) {
    const char *setting = std::getenv("OPENREMIX_USD_CACHE");
    if (setting && std::strcmp(setting, "0") == 0)
      return {};
    std::error_code error;
    const auto working = std::filesystem::current_path(error);
    const char *directory = std::getenv("REMIX_CONFIG_DIR");
    const auto config =
        directory && *directory ? utf8Path(directory) : (error ? std::filesystem::path(".") : working) / "openremix";
    const auto name = scene::hashBytes(modPath.data(), modPath.size());
    return config / "cache" / "usd" / (std::to_string(name) + ".bin");
  }

  std::optional<CachedMod> readModCache(const std::filesystem::path &file, double meters, uint32_t axis,
                                        Progress &progress) {
    if (file.empty())
      return std::nullopt;
    try {
      std::ifstream in(file, std::ios::binary | std::ios::ate);
      if (!in)
        return std::nullopt;
      const uint64_t length = uint64_t(in.tellg());
      Header header{};
      if (length < sizeof(header) + sizeof(endMagic))
        return std::nullopt;
      in.seekg(0);
      if (!in.read(reinterpret_cast<char *>(&header), sizeof(header)) || header.magic != magic ||
          header.format != format || length != sizeof(header) + header.tables + header.blob + sizeof(endMagic))
        return std::nullopt;
      // The last thing written: a file ending in it was written whole.
      uint32_t end = 0;
      in.seekg(std::streamoff(length - sizeof(end)));
      if (!in.read(reinterpret_cast<char *>(&end), sizeof(end)) || end != endMagic)
        return std::nullopt;
      std::vector<uint8_t> bytes(static_cast<size_t>(header.tables));
      in.seekg(sizeof(header));
      if (!in.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(bytes.size())))
        return std::nullopt;
      progress.enter(LoadStage::Cache, 0);
      const Json tables = Json::from_cbor(bytes);
      std::vector<uint8_t>().swap(bytes);
      if (tables.at("identity") != identity(meters, axis))
        return std::nullopt;
      CachedMod mod;
      for (const auto &entry : tables.at("stamps")) {
        const auto path = entry.at(0).get<std::string>();
        if (stamp(utf8Path(path)) != entry.at(1))
          return std::nullopt;
        mod.snapshot.dependencies.insert(path);
      }
      mod.snapshot.scene = tables.at("scene");
      const uint64_t blobStart = sizeof(header) + header.tables;
      struct Job {
        std::shared_ptr<scene::Surface> surface;
        uint64_t offset;
      };
      std::vector<Job> jobs;
      for (const auto &part : tables.at("parts")) {
        std::vector<std::shared_ptr<scene::Surface>> surfaces;
        std::vector<std::string> materials;
        for (const auto &source : part.at("surfaces")) {
          auto surface = std::make_shared<scene::Surface>();
          surface->vertices.resize(source.at("vertices").get<size_t>());
          surface->indices.resize(source.at("indices").get<size_t>());
          surface->weights.resize(source.at("weights").get<size_t>());
          surface->bones.resize(source.at("bones").get<size_t>());
          surface->bonesPerVertex = source.at("bonesPerVertex").get<uint32_t>();
          surface->center = source.at("center").get<std::array<float, 3>>();
          const auto offset = source.at("offset").get<uint64_t>();
          if (offset > header.blob || arrayBytes(surface->vertices) + arrayBytes(surface->indices) +
                                              arrayBytes(surface->weights) + arrayBytes(surface->bones) >
                                          header.blob - offset)
            return std::nullopt;
          jobs.push_back({surface, offset});
          surfaces.push_back(std::move(surface));
          materials.push_back(source.at("material").get<std::string>());
        }
        mod.meshes.parts.emplace(part.at("key").get<uint64_t>(), std::pair{std::move(surfaces), std::move(materials)});
      }
      // In parallel: geometry is nearly all of the time here.
      progress.enter(LoadStage::Cache, uint32_t(jobs.size()));
      std::atomic<size_t> next{0};
      std::atomic<bool> bad{false};
      const auto work = [&] {
        std::ifstream blob(file, std::ios::binary);
        for (size_t i; !bad && (i = next++) < jobs.size();) {
          auto &surface = *jobs[i].surface;
          const auto read = [&](auto &values) {
            return values.empty() ||
                   bool(blob.read(reinterpret_cast<char *>(values.data()), std::streamsize(arrayBytes(values))));
          };
          blob.seekg(std::streamoff(blobStart + jobs[i].offset));
          bool good = read(surface.vertices) && read(surface.indices) && read(surface.weights) && read(surface.bones);
          // As createMesh checks them: the renderer indexes with these.
          for (const auto index : surface.indices)
            good = good && index < surface.vertices.size();
          for (const auto bone : surface.bones)
            good = good && bone < 256;
          if (!good)
            bad = true;
          ++progress.done;
        }
      };
      const unsigned threads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
      std::vector<std::thread> pool;
      for (unsigned t = 1; t < threads && t < jobs.size(); ++t)
        pool.emplace_back(work);
      work();
      for (auto &thread : pool)
        thread.join();
      if (bad)
        return std::nullopt;
      for (const auto &source : tables.at("meshes")) {
        PreparedMesh mesh;
        mesh.id = source.at("id").get<std::string>();
        const auto transform = source.at("transform").get<std::vector<float>>();
        if (transform.size() != 12)
          return std::nullopt;
        std::memcpy(mesh.transform.matrix, transform.data(), sizeof(mesh.transform.matrix));
        mesh.doubleSided = source.at("doubleSided").get<bool>();
        mesh.alphaState = source.at("alphaState");
        mesh.bones = source.at("bones").get<uint32_t>();
        for (const auto &part : source.at("parts")) {
          PreparedMesh::Part out;
          out.key = part.at(0).get<uint64_t>();
          out.flags = part.at(1).get<uint32_t>();
          const auto &known = mod.meshes.parts.at(out.key);
          out.surfaces = known.first;
          out.materials = known.second;
          mesh.parts.push_back(std::move(out));
        }
        mod.meshes.meshes.push_back(std::move(mesh));
      }
      mod.meshes.failed = tables.at("failed").get<std::set<std::string>>();
      mod.meshes.bytes = tables.at("bytes").get<uint64_t>();
      return mod;
    } catch (...) {
      // Anything unexpected in the file: the mod loads from its USD.
      return std::nullopt;
    }
  }

  std::optional<ModCacheWrite> prepareModCacheWrite(const std::filesystem::path &file, const Snapshot &snapshot,
                                                    const PreparedMeshes &meshes, double meters, uint32_t axis,
                                                    std::filesystem::file_time_type loadStarted) {
    if (file.empty())
      return std::nullopt;
    Json tables = {{"identity", identity(meters, axis)},
                   {"scene", snapshot.scene},
                   {"stamps", Json::array()},
                   {"meshes", Json::array()},
                   {"parts", Json::array()},
                   {"failed", meshes.failed},
                   {"bytes", meshes.bytes}};
    // A change in the moments before the load counts too: file times are coarse.
    const auto limit = stampTime(loadStarted - std::chrono::seconds(2));
    auto &stamps = tables["stamps"];
    for (const auto &path : snapshot.dependencies) {
      auto current = stamp(utf8Path(path));
      if (current[0].get<int64_t>() > limit)
        return std::nullopt;
      stamps.push_back(Json::array({path, std::move(current)}));
    }
    ModCacheWrite write;
    write.file = file;
    uint64_t offset = 0;
    for (const auto &[key, part] : meshes.parts) {
      Json surfaces = Json::array();
      for (size_t i = 0; i < part.first.size(); ++i) {
        const auto &surface = part.first[i];
        surfaces.push_back({{"material", part.second[i]},
                            {"vertices", surface->vertices.size()},
                            {"indices", surface->indices.size()},
                            {"weights", surface->weights.size()},
                            {"bones", surface->bones.size()},
                            {"bonesPerVertex", surface->bonesPerVertex},
                            {"center", surface->center},
                            {"offset", offset}});
        offset += arrayBytes(surface->vertices) + arrayBytes(surface->indices) + arrayBytes(surface->weights) +
                  arrayBytes(surface->bones);
        write.surfaces.push_back(surface);
      }
      tables["parts"].push_back({{"key", key}, {"surfaces", std::move(surfaces)}});
    }
    for (const auto &mesh : meshes.meshes) {
      Json parts = Json::array();
      for (const auto &part : mesh.parts)
        parts.push_back(Json::array({part.key, part.flags}));
      std::vector<float> transform(12);
      std::memcpy(transform.data(), mesh.transform.matrix, sizeof(mesh.transform.matrix));
      tables["meshes"].push_back({{"id", mesh.id},
                                  {"transform", std::move(transform)},
                                  {"doubleSided", mesh.doubleSided},
                                  {"alphaState", mesh.alphaState},
                                  {"bones", mesh.bones},
                                  {"parts", std::move(parts)}});
    }
    write.tables = Json::to_cbor(tables);
    write.bytes = offset;
    return write;
  }

  void writeModCache(ModCacheWrite write, const std::atomic<bool> &cancel) {
    auto temporary = write.file;
    temporary += ".tmp";
    std::error_code error;
    try {
      std::filesystem::create_directories(write.file.parent_path(), error);
      const auto space = std::filesystem::space(write.file.parent_path(), error);
      if (error || space.available < write.bytes + write.tables.size() + spareDisk)
        return;
      bool good = false;
      {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        const Header header{magic, format, write.tables.size(), write.bytes};
        out.write(reinterpret_cast<const char *>(&header), sizeof(header));
        out.write(reinterpret_cast<const char *>(write.tables.data()), std::streamsize(write.tables.size()));
        const auto put = [&](const auto &values) {
          if (!values.empty())
            out.write(reinterpret_cast<const char *>(values.data()), std::streamsize(arrayBytes(values)));
        };
        for (const auto &surface : write.surfaces) {
          if (cancel)
            break;
          put(surface->vertices);
          put(surface->indices);
          put(surface->weights);
          put(surface->bones);
        }
        out.write(reinterpret_cast<const char *>(&endMagic), sizeof(endMagic));
        out.flush();
        good = bool(out) && !cancel;
      }
      if (good)
        std::filesystem::rename(temporary, write.file, error);
      if (!good || error)
        std::filesystem::remove(temporary, error);
    } catch (...) {
      std::filesystem::remove(temporary, error);
    }
  }
} // namespace remix::usd
