# Forward material and lighting architecture

## Direction

Forward+ is the rendering framework, not a lighting technique. Its default preset
is named **Source 2**: baked static direct lighting and diffuse irradiance on
surface lightmaps, volume probes for moving receivers, and parallax-corrected
cubemap reflections. Dynamic casters modify baked lighting through mixed shadows.
Full-resolution SSR remains the screen-space reflection layer. MSAA remains the
default antialiasing method. This describes our preset, not exact engine parity.

Material authors should initialize a surface, change its properties and call one
standard shading entry point. Material evaluation must not know the lightmap
layout, probe lookup, shadow atlas, cluster format or reflection source. Source
engine classification and entity/light-style translation stay in the adapter.

The s&box reference is `Material::Init` followed by
`ShadingModelStandard::Shade`, with direct and indirect terms combined internally.
Our GLSL interface should offer that simplicity using ordinary structs/functions;
matching syntax does not require migrating shader languages or exposing GPU ABI.

## Contracts

- **Surface:** linear albedo, emission, roughness, metalness, opacity and shading
  normal. Keep sampled material values separate from packed GPU material records.
- **Receiver:** position, geometric normal, view direction, coverage, receiver
  classification and optional baked coordinates/identity. Viewmodels change camera
  and shadow policy, not the meaning of lighting or material properties.
- **Lighting:** separate diffuse irradiance, direct/specular response and reflected
  radiance. Document irradiance versus irradiance/pi at each boundary. Apply albedo,
  BRDF, exposure and display conversion exactly once at their respective stages.
- **Visibility:** attributed static visibility and dynamic occlusion are distinct.
  Baked lights must not silently return to static-world shadow rendering.
- **Reflection transport:** scene-linear radiance and confidence, before receiver
  BRDF. SSR composes once with the selected fallback; normalize captured lighting
  against receiver irradiance before that composition.

Initial extraction: `shaders/receiver-lighting.glsl` owns diffuse receiver lookup
for world and viewmodels. It preserves the existing surface/probe/live-GI behavior
and aggregate-light bookkeeping.

`shaders/surface.glsl` now defines evaluated surface values and the standard PBR
decoder. It separates material sampling, emission, normal mapping and roughness
filtering from receiver lighting. The packed `Material` record remains an internal
compatibility input; it is not the shader authoring contract.

`shadeStandardSurface(Surface, ShadingReceiver)` is the standard world/viewmodel
entry point. `ShadingResult` contains scene-linear colour and reflection guides;
pass adapters retain coverage/depth policy and output attachment writes. Baked
receiver coordinates, chart bounds and light identities are explicit inputs to
local-light evaluation, shared with the specialized water path. These are internal
contracts, not a stable public shader ABI. Independent provider lifecycles and runtime
material program loading remain pending.

Reflection normalization uses the existing RGBA16F guide: octahedral world normal
in XY, roughness in Z, receiver irradiance/pi luminance in W. Negative roughness
marks invalid geometry; negative luminance marks unavailable lighting; zero
luminance is known darkness. All consumers use `reflection-guide.glsl` to decode
the normal. The same covered MSAA sample supplies normal, roughness, irradiance,
F0 and depth. Metal-only shading still evaluates incident irradiance for this
purpose. The earlier separate R16F/R32F irradiance targets and fifth storage-image
write have been removed. Scene quality and performance qualification still precede
playable deployment; compact storage alone does not establish a frame-time gain.

World, viewmodel and water cubemap fallback now share lighting normalization.
Capture reference luminance follows the same spatial, priority and feather weights
as radiance; explicit assignments use their selected layer's reference. Correction
is applied before SSR composition. Unknown receiver lighting and nonpositive
capture references remain neutral; known black can suppress reflection energy.
The initial gain ceiling is one, matching VRF's modern non-VR profile. This does
not implement every Source 2 lighting profile or its rough nonmetal lightmap-SH
reflection transition. Severe HDR haze remains an open visual diagnosis.

## Opt-in material shader overrides

Lighting presets select techniques. Material shader selection is independent:
the adapter may map a game's shader identity to an optional material implementation.
Unmapped materials keep standard PBR conversion, AutoPBR and Remix replacements.
Source shader names and VMT interpretation belong in the GMod adapter, not native
renderer dispatch. Exact Source material compatibility is optional and must not
implicitly replace the selected lighting providers.

A future single-file authoring format can declare parameters, textures, stage code,
supported passes and named resource requirements. Scene colour/depth requirements
must participate in pass planning; shaders must not rely on internal descriptor
numbers or access arbitrary renderer resources. Depth, alpha coverage, shadow,
MSAA and viewmodel behavior need explicit contracts and sensible fallbacks.

Evaluate HLSL/Slang compiler integration separately from the file format. Do not
migrate all shaders or expose a new public ABI just to resemble `.shader`/`.vfx`
syntax. Start with the standard Surface and shading interface, then prove the
override path with a Refract material before broad compatibility work. Envmap masks
can control reflection response while reflection radiance still comes from the
chosen provider. Capability validation, resource ownership and cached variant
identity must be part of the design before accepting adapter-supplied programs.

## Modules and presets

Resolve configuration into an immutable per-frame lighting plan on the CPU. A
preset supplies defaults; explicit component overrides produce a Custom selection.
Keep quality parameters independent of technique selection. Preserve existing
configuration keys through deliberate migration rather than resetting user files.

Independent services are static direct lighting, diffuse irradiance, reflection
fallback, screen-space reflection transport and dynamic visibility. Modules declare
the services they provide, required geometry/resources, update policy and supported
receiver types. A unified solution can provide multiple services with shared state;
it must replace those services together without duplicate baked/analytic energy.
Unsupported combinations need explicit diagnostics and documented fallback.

Do not implement per-pixel virtual dispatch or a generic runtime shader plugin
system. Select bounded shader variants and passes from the resolved plan, allocate
only required resources, and retain existing fenced publication/lifetime rules.
Providers should own bake/cache invalidation and their GPU resources. Material
changes and viewmodel changes must not invalidate unrelated static lighting.

Initial CPU integration: `lighting_plan.h` resolves existing baked/live-GI energy
ownership and mixed-light selection without mutating requested quality or custom
bake settings. Bake publication and authored reflection composition share one
resolved configuration per recording. `presets::source2()` now constructs the
runtime default profile under the name Source 2, preserving its previous values.
Typed technique selection now lives in `techniques.h`: static direct lighting,
diffuse transport, reflection fallback, SSR and dynamic shadow requests are separate
fields. The default preset and UI use this shared selection layer. The F12 Rendering panel now offers Source 2
selection and derives Custom from individual technique changes. Selecting Source 2
preserves quality, display, cache and authored-volume preferences. Existing component
keys remain the persisted representation; no separate preset state can contradict
them. A disabled zero cubemap budget is restored to the default positive budget
when the preset enables cubemaps. Existing supported technique combinations can
be selected without exposing storage details to materials. This is not a generic
provider registry: bake captures, surface fields and probe fields still share a
controller and resource lifecycle. Fully independent resource ownership and
shader-module composition remain pending. Disabled baking also stops cubemap
capture updates; the UI explains that coupling rather than implying independence.

Preset application preserves model lightmap chart eligibility as a receiver-coverage
preference. The default profile enables it, but manual-density configurations may
disable it because model charts require budget fitting. Technique selection must
not silently change that quality policy or enable an unsupported combination.

Candidate future providers include DDGI, radiance cascades, distance-field or
volume-based reflections and virtual shadow maps. They are design targets, not
implemented options or promises of interchangeable quality. Their directional,
visibility, temporal and specular limits must be measured before presets use them.

## Migration sequence

1. Extract current receiver lighting without changing images. Establish material,
   receiver and lighting result contracts; migrate standard world and viewmodel
   shading to one entry point. Keep specialized water/coverage paths explicit.
2. Extract baked direct, diffuse surface/probe and reflection implementations behind
   those contracts. Finish cubemap normalization with actual surface irradiance.
3. Introduce the resolved lighting plan and Source 2 preset in settings and UI.
   Migrate existing settings, preserve overrides, and test cache/resource transitions.
4. Inventory experimental flags, debug environment switches, duplicated paths and
   stale aliases. Classify each as supported, diagnostic, pending evidence or retired.
   Remove retired implementations and callers together; do not delete working
   fallbacks or historical evidence merely because they are old.
5. Profile CPU submission, shader execution, overdraw, light lists and reflection
   work independently. Extend aggregation only where measured: depth batching is
   already supported, colour batching needs material/visibility/receiver correctness.
   Avoid claiming fewer source draws means a proportional frame-time improvement.
6. Add alternative providers only after the default path is verified and the first
   real alternative proves the contracts useful. Keep quality issues and the existing
   roadmap active throughout the migration.

## Acceptance

Behavior-preserving steps require shader compilation across world, viewmodel,
zero-diffuse, live GI, water, MSAA and depth variants, plus relevant GPU readbacks.
Use frozen scene comparisons before deployment and measure performance separately.
Preset switching must preserve styled-light behavior, dynamic shadows, authored
cubemaps, cache reuse and frame-slot ownership. No public Remix ABI changes are
required. Reference VRF for Source 2 lighting behavior and s&box for shader authoring
structure; distinguish their observed behavior from claims about native CS2.

Known quality issues remain: HDR/reflection haze, rough prop probe interpolation,
Construct lightmap artifacts, outdoor cubemap coverage, and bake/runtime cost.
Architecture work does not mark those fixed. Detailed tasks remain in the
[Forward+ roadmap](../features/forward-plus-roadmap.md).

## Initial settings audit

`lighting_plan.h` now resolves the ownership policy behind several legacy switches:
`bakedLighting`, `selectedBakedLights`, `aggregateDiffuse`, `fullyBakedLights`,
`bakedGeometryLights` and `groupedStyledDirect`. Some still carry experimental
labels even though runtime baked mode selects related paths automatically. These
still need a behavior/caller inventory and migration to independently selected
providers. Live GI ownership is resolved centrally; callers must not create a
second policy through independent booleans. None of these legacy paths is declared
unused merely because it has an experimental label.

The rough-metal fixture exposed fallback cubemap coverage holes: subdivision
created more cells than valid capture sites, then discarded empty cells. Fallback
placement now splits populated site sets, retaining coverage with sparse origins.
This concerns scenes without a closed spatial partition; fitted BSP coverage and
Construct's outdoor gaps remain separate quality work.

## Irradiance encoding as a provider boundary

The current surface path stores full nine-coefficient SH and performs up to four
filtered atlas fetches per coefficient with bicubic reconstruction. Exactly-zero
basis terms skip their fetches; no SH bands or weak nonzero terms are discarded.

A useful next alternative is the compact lightmap representation in the audited
VRF `Renderer/Shaders/common/lighting.slang`: DC plus luminance or per-channel
linear directional bands, with a reconstructed zonal band. Its explicit bicubic
helper covers shadow masks; do not assume all reference irradiance paths use our
nine-coefficient bicubic reconstruction. This describes that reference profile,
not every Source 2 game or material.

Compare encoding error, normal-map response, styled contributions and rough-surface
reflection behavior before selecting a compact provider by default. Keep material
shading dependent on irradiance units and receiver metadata, not coefficient count.
An encoding change must version GPU/cache layouts and retain chart isolation;
rough-lightmap reflections must replace the appropriate fallback before SSR rather
than adding another specular contribution. Runtime provider lifecycles remain a
separate migration from this typed technique selection layer.

## Camera visibility versus scene retention

The current Forward+ path rejects draw bounds outside the camera frustum and
uses a depth prepass. It consumes the camera backface flag but does not yet apply
the relay's camera PVS flag. The adapter submits the retained BSP independently
of the engine's world draw list; engine PVS therefore does not provide equivalent
visibility rejection for that retained world. The path tracer already consumes
camera visibility metadata; Forward+ must not be assumed to share that behavior.

Keep the complete scene resident for baking, reflections and shadow queries.
Derive camera-visible draw lists using shared scene visibility/PVS data and
frustum checks, while deriving caster lists from each light's influence. Camera
invisibility must not remove offscreen mixed-shadow casters or retire geometry.
Place reusable CPU visibility policy in scene code, not the path tracer or a
Source-specific native parser. Missing visibility data and uncertain bounds must
fail open. Per-view state must also respect skybox and viewmodel exemptions.

Measure visibility rejection on corridor and open maps separately. Early depth
already removes much hidden fragment work; PVS can reduce submission and vertex
work without necessarily reducing the visible material shading bottleneck.
Occlusion/Hi-Z is a further candidate, not an implemented equivalent of PVS.


### Technique separation in progress

The standard material entry point delegates direct illumination to
`shaders/direct-lighting.glsl`. That module accumulates sun and clustered local
responses in the existing order, including attributed visibility, mixed shadows,
and specular. World and viewmodel materials share this entry point; exposure,
emission, and reflection composition stay outside the direct provider.

This is a shader boundary, not a completed provider framework. Remaining work
includes independently owned diffuse and reflection resources and update passes,
compile-time provider selection, capability resolution, and removal of the
bake/cubemap lifecycle coupling. A future radiance-cascade provider should supply
diffuse transport through the shared receiver contract rather than teach each
material about cascade storage. Keep direct illumination ownership explicit so
replacing indirect transport cannot double-count baked direct light.

Performance acceptance requires comparison against the unchanged Source 2
profile, including shader work, CPU recording, GPU timings, and memory. Shader
function extraction alone does not establish performance equivalence.


### Ray-query provider lifecycle

`forward/ray/provider.h` owns the optional live scene and reflection continuation.
It resolves device support at initialization, allocates only requested supported
ray effects, selects frame slots after their fences, and prepares diffuse/water
transport descriptors. Reflections retire before their shared scene. The main
renderer schedules this concrete module; no virtual dispatch or provider-ID loop
is introduced. The Source 2 profile retains no live ray scene and skips provider
preparation. Resource access and disabled-path checks are inline.

`shaders/receiver-lighting.glsl` is the shared diffuse contract. It includes
`diffuse-baked.glsl` or `diffuse-ray.glsl` at compilation, retaining the existing
`LIVE_GI` pipeline selection. Both return irradiance divided by pi before material
response. The aggregate flag describes baked direct ownership; a new indirect
provider must not set it merely because it supplies diffuse GI. Baked chart
visibility and incident irradiance used for reflection normalization still need
separation from the standard material orchestration. Automatic cubemap capture
also still depends on the bake controller; the full provider split is unfinished.


### Reflection capture input boundary

Authored capture consumes `probes::CaptureSource` (matching ready scene hash and
optional automatic image) and `probes::CaptureConfig` (reflection transport and
execution settings). It no longer includes or reads `bake::Volume` or compares
diffuse layout settings. `Options::captureConfiguration()` maps the existing
saved keys; no second persisted configuration is introduced. The configuration
header stays CPU-only and does not include Vulkan resource types.

The renderer supplies baked publication readiness for the Source 2 profile,
preserving capture-after-bake ordering. With diffuse baking disabled it can supply
ready retained-scene identity directly, allowing authored capture without any
diffuse volume. Automatic fallback is optional: its absence does not block an
otherwise ready authored set. The bake-controller adapter returns the independent
published image owner without copying images or allocating a new GPU resource.
Submitted capture/composition frames retain that owner through their fences.

Automatic placement, capture transport and cache publication are still inside the
bake controller. Moving that lifecycle to a standalone reflection provider remains
required; the input boundary alone does not complete interchangeable providers.


### Automatic capture work ownership

`probes::AutomaticCapture` now owns automatic site validation state, fitted
regions/projection audits, selected raw captures, shared capture-ray mapping,
input provenance, filtered cubemaps and the prepared GPU image. Its placement,
selection, ray preparation/completion and filtering operations have no dependency
on the bake controller, diffuse atlases or diffuse probe values. Placement uses compact owned
usable positions with original task indices as spatial seed inputs. State is stored inline in the
owning job: this extraction adds no resource allocation or duplicate ray arrays.

The current Source 2 scheduler still lends its transport, retires readbacks after
submission fences, performs optional receiver diagnostics, and publishes/caches
reflection output with the diffuse assets. This preserves existing cold/warm
behavior while extracting reusable work. A standalone automatic reflection
scheduler and independent publication timing/physical persistence remain unfinished. Keep the
source ownership and cancellation token alive through every worker and submitted
frame when moving that scheduling next.


### Automatic query scheduling and image lifetime

`AutomaticCapture::nextStep()` selects placement, validation, fitting, geometry,
selection, capture or completion after the owning preparation ticket/fence is
observed. `record()` borrows `bake::Transport` and `bake::Budget`, emits the same
bounded query batches, advances submitted counters, and returns plain batch
metadata. Pending readbacks keep their query phase with an empty dispatch;
submission does not imply completion. The adapter scheduler retains job owners
and advances completed counters after readback. No virtual callback, second ray
scene, per-pixel branch or additional GPU pass is introduced.

Prepared and published automatic reflection images now have independent shared
ownership. `make_shared<ImageAsset>` constructs the object and control block in
one allocation; `CaptureSource` retains that image directly rather than aliasing
the diffuse volume. Frame-fence retention still protects uploads and sampling.
The diffuse `Volume` no longer stores the reflection image or a duplicate header.
The controller holds a separate image/scene identity, and frame slots retain
reflection upload and sampling owners independently from diffuse owners. Header
and descriptor queries read that publication directly; memory accounting deduplicates
images across active/retired frame references. Neither owner needs to retain the
other after its fences. This adds frame-slot shared ownership, not per-draw or
per-pixel dispatch; matched frame-cost qualification remains required.
The Source 2 scheduler still activates both outputs together;
independent cache/publication and complete technique dependency resolution are
outstanding.


### Device-resolved resource plan

`provider_plan.h` is a CPU-only boundary between requested techniques and GPU
resource creation. `Capabilities` describes ray-query, fragment-GI and water
limits; `resolveResources()` derives the diffuse provider, shared live scene,
ray reflection/water effects and reflection attachment requirement. It does not
mutate quality settings or introduce another saved preset/configuration.

The renderer resolves this plan at initialization and compatible settings edits.
Ray provider initialization, pipeline/layout selection, render targets, frame
lighting ownership, and viewmodel reflection guides consume that same plan.
Diffuse ownership is represented by one enum; the ray predicate is an inline
query rather than a second mutable selector. No provider dispatch is added to
material shaders. Unsupported live GI retains the existing baked/disabled
fallback, while live GI cannot also claim aggregate baked receiver lighting.

Reflection attachments follow supported sources: SSR, enabled cubemaps or supported
ray reflections. Unsupported ray-only requests allocate no guides or resolve.
`Options::sameTargets` distinguishes zero/nonzero cubemap fallback, so runtime
replacement and in-place update validation agree on the attachment boundary.
Changing a nonzero capture count or toggling cubemaps while SSR is enabled still
preserves target compatibility. For ray-only requests on capable devices, toggling
cubemap fallback conservatively prepares replacement targets as well; this is an
options-edit cost, not work added to rendered frames.
Automatic capture cache/publication is still coupled to the bake, and direct
versus indirect diffuse storage/pipeline composition is not yet a fully generic
provider API. These remain required work before claiming the split complete.


### Reflection persistence codec

`probes/cache_codec.h` owns the raw-capture and filtered-image payload codec.
It consumes bounded cache streams and reflection/device limits, with no diffuse
layout, grid, light ownership policy or GPU resource state. Validation retains
capture/projection correspondence, finite HDR values, dimensions, device limits
and the existing padded 32-record disk blocks. Preflight validation still runs
before creating the bake cache's temporary file.

`forward/cache_io.h` provides the existing bounded/checksummed stream machinery;
`bake/cache_io.h` forwards its old internal names for the other cache consumers.
The combined bake cache delegates its reflection section to this codec without
changing its key, schema, payload order or serialized bytes. Optional indexing
adds a section checksum during background persistence.

Reflection transport has an independent input key and optional 68-byte
`.reflection-index` locator. The key includes frozen source content, exact ordered
procedural ray inputs, projection/feather metadata, capture quality, transport shaders,
component/direct ownership and device identity. It excludes diffuse atlas/grid
settings and execution controls. Captured depth confidence is output, not key
material; `projectionInputRecord` avoids scanning radiance for key construction.
The versioned procedural signature includes origin bit patterns, resolution and
unique-owner order without emitting a Point array. CPU ray generation/Point layout
changes must bump its format revision. This is distinct from published ray-byte
provenance, which retains the existing hash and is computed one cube at a time on
a hit. Misses allocate/generate their ordinary complete work list once. The v2
reflection key leaves older independent locators unused; full legacy bake keys
and warm loads stay unchanged.

After fitting, the automatic scheduler can read just the located reflection
section and reuse raw radiance plus filtered images. The bounded reader validates
container identity, file/range bounds and a section checksum before allocating
from counts. Unrelated diffuse corruption does not invalidate that section.
Missing, cancelled, stale or corrupt indices fall back to ordinary capture.
The index is atomically published only after the containing bake is replaced;
its payload contains no paths or duplicated cubemap texels. The existing combined
cache warm-load path remains unchanged.

Legacy capture records omit explicit blend distance. Reuse validates the newly
fitted projection against the filtered header and keeps new fitting metadata
while moving cached radiance. It must not infer feather widths from incomplete
legacy raw metadata. Exact input/radiance/image provenance remains unchanged.

This is independent lookup, not fully independent scheduling or physical storage.
Placement still runs before lookup and is seeded from diffuse planning data.
Combined containers still store their original full payloads; the locator avoids
a second sidecar image but does not deduplicate old/new bake containers.
Publication timing and persistence still share the bake scheduler. Worker I/O,
checksum cost and transient cache-hit memory need representative qualification;
no frame/draw/shader work is added by the locator path.


### Reflection spatial inputs

`probes::PlacementSeeds` contains the placement domain and usable positions with
original indices, kept in task order. The fitting/selection implementation
accepts these fields directly. `placement_grid.cpp` adapts the current diffuse
grid and preserves original indices and ordering for distance ties; legacy CPU
callers retain their existing helper signatures. Candidate-site-only requests
build only a domain, without copying point arrays.

Automatic capture prepares one compact spatial snapshot on its worker, reuses it
through validation/fitting, and releases it before capture-ray allocation. It no
longer needs a diffuse grid in the placement worker API. This incurs transient
cold-preparation storage (up to 16 bytes per input task) and a copy;
it adds no frame-rendering allocation or shader work. Its cold-bake cost must be
qualified rather than described as allocation-free. Persisting/recovering these
spatial inputs and independently scheduled capture publication/persistence remain pending.


Placement snapshots filter unusable geometry while gathering positions, preserve
original IDs for distance tie-breaking, and avoid an array indexed by every grid
slot. Core fitting streams this compact input instead of repeatedly touching SH
records. Synthetic placement comparisons cover dense/sparse inputs and open/BSP
rooms, with identical origin/bounds/count identities. This is CPU preparation
work, not a GPU frame-time or complete bake-time result; small-budget copy costs
and representative map qualification remain part of performance acceptance.


### Source 2 / s&box shader-facing API direction

The [shading API design](shading-api.md) records the audited reference and current
contracts. Provider resource ownership alone does not finish the requested API.
Materials evaluate Surface values, shading models apply BRDF/composition, and
lighting-source queries supply lighting/visibility independently of material response.
Local/sun samples, receiver-local enumeration/context, diffuse queries and shared
environment reflection radiance are in place. Full composition, reflection pass
dependencies, water integration and host shader programs remain work.
