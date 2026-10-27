# Forward material and lighting architecture

## Direction

Forward+ is the rendering framework, not a lighting technique. Its default preset
will be named **Source 2**: baked static direct lighting and diffuse irradiance on
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
contracts, not a stable public shader ABI. Provider selection and runtime material
program loading remain pending.

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

`Options::bakeConfiguration` already translates several overlapping switches:
`bakedLighting`, `selectedBakedLights`, `aggregateDiffuse`, `fullyBakedLights`,
`bakedGeometryLights` and `groupedStyledDirect`. Some still carry experimental
labels even though runtime baked mode selects related paths automatically. These
need a behavior/caller inventory and migration into the resolved lighting plan;
renaming the UI alone would leave the ambiguity intact. Live GI also changes this
configuration, so it belongs in the same resolution step rather than a second
independent set of booleans. None of these paths is declared unused by this audit.

The rough-metal fixture exposed fallback cubemap coverage holes: subdivision
created more cells than valid capture sites, then discarded empty cells. Fallback
placement now splits populated site sets, retaining coverage with sparse origins.
This concerns scenes without a closed spatial partition; fitted BSP coverage and
Construct's outdoor gaps remain separate quality work.
