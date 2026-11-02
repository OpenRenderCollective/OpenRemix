# Forward+ shading API

The target is the Source 2 / s&box style of shader authoring: evaluate a material,
choose a shading model, and fetch lighting through shared source APIs. The Source 2
preset selects the existing baked/probe/cubemap techniques. Custom shading models
and future GI techniques should reuse the same source interfaces.

## Material authoring entry point

The evaluated value contract lives in `forward/shaders/surface-values.glsl`, separate
from the packed-material/texture compatibility decoder in `surface.glsl`. It can
be included in vertex, fragment or compute code without material bindings or
lighting resources. `initSurface()` initializes white color/alpha, black emission,
roughness 1, metalness 0 and a +Z normal; `initSurface(normal)` uses the caller's
normalized world shading normal. It performs no texture lookup or derivative work.
Receiver geometry and lighting identity remain pass-owned inputs.

A material author can use the existing standard shading model like this, once the
pass has supplied the common shading declarations:

```glsl
ShadingResult shadePaint(ShadingReceiver receiver, vec3 baseColor) {
  Surface surface = initSurface(normalize(receiver.geometric));
  surface.color.rgb = baseColor;
  surface.roughness = .65;
  return shadeStandardSurface(surface, receiver);
}
```

This follows the reference's material initialization and shading model call. The
material function does not choose lightmap/probe/reflection storage. The standard
model supplies its reflection guides; pass adapters still own alpha/depth/MSAA
coverage and output writes. Custom models can use the lighting queries below.
This is a shader-source contract, not a runtime custom-program loader or a
`.shader` parser. Do not add unused material properties until their semantics are
implemented through the selected shading model.

The tracked `tests/forward/shaders/custom-model.glsl` fixture demonstrates a
minimal alternate diffuse response using the shared diffuse/local-light queries.
`custom_shading.py` substitutes it into the real pass with the standard shading
model and its direct-light composition removed, then compiles and validates world,
reflection-guide, viewmodel and live-diffuse variants. The pass still decodes its
usual material and owns coverage/output writes. This verifies the composition
boundary, not visual parity, sun/reflection completeness or runtime shader loading.

## Reference snapshot

Audited `sbox-public` revision `e545b33037b49c59bff80b64359747436279d7f6`:

- [material.hlsl](https://github.com/Facepunch/sbox-public/blob/e545b33037b49c59bff80b64359747436279d7f6/game/core/shaders/common/material.hlsl): evaluated material properties and initialization.
- [shadingmodel.hlsl](https://github.com/Facepunch/sbox-public/blob/e545b33037b49c59bff80b64359747436279d7f6/game/core/shaders/common/shadingmodel.hlsl): `ShadingModelStandard::Shade`, material-to-combiner conversion, direct/indirect terms and final composition.
- [Light.hlsl](https://github.com/Facepunch/sbox-public/blob/e545b33037b49c59bff80b64359747436279d7f6/game/core/shaders/common/classes/Light.hlsl): `Light::Count/From`, source color/direction/attenuation/visibility, and probe/lightmap indexed sources.
- [AmbientLight.hlsl](https://github.com/Facepunch/sbox-public/blob/e545b33037b49c59bff80b64359747436279d7f6/game/core/shaders/common/classes/AmbientLight.hlsl): one ambient query selecting DDGI, probe volume or environment lighting.
- [EnvMap.hlsl](https://github.com/Facepunch/sbox-public/blob/e545b33037b49c59bff80b64359747436279d7f6/game/core/shaders/common/classes/EnvMap.hlsl) and [DynamicReflections.hlsl](https://github.com/Facepunch/sbox-public/blob/e545b33037b49c59bff80b64359747436279d7f6/game/core/shaders/common/classes/DynamicReflections.hlsl): reflection sampling wrappers.
- [line.shader](https://github.com/Facepunch/sbox-public/blob/e545b33037b49c59bff80b64359747436279d7f6/game/core/shaders/line.shader): custom composition reusing direct/indirect lighting terms.

The standard model uses `ComputeDirectLighting` / `CalculateIndirectLighting`;
the convenience classes expose lower-level access for custom shading. These are
distinct entry points in that snapshot. Its ambient helper contains runtime
provider selection; the API pattern does not imply every choice is compile-time.
Its four-slot static-light convenience path must not impose a four-light limit
on OpenRemix's scalable indexed fields.

## Layers

| Layer | Responsibility | Current implementation |
| --- | --- | --- |
| Material | Evaluate color, roughness, metalness, emission and normals | `Surface`, `evaluateStandardSurface` |
| Receiver | Supply world/screen geometry and baked bindings | `ShadingReceiver`, separate from material values |
| Lighting sources | Fetch source lighting and visibility independently of BRDF | local/sun samples, receiver-local enumeration and diffuse query and environment reflection radiance; full reflection-provider integration pending |
| Shading model | Apply material response and compose contributions | `shadeStandardSurface`, `shadeStandardLocalLight` |
| Renderer | Select providers, allocate/update resources and choose pipelines | typed selections and CPU `ResourcePlan`; lifecycle migration ongoing |

Keep receiver geometry separate even though the reference Material also carries
it. Shared exposure/output transforms remain outside material lighting. Source
shader names, VMT semantics and lightstyle translation belong in the host adapter.

## Local-light prototype

`shaders/light-sample.glsl` defines `LocalLightSample` and `sampleLocalLight`.
The query accepts geometry and baked receiver context, with no base color,
roughness, metalness, view vector or dielectric F0. Its output supplies:

- world-space light direction and linear HDR source radiance;
- unit diffuse irradiance and analytic geometric/visibility weight;
- moving visibility, influence fade and selected-highlight weight;
- aggregate ownership, identifying energy already present in the bake.

Read the output only when the boolean query succeeds. `frontFacingOnly` is a
query policy: the standard model keeps its existing rejection, while a custom
wrap/transmission model can retain back-light samples. Aggregate diffuse data
requires the existing mixed-shadow residual; adding it again would double-count
baked direct lighting. This ownership still needs a friendlier high-level facade.

`direct-light.glsl` applies the standard PBR response to the sampled data. Its
standard entry and raw query share `local-light-query.glsl`, a function-body
template with one caller-specific miss exit. This preserves the standard early
returns without a runtime shader callback or duplicated transport implementation.
The body template is backend implementation detail; material authors call functions.

## Receiver enumeration and sunlight

`localLightsForReceiver(receiver, aggregate)` returns a `LocalLightList` with a
count. `localLightIndex(list, ordinal)` resolves the source index for
`sampleLocalLight`. Cluster headers, baked-tail handling and overflow fallback
are implementation details. The aggregate argument describes receiver ownership,
not a global limit on static lights. Query once per receiver, then iterate its
count; no temporary light array is allocated.

`sampleSunLight` supplies direction, linear HDR radiance, unit diffuse irradiance
and static visibility without material parameters. Its radiance includes the
moving-caster shadow factor. `shadeStandardSunLight` applies the standard BRDF.
The existing `sunLightResponse` entry is a compatibility wrapper over both, so
world and viewmodel callers share transport without changing their passes.
`sampleUnbakedSunLight` exposes analytic/CSM visibility for passes without baked
sun coverage. It is a source policy, not a BRDF or game-specific shader choice.

## Local receiver context

`prepareLocalLighting(receiver, normal, diffuseRequired, aggregate)` prepares the
local-light list and baked direct channels once. `diffuseRequired` describes the
shading model's work, not a material type or engine shader name. The aggregate
ownership comes from the selected diffuse query.

Iterate `context.lights.count`, then call
`sampleReceiverLocalLight(ordinal, receiver, normal, context, frontFacingOnly, sample)`.
This entry resolves the index, geometric receiver normal and channel data;
custom models do not need atlas, cluster or four-channel sentinel knowledge.
It returns the same `LocalLightSample` as the lower-level source-index query.
Its ordinal must be within the prepared list, and output is valid only on success.
The standard model retains its specialized miss exits through `localLightResponse`.

## Diffuse receiver query

`prepareReceiverLighting(receiver, normal)` prepares chart irradiance and sun
irradiance once. `sampleDiffuseLighting(receiver, normal, context, required)`
selects the compiled diffuse provider. Both standard and custom models can use
this entry without accessing the surface atlas or probe storage. The returned
`ReceiverLighting.diffuse` is linear HDR irradiance / pi, before material color
and metalness. The value type and `composeDiffuseLight` live in resource-free
`diffuse-values.glsl`, separate from receiver bindings and provider queries.
Its independent policy fields are:

- `direct.diffuse` and `direct.specular`: permit these analytic lobes in addition
  to the incident field. A unified source owning direct diffuse can disable that
  lobe while retaining analytic highlights. Owning both lobes skips standard
  direct-light evaluation. Reflected environment radiance remains separate.
- `direct.attribution`: apply the receiver's per-light baked attribution policy.
  This does not mean every light is included; membership and moving-caster
  residuals remain source-query responsibilities. Keep the diffuse lobe enabled
  when those residual corrections must still be applied to the field.
- `reflectionReference`: use this field under the selected reflection provider's
  local incident-light normalization convention. It does not imply baked direct
  membership or authorize subtracting a light contribution.

`analyticDirectLighting(attribution)` preserves the existing analytic diffuse
and specular paths. Source 2 enables attribution and a reflection reference for
valid chart/probe fields; live ray diffuse and ambient fallback retain analytic
lobes without attribution/reference. The alternate diffuse compile fixture owns
direct diffuse while retaining analytic specular. World, viewmodel, water and
the example custom model select direct contributions through the same policy.

This separates lobe ownership, per-light residuals and reflection normalization.
It does not yet describe full transport provenance or compatible normalization
for arbitrary unified GI implementations; a new provider must supply those.

Reflection-normalization luminance remains in standard composition. When a
metal-only variant skips diffuse evaluation, the compiled provider's pure
`needsDiffuseReferenceQuery(hasSurfaceField)` predicate selects whether to call
`diffuseReferenceLuma(position, normal)`. A negative result means unavailable.
Source 2 reads its baked incident field; live diffuse does not advertise a
compatible frozen reference. A replacement provider implements these queries
without forcing the model to read a baked probe. Evaluate the pure predicate
before combining it with the reference flag: a function call inside `&&` added
control flow in the metal variants. The current arrangement preserves the old
optimized work. Diagnostic baked-source views still query baked data explicitly.

## Reflection radiance

`sampleEnvironmentReflection(ReflectionReceiver)` returns scene-linear incident
radiance from the authored/automatic probe source or visibility-filtered sky.
World reflection resolve, viewmodels and water share this query. It applies neither
Fresnel nor SSR/ray confidence, so composition owns those factors exactly once.
The pass adapter supplies the candidate mask, explicit layer and existing biased
probe position for tiled queries. The non-tiled water variant uses automatic
selection. Probe images and parallax selection remain implementation detail;
`environmentReflection` is the pass-owned sky fallback implementation.

`sampleViewModelReflection` combines that environment source with current-frame
SSR and returns radiance without material color/metalness/Fresnel. The standard
`viewModelReflection` wrapper applies its existing roughness-aware Fresnel term.
This is a viewmodel adapter, not a universal world-space SSR API: its screen
projection, source guides and descriptor layout belong to that pass.

World SSR/ray transport still publishes a confidence/radiance image consumed by
its resolve. Water retains separate optical composition and sky policy. A complete
provider contract must accommodate these pass dependencies; the environment query
alone does not make every reflection technique interchangeable.

## Optical water adapter

Water builds the same `ShadingReceiver` and uses `sampleDiffuseLighting`,
`prepareLocalLighting` and `sampleEnvironmentReflection`. Its diffuse shader
variant currently selects baked volume probes, with no surface-chart allocation.
A constant false direct-channel request removes unused atlas reads. The optical
model still owns absorption, sheet/body Fresnel, refraction and screen/ray hit
composition. Its light-source queries do not apply those material weights.

Water uses `sampleUnbakedSunLight` to preserve its existing CSM policy and supplies
its existing unfiltered/visibility-gated sky fallback. Switching it to the same
baked/mixed sun ownership as world/props and selecting live diffuse providers for
water still need explicit quality/resource qualification. Shared queries alone
must not be reported as finishing those policies.

## Material response and composition

`BrdfLobes` describes material response; `LightContribution` describes scene-linear
radiance after source weighting. They are distinct types even though both have
diffuse/specular fields. A mixed-shadow diffuse contribution can be negative:
accumulate it into baked lighting before clamping the final color.

- `composeDiffuseLight(incident, diffuseReflectance)` applies material color to
  receiver irradiance / pi. The standard reflectance is base color times
  `(1 - metallic)`; do not divide by pi again.
- `composeLocalLight(source, response)` applies source irradiance, static/moving
  visibility, range fade, selected-highlight weight and aggregate subtraction.
  Diffuse response is a weight per unit irradiance; specular response includes
  the analytic receiver cosine, matching `BrdfLobes` from the standard BRDF.
- Models with different analytic and preintegrated diffuse responses can call
  `composeLocalLight(source, analyticResponse, bakedDiffuseWeight)`. It chooses
  the correct diffuse response without exposing visibility/fade bookkeeping.
  The baked weight must match the model's response to the aggregate field.
- `composeSunLight(source, response)` applies the separate sunlight query's
  irradiance and visibility to custom or standard material lobes.

For a custom Lambert model, use `BrdfLobes(albedo / Pi, vec3(0))` for local lights
and `composeDiffuseLight(incident, albedo)` for the aggregate field. The local
compositor subtracts newly occluded baked energy instead of adding the same
static light twice. The model does not inspect channel masks, visibility flags,
range fades or selected-shadow/highlight controls. Source enumeration and
response remain per-light; the standard loop retains its fused accumulation
order rather than allocating extra diffuse/specular accumulation buffers.

These helpers cover the existing source contracts. They do not yet define a
generic ownership schema for every future unified GI provider, nor replace the
pass-specific reflection guides and final display transform.

Receiver diagnostics and sun visibility also live behind the
receiver-lighting API: `receiverBakedCoverage`, `receiverBakedDiffuse`,
`receiverSunVisibility`; metal reference lookup belongs to the diffuse provider. The standard model does not
read chart channels or query baked probe storage directly; its prepared context
still exposes whether the receiver has a surface chart. The diagnostic helpers
intentionally show the baked reference field even with live diffuse selected;
that is a debug policy, not a generic fallback for future GI providers. The
normalization query preserves incident lighting for metal-only materials whose
diffuse response can skip probe evaluation.

## Resolved provider dependencies

`techniques::ResourcePlan` carries active diffuse, screen/ray reflection and
shared-scene requirements after capability resolution. Screen reflection
allocation and hierarchy recording consume the resolved screen choice; quality
and range values remain in `Options`. The live scene receives its compute
reflection-batch requirement from the same plan instead of reading the saved ray
request during capture. GI-only consumers keep their one-point dispatch capacity.
Viewmodels consume the prepared screen descriptors, which are absent when that
source is inactive. Material shaders do not inspect these plan IDs.

Saved requests and resolved capabilities remain distinct, so unsupported effects
retain their configured fallback without allocating the requested ray provider.
This plan is derived once during target preparation and is not another persisted
selector. Existing source modules consume it without a dynamic registry, virtual
per-draw dispatch or additional shader branch.

## Provider scheduling and transport sharing

Technique independence does not require separate acceleration structures or
independent ray budgets. The Source 2 preset currently lends the automatic
reflection provider its admitted placement seeds and the active bake transport.
The reflection provider owns placement transitions, GPU ranges/readback,
radiance-cache lookup, filtering, immutable payload and image publication. The
preset-specific scheduling/publication bridge lives in
`forward/bake/reflection_schedule.cpp`, separate from the diffuse controller's
main recording loop. `AutomaticCapture` now owns its preparation ticket and
rejects replacing active preparation. Default and standalone jobs retain their
initial transport/diffuse ticket and inspect both owners before touching worker
outputs, cancelling or retiring jobs. Queued cancellation releases captured inputs
without executing the callback; cancellation is never successful readiness.

Reflection finishing has three explicit provider-owned retention paths after
transport completion (or cache restoration): `finishRetainedPublication` computes
the default source's provenance and keeps its immutable payload;
`finishTransferredPublication` moves nonempty capture/filter arrays into the
existing unique persistence payload; `finishImageOnlyPublication` releases those
CPU arrays without allocating a persistence payload. Warm standalone restores
and persistence-disabled captures use the last path. Empty completion uses the
retained path, which needs no GPU device or image. Source 2 cache restoration
still interleaves its auxiliary-field restore before sealing publication, keeping
that existing order explicit in its preset adapter.

The preset still determines admission and the order around diffuse finishing.
The same callbacks, queue order, transport budget and early publication boundary
are retained; this split creates no additional jobs, scene allocations or waits.
Standalone reflection capture supplies its own scheduler when diffuse baking is
disabled. Full provider dependency and finishing orchestration remain separate
integration work.

`ResourcePlan::captureTransport` explicitly selects the automatic capture and
shared-admission owner: the baked scene scheduler or the standalone scene
scheduler. Recording, authored-work admission, automatic image/header fallback
and reflection scale reporting consume this selection rather than independently
inferring it from diffuse-provider flags. Turning capture images off does not
remove their retirement owner; an already-used scheduler still records its
disabled transition so outstanding workers/frames can retire. This resolved
selection is not saved separately and introduces no per-draw dispatch or scan.

When supported live diffuse is selected, the resolved lighting plan disables the
unused diffuse bake controller without changing saved bake preferences. Automatic
cubemaps then use the standalone reflection scheduler; source selection, descriptor
selection and reported scale follow the resolved diffuse provider rather than the
saved bake-enable flag. Unsupported live GI still resolves to the configured baked
fallback, and selecting Source 2 schedules the baked provider again. This avoids
baking surface/probe fields merely to obtain cubemaps under a live diffuse provider.
Viewmodels select the same live diffuse source when its frame descriptor is
available, through dedicated viewmodel shader variants that share the world
transport scene/set. Their overlay target, prepass, self shadows and reflection
composition stay pass-owned. The existing material shader remains the fallback
until the live descriptor is available; the Source 2 path creates no live-viewmodel
pipelines. Live diffuse also omits the baked receiver-mask gather cache, which it
never consumes. Non-live modes retain that capability because bake-enable edits
do not require target recreation.

It does not yet separate baked direct ownership from an independently selected
indirect-only GI field.

Reflection publication is optional when the completed capture payload is empty.
`AutomaticCapture::sealPublication` seals either an image or a retained empty
payload; a nonempty payload still requires an image. Empty completion allocates
no per-job image, starts no filtering pool and records no image upload. The
controller's existing fallback descriptors remain valid, and diffuse activation
no longer depends on creating another empty reflection asset. The scene identity
and complete diffuse data still publish normally, including on a warm cache load.
Readiness/admission must follow job state rather than the presence of an image.

The next lifecycle split must preserve these boundaries:

- A prepared transport scene can have independent dispatch consumers and frame
  slots. Sharing requires the preparation fence and `releaseBuildInputs` boundary;
  a worker must not borrow mutable descriptors or another consumer's slots.
- `Transport::matchesRequest` checks exact snapshot identity and all options when
  reusing an existing mutable consumer after its fences. Authored capture edits
  use this check to retain their existing slots and descriptors.
- `Transport::canShareScene` instead checks immutable scene policy: texture
  resolution/budget, queue ownership, direct-light ownership and component data.
  Independent consumers may choose their own batch capacity, readback memory,
  dispatch budget, sampling policy, optional depth pipeline and distance/bias.
  Sun-query range is computed per consumer from retained geometry bounds.
- Request-aware `initializeShared` checks this compatibility before allocating or
  replacing a consumer. Snapshot hashes alone do not authorize reuse; packed
  input overrides are excluded from inferred compatibility. Explicit donor
  inheritance accepts those inputs, with either inherited or compatible private
  settings. These are preparation-time checks, not per-frame scene scans.
- Scene compatibility is distinct from consumer reuse. Runtime authored requests
  can accept compatible automatic transport inputs before the donor retires.
  Ordinary unstyled radiance requests have an explicit policy-inheritance path;
  general diffuse compatibility remains strict.
- `Transport::leaseScene` now provides scene-only retention after the preparation
  fence. A lease retains geometry, packed lighting and texture inputs without
  retaining donor pipelines, dispatch buffers, worker tickets or the bake job.
  `initializeShared(lease, source, options)` creates an independent consumer on a
  preparation worker, with the same snapshot/policy checks. Releasing the donor
  or the request lease cannot invalidate the admitted consumer. Lease memory
  snapshots contain only the shared allocation domain and deduplicate with
  consumer snapshots in `TransportMemory`.
- Superseding an unpublished authored capture can borrow its prepared immutable
  scene after the worker handoff, even if the automatic donor already retired.
  The replacement receives independent dispatch state; partial captures are not
  reused, and submitted slots retain their original job until their frame fences.
- A requesting provider must obtain a compatible, retained scene lease before
  the default job retires. Do not retain an acceleration structure indefinitely
  merely to make a later request cheaper. Define demand and retirement explicitly.
- Separate consumers must use one admission/budget policy while competing for
  bake work. Publishing an early diffuse volume does not mean reflection work
  has finished or a second scheduler may start spending the full budget.
- When diffuse baking is disabled, standalone automatic capture work also holds
  admission ahead of authored edits. `StandaloneCapture::pending()` describes its
  current job, not image availability: publication, disable or failure releases
  admission. The renderer checks it after recording automatic work. Previously
  this configuration could run two full capture budgets concurrently. Published
  authored images remain available while new authored work is paused.
- Consumers retain submitted resources until their own fences retire. Provider
  cancellation cannot invalidate another provider's published image or slots.
  Account shared scene memory once and private consumer memory separately.

`Authoring::requestScene` registers pending demand before automatic producers
advance. `SceneRequest::offer` accepts one compatible prepared scene; repeated
frames do not acquire more leases. The default and standalone controllers offer
only after their worker handoff and before job retirement. The authored worker
consumes that lease when new rays require a consumer, then releases the request.
No demand is registered for an absent/disabled document, a current publication,
or a reusable prior consumer. Map/config replacement cancels stale demand.
Pending lease memory participates in the shared allocation collector.

The runtime handoff retains two constraints:

- `Controller::activate` releases its job before publishing the ready state.
  Authored captures wait for that state, so looking up the donor after readiness
  cannot recover a transport. Register actual authored demand before activation;
  transfer a prepared scene owner after its preparation fence, and release the
  request on cancellation or map/config replacement. A warm-cache activation may
  have no transport at all, so the independent preparation path remains required.
- The default job applies `DirectPolicy` to packed light ownership and may retain
  styled component proposals. Ordinary unstyled outgoing-radiance queries do not
  read aggregate ownership, and select the ordinary proposal tree even when a
  component tree exists. `canShareRadianceScene` checks that narrower query
  contract. `SceneLease::radianceOptions` explicitly inherits the donor's actual
  ownership/component fields; normal `initializeShared` still validates those
  complete options. Authored jobs retain both the requested capture policy and
  actual transport options, so later edits reuse the same consumer without
  pretending its packed inputs changed. Component queries cannot request this
  unstyled contract. Diffuse queries still require exact ownership policy.
- Generated receiver charts are private atlas inputs and do not alter frozen
  transport geometry, materials or lights. The default job keeps the original
  snapshot as its transport source while using derived charts for atlas work.
  This preserves exact caller identity without accepting hashes as proof of
  transport compatibility.

The cold automatic-to-authored integration reuses a prepared scene after its
donor job retires, without relaxing sequential capture admission. Warm cache
loads with no donor retain independent preparation. Demand ownership is tested
separately from mutable slot ownership; further lifecycle integration must
preserve cancellation during preparation and replacement after submission.
Requests without authored demand must retain no extra scene or bake-job owner.
A shared radiance consumer uses the same shaders and buffers; policy inheritance
adds no shader branch or duplicate geometry/light data.

## Reflection source values

`reflection-values.glsl` now owns the resource-free `ReflectionReceiver` query
and includes the confidence-weighted sample values. The default include wiring
in `reflection-lighting.glsl` selects `reflection-environment.glsl`, which owns
Source 2 parallax-probe selection and the sky fallback. A replacement environment
module implements `sampleEnvironmentReflection(ReflectionReceiver)` and returns
scene-linear incident radiance. Fresnel, SSR/ray confidence composition, water
optics and output guides remain with their existing shading/pass adapters.

The custom-source fixture substitutes a resource-free horizon source through the
actual standard viewmodel, live viewmodel, water, ray-water and world resolve
shaders without changing their material calls. This is a compile-time module
boundary, not a public runtime shader-program loader or a new selectable preset.
Default pass descriptor layouts and candidate preparation remain unchanged.

`reflection-sample.glsl` is resource-free, like evaluated material values.
`ReflectionSample` contains scene-linear incident `radiance`, already multiplied
by source `confidence`, and that confidence in [0, 1]. A valid black sample has
confidence 1; an unavailable sample has confidence 0. Roughness filtering and
confidence validation belong to the source adapter, not the material response.

`resolveReflection(sample, fallbackRadiance)` adds fallback only for the remaining
confidence. It does not clamp or reweight the sample, fetch another provider,
apply Fresnel, normalize probe energy or transform display values. World, water
and viewmodel adapters share this resolver while retaining their existing
confidence clamps/filtering. Their existing source queries and pass-owned depth,
probe candidates and reflection guides remain unchanged. A replacement source
can supply the same value contract without exposing its texture/volume layout.

The `shading_values.py` compile fixture includes material/diffuse/reflection headers twice
and uses them in vertex, fragment and compute code without renderer bindings.
It guards the resource boundary; it does not implement a runtime shader loader or
prove visual equivalence for a new provider.

## Transport query limits

The provider-level radiance traversal takes explicit `maximumDistance` (scene
units) and `maximumBounces` arguments, separate from immutable scene bindings.
Its direct-light query also receives the sun visibility distance explicitly.
Offline capture, live diffuse and water reflection callers currently supply their
existing prepared limits. Keep these low-level traversal inputs below the material
API; material shaders should request lighting rather than manage ray-scene sets.
Callers own validation and sampling policy. Bias, materials, light identities and
other shared scene data retain the existing transport contract.

Water selects the common live diffuse source in dedicated shader variants when
supported. Both water queries use the world's GI scene descriptor at set 3;
reflection-only range/bounce limits occupy the extra eight bytes of the live-water
push constant range. Ordinary water layouts/shaders retain their eight-byte draw
range. Rebind water set 2 when entering the live layout because push-constant
ranges participate in Vulkan layout compatibility. Pass adapters keep refraction,
optical composition, coverage and output storage outside the lighting source.

`Transport::fragmentQuery` validates sampling/range policy and converts metres using
the active immutable snapshot's scale without allocating descriptors or buffers.
`prepareFragment` uses that same validation. Live water obtains its independent
reflection limits from this query, then reuses the GI descriptor instead of making
another reflection constants buffer/set. Other reflection-only users retain their
existing prepared context. The enabled water pass requires the existing four-set
water-ray capability; unavailable dependencies resolve coherently to the saved
baked fallback. Disabling water does not impose that capability on opaque GI.

The live-GI fixture checks colored water body lighting at 1/2/4x MSAA, with reflection
rays on/off and different configured diffuse/reflection ranges. Hardware transport
fixtures check query conversion across three host-unit scales and no allocation
on query validation. Baked/mixed-sun ownership for water remains a separate task.

## Provider-owned local-light context

`LocalLightSample` is defined in resource-free `local-light-values.glsl`.
Material models use `receiverLocalLightCount(context)` and
`sampleReceiverLocalLight(ordinal, receiver, normal, context, policy, sample)`;
ordinals are source-local, not captured/global light indices. The source owns
clustered lists, baked channels, selected-light bookkeeping and shadow inputs.
Default PBR world/viewmodel and water composition use
`standardReceiverLocalLight` with the same context and their dielectric response.
Custom models compose the source sample with their own `BrdfLobes` instead.

No material loop reads context fields. The alternate local-source fixture uses a
single-entry context with no light-list or baked-channel fields through actual
standard world, viewmodel, live-world and water shaders. Existing source query
and mixed-shadow behavior remain below these functions. This boundary does not
yet select custom host programs or allocate their descriptors at runtime.

## Sun source queries for material models

`sun-values.glsl` defines the resource-free `SunLightSample`. The receiver-level
`sampleReceiverSunLight(receiver, normal, unitIrradiance)` adapter owns chart
coordinates, bounds, position/depth and normalized geometric shadow inputs.
Custom models compose this sample through `composeSunLight` with their own
`BrdfLobes`; the standard model uses the shared standard response. The alternate
model fixture combines sunlight and local light samples without the standard
composition implementation. The unused legacy `sunLight` and low-level combined
`sunLightResponse` wrappers have been removed.

Water retains its explicit unbaked-sun query in this refactor. Its baked/mixed
sun ownership still needs separate qualification; a source API change must not
silently alter that optical/shadow policy.

## Provider-owned diffuse context

Material models no longer access `ReceiverLightContext` fields. They obtain a
context through `prepareReceiverLighting` and ask `receiverNeedsDiffuseReference`
when a metal needs a compatible reflection reference. Volume-only receivers use
`prepareVolumeReceiverLighting`; water does not construct a chart-shaped context.
The default source retains the same chart/probe queries and diagnostic semantics.

A source owns its context representation and the sampling/reference/diagnostic
functions in `receiver-lighting.glsl`. The alternate-source compilation fixture
uses a context with no chart or probe fields through the actual standard world,
metal, viewmodel, live viewmodel, water and live-water shaders, alongside the
default environment source. This prevents material code from silently depending
on the baked source's field layout. It is an internal shader module boundary;
public custom program registration and provider resource selection are separate.

## Standard reflection response

`standard-reflection-response.glsl` is a resource-free material response shared
by world reflection composition and the standard viewmodel shading model. It
applies the existing roughness-aware Fresnel to incident radiance. Viewmodel
source code supplies radiance and availability only; it no longer applies the
standard material response. Custom shading models can choose their own response
without importing standard shading. Disabled reflections retain an early exit
before response evaluation. Water keeps its separate optical-interface weights.

## Reflection transport source

The world reflection resolve queries `sampleTransportReflection(pixel, guide,
centerDepth)` from `reflection-transport.glsl`. Its `ReflectionSample` contains
current-frame incident radiance weighted by confidence. The default source owns
screen/ray buffer sampling and the s&box-style bilateral reconstruction. The pass
adapter supplies the receiver pixel, resolved guide and view depth; material
composition retains environment fallback, confidence clamping and Fresnel. This
is a compile-time source boundary, not a new GPU pass or runtime dispatch.

The alternate-source compile fixture replaces this implementation without any
raw radiance or bilateral storage access while retaining the actual resolve and
environment source. CPU producer/resource selection for a new implementation is
still required; implementing this function alone does not register a provider.

## Remaining API work

1. Extend transport provenance beyond the lobe ownership/per-light attribution contract for
   future unified GI providers. Keep source-index and receiver-ordinal query
   semantics distinct; existing mixed/range/highlight composition is shared.
2. Connect reflection radiance to interchangeable screen/ray and environment
   providers; qualify water baked/mixed-sun ownership.
3. Qualify direct ownership and indirect transport for complete provider replacements.
   Analytic-lobe, direct-attribution and reflection-reference policies are distinct, but the
   underlying per-light baked policy does not describe arbitrary GI provenance.
   Define contribution ownership and compatible normalization before LightingTerms.
4. Provide standard/custom composition with depth, shadow, MSAA and reflection
   guide compatibility. A material shader should not need pass-specific storage
   knowledge to remain correctly lit.
5. Complete independent provider scheduling/publication and dependency/pipeline
   selection. Reflection lookup/publication and content-addressed persistence are separated;
   default orchestration still borrows admitted diffuse seeds and one transport budget. Radiance cascades should implement the diffuse source interface;
   material shaders should not learn cascade storage or update details.
6. Add opt-in host material/shader programs and HLSL/`.shader` authoring above
   these contracts. No game-specific shader dispatch belongs in the native core.

The public custom-material program loader and full provider framework are not
implemented by the local-light prototype. Performance acceptance includes
compiled shader work, visual regressions, memory and matched CPU/GPU timings;
source organization alone does not prove zero overhead.
