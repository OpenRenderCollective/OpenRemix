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
  return shadeSurface(surface, receiver);
}
```

This follows the reference's material initialization and shading model call. The
material function does not choose lightmap/probe/reflection storage. The standard
model supplies its reflection guides; pass adapters still own alpha/depth/MSAA
coverage and output writes. `shading-model.glsl` selects the model entry at compile
time. Its default aliases `shadeSurface` to `shadeStandardSurface`, without adding
a function wrapper. A custom model implements the same `shadeSurface` signature;
material pass includes and call sites do not change. Custom models can use the lighting queries below.
This is a shader-source contract, not a runtime custom-program loader or a
`.shader` parser. Do not add unused material properties until their semantics are
implemented through the selected shading model.

The tracked `tests/forward/shaders/custom-model.glsl` fixture demonstrates a
minimal alternate diffuse response using the shared diffuse/local-light queries.
`custom_shading.py` replaces only `shading-model.glsl`, leaves the actual material
pass unchanged and removes the default model/direct-composition/response-adapter
files before compiling world, reflection-guide, viewmodel and live-diffuse variants. The pass still decodes its
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
| Lighting sources | Fetch source lighting and visibility independently of BRDF | local/sun samples, receiver-local enumeration, diffuse queries and replaceable environment/screen-ray reflection sources |
| Shading model | Apply material response and compose contributions | `shadeSurface` selects the model; default uses `shadeStandardSurface` and `shadeStandardLocalLight` |
| Renderer | Select providers, allocate/update resources and choose pipelines | resolved `ResourcePlan`, concrete provider owners and recipe scheduling |

Keep receiver geometry separate even though the reference Material also carries
it. Shared exposure/output transforms remain outside material lighting. Source
shader names, VMT semantics and lightstyle translation belong in the host adapter.

Default surface/probe/style storage is imported through `baked-storage.glsl` by
sources that query it: baked diffuse (including explicit live diagnostic views),
local lights, sun, and the viewmodel/water environment fallbacks. The material
pass does not import that storage. Shared includes are guarded, so sources can
use the same field without duplicate declarations. World reflection resolve
retains its separate irradiance binding through its own environment source.

`receiver-coverage.glsl` owns pass coverage specialization IDs 3 and 4 independently
of baked storage. A replacement lighting set can therefore use normal geometry
coverage without declaring lightmaps/probes. The full replacement compilation
removes the default baked field, style, atlas, visibility and local-shadow files
and compiles world, metal, viewmodel, live, water and reflection-resolve passes.
This proves shader composition; host resources still follow the resolved plan.

## Local-light samples

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
baked direct lighting. Custom models use the receiver-level queries below; the
standard model uses `shadeSurface`.

`local-light-response.glsl` applies the standard PBR response to sampled values
without importing transport resources. The default local source owns the static
visibility and local shadow-map imports (`occlusion.glsl`, `local-shadows.glsl`)
while its optional `local-response-source.glsl` adapter imports `direct-light.glsl`
only for standard materials. Its optimized standard entry and raw query share
`local-light-query.glsl`, a function-body
template with one caller-specific miss exit. This preserves the standard early
returns without a runtime shader callback or duplicated transport implementation.
The body template is backend implementation detail; material authors call functions.

The shadow-filter GPU fixture compares the standard response with composition of
the raw query across a filtered moving-shadow boundary. It checks positive,
known-black and missing captured fields, including finite-emitter energy outside
the analytic cone and exactly unshadowed receivers. Raw queries retain their full
unit-irradiance contract even when a standard mixed residual would be zero.

## Receiver enumeration and sunlight

`localLightsForReceiver(receiver, aggregate)` returns a `LocalLightList` with a
count. `localLightIndex(list, ordinal)` resolves the source index for
`sampleLocalLight`. Cluster headers, baked-tail handling and overflow fallback
are implementation details. The aggregate argument describes receiver ownership,
not a global limit on static lights. Query once per receiver, then iterate its
count; no temporary light array is allocated.

`local-light-storage.glsl` owns default light records, cluster buffers and their
layout/lookup helpers. The default local source and cluster-building pass import
it explicitly; common material bindings no longer declare those resources. The
membership/overflow fixture uses the same storage module. Replacement local and
full lighting-set compilation removes the module, so custom queries do not
inherit clustered enumeration or default light-record types.

`sampleSunLight` supplies direction, linear HDR radiance, unit diffuse irradiance
and static visibility without material parameters. Its radiance includes the
moving-caster shadow factor. `shadeStandardSunLight` applies the standard BRDF.
Models use `sampleReceiverSunLight` and `composeSunLight`; the standard model
uses `shadeStandardSunLight`. The old combined `sunLightResponse` wrapper was removed.
`sampleUnbakedSunLight` exposes analytic/CSM visibility for passes without baked
sun coverage. It is a source policy, not a BRDF or game-specific shader choice.

The sun shadow sampler is declared by `sun-shadows.glsl`, not common pass
bindings. Its default binding remains 4; the numerical lookup fixture selects
binding 3 through `SUN_SHADOW_BINDING`. Replacement-source compilation removes
the default sun implementation and shadow-storage file together. Custom sun
queries therefore do not inherit an unused default shadow texture declaration.

## Local receiver context

`prepareLocalLighting(receiver, normal, diffuseRequired, aggregate)` prepares the
local-light list and baked direct channels once. `diffuseRequired` describes the
shading model's work, not a material type or engine shader name. The aggregate
ownership comes from the selected diffuse query.

Iterate `receiverLocalLightCount(context)`, then call
`sampleReceiverLocalLight(ordinal, receiver, normal, context, frontFacingOnly, sample)`.
This entry resolves the index, geometric receiver normal and channel data;
custom models do not need atlas, cluster or four-channel sentinel knowledge.
It returns the same `LocalLightSample` as the lower-level source-index query.
Its ordinal must be within the prepared list, and output is valid only on success.
The standard model calls `standardReceiverLocalLight`. The shared material
adapter supplies it for query-only sources; Source 2 retains its specialized
miss exits through an optional source-specific implementation.

## Diffuse receiver query

`prepareReceiverLighting(receiver, normal)` prepares the selected source's private
context. The baked source prepares chart irradiance and sun irradiance once; the
live source retains its geometric ray basis without chart fields.
`sampleDiffuseLighting(receiver, normal, context, required)`
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

The shadow-filter GPU regression checks an energy invariant using production
shadow lookup, local composition, diffuse composition and lobe selection. For
unit diffuse reflectance, a field containing unoccluded energy plus its mixed
residual must equal a unified field containing already-shadowed energy. Both
retain independently selected highlights; disabled lobes add zero. It covers
fractional point/sun visibility, baked static exclusion and viewmodel shadow
queries. It does not validate the transport of a future GI implementation.

This separates lobe ownership, per-light residuals and reflection normalization.
Reflection compatibility is not inferred from energy ownership: providers declare
a frozen reference domain/input identity or mark their reference unavailable.
Transport and normalization of an unimplemented GI provider remain unqualified.

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
SSR and returns radiance without material color/metalness/Fresnel. The standard model
uses `standardViewModelReflection` to apply its roughness-aware Fresnel term.
This is a viewmodel adapter, not a universal world-space SSR API: its screen
projection, source guides and descriptor layout belong to that pass.

World SSR/ray transport still publishes a confidence/radiance image consumed by
its resolve. Water retains separate optical composition and transmission background. A complete
provider contract must accommodate these pass dependencies; the environment query
alone does not make every reflection technique interchangeable.

## Optical water adapter

Water builds the same `ShadingReceiver` and uses `sampleDiffuseLighting`,
`prepareLocalLighting` and `sampleEnvironmentReflection`. The default Source 2
source selects volume probes without surface-chart allocation. Supported live
ray diffuse variants share the world's prepared scene with explicit water query
limits. A constant false direct-channel request removes unused atlas reads.
The optical model owns absorption, sheet/body Fresnel, refraction and screen/ray
hit composition. Its light-source queries do not apply those material weights.

Water obtains sunlight through `sampleOpticalReceiverSunLight`; the default
source preserves its existing analytic/CSM policy. It retains the existing
unfiltered/visibility-gated sky fallback. Switching static sunlight to the same
baked/mixed ownership as world/props still needs explicit quality/resource
qualification. Shared queries alone do not establish that policy.

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

The receiver-lighting API exposes `receiverBakedCoverage`, `receiverBakedDiffuse`,
and `receiverSunIrradiance`. The last query supplies unit sun irradiance from the
chart when the selected source has it. The standard material obtains the
reflection normalization reference from the sampled diffuse lighting. Baked
diagnostic views explicitly inspect the baked reference field even with live
diffuse selected.

Water retains its explicit unbaked-sun query in this refactor. Its baked/mixed
sun ownership still needs separate qualification; a source API change must not
silently alter that optical/shadow policy.

## Provider-owned diffuse context

Material models receive the source's private context through
`prepareReceiverLighting`. The light-work diagnostic calls
`receiverUsesSurfaceLighting` to mark receivers that sample a surface field.
Volume-only receivers such as water use `prepareVolumeReceiverLighting`.
`receiver-lighting.glsl` selects both the value evaluator and the complete
context implementation. `receiver-baked.glsl` retains chart/probe preparation;
`receiver-live.glsl` stores only the geometric basis and supplies no chart unit
irradiance or frozen reflection reference. The ray evaluator takes no chart
arguments. Explicit legacy baked diagnostic views retain their baked lookups,
prepared within those diagnostic functions rather than ordinary live shading.

A source owns its context representation and the sampling/reference/diagnostic
functions in `receiver-lighting.glsl`. The alternate-source compilation fixture
uses a context with no chart or probe fields through the actual standard world,
metal, viewmodel, live viewmodel, water and live-water shaders, alongside the
default environment source. This prevents material code from silently depending
on the baked source's field layout. It is an internal shader module boundary;
public custom program registration and provider resource selection are separate.

## Shading result initialization

`shading-values.glsl` defines `ShadingResult` and
`initShadingResult(surface, opacity)` without receiver or source resources. The
initializer supplies normal/roughness, zero reflection response, unavailable
reflection reference and black output with the supplied opacity. Standard shading
and the alternate diffuse model use the same initialization; a simple custom
model only assigns its resulting color instead of manually initializing guide
fields. Models requesting reflections still explicitly supply their response
and reference. Results carry normal, roughness, RGB reflectance and its presence
as ordinary material values. Cubemap-layer byte encoding stays in the forward
pass adapter, alongside MSAA guide packing, rather than the shading model. The
adapter preserves the legacy zero guide when a model supplies no reflectance.
This does not complete the public custom-program or arbitrary shading-model contract.

The resource-free values fixture includes this header twice and consumes its
result in compute as well as compiling it in vertex and fragment stages. Default
world/viewmodel/water and live/metal variants preserved optimized sizes and opcode
counts for the initialization extraction. The later unpacked-result adapter split
changes data-extraction instructions; after shaded-path specialization it preserves
arithmetic, texture and control-flow instruction counts. This is compiler evidence,
not a runtime-equivalence claim.

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

## Extension and qualification work

The implemented source/model boundary covers the current baked, ray-query and
adapted cascade recipes. The following work is distinct from those contracts:

1. Qualify contribution ownership and frozen-reference domains for new unified GI
   providers. Source-index and receiver-ordinal queries stay distinct; sharing
   units does not establish compatible normalization or transported components.
2. Qualify new screen/ray/environment implementations through the existing
   radiance/confidence interfaces. Water shares those queries but retains its
   explicit optical sunlight policy and needs separate visual qualification.
3. Connect each new provider's resources, source/readiness and pass dependencies
   to the central plan/recipe adapter. Source 2 intentionally reuses admitted
   seeds and one transport budget. Separate publication ownership does not require
   parallel jobs or duplicated scenes. The unmerged Radiance Cascades PR is a
   concrete extension, not evidence about every possible provider.
4. Add the deferred opt-in host material-program loader and HLSL/`.shader`
   authoring above Surface/source/model contracts. Its depth, shadow, MSAA and
   reflection-guide requirements must be validated before accepting programs.
   No game-specific shader dispatch belongs in the native core.
5. Finish matched runtime and visual qualification of the accumulated split.
   Standard/custom composition and replacement-source compilation are implemented;
   they do not establish whole-renderer CPU/GPU or memory equivalence by themselves.

There is no runtime provider registry or public shader-program ABI in this split.
Performance acceptance includes compiled shader work, visual regressions, memory
and matched CPU/GPU timings; source organization alone does not prove zero overhead.

### Diffuse publication during reflection refresh

A reflection count, resolution or bounds-tolerance edit retains the currently
published diffuse volume when the frozen source, lighting cutoff and diffuse
configuration still match. The capture refresh starts despite that retained
publication; frame submissions keep their normal shared asset leases. Changes
to diffuse settings, source bindings/content or baking enable still retire the
incompatible volume. This separates publication lifetime from refresh-job
lifetime. A capture-only refresh job borrows the published volume and immutable
admission metadata, bypassing diffuse preparation, tracing, denoising and GPU
uploads even when the disk cache is disabled. The volume retains only compact
admitted positions/original indices (16 bytes per usable probe, plus bounds and
allocation metadata), not diffuse SH or atlas pixels. Existing placement storage
is transferred where available; complete cache hits recover origins from the
same canonical reconstruction used by component recovery.

Reflection-only jobs keep their transport and cancellation state private. They
use the normal capture admission/budget and preserve the published direct-light
ownership policy. The capture-policy query borrows that policy directly; a
reflection-only job does not allocate a dummy mixed-light plan or copy receiver
identity/visibility maps. Newly captured radiance is saved as a shared blob/index by a
bounded background service, without serializing the diffuse snapshot again.
The service starts lazily and drains at runtime shutdown. Subsequent map loads
may still use the existing diffuse-index recovery path to assemble a combined
cache for the requested reflection configuration. Optional diagnostic local
capture companions are regenerated when their combined container is absent.
This lifecycle change adds no shader variants or per-draw dispatch; matched
whole-renderer performance acceptance remains separate.

### Compatible reflection normalization references

`lighting-reference.glsl` is resource-free. `LightingReference` carries incident
irradiance/pi luminance and a provider contract domain; zero means unavailable.
A valid zero-luminance receiver is distinct from an unavailable reference.
`normalizeReflectionReference` attenuates only matching, nonzero domains, never
amplifies a capture, and keeps unknown receivers/dark capture references neutral.
Different domains are neutral even when both values use the same radiometric
units. Providers must declare comparable lighting contents as well as units.

The Source 2 probe adapter explicitly specializes both sides to its existing
frozen-static domain and preserves the old scalar guard/vector operations. That
adapter still transports receiver luminance through its existing float guide;
this change does not add an arbitrary provenance ID to the guide ABI. Other
providers may use the typed entry independently, but must not claim Source 2
compatibility merely because they provide diffuse irradiance. CPU capture binding
additionally checks frozen scene/component provenance as described below; arbitrary
cross-provider guide transport remains an extension point.

### Optical receiver sunlight

`sun-lighting.glsl` supplies both `sampleReceiverSunLight` and
`sampleOpticalReceiverSunLight`. Opaque/viewmodel and optical materials consume
`SunLightSample` direction, radiance and visibility; each material retains its
own BRDF/optical response. Water no longer reads the packed frame sun direction
or chooses low-level CSM/baked visibility itself. Its query uses the receiver's
existing geometric normal rather than recomputing screen derivatives.

The default optical adapter preserves the existing analytic/CSM policy. This
interface split does not claim that water's static visibility is already mixed
like opaque props. A replacement source may implement both queries using its own
transport without changing water's material. The alternate-source fixture swaps
sun queries independently through world, viewmodel, live-GI, water and ray-water
material variants. Composed optical sunlight now uses the shared probe/moving
visibility policy described below; the default preset retains its existing policy.

### Sun transport and material response

`sun.glsl` owns visibility and incident sunlight samples. `sun-response.glsl`
applies standard GGX, while `sun-composition.glsl` composes sampled values with
caller-supplied lobes. Both have no transport resources. The pass imports generic
composition; the standard model imports its response. Custom models may consume
`SunLightSample` directly or reuse that response, while replacement sun sources
retain the same sample contract.

### Reference domains through material composition

Diffuse sources declare `diffuseReflectionReferenceDomain()` independently of
`ReceiverLighting.reflectionReference`, which still says whether a particular
receiver has usable reference energy. The domain describes comparable lighting
contents, not just units or validity. The baked source declares the frozen
Source 2 static domain; live ray diffuse declares no compatible reference.
Replacement sources choose their own contract rather than implicitly inheriting
the baked domain.

`ShadingResult.reflectionReference` and `ReflectionReceiver.reference` retain the
typed `LightingReference` through standard/custom composition, viewmodel and
water queries. The current world guide still stores a single luminance. Its
pass adapter accepts only Source 2 static references, converting a foreign or
unavailable domain to the existing unknown sentinel. Resolve reconstructs that
explicit domain; the default environment source applies the same compatibility
adapter before its scalar cubemap lookup. A compatible zero remains black.
This prevents a foreign reference being silently relabeled as frozen static
lighting without adding a guide attachment or changing the packed ABI.

Source domain declarations specialize away in the default shaders. The current
guide cannot transport arbitrary domains; a future provider requiring another
domain across that pass must add a compatible guide adapter. CPU capture binding
separately validates scene/component provenance; shader domain compatibility alone
does not authorize normalization.

### Local source isolation

The pass imports the resource-free local material response independently of the
selected local-light source. Only the default `local-lighting.glsl` imports
`direct-light.glsl` and its Source 2 raw sampler. Replacement sources implement
the receiver/context queries and may reuse `shadeStandardLocalLight` without
bringing in chart visibility, directional fields or default sampling functions.
The default optimized caller-specific miss exits are unchanged. The alternate
source compile fixture removes the default sampler file before compiling world,
viewmodel, live and water materials, so an incidental import fails immediately.

### Sun source isolation

Only the default `sun-lighting.glsl` imports `sun.glsl` and its baked visibility
and CSM implementation. The pass imports the material response independently.
An alternate source supplies the receiver and optical sun queries without
importing default sunlight transport. The custom-source fixture removes the
default sun implementation before compiling world, viewmodel, live, water and
ray-water materials. This is a compile-time boundary; CPU shadow resource and
caster scheduling still require the resolved provider plan.

### Explicit capture scheduling inputs

`CaptureProviders::record` records reflection work only. The renderer composes
the current preset by requesting compatible scene sharing, recording the
selected diffuse/bake work, then passing the automatic capture publication and
shared-transport readiness into the reflection scheduler. The scheduler does
not call the diffuse controller or derive its lighting configuration. A live
source can therefore supply its own readiness/publication contract without
making the capture scheduler start a diffuse bake.

The Source 2 adapter retains its existing order and single transport budget:
a published image alone does not release admission while diffuse/style work
is pending. Standalone retirement still records after switching back to baked
captures. Capture source ownership and frame-fence retention are unchanged.
Metadata lookup now exposes borrowed capture publications and accepts fallback
descriptors explicitly. The renderer supplies the Source 2 fallback; the capture
module has no diffuse-controller dependency. The renderer remains the recipe
adapter for source/readiness handoff. This changes CPU call organization, so
shader checks alone cannot prove whole-renderer performance equivalence.

### Capture publication lookup

`CaptureProviders` exposes a borrowed standalone source, capture-only header
selection and descriptor selection with an explicit fallback. The renderer
supplies the default baked descriptor/header/source and retains the original
precedence: authored header, available standalone image, then default source;
descriptors retain their existing authored-overlay selection. A published
diffuse volume still determines the reported scene scale before standalone or
default capture metadata. An active standalone owner with no image remains
selected for source identity and retirement; it does not manufacture a new
image or discard the default descriptor fallback.

The small selection adapters are inline and borrow existing owners. They add
no publication snapshot, reference-count copy, resource allocation or GPU work.
Borrowed pointers are consumed immediately on the render thread; submitted
frames retain their existing image leases. The capture module no longer includes
or takes `bake::Controller`; the shared prepared-scene request remains the
lower-level transport handoff.

### Query-only local providers and standard materials

`standard-local-lighting.glsl` supplies `standardReceiverLocalLight` from the
selected source's sampler and the shared resource-free PBR response. A new local
provider only needs its context preparation, count and sample queries. It does
not need to implement material BRDF functions just to work with standard world,
viewmodel or water materials.

The default Source 2 source declares the private compile-time
`OPENREMIX_SPECIALIZED_LOCAL_RESPONSE` hook and supplies its existing optimized
entry through `local-response-source.glsl`, imported only when the standard
model requests it. This preserves caller-specific miss exits and avoids the code-generation
cost previously observed when forcing that source through a boolean sample
wrapper. Other sources use the shared adapter automatically. There is no runtime
callback, setting, extra pass or duplicate material equation. The query-only
fixture removes the default sampler implementation while exercising the real
standard material across its supported passes.

### Capture policy without saved renderer options

`probes::CapturePolicy` carries the transport owner, automatic probe count,
fitting tolerance and authored fallback choice. `CaptureTransport` is owned by
the capture module: shared scene transport or standalone scene transport, without
requiring the shared owner to be a diffuse baker. `ResourcePlan` stores the
policy resolved at initialization/settings changes. Recording borrows it rather
than building another policy every frame.

The capture module accepts neither the full renderer `Options` nor its complete
resource plan. Metadata lookup takes only the selected transport; sampling still
uses the existing `CaptureConfig`. Selection policy is separate from radiance
configuration and cache identity, preserving the existing sampling/cache formats.
The Source 2 adapter still selects shared captures for baked diffuse and
standalone captures for supported live or disabled diffuse. Image disable retains
the owner that must retire pending work. No new runtime setting or GPU work is
introduced; CPU timing acceptance still needs to include this boundary.

### Component composition and remaining qualification

The normal Source 2 preset retains combined chart/probe irradiance. The existing
live-GI choice retains whole-field replacement with analytic direct lighting.
An explicit `Diffuse::RayIncoming` selection now composes retained baked direct
with live incoming light. Its saved representation uses `rtGI` together with
`bakedDirectWithLiveGI`; old saved live-GI profiles leave the new switch off.
Direct technique selection still controls whether baked receivers are requested.
In the renderer menu choose aggregate baked direct plus "Live incoming + baked
direct". The equivalent saved keys are `rtx.native.forward.bakedLighting=1`,
`rtx.native.forward.rtGI=1` and `rtx.native.forward.bakedDirectWithLiveGI=1`, with
map baking and direct diffuse enabled. Unsupported fragment/water ray capability
falls back to the configured baked source.

The composed plan requests ReceiverDirect data, one transport bounce and no
baked styled indirect. Per-light sun/controllable fields and mixed attribution
remain active. Standalone captures own complete reflection radiance; capture
resolution/count/tolerance do not participate in the direct producer's derived
configuration. Missing direct coverage retains analytic direct response.
Specialized composed world/viewmodel/water shaders are chosen at pipeline
creation. Switching direct ownership while live GI is active recreates those
pipelines; no per-draw source switch is added.

Renderer fixtures qualify seven-owner static corrections on surfaces/dynamic
probes and baked sunlight with moving-caster corrections for this composition.
Real-map hybrid/optical edge quality, reference compatibility of additional
component recipes, and matched runtime performance remain open. This does not
qualify every future provider combination.
On the unmerged PR #37 branch, cascade incoming uses the same query and explicit
baked-direct composition as ray incoming. Its native build, policy, replacement
sources and focused runtime fixtures pass. The private cube/sky source boundary
and receiver-direct fingerprint/reuse changes are included. Functional coverage
includes local direct plus incoming transitions, optical mixed sunlight and
viewmodel self/world isolation. The private cascade frame tail is declared and
uploaded only by selected cascade receivers; inactive Source 2 retains the common
frame and descriptor set. Real-map quality and matched runtime performance remain
unqualified. No playable deployment or PR merge is implied.
Preserve the combined Source 2 fast path while extending providers.

### Receiver-direct irradiance producer

`bake::Options::receiverDirectOnly` selects an offline consumer producing only
the eligible captured local-light receiver NEE term. It borrows the same prepared
scene as combined/incoming consumers; membership still comes from `DirectPolicy`.
Dedicated serial/parallel aggregate kernels omit incoming radiance integration
and retain primary geometry evidence. No receiver-direct kernels are created for
ordinary consumers. Existing kernel bytes and default pipeline counts remain
unchanged. Request one bounce for this consumer's work-budget accounting; its
receiver estimator itself does not trace diffuse bounce paths.

This is an algorithmic partition of the current aggregate, not a claim that every
physical direct/indirect source is already classified. Incoming paths include
sky, visible emission and bounced light. Sun and controllable direct fields remain
separate. The GPU fixture checks direct + incoming SH against combined energy
for volume and surface receivers, serial/parallel execution, mixed eligibility
and empty ownership, including signed coefficients. It verifies shared prepared
inputs and rejects incoming-field requests on a direct-only consumer before
recording. Mutable-lighting slots cannot produce this immutable direct field.

The explicit live-incoming plus baked-direct selection exposes this producer
through the technique UI and saved `bakedDirectWithLiveGI` request, with baking,
direct fields and aggregate receivers enabled. The resolved plan carries its
contents through tracing, cache recovery, snapshots and publication. Controlled
direct fields, sun ownership and mixed masks remain active; incoming light comes
from the independently selected live query. Combined Source 2 remains the default
and cannot be relabeled or reused under a receiver-direct cache identity.

### Irradiance cache content identity

`IrradianceContents` distinguishes the default combined estimator from the
receiver-direct estimator. Transport options expose their kind for a caller
constructing cache metadata. Internal `Config::irradianceContents` supplies the
expected kind to both container/diffuse keys and cache readers;
receiver-direct keys add their own version and both estimator shader hashes.
Combined keys retain their legacy computation.

`cache::View` and decoded `Snapshot` carry the kind. A previously unused payload
flag records receiver-direct contents; default combined bytes are unchanged.
Decoding checks the expected kind independently of the supplied key, and diffuse
index recovery threads it through both identity reconstruction and payload
validation. The background snapshot-to-view transfer preserves the same tag
through queued map writes. Thus a correct container key or spoofed index cannot
relabel combined energy as receiver-direct, or vice versa. Direct-only recovery needs no reflection
work or transient probe-origin array.

Receiver-direct cache views currently accept diffuse data only. Bundled or
referenced reflection captures and local-reflection companions are rejected until
their normalization reference contract is qualified. This tag describes RGB
irradiance contents, not reflection compatibility.

Controller jobs and published volumes carry the kind as CPU metadata; GPU data
layouts and the existing material shader variants are unchanged. The internal
receiver-direct mode currently requires aggregate direct ownership, direct fields,
one bounce, no styled indirect, and no automatic reflections. One bounce avoids
charging the scheduler for diffuse bounce paths that this estimator never traces.
Initialization rejects unsupported combinations and combined-reference diagnostics
rather than silently tagging mixed energy as direct-only. The mode participates in
configuration equality, so changing it invalidates the old diffuse publication.

Runtime Source 2 selection requests combined data; explicit live incoming with
baked direct requests the restricted direct-only mode. A direct-only volume is
never advertised as a complete Source 2 reflection-normalization reference.


### Composing baked direct with live incoming diffuse

`diffuse-baked-field.glsl` provides the shared chart/probe lookup without owning
the final combination. `incoming-lighting.glsl` supplies an incoming-only query;
its current adapter owns the ray-transport import. The material pass does not
import ray GI directly. Incoming diffuse and hardware water reflections share
the guarded `ray-transport.glsl` implementation, with descriptor-set selection
owned by the source. Incoming means sky, visible
emission and bounced radiance, excluding receiver-direct local-light NEE. A
replacement source implements that query without changing the material model,
baked field lookup or per-light mixed-shadow response.

`diffuse-compose.glsl` adds incoming irradiance / pi to a receiver-direct field.
It preserves the direct diffuse/specular/attribution policy: moving occluders
subtract from captured direct energy, while incoming energy remains present.
Missing baked coverage retains ordinary analytic direct response. Composition
never advertises the sum as a compatible frozen-cubemap normalization reference.

`OPENREMIX_BAKED_DIRECT_LIVE_INDIRECT` selects the composed source at compilation,
using the baked receiver context and the incoming query. Composition itself does
not require `LIVE_GI`: ray capability belongs to the selected incoming source.
Standard and custom materials compile through this source, including viewmodel,
and water passes. A query-only incoming replacement compiles
with the ray implementation physically absent from the test tree, both with and
without the ray-pass define. Sixteen existing default/custom/composed shader
variants remain optimized-byte-identical after removing the unnecessary guard.
This shader contract does not by itself enable composed cascade host pipelines. GPU numerical checks cover
mixed visibility, valid black and missing baked coverage alongside independent
producer energy-conservation tests.

The explicit live-incoming technique now connects this shader boundary to host
pipeline/resource selection and ReceiverDirect publication. The default Source 2
preset and the legacy whole-field ray-GI choice remain unchanged. Surface/probe
mixed attribution remains active during live incoming evaluation. Independent
cubemap capture edits retain the direct volume, and the sum keeps an unavailable
frozen normalization reference. Composed optical sunlight uses the same volume
visibility source with complete fallback; wider source/reference provenance and
real-map edge quality remain separate qualification tasks.

The earlier shader extraction's checked default optimized variants are identical
after stripping debug data and compacting SPIR-V identifiers. The composed mode
adds work only when selected; matched runtime performance acceptance and broader
real-map coverage remain open. It has not been promoted to the playable install.


### Component-scoped captured light validity

`scene::BakeLightScope` distinguishes complete captured radiance dependencies
from receiver-direct dependencies without depending on a GPU renderer. The API
registry classifies each captured definition by whether it supplied local
emitters. The existing dependency scan computes both validity answers together;
frame publication retains both as immutable values, so the render thread needs
no registry access or additional dependency traversal.

Combined data remains stale after any captured definition changes. ReceiverDirect
also becomes stale after local-emitter changes or invalid controllable-light
envelopes, but sun/dome definition changes do not invalidate its local direct RGB.
The sun fields store geometry-qualified unit responses: receivers still reject
changed sun shapes and use full current shadow fallback, and can reuse the field
when the captured shape returns. This lets current sun colour change without
restarting direct-only baking. Shape classification belongs to the captured
definition, rather than whichever representation a later update happens to use.

This is validity for an existing immutable capture, not a component-specific
source fingerprint. Explicitly republishing a new capture can still change the
whole scene identity; splitting those incoming-only identity inputs remains
separate work. No default shader or GPU work changes accompany this boundary.
Matched CPU/frame-time acceptance must include the scene-publication change.


### Composed viewmodel sunlight coverage

A dedicated renderer fixture now exercises composed viewmodel sunlight at 1x,
2x and 4x MSAA. A frozen occluder supplies a static sun mask through world-space
probes. Current world casters and viewmodel self casters use the close-up cascade,
with self casters excluded from world shadows. Removing the casters restores
exact pixels, and changing sun colour retains the same direct-only publication.
Moving the viewmodel outside probe coverage verifies complete static fallback.
The incoming ray range is bounded to isolate direct ownership; separate material
fixtures cover changing incoming illumination.

This qualifies the existing source/context composition for these cases; it adds
no production path. It does not prove every viewmodel shadow submission is
necessary: fallback caster preparation and close-up shadow cost still belong in
runtime profiling. Composed optical water sunlight coverage is described below.


### Composed optical sunlight

In composed mode, `sampleOpticalReceiverSunLight` now uses the dynamic receiver's
baked sun visibility, moving-caster shadow term and complete static fallback.
It has no chart coordinate or pre-integrated surface sun term. The water material
continues to consume `SunLightSample` and apply its own optical/PBR response;
no packed storage or shadow-policy selection moves into that material.
The default Source 2 and legacy live-GI optical adapters retain their prior
analytic/CSM policy through compile-time selection.

A renderer fixture exercises static probe occlusion outside realtime cascades,
moving shadows, unchanged static-mask values outside the moving caster, exact
restoration, sun-colour reuse and complete fallback outside probe coverage.
It covers 1x/2x/4x MSAA and both composed water pipeline variants, with reflection
rays on/off. A custom sun source also compiles through composed water with the
default sun implementation absent. Existing scalar-probe interpolation is reused;
this boundary does not promise sharper or leak-free probe visibility edges.

Optimized descriptor reflection reports twelve storage buffers in the composed
ray-water fragment, within the existing water capability gate; no new capability
requirement is introduced. Checked default/custom optimized shader variants are
unchanged after identifier normalization. Matched runtime performance and wider
real-map optical quality acceptance remain open.


### Receiver-direct snapshot identity

Frozen scenes retain a separate receiver-direct fingerprint alongside the
unchanged combined fingerprint. The direct domain includes the existing frozen
geometry/material records, local emitters, partition, receiver layout and sun
visibility shape. It excludes reflection hints, sun power and dome inputs, which
belong to reflection or incoming-light providers. Both hashes are computed at
snapshot creation; frame recording selects one scalar without traversing inputs.
Receiver-direct controller/cache lookup and diffuse-index lookup use this domain.
Persistence regression coverage warm-loads unchanged direct payloads after incoming
power/reflection-hint edits, rebinds mixed ownership to the replacement snapshot,
and rejects local-light radius edits. The seven-light surface/probe GPU fixture
uses the same selected component identity for persistence lookup and passes its
correction/cache checks.

Completed receiver-direct publications survive snapshot replacement when component
content, configuration, ordered records, instance/light identities, chart order
and live bindings all match. The publication retains its original immutable
snapshot; submitted-frame leases and GPU allocations are not mutated. Ongoing
jobs, reordered/replaced bindings and other content take the cancellation/cache
path. The binding comparison runs only when the snapshot pointer changes.

CPU regressions reject changed instance identities, missing bindings and reordered
charts despite equal unordered content. The composed viewmodel sunlight fixture
explicitly recaptures changed sun power with disk caching disabled at 1/2/4x MSAA;
it requires the same publication pointer, ready state and exact rendered output.
Direct identity excludes roughness scalar/image/remapping; same-live-material
roughness edits preserve exact GPU publication with disk caching disabled. Material
replacement still requires rebinding and can warm-load compatible direct data.
Alpha and measured optical inputs remain dependencies. Other material records stay
conservative: albedo/emission edits can still invalidate direct content. Broader
material dependency pruning, real-map reuse and matched timing remain unqualified.


### Private environment storage and candidate gathering

The default `reflection-environment.glsl` owns the parallax-probe storage include
and, for world resolve, its tile-candidate implementation and sky fallback.
Viewmodel sky resources and probe-position bias are private to that source too.
Material passes supply binding assignments without including this storage.
Replacements implement `sampleEnvironmentReflection(ReflectionReceiver)`, the
viewmodel query `environmentReflectionPosition(position, normal)` and the
world-resolve query `environmentReflectionCandidates(position, valid)`.
A source with no position bias returns the position unchanged. The candidate query runs
uniformly before out-of-bounds/invalid lanes return, preserving the default
provider's workgroup barrier contract. A source without candidates returns zero.
Viewmodel and water composition continue to use the same incident-radiance query.

The alternate-environment test removes `probes.glsl`, `probe-tiles.glsl`,
`environment.glsl`, `environment-sampling.glsl` and the viewmodel environment
resource file before compiling real world resolve, viewmodel and water material
passes. The default water reflection fallback also lives in the selected source's
`water/shaders/environment.glsl`; its baked-sky visibility policy is no longer
defined by the optical material. Replacement compilation removes that file too.
Water's sky transmission background remains a separate material input;
replacing reflected radiance does not replace transmitted background lighting. Default
and custom-material shader checks preserve sizes and opcode counts across twelve
variants. After structural global-ID/declaration-order normalization, their
ordered executable instructions and global declarations match. Viewmodel
reflection and water environment/HDR/MSAA GPU fixtures pass. The source-file move
changes some raw SPIR-V declaration order; it is not byte-identical. This is not
a new matched runtime FPS study. CPU scene/component provenance is checked at
binding time; the default shader environment still accepts only the typed Source 2
static reference domain.

The scalar reflection-guide convention belongs to
`reflection-guide-reference.glsl`. Its typed `packReflectionGuide` overload and
`reflectionGuideReference` decoder form one adapter: replace them together when
a provider uses another normalization domain. Material passes and the reflection
resolve do not assign the Source 2 domain themselves. The default adapter rejects
foreign references and keeps the existing RGBA16F layout; no extra guide image
or domain channel is allocated. This is a compile-selected convention, not
per-pixel support for mixed domains or proof of scene/component compatibility.

### Capture-reference provenance at binding time

Shader `LightingReference` domains describe comparable lighting conventions.
CPU `probes::ReferenceProvenance` separately carries a nonzero domain and frozen
input identity; both must match before an image advertises normalized metadata.
`source2Reference` uses the combined static-lighting input hash. Complete combined
receiver fields can advertise that identity; receiver-direct and incomplete fields
cannot. Capture output hashes, quality counts and projection bounds do not define
which lighting components the normalization reference represents.

ImageAsset retains normalized and neutral metadata views over the same radiance
and projection. Binding an unknown/mismatched receiver selects neutral metadata
without a shader comparison. Composition/selection carry per-layer provenance,
but one descriptor selects the whole image: mixed-provenance images conservatively
neutralize normalization for the whole image. Per-layer normalization is not
implemented. Zeroing foreign layers' reference coefficients alone would be wrong,
because the shader blends their radiance before applying the common ratio. A
future mixed-layer scheme needs matching radiance/reference composition semantics.
New lighting recipes must provide a qualified domain/input identity or stay neutral.

### Material response ownership

The pass imports `brdf-values.glsl` (Pi and BrdfLobes), `sun-composition.glsl`
and `local-light-composition.glsl`. These express response values and visibility
composition without choosing a BRDF. Standard GGX lives in `brdf.glsl`; the
standard model and water opt into it through their response adapters. Custom
models can use the same light samples with another response while all standard
BRDF, sun-response and local-response implementation files are absent.

The default local source advertises an optional specialized standard-response
module, preserving its early miss exits. Generic material code requests that
module at compilation; the query-only source itself no longer declares or
imports the standard response. Replacement sources without the hook receive
the generic standard adapter. Source 2 chart and cluster traversal lives in
`local-accumulation-source.glsl`. Other sources use the generic ordinal loop
without access to the Source 2 buffers. Both paths are selected at compilation.
