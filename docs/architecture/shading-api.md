# Forward+ shading API

The target is the Source 2 / s&box style of shader authoring: evaluate a material,
choose a shading model, and fetch lighting through shared source APIs. The Source 2
preset selects the existing baked/probe/cubemap techniques. Custom shading models
and future GI techniques should reuse the same source interfaces.

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
world and viewmodel callers share transport without changing their passes. Water
still has its optical BRDF and separate sun composition; that path needs integration.

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
and metalness. `aggregate` identifies baked direct energy already included in
that value; it must be passed to the direct-light mixed-shadow correction.

Reflection-normalization luminance remains in standard composition. Extracting
that calculation into a helper increased control flow for metal-only variants;
it needs a better shared contract before moving. Diagnostic baked-source views
still query baked data explicitly, including when live GI is selected.

## Reflection radiance

`sampleEnvironmentReflection(ReflectionReceiver)` returns scene-linear incident
radiance from the authored/automatic probe source or visibility-filtered sky.
World reflection resolve and viewmodels share this query. It applies neither
Fresnel nor SSR/ray confidence, so composition owns those factors exactly once.
The pass adapter supplies the candidate mask, explicit layer and existing biased
probe position; probe images and parallax selection remain implementation detail.

`sampleViewModelReflection` combines that environment source with current-frame
SSR and returns radiance without material color/metalness/Fresnel. The standard
`viewModelReflection` wrapper applies its existing roughness-aware Fresnel term.
This is a viewmodel adapter, not a universal world-space SSR API: its screen
projection, source guides and descriptor layout belong to that pass.

World SSR/ray transport still publishes a confidence/radiance image consumed by
its resolve. Water retains separate optical composition and sky policy. A complete
provider contract must accommodate these pass dependencies; the environment query
alone does not make every reflection technique interchangeable.

## Remaining API work

1. Complete the composition contract over receiver-local queries, including
   explicit ownership of aggregate baked energy and mixed-shadow corrections.
   Keep source-index and receiver-ordinal query semantics distinct.
2. Connect the reflection radiance interface to interchangeable screen/ray and
   environment providers, and integrate optical water with shared queries.
3. Separate direct ownership and indirect transport in the composition contract.
   The current aggregate-bake flag and normalization bookkeeping still couple
   these concerns. Define units and provenance before exposing LightingTerms.
4. Provide standard/custom composition with depth, shadow, MSAA and reflection
   guide compatibility. A material shader should not need pass-specific storage
   knowledge to remain correctly lit.
5. Complete independent provider scheduling/publication and dependency/pipeline
   selection. Reflection lookup has an independent indexed section, but fitting
   and physical persistence still share diffuse bake planning. Radiance cascades should implement the diffuse source interface;
   material shaders should not learn cascade storage or update details.
6. Add opt-in host material/shader programs and HLSL/`.shader` authoring above
   these contracts. No game-specific shader dispatch belongs in the native core.

The public custom-material program loader and full provider framework are not
implemented by the local-light prototype. Performance acceptance includes
compiled shader work, visual regressions, memory and matched CPU/GPU timings;
source organization alone does not prove zero overhead.
