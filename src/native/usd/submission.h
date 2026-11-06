#pragma once
#include "scene/resources.h"
#include "usd/loader.h"
#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <remix/remix_native_ext.h>
#include <set>

namespace remix::usd {
  // CPU resources only. Draws hold shared ownership while GPU retirement proceeds.
  // A mod's mesh made ready off the Present thread (a mod's load): surfaces with
  // their corners welded, and all but their materials' handles set.
  struct PreparedMesh {
    std::string id;
    remixapi_Transform transform{};
    bool doubleSided = false;
    Json alphaState;
    uint32_t bones = 0; // the most a surface's joints name
    // One draw per run of surfaces with the same category flags.
    struct Part {
      uint32_t flags = 0;
      uint64_t key = 0; // the same in a mod: the same geometry
      std::vector<std::shared_ptr<scene::Surface>> surfaces;
      std::vector<std::string> materials; // per surface
    };
    std::vector<Part> parts;
  };
  // A mod's load: what was made ready, shared between its meshes.
  struct PreparedMeshes {
    std::vector<PreparedMesh> meshes;
    std::map<uint64_t, std::pair<std::vector<std::shared_ptr<scene::Surface>>, std::vector<std::string>>> parts;
    std::set<std::string> failed; // replacements with a mesh that could not be made
    uint64_t bytes = 0;           // of the geometry
    // Any thread, with `mutex` guarding all of this.
    void add(ComposedMesh &&mesh, std::mutex &mutex);
  };
  // A mod's textures as its load read them: each file's low levels, and in full
  // the few that lights use.
  struct PreparedTextures {
    std::map<std::string, std::shared_ptr<scene::Texture>> previews, whole;
  };
  // The largest edge of the levels a mod's load reads of each texture file.
  inline constexpr uint32_t previewEdge = 128;
  // A mod ready to publish, made off the Present thread (prepareMod).
  struct PreparedMod {
    // A replacement: its attached lights (lights) and, once published, its
    // instances.
    struct Group {
      bool failed = false; // A mesh or material could not be made: not applied.
      std::vector<size_t> lights, instances;
    };
    // A material's stream state. Its textures are the low levels until it is
    // first drawn, and again after pressure lets it go.
    struct Streamed {
      std::string id;
      remixapi_MaterialHandle handle = nullptr;
      std::vector<std::string> paths;
      std::shared_ptr<uint8_t> use = std::make_shared<uint8_t>(0);
      uint64_t lastUse = 0;
      bool full = false, queued = false;
    };
    PreparedMeshes meshes;
    PreparedTextures textures;
    std::map<std::string, Json> materials;
    std::map<std::string, Group> groups;
    std::vector<Json> lights;
    std::vector<Streamed> streamed;
  };
  PreparedMod prepareMod(const Snapshot &snapshot, PreparedMeshes &&meshes, PreparedTextures &&textures);
  remixapi_Transform transformed(const remixapi_Transform &delta, const remixapi_Transform &original);
  class Submission {
  public:
    explicit Submission(NativeResources &resources, const Snapshot &snapshot, uint64_t namespaceId,
                        bool applyMods = true);
    // A mod's replacements (/RootNode/meshes/mesh_<hash>, and the lights under
    // /RootNode/lights/light_<hash>). publish creates them; afterwards each
    // draws from its first draw on. A material starts on its textures' low
    // levels and takes the whole files once drawn (service).
    Submission(NativeResources &resources, PreparedMod &&mod, uint64_t namespaceId);
    ~Submission();
    // Prepared: creates what it can until `deadline`. True once all is created.
    bool publish(std::chrono::steady_clock::time_point deadline);
    // Prepared: whether the replacement at `root` draws. Always true otherwise.
    bool ready(const std::string &root) const;
    // Prepared, once a frame: pulls in the textures of the materials drawn
    // since, and with `pressure` (video memory is short) lets go of those of
    // materials not drawn for a while.
    void service(bool pressure);
    // Prepared: the mod material keys (scene's m_modMaterials) a material has,
    // kept up to date as its object changes.
    void setMaterialKeys(std::map<std::string, std::vector<uint64_t>> keys) { materialKeys_ = std::move(keys); }
    size_t materialCount() const { return streamed_.size(); }
    size_t materialsInFull() const { return materialsInFull_; }
    void retireAttachments();
    // `meshes`/`lights` select what of the subtree draws (mod part toggles).
    void draw(bool camera, float aspect, const remixapi_InstanceInfo *parent = nullptr,
              const std::string &root = "", bool meshes = true, bool lights = true);
    // Materials are created on first use; their textures load then unless
    // already cached (NativeResources::cacheTexturePath).
    remixapi_MaterialHandle materialHandle(const std::string &id);
    const std::vector<remixapi_InstanceInfo> &instances() const { return instances_; }
    // Multiplies the intensity of the lights it creates from now on (a mod's
    // light scale; attached lights are created each frame).
    void setLightScale(float scale) { lightScale_ = scale; }

  private:
    void clear();
    void material(const Json &);
    void mesh(const Json &);
    void mesh_group(const Json &, uint32_t);
    void add_instance(const Json &, remixapi_MeshHandle, uint32_t);
    void add_light(const Json &);
    using Group = PreparedMod::Group;
    using StreamedMaterial = PreparedMod::Streamed;
    // Whole files decoding off the Present thread for the materials named.
    struct Pull {
      std::vector<size_t> materials;
      std::vector<std::string> paths;
      std::vector<std::shared_ptr<scene::Texture>> textures;
    };
    void createStreamed(StreamedMaterial &, bool full);
    void publishMesh(PreparedMesh &);
    bool prepared_ = false;
    bool published_ = false;
    float lightScale_ = 1;
    PreparedMeshes prepared_meshes_;
    size_t next_material_ = 0, next_mesh_ = 0;
    std::map<std::string, Group> groups_;
    std::map<std::string, std::shared_ptr<scene::Texture>> previews_, whole_;
    std::map<std::string, std::weak_ptr<scene::Texture>> pulled_;
    std::map<std::string, std::vector<uint64_t>> materialKeys_;
    std::vector<StreamedMaterial> streamed_;
    std::vector<size_t> pull_queue_;   // drawn, in the order first seen
    std::vector<size_t> pull_ready_;   // decoded, not yet given their files
    std::vector<std::shared_ptr<scene::Texture>> pull_held_; // what pull_ready_ takes
    std::future<Pull> pull_;
    uint64_t frame_ = 0;
    size_t materialsInFull_ = 0;
    remixapi_LightHandle create_light(const Json &, const remixapi_Transform &);
    NativeResources &resources_;
    uint64_t namespace_;
    bool applyMods_;
    std::vector<uint64_t> instance_hashes_;
    std::vector<uint32_t> instance_bones_; // bones an instance's skinned mesh needs
    Json snapshot_;
    std::map<std::string, remixapi_MaterialHandle> materials_;
    std::map<std::string, Json> material_defs_;
    std::vector<remixapi_MeshHandle> meshes_;
    std::map<uint64_t, remixapi_MeshHandle> mesh_cache_;
    std::vector<remixapi_InstanceInfo> instances_;
    std::vector<remixapi_InstanceInfoBlendEXT> blend_states_;
    std::vector<std::pair<std::string, remixapi_Transform>> instance_origins_;
    std::vector<remixapi_LightHandle> lights_;
    std::vector<Json> light_origins_;
    std::set<remixapi_LightHandle> attached_lights_, used_attachments_;
  };
} // namespace remix::usd
