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

For the baked profile, placement, capture transport and combined cache publication
still live in the bake controller. The standalone provider handles the diffuse-disabled
case separately. Default-profile lifecycle migration remains required; the input
boundary alone does not complete interchangeable providers.


### Automatic capture work ownership

`probes::AutomaticCapture` now owns automatic site validation state, fitted
regions/projection audits, selected raw captures, shared capture-ray mapping,
input provenance, filtered cubemaps and the prepared GPU image. Its placement,
selection, ray preparation/completion and filtering operations have no dependency
on the bake controller, diffuse atlases or diffuse probe values. Placement uses compact owned
usable positions with original task indices as spatial seed inputs. State is stored inline in the
owning job: this extraction adds no resource allocation or duplicate ray arrays.

The Source 2 scheduler still lends its transport, retires readbacks after
submission fences, performs optional receiver diagnostics and keeps a combined
cache container. Its reflection image can now publish before diffuse finishing
completes. The standalone scheduler/persistence covers the diffuse-disabled case;
default lifecycle migration and cross-layout physical deduplication remain open.
Keep source ownership and cancellation alive through every worker/submitted frame.


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
The Source 2 image handoff now permits reflection publication before diffuse
activation, using one upload. Default planning/cache scheduling and complete
technique dependency resolution remain coupled.


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
Default automatic capture planning and cache-container scheduling remain coupled
to the bake, although image publication is independent. Direct versus indirect
diffuse storage/pipeline composition is not yet a fully generic provider API.
These remain required work before claiming the split complete.


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
qualified rather than described as allocation-free. Persisting/recovering the
baked profile's spatial inputs and migrating its capture scheduling remain pending;
standalone geometry planning and persistence have their own path.


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

### Automatic reflection completion ownership

`AutomaticCapture::record` borrows the shared transport and budget. Its matching
`observe` consumes fenced site, projection and radiance batches, updates provider
completion counters and writes provider-owned capture storage. The scheduler
submits each range exactly once and may deliver different slots out of order.
Site/radiance batches validate finite irradiance before publishing any result;
projection assessment retains its own range/duplicate validation. No callback,
virtual dispatch or extra ray/result array is introduced by this boundary.

For the baked profile, diffuse planning still provides placement seeds and the
bake controller orders preparation/filtering, while image publication has its own
readiness handoff. Completing default lifecycle migration must share prepared
transport resources rather than constructing another scene for each technique.

### Shared transport scene and independent consumers

`bake::Transport` now separates a retained scene owner from consumer-local
execution state. The scene owns frozen geometry/material/light buffers, light
proposal trees, acceleration structures, texture atlas/views and upload scratch.
Each consumer owns its descriptor pool, dispatch/result buffers, constants,
optional live-light overrides, indexed receiver storage and timing queries.

After the preparation fence, `releaseBuildInputs` retires scene build scratch
and marks it available for sharing. A preparation worker can then call
`initializeShared(prepared)` to retain that scene and allocate independent
execution state. All options and direct-light ownership policy are inherited;
this API does not silently reinterpret the scene for another lighting policy.
It does not repack materials, recapture textures or rebuild acceleration. The
original transport may retire after consumer initialization, provided its own
submissions have completed. Consumers may submit different work using the same
slot index without overwriting one another's constants or readback storage.

The normal Source 2 path still uses its existing single transport. Separate
technique controllers have not yet adopted independent consumers or independent
publication. `memoryBytes` retains its legacy single-consumer total; renderer
statistics use the split shared-scene/private-consumer accounting described below.
Shared ownership is acquired on preparation, not copied for every dispatch.
No shader, descriptor binding layout or GPU dispatch algorithm changes here;
CPU and complete frame-performance acceptance remain separate requirements.

### Diffuse-independent reflection seed planning

`AutomaticCapture::planSites(source, count, cancellation)` can plan reflection
placement without a diffuse grid. It gathers bounds from explicit map bounds or
frozen geometry, distributes a bounded candidate grid by scene aspect ratio,
and includes the existing BSP room/hint candidates. These are candidates only:
all standalone fallback points pass through the provider's hardware site
admission before fitting. Rejected points cannot become fallback capture origins.
BSP fits still pass through projection geometry assessment before selection.
Planning retains positions and stable IDs, not diffuse SH or lightmap storage.

The existing Source 2 bake uses the other overload, moving its already-admitted
`PlacementSeeds` into the provider. That path adds no duplicate admission queries
and retains its existing placement identities. Planning/fitting live in
`automatic_placement.cpp`; image creation/filtering remain in the image-facing
capture implementation.

Standalone candidates are capped at 4096, with budget derived from capture count.
This is an independent provider entry point, not a newly enabled runtime mode.
Representative map coverage, top-level independent scheduling/publication and
cache persistence still need qualification and integration. A synthetic open
scene or room is not evidence of adequate coverage on every map.

### Standalone automatic reflection controller

When diffuse baking is disabled and the cubemap budget is nonzero, the renderer
runs `probes::StandaloneCapture`. It uses only the retained static source,
`CaptureConfig`, capture count and projection tolerance. It owns independent
worker tickets, geometry admission/fitting, radiance capture, filtering and image
publication; it never asks for a diffuse volume or lightmap readiness.
Per-slot receipts retain query owners and upload/sampling images until their
fences complete. Source replacement cancels pending work and clears the old map's
publication. Execution-only budget edits preserve lighting. Invalid or wholly
unusable placement stops with a provider status instead of retrying each frame.

Compatible target replacements retain this controller. Preparation reads only
constructor identity and an atomic used marker; queue-idle activation rechecks
the marker because the old renderer may first use the provider after worker
preparation. Only the active render thread advances its mutable state.
Published images retain no transport scene once all query receipts retire.

The normal Source 2 bake path is unchanged and does not schedule this controller's
capture work. This alternate path currently builds one transport for its frozen
source and releases it after publication; it does not yet borrow a scene from a
concurrent compatible provider. Disk persistence is independent, as described
below. Receiver-guided repair and dedicated UI progress/status wiring remain
unfinished. Shared prepared scene adoption in the default scheduler, real-map
coverage and full CPU/frame performance qualification remain necessary before
the backend split is complete.

### Independent reflection persistence

Standalone captures persist a reflection-only snapshot: raw capture radiance,
projection metadata and filtered HDR texels. The key includes the frozen source
content, capture count/quality/tolerance, an explicit CPU planner/filter/defaults
version, atlas/radiance shader fingerprint and device/driver/limit provenance.
Source identity includes BSP data and capture hints. Diffuse atlas/probe settings,
batch size, timing budget and disk-cache policy do not enter this identity.
Increment the standalone key's CPU version when placement, filtering or fixed
transport defaults change. A warm hit bypasses admission, fitting, acceleration
construction, radiance queries and filtering; image preparation/upload still run.

Files live under the existing Forward+ cache directory in
`reflections/<key>.reflection`, using bounded/checksummed streams and atomic
replacement. Missing, stale, invalid, truncated or corrupt files are normal
misses. The existing disk-cache setting controls this provider too. Enabling
persistence does not force an otherwise unnecessary lighting restart.

`cache_io::Persistence<Snapshot>` is the common CPU-only background writer. The
bake and reflection adapters supply their own immutable payload, memory charge
and serializer. The queue preserves one active/one pending write and a combined
retained-byte ceiling. Standalone snapshots and reads are capped at 256 MiB;
larger captures still publish but skip optional persistence. Writes are submitted
after publication without waiting for disk. No renderer, GPU image or transport
is held by the standalone disk snapshot. Orderly shutdown drains/joins writers.
The bake adapter keeps its existing budget and serialization format.

This is independent persistence for the standalone planner. It does not deduplicate
individual cubemaps across the older combined cache and standalone layouts, or
complete migration of Source 2's default capture scheduling to independent owners.

### Default reflection readiness and diffuse finishing

`Job::finishDiffuseAssets` owns lightmap denoising, atlas encoding, styled diffuse
preparation and surface binding creation. The reflection finishing worker calls
it after capture completion/filtering, preserving existing work and order.
`reflectionReady` is a release/acquire handoff for the completed immutable image;
the worker never touches that image after signaling it. The render thread may
upload/publish it while diffuse finishing continues, and retains upload/sampling
owners through frame fences. Later diffuse activation reuses the image and skips
its upload. The same handoff applies to successfully prepared cache hits.

This adds no worker task, capture query, filtering pass, ray array or image upload.
The readiness poll runs only while a bake job is pending; the steady active-volume
path does not poll it. Reflection shading can start earlier during loading, so
this is not a claim of identical transient GPU frame cost. Authored captures keep
the baked profile's transport priority until diffuse publication, preventing two
independent transport budgets from stacking during styled work.

Memory reporting observes private preparation outputs only after ticket completion;
independently published images are counted through the fenced image owner and
deduplicated when a completed job still references them. The deterministic
`reflection_publication.cpp` test links a paused test denoiser to prove publication
before diffuse completion, image identity through later activation and retained
image validity on cancellation. No production timing hook or artificial delay
is used. Default planning/receiver audits/combined persistence are still not a
fully independent reflection controller.

### Completed reflection payload ownership

`probes::CapturePayload` is the common CPU representation of raw captures and
filtered HDR data. Standalone cache and indexed-section adapters use the same
payload type. It owns no diffuse grid/atlas, scene, scheduler, transport or GPU
image. `AutomaticCapture::retainPayload` moves completed vector storage into an
immutable shared owner once; `captureView` and `filteredView` select preparing
or retained storage. Mutation/query APIs reject a retained payload. The baked
profile retains it after filtering (and after cached projection restoration),
before signaling image readiness.

Runtime bake-cache snapshots retain that component through `reflectionOwner`;
legacy cache decoding and CPU construction still use inline capture/specular
fields. Snapshot read views choose the retained component when present. The disk
adapter uses those views and includes shared storage/control allowance in its
memory charge. No large capture/texel array is copied, no new cache file is
written, and the existing combined container bytes/key remain unchanged.

Retention adds a preparation-time payload/control allocation and transfers fixed
metadata; it is not allocation-free. Image sampling and steady rendering do not
retain or query this CPU payload. Default persistence still starts with the
combined snapshot after diffuse activation. This separates storage ownership;
it does not yet independently schedule the baked profile's reflection persistence
or physically deduplicate its combined containers.

### Reflection capture storage

`probes/capture_cache` owns CPU-only reflection payload persistence. Standalone
capture keys describe planner, shader, quality and device provenance; they do not
include diffuse layout or execution budgets. `CapturePayload` remains the common
raw capture and filtered HDR representation.

The same module provides content-addressed shared blobs. Their keys include a
storage domain/version, serialized byte count and checksum. Serialization and
fingerprinting use one codec, so changes in raw radiance, filtered texels or
projection metadata change the stored identity. Existing objects are checksum
verified before reuse without allocating decoded capture arrays. Missing or
corrupt objects are ordinary misses; writes are atomic and cancellation-aware.
These are local cache integrity checks, not cryptographic authentication.

Default runtime snapshots with a retained reflection owner now store one shared
blob, then atomically publish the diffuse container referencing its numeric key.
Bit 256 in the container flags selects that reference; old inline containers
remain readable. Direct CPU writers default to the legacy inline format. Failure
to save a blob falls back to inline storage, within the existing writer thread.
The independent reflection index accepts legacy section locations (version 1)
or shared blob keys (version 2). Full diffuse reads use their own stored blob key,
so removing or replacing the index cannot redirect them.

Strict readers treat a missing/corrupt blob as a cache miss; the default controller
can recover the reflection component as described below. Old binaries cannot read
new reference containers. Existing inline caches are not automatically rewritten,
and garbage collection must treat referenced blobs as live. Default planning/scheduler separation, automatic garbage collection,
real-map performance acceptance and playable deployment remain unfinished.

### Automatic reflection image handoff

`AutomaticCapture` owns worker completion and one-time image upload state for
both default and standalone scheduling. The worker seals its prepared image with
a release store; the render thread acquires readiness and records an upload only
once. The returned shared image must stay in the submitted frame until its fence
completes. The provider rejects sealing a missing/already sealed image and rejects
further capture mutation after sealing. The image and retained CPU payload have
separate lifetimes.

The default bake job no longer owns reflection-ready/published flags. It can bind
an early reflection image while diffuse finishing continues, then reuse that image
at volume activation. This does not yet separate default placement seed creation,
receiver diagnostics, worker tickets, or transport scheduling from the diffuse job.
Those still preserve the admitted-seed reuse and existing single GPU budget.

### Automatic reflection CPU preparation

`AutomaticCapture::prepareNext` advances the provider's CPU placement stages:
site planning, fitting admitted sites, then capture selection. GPU site/projection
checks must retire through the existing provider readback API before another CPU
transition. A caller cannot submit admitted seeds after planning has begun. The
requested placement count and device capture capacity remain distinct, preserving
candidate fitting when the device can publish fewer images.

Default and standalone controllers use this entry point inside their existing
preparation tickets. The default host supplies moved admitted diffuse seeds only
at the first transition; the standalone host omits them and uses geometry seed
admission. Selection preserves the existing candidate reporter. No diffuse grid,
atlas, worker queue, or transport is owned by this CPU preparation API. The default
host still constructs borrowed seeds, schedules diagnostics/cache lookups and owns
the combined preparation ticket/transport budget; those remain migration work.

### Planned reflection cache policy

`capture_cache::plannedKey` owns identity for selected captures. Transport shader
fingerprint, device limits, lighting-component mode and direct-ownership bytes
are explicit inputs; no diffuse grid or lightmap setting enters the key. The
historical bake-cache function delegates to this identity implementation, retaining
its shader-byte fingerprint and existing key format for compatibility.

`AutomaticCapture::prepareRadiance` looks up selected captures, validates restored
projection/radiance using the existing restore contract, and generates rays only
on a miss. Its `Planned` request is borrowed synchronously on the worker; ownership
bytes are not retained. An empty directory skips lookup and identity hashing.
Default host provenance construction also skips shader hashing/style scans when
no cache is available. Standalone cold captures use the same preparation method
without a planned-cache request; standalone warm lookup remains before placement
and transport creation.

The default host still supplies transport provenance, admitted placement seeds,
diagnostic scheduling, preparation tickets and the shared transport budget. This
moves cache/reuse policy, not the entire scheduler. Full frame-time acceptance
and independent default technique scheduling remain open.

### Reflection submission identity

Frame submissions carry `CaptureStep`, a small resource-independent stage type.
The default scheduler shares its existing first/count range with that stage,
replacing three reflection booleans without a second range or submission array.
Both schedulers retire a typed `AutomaticCapture::Dispatch`; the provider rejects
zero/truncated result ranges before updating completion. Its `workType` maps only
GPU query stages to adaptive budget categories and rejects CPU preparation stages.
The existing job/frame owners and shared budget still govern fence lifetime and
throughput. This does not transfer default worker tickets or transport ownership.

### Missing reflection component recovery

The default controller reads combined caches with `ReflectionRead::RecoverMissing`.
A verified diffuse container whose external reflection blob is missing, corrupt,
or incompatible retains its atlas, irradiance, mixed masks and styled data.
Strict readers still require every component. Recovery is reported explicitly in
`Snapshot::missingReflections`; malformed diffuse data remains a complete miss.

Only recovery loads reconstruct the admitted probe trace positions from dense or
indexed origins and stored relocation offsets. The controller prepares one
transport and schedules reflection placement/capture/filtering directly, bypassing
diffuse queries, denoising and atlas encoding. The existing persistence thread
then publishes a repaired combined reference, allowing the following complete
warm load to skip capture. Complete warm loads allocate no reconstructed Point
array. Cached diffuse lighting can publish before recovered reflections; fully
independent default scheduling and shared-scene adoption remain future work.

### Diffuse volume publication boundary

`bake/diffuse_publication.cpp` builds and uploads one diffuse `Volume`: irradiance,
styles, mixed visibility, surface atlas, bindings and optional sun depth. It reads
no reflection capture/image state and does not select or publish reflection assets.
The controller composes its result with completed reflection metadata and images,
then performs the existing status/persistence/job-retirement transition. The split
adds no volume/image allocation or GPU pass; the existing construction runs once.

During reflection recovery the prepared diffuse volume publishes once before
capture work starts. Final activation reuses its buffers/images; it does not recreate or upload
diffuse resources or change diffuse content metadata. Reflection diagnostics
and optional attachments publish separately. Published diffuse GPU data is unchanged. Workers must
not read the render-thread `publishedDiffuse` owner; preparation inputs remain
in their private job.

Local-reflection image uploads have explicit owners in their originating frame
submission. Retirement never discovers their staging through a volume that might
have gained attachments later. Memory reporting deduplicates the frame-held image
with published reflection/job owners. Sun-depth dependencies still complete before early diffuse
publication. Default independent scheduling and matched CPU/frame performance
remain open; optional late-local-layer combinations need broader map coverage.

### Optional local reflection source

`probes::LocalCaptureSource` owns prepared local image layers, their coverage plan,
ready-layer mapping and immutable receiver evidence. It is a reflection-only
publication, independent of `bake::Volume`. Default activation publishes the
completed source after its preparation ticket and binds its upload to the
originating frame. Source invalidation retires it independently of diffuse data.

The optional local-selection path obtains this source directly from the controller;
it no longer reads local layers or evidence through a diffuse volume. Its existing
disabled early return is preserved, so the default material path adds no local-source
lookup or shared-pointer copy. Selection workers retain their source image/evidence
inputs; submitted uploads retain their image until the matching fence. Memory
reporting deduplicates published/job/frame owners. Controller scheduling still
coordinates their preparation; the new source is not a separately scheduled
producer by itself.

### Independent content diagnostics

Diffuse `Volume::content` contains only `DiffuseContent` fingerprints. Reflection
`CaptureContent` belongs to the capture provider and publishes through the image
release/acquire handoff. The controller combines those fixed-size values for the
existing statistics/JSON schema, including reflection readiness before diffuse
publication. It never writes reflection fields into a published diffuse volume.

The default preparation worker computes capture RGB/input fingerprints and the
filtered-image fingerprint once. Streaming `HashStream` covers the exact header
and texel bytes used by the legacy serializer, preserving hash values without a
joined image allocation. Cancellation is checked between bounded chunks. Normal
activation performs no capture scan or serialized-image hash. Explicit capture
dumps still serialize the image when their debug prefix is requested. Standalone
image preparation does not request these diagnostics, preserving its previous work.

This removes one render-thread preparation cost; it does not establish the full
no-overhead frame gate or complete independent default scheduling.

### Shared transport session ownership

`bake::TransportSession` contains the frozen source owner, mutable transport
consumer, adaptive budget and cancellation source. Default bake and standalone
reflection jobs embed it as a nonvirtual base. Their existing frame/worker owners
still retain the concrete job; there is no separately allocated session or shared
session pointer. Provider-specific state and preparation tickets stay in the
concrete jobs.

The Source 2 recipe intentionally sequences its techniques through one consumer
and budget, retaining admitted-seed and GPU-work reuse. Separately scheduled
providers can use the existing fenced `Transport::initializeShared` contract for
immutable scene sharing, but current default tickets have not yet migrated to
independent provider scheduling. Runtime adoption by additional coexisting
consumers and complete CPU/frame performance acceptance remain required.

### Shared transport memory accounting

`Transport::memory` separates shared scene buffers/images/acceleration from private
consumer dispatch allocations and supplies borrowed identities for each domain.
The legacy `memoryBytes` total remains their sum, preserving its existing category
coverage. This is reported transport memory, not a new full Vulkan allocator census.

One `TransportMemory` collector spans default/authored/standalone controller snapshots in
the renderer. It counts a scene once and every distinct consumer once, even if a
consumer appears through several retained frame/job owners. Its normal eight
entries are inline; cancellation bursts can spill without truncating accounting.
It retains identities only for the synchronous snapshot, never GPU objects.
Future providers using shared transport must pass this collector too.

This enables correct accounting for shared-scene consumers; it does not itself
make current default/standalone jobs share a scene, transfer their tickets, or
prove no-overhead gameplay performance. Those remain separate qualification work.

### Secondary capture work admission

Authored capture accepts `captureReady` separately from its automatic image/source
identity. The Source 2 scheduler grants this work only when the baked-lighting
controller reaches `REMIXAPI_BAKE_READY_EXT`, including during reflection-only
recovery. An early diffuse volume or automatic image does not grant a second
capture budget. The authored producer can retain its published image while paused;
when admitted, its existing job/worker/fence scheduling resumes normally.

Authored memory reporting participates in the renderer's shared transport collector.
Buffer totals exclude the scene's image and acceleration categories rather than
counting those categories both in buffers and again separately. Sharing an authored
consumer with a default prepared scene still requires compatible source/options and
policy provenance; this admission/accounting change does not enable that reuse.
