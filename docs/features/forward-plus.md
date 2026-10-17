# Experimental Forward+ backend

See the [delivery roadmap](forward-plus-roadmap.md) for remaining integration and quality gates.

The reference renderers are Counter-Strike 2, Half-Life: Alyx and s&box. Use their
lighting decomposition, forward PBR/MSAA behavior, mixed shadows, probe volumes
and reflection handling as references while checking the actual version and
available implementation. Public viewer code and extracted resources do not
establish Valve compiler limits or defaults. A light's authored range, runtime
light count and baked visibility-channel capacity are separate properties.

Select `Forward+ (experimental)` in the Rendering tab, or set:

```ini
rtx.native.backend = forwardplus
rtx.native.forward.samples = 4
rtx.native.forward.sceneScale = 0.0254
```

The GMod wrapper also accepts `OPENREMIX_PROXY_BACKEND=forwardplus` at startup.
The scale above is for Source's inch-based coordinates. Other hosts default to one metre per unit.
The GMod wrapper supplies the Source value as a profile default. Existing saved preferences take precedence.
It converts metric distances (luxel/probe spacing, ray bias and reflection reach) to host coordinates;
it does not multiply emitter positions or radii. Light geometry uses the same host units in every backend.

## Current behavior

The backend renders supported opaque and alpha-tested world surfaces into an HDR target using forward
PBR shading. Compute shaders build logarithmically sliced light lists. Local analytic lights use bounded
influence spheres and smooth distance falloff; four shadow-map cascades provide sun visibility. MSAA
supports 1x, 2x and 4x, falling back to the highest supported colour/depth sample count at or below the request.
Camera-only backface flags hide world backs without changing double-sided shadow or bake transport.
Two-sided camera materials retain both faces; seeing a back face never allocates a second lightmap chart.

The renderer requires a graphics/compute queue, storage buffers and sampled HDR/depth attachments.
It does not require ray-query, acceleration-structure, descriptor-indexing or temporal-upscaling features.

Local lights use a bounded budget of cached static maps plus independent live caster maps. Hardware
ray-query baking supplies indirect irradiance and soft per-light static visibility on selected charted
surfaces. Receivers without a baked channel use static plus dynamic depth maps when their light fits
the budget. Surfaces without usable indirect lighting retain configurable ambient fill. Baked parallax
cubemaps supply static specular transport; optional live hardware reflection rays fill SSR gaps.
Captured geometry is excluded from moving-caster shadow maps. Static fallback maps
remain available for receivers without a matching baked shadow; static sun cascades
reuse their depth until the exact fitted projection or frozen scene changes. The
Rendering tab reports captured/live caster candidates and static cascade updates.
Developer frame captures include `.shadow-policy.bin`: five little-endian
`uint32_t` values for captured candidates, live candidates, static sun cascade
updates, static local-map updates, and unmatched statically classified candidates.
Candidate counts precede shadow-frustum clipping; they are not submitted draw-call counts.
Frozen caster bounds are cached with the immutable scene. Per-frame frozen draw
records and material descriptors are prepared only when a static sun cascade or
local-light map needs rebuilding. `.shadow-preparation.bin` records that frame's
prepared frozen draw count as one little-endian `uint32_t`.
General transparency, viewmodels, decals and unsupported masking operations stay in compatibility
rasterization. Measured transmission bodies use the late Forward+ water pass by default.
The Rendering panel's Water refraction and reflections control, or `rtx.native.forward.water = 0`,
restores compatibility rendering. An explicitly saved zero remains respected.
Pipeline preparation runs with the renderer's worker preparation; independent refraction images are
allocated only on first water use and remain reusable until target replacement. Dry scenes allocate
no water snapshot images. Target-memory statistics include those images once allocated.
Water's sky-reflection fallback and underwater body lighting also accept the camera's uniform
environment radiance when no dome is authored, matching the primary path tracer's environment
selection. An authored dome replaces that uniform environment. This does not paint a sky background
for the uniform environment or bypass the baked-visibility requirement for uncovered reflections.

`water/optics.glsl` provides the primary path tracer's Schlick/Snell
boundary, total internal reflection, measured absorption and surface/through weights. This stays distinct
from the exact-Fresnel smooth transport used by the light baker. `water/screen.glsl` supplies bounded
opaque-depth refraction queries with orthographic/perspective reconstruction, either camera-Z sign,
foreground rejection and a contact tolerance for shorelines. A caller supplies resolved opaque depth
sample in Vulkan zero-to-one depth coordinates; a depth of one means no opaque layer. Unknown/off-screen
layers remain misses. The query brackets crossings even if a coarse step passes the camera's far plane.

The late pass samples independent snapshots of completed opaque HDR and opaque depth,
then writes water into the existing one/two/four-sample colour and depth attachments. It uses shared
replacement materials, PBR maps, clustered lights, mixed shadows and filtered parallax cubemaps.
Source water normals retain their authored scroll rates. Camera-medium absorption happens before
tone mapping. Water shares the opaque hierarchical SSR tracer and rough colour-pyramid filtering;
optional hardware rays fill unresolved water reflections. Existing opaque reflections are included once
in the transmitted background. Nested transparent layers are unsupported.
For standard 2x/4x sample locations, the depth snapshot reconstructs pixel-centre depth only when
the pixel and four neighbouring sample sets agree on one plane. This avoids a nearest-sample bias
making a shallow shoreline look like a missing/deep layer. Discontinuities, missing coverage,
image borders, numerically unresolved slopes and unknown sample patterns retain nearest depth.
The test uses a fixed D32 tolerance; it does not increase the refraction wall-gap allowance or add images.

## Ownership and modules

`src/native/forward` owns its GPU state and reads the shared CPU scene through the raster resource provider:

| Module | Responsibility |
| --- | --- |
| `options.h` | Backend settings, independent of path-tracing settings |
| `lighting.h/.cpp` | CPU transforms, cluster-depth policy, draw eligibility and cascade fitting |
| `bindings.h` | Explicit CPU/GPU buffer layout contracts |
| `material.h/.cpp` | Shared draw/material packing and normal transforms |
| `water/data.h/.cpp` | Measured-body draw eligibility and resolved optical/normal-scroll material data |
| `water/optics.glsl`, `water/screen.glsl` | Primary water boundary and bounded screen-depth tracing primitives |
| `water/snapshot.h/.cpp`, `water/shaders/snapshot.comp` | Independent completed opaque HDR/depth inputs for water |
| `water/pass.h/.cpp`, `water/shaders/surface.glsl` | Late MSAA water shading, attachment transitions and opaque specular composition |
| `water/shaders/hardware.glsl`, `water/ray_shaders/embedded.h` | Optional fragment reflection continuation through the shared live ray scene |
| `resources.h/.cpp` | Noncopyable Vulkan buffer/image ownership and allocation |
| `targets.cpp` | HDR/MSAA/depth/shadow attachments and descriptor layouts |
| `pipelines.cpp` | Forward, shadow, cluster and display pipeline creation |
| `scene.cpp` | Immutable geometry cache, skinning, light/material packing and per-frame descriptors |
| `rendering.cpp` | Cluster dispatch, shadow/world recording and output composition |
| `reflections.h/.cpp` | Current-frame SSR targets, guide resolve, depth hierarchy, tracing and reconstruction |
| `shaders/screen-reflection.glsl` | Shared opaque/water depth-hierarchy traversal and confidence-weighted hit radiance |
| `shaders/reflection_math.glsl` | Shared fused projection, normalization and reflection math for screen and hardware reflection rays |
| `image_dump.h/.cpp` | Optional, fenced developer image/buffer readback |
| `ray_scene.h/.cpp` | Optional triangle acceleration, independent of camera and material layouts |
| `ray/capture.*` | Complete authored plus current posed-scene capture and transport cache identity |
| `ray/live_scene.*` | Shared live transport, lighting streams, cache and frame-fence ownership |
| `ray/reflections.*` | GPU-generated reflection rays and confidence merge, consuming the shared scene |
| `shadows/` | Local-light selection, exact caster membership and separate static/dynamic depth arrays |
| `bake/` | Static scene packing, texture capture, bounded hardware transport, probe placement and publication |
| `bake/budget.h`, `bake/timing.h` | CPU batch policy and private, fenced GPU timing for adaptive bake throughput |
| `bake/cache.h/.cpp`, `bake/cache_io.h` | CPU-only finished-lighting cache, bounded wire decoding and atomic disk publication |
| `bake/shaders/sample-sums.glsl` | Workgroup sample reduction and shared containing-medium classification |
| `probes/` | Reflection capture placement, packed cube assets, seam-aware sampling and GGX mip filtering |
| `mixed/refine.cpp`, `mixed/occlusion.*`, `mixed/map.*` | Visibility-driven channel reuse, CPU bitset encoding and private GPU occlusion-map ownership |
| `mixed/overflow.*`, `mixed/masks.*` | Streaming overflow audits and compact partial visibility masks with bounded storage |
| `runtime/forward.cpp` | Background preparation, activation, retained map-bake ownership and backend retirement |
| `scene/bake_scene.h/.cpp` | CPU-only authored bake snapshots, ownership and input fingerprinting |

The compatibility raster renderer and Forward+ use the same `forward::accepts` predicate. Every supported
world draw belongs to one path. Materials and meshes remain alive through each submitted frame's fence.
Texture uploads and samplers remain owned by the existing raster resource provider. Forward+ keeps no
borrowed caller pointers and changes no core Remix struct layouts.

Frame buffers and descriptors are private to each frame slot. Geometry is shared while immutable;
skinned vertices belong to their submitted frame. Render targets are shared on one queue with explicit
dependencies covering previous composite reads and subsequent attachment writes. Backend replacement
and target destruction wait for submitted GPU work. Superseded preparation jobs retain their resources
until the worker finishes.

## Lighting and composition

The light list has 64 entries per cluster. A cluster that overflows evaluates the full light array in
the fragment shader rather than silently omitting lights. Sphere and triangle emitters use approximate
solid-angle direct lighting. The shared Forward+ emitter helper follows the path tracer's angular
spotlight softness/focus and cancellation-resistant sphere solid angle. The baker samples actual emitter
positions/directions for radial energy and soft visibility; it does not blur a hard shadow after baking.
Analytic sphere connections end at the sampled emitting surface without subtracting the receiver's
ray bias: these lights have no geometry to self-intersect, and shortening the connection can miss a
wall immediately in front of the emitter. Triangle emitters retain an endpoint tolerance for their
actual emitting geometry. Partly embedded lights illuminate through their exposed samples; opaque
blockers are never exempted to recover a light whose centre lies inside a fixture.
Sphere intersection chords use a perpendicular-distance calculation to avoid distant-light cancellation.
These analytic
approximations do not reproduce every path-traced area-light behavior.

The Source bridge retains BSP texture lights as a distinct internal light type. Its shared game/replay
conversion applies hemisphere emission with a cosine focus along the authored surface normal, rather
than Source's isotropic model-light approximation. This uses the existing sphere sampler and keeps one
runtime emitter per BSP light; forward lighting, baking and path tracing consume the same shaping.
The fixed sphere radius still approximates the source patch: reconstructing patch extents and handling
emitters embedded in fixtures remain separate placement work. The conversion does not move lights or
disable wall occlusion. Direction, cone and focus participate in bake cache identity.

Mixed-light planning rejects chart/light pairs wholly outside an emitter's emitting hemisphere before
allocating visibility work. The test includes every chart vertex and surface/seam sample, the sphere
radius or triangle corners, and a floating-point uncertainty margin. Wide unfocused spotlights and
isotropic lights retain their full range. Bounds also enclose the baker's surface-normal origin bias.
Rejected pairs do not become blocked-visibility metadata:
they emit no light toward those receivers. Diagnostic bake capture logs report the rejected pair count.

Overflow audits group receiver samples and active atlas donors by chart once. Filtering samples
that no longer own an atlas texel cannot affect classification or masks and receive no overflow rays.
The visibility-only GPU path reduces and transfers one float per point; direct-light batches retain
their nine directional coefficients. This preserves the same emitter samples, blocked/clear endpoints
and mask encoding. It does not remove the per-texel/per-light visibility workload: the four retained
direct-light channels and separate overflow masks remain a scalability limitation.

Published scalar masks can use a pooled R8 texture array. `mixed/mask_pages.*` packs
chart/light rectangles into shared layers, retaining an independent lookup for each
captured light. Layer ownership is local to a rectangle rather than one layer per
light. Sampling clamps the four bilinear taps to that rectangle, preserving chart
edges without allocating gutters or reading another light's texels. The cache keeps
its existing compressed payload; page publication decodes those exact R8 values.

Selection compares page padding and lookup storage against compressed storage,
then compares actual Vulkan allocations, including image alignment. Pages are used
only when they save allocated image memory and fit device dimensions/layer limits;
dense masks retain their compressed representation. Both paths use the same indexed
chart/light lookup and can represent more than four partial shadow masks per chart.
This changes storage and sampling, not the number of visibility rays required to bake
those masks. `REMIX_DEBUG_MASK_PAGES=0` retains compressed publication for comparisons.

The private bake transport also accepts explicit fully baked diffuse membership through
`bake/policy.*`. It preserves captured indices, rejects moving-light membership and samples
selected static direct emitters into the existing RGB irradiance SH using the all-light mixture PDF.
The direct estimator has an independent ordinal-based random stream; visibility/direct-SH,
radiance captures and streamed live lighting retain their existing contracts. The ordinary map
shaders exclude the aggregate estimator, and its separate pipeline is compiled only when needed.
The mixed planner omits aggregate lights from visibility tasks while retaining their identities.
Caches store explicit ownership, reject aggregate/mixed owner conflicts and require the caller's
expected policy; empty-policy entries keep the legacy layout and key. Aggregate matching also
requires unchanged radiance, while mixed unit-radiance data can follow colour changes. A shared
source identity cannot ambiguously span both ownership policies.
The ordinary map controller still uses the legacy policy. Automatic mixed-light selection,
runtime diffuse suppression and invalidation of stale aggregate RGB must be integrated before enabling it.
Generated emissive geometry needs an explicit sampling/double-counting treatment as well.

An enabled live dome is drawn as a display background, followed by the existing raster sky. The dome uses
the camera rotation and the light's world-to-local transform, with horizontal panorama wrapping and
vertical clamping. Camera translation does not move it. This pass needs no hardware ray queries or
anisotropy support. Forward HDR resolves before a display composite writes colour
and representative opaque depth into the output target. Uncovered samples retain sky colour through
premultiplied coverage. Raster effects, viewmodels and UI then use the established ordering, including
soft-depth capture, mask passes and halos. The display transform currently uses fixed exposure and a
bounded filmic curve; shared automatic exposure/bloom integration is future work.

MSAA covers geometry, including alpha-tested geometry silhouettes. Alpha tests remain thresholded;
alpha-to-coverage and sample-rate shading are not enabled. Normal variance increases roughness to reduce
specular shimmer. Screen effects and compatibility depth currently see the nearest covered depth at
MSAA silhouettes, so fine transparency intersections need further validation.

## Screen-space reflections

SSR runs after opaque shading and before the display transform. It traces the current frame's opaque
HDR colour, with no temporal history or reflection feedback. Full, half and quarter resolution are
available. A hierarchical depth traversal skips empty screen regions, then checks candidate hits against
a world-space thickness tolerance and the hit normal. Screen edges, distance and roughness reduce
confidence. Off-screen static objects and the captured environment use the baked cubemap fallback.
With hardware rays disabled, dynamic objects outside the screen have no reflection source. Transparent surfaces and viewmodels remain outside the reflected opaque scene.

MSAA normals, roughness, reflectance and depth resolve from the same nearest covered sample. A linear
HDR colour pyramid is built alongside the depth hierarchy. Each hit selects a trilinear colour mip from
the receiver's squared roughness, screen-space hit distance and grazing-angle footprint. A 2x2 bilateral
reconstruction uses depth, normal and roughness agreement at reduced resolution; full-resolution mirrors
keep their original sample. The footprint and reconstruction are adapted from Facepunch's MIT-licensed
s&box shader, with attribution in `src/native/forward/shaders/SBOX-LICENSE.txt`.

Colour levels use area-weighted reduction for odd dimensions and retain premultiplied coverage. Finite
HDR values are kept within half-float range. The chain adds approximately 84.4 MiB at 3840x2160 and is
replaced by a one-pixel dummy when SSR is disabled. It uses the existing resolve/reduce dispatches and
descriptor bindings. This is an isotropic footprint approximation: it softens valid screen hits, but does
not trace a full GGX lobe or recover missed geometry. Cubemaps remain the fallback for those misses.
A Fresnel-weighted scene-linear specular contribution is added before exposure and tone mapping.
Rough surfaces above the configured cutoff receive no SSR; they can still use filtered cubemaps.

Reflection reconstruction and ray projection use shared, explicit fused arithmetic.
Depth unprojection can subtract nearly equal terms, while ray-hit and probe-box
checks turn small numerical differences into a change of reflection source.
`reflection_math.glsl` fixes the fused operation order for transforms, dot
products, normalization and reflected directions. Ray evaluation also uses
explicit fused multiply-adds. The optional hardware reflection launcher shares
the same helpers. This preserves the existing depth/thickness and room-boundary
policies; numerical stability does not require enlarging every probe volume.

The fallback prefers a fitted cubemap. When none covers the receiver, an enabled live dome can fill
the gap using the published irradiance grid's directional miss fractions as approximate sky visibility.
It reuses the grid's distance-variance and receiver-facing weights; unknown, invalid and unsupported
regions contribute zero. The cubic visibility response suppresses small openings. Closed directional
cones do not receive an unrestricted sky reflection. The same mechanism works with zero cubemap captures.
The sky fallback uses at most eight fixed GGX samples, or one for a nearly smooth surface, and adds no
ray-query dispatch or environment prefilter allocation. It samples the current live sky while visibility
remains static. This coarse approximation can over-occlude or leak near doorways, thin walls and moving
occluders; its fixed sample footprint can alias small bright sky details. It is not a replacement for local
captures or optional hardware reflection transport.
The composition is `SSR.rgb + fallbackRadiance * (1 - SSR.confidence)`, followed by one Fresnel weighting.
Optional hardware transport fills screen misses before that composition. Each fallback therefore fills
missing transport without adding the same specular lobe twice. Sky fallback stays unavailable until a
usable visibility bake is published and disappears when that bake is invalidated.

## Parallax cubemap probes

After diffuse tracing, the same static transport module captures directional radiance into six cube faces.
Offline captures use the configured bake sample count at each fixed primary direction, rather than one
noisy lighting path per texel. Lighting samples are distributed across a 64-lane workgroup and reduced
as RGB; the first sample retains hit-distance metadata. Live single-ray reflection queries retain their
existing stream and cost. Capture dispatches have their own timestamp-guided work budget, bounded by
the same point/sample/bounce ceiling as the rest of the baker. This adds capture work without increasing
the cubemap resolution, storage or per-frame sampling cost.
It reuses the prepared acceleration structures and texture atlas. The directional mode returns radiance
and first-hit distance without the irradiance mode's cosine convolution. Geometry emission, analytic light
visibility and diffuse continuation use the shared material rules. The capture currently models diffuse
outgoing radiance at hits; secondary glossy reflections and normal-map detail are not captured.
Radiance sample streams use the ray origin and direction, so splitting captures or live reflection
queries into different dispatch batches does not change their stochastic lighting samples.

Capture origins rank authored cubemap hints and automatically searched empty-leaf sites by nearby axial
enclosure, preferring sheltered rooms over open areas. Authored positions break equal-coverage ties
within an enclosure class; validated irradiance sites provide a fallback. When a closed BSP partition is available,
a worker grows axis-aligned boxes across empty leaves, clipping conservative
bounds down each BSP path. Artificial empty-leaf splits do not become room boundaries. Nearly duplicate
fits share one capture. A bounded shortlist spreads fitting work across origins using distance weighted
by clearance, capped at four metres. Up to 128 primary fits and 32 grid fallback fits form a bounded
candidate list. On closed BSP maps, geometry-only hardware queries assess six 8x8 directional faces per
candidate before lighting is traced. They reuse the captured scene and material-aware traversal, report
first-hit distance and explicit backface/miss flags, and skip lighting and medium classification. A miss
does not count as a supporting wall merely because its distance reaches the trace cap.

Selection scores newly covered proxy volume by measured geometric agreement, subtracting the union of
earlier boxes as disjoint slabs. Agreement measures the solid angle with a hit no farther than the proxy
exit plus a 5% relative margin; nearer interior props remain allowed. A small positive floor preserves
fallback allocation, and equivalent scores retain input/authored preference. Thin ledges and overlapping
fits cannot consume slots just because they appeared first. Only selected captures receive the configured
lighting samples. Geometry checks have a separate adaptive work budget and progress status. Without a
closed partition, the existing coarse grid fallback remains available without this assessment.

This remains a sampled, greedy policy, not optimal room or receiver coverage. It does not grow bounds
through walls, fix a doorway's low projection box, or guarantee capture visibility at every receiver.
Capture origins always require strict empty-space clearance. Optional `REMIX_DEBUG_PROBE_CANDIDATES`
names a fresh JSON report of all fits, geometry agreement, miss/backface fractions and selected origins.

The default fit tolerates small solid details rather than shrinking an entire face around a ceiling bevel.
Touching solid fragments are merged when their union nearly fills its bounding box, reassembling
rectangular wall subdivisions. L-shaped trim keeps its separate bounds, so a ring of ceiling bevels does
not become one room-sized solid box. A component must be thin in two axes (bounded by both the configured depth and a
quarter of each box extent), or inset in two axes and no wider than half those extents. The conservative
component volumes together must occupy at most 8% of the box. This allows small interior props and
narrow columns while rejecting room-spanning sheets regardless of thickness. Fitted boxes are slightly
inset; surface lookup biases into the lit side. Non-BSP props do not constrain this structural fit, but do
appear in the captured radiance when included in the static bake scene.

Overlapping captures also use a preference derived from their recorded first-hit distances. The baker
measures the solid-angle fraction whose geometry lies no farther than the projection box, with a 5%
relative margin for the inset and sampling error. A doorway's low corridor-like fit can therefore have
less influence than a capture that better describes the surrounding room. Nearer interior props do not
lower this preference; unknown distances remain neutral. Preference has a positive floor and participates
in normalized blending, so a sole capture retains its radiance. The scalar occupies existing probe
metadata and adds no texture reads or images. Valid disk caches refresh it from retained raw captures
without retracing or changing their filtered lighting. This is an overlap heuristic, not a repair of the
projection geometry: open portals, distance-cap misses and non-box rooms still need better proxies.

`rtx.native.forward.reflectionDetailTolerance` controls trim depth in metres (default 0.5, range 0-2).
Zero restores strict empty-space fitting. The tolerance is an approximation, not a proof that receivers
share one room: complex connected trim can be over-rejected, and small or irregularly fragmented dividers can be misclassified. Oblique
planes are bounded conservatively. Rotated/concave rooms, doorways, authored volumes and capture
relocation still need refinement and real-map evaluation. Without a closed partition, placement uses
coarse overlapping boxes. PVS visibility is never evidence that a box is empty.

A cancellable worker builds GGX-filtered roughness mips, preserving linear HDR radiance. CPU and GPU
lookups remap bilinear taps across face boundaries and interpolate between roughness mips. The packed
storage-buffer layout avoids cube-array and descriptor-indexing requirements. Face orientation follows
the [Vulkan cube-map selection convention](https://github.khronos.org/Vulkan-Site/spec/latest/chapters/textures.html).
Parallax proxy bounds and receiver influence bounds are separate fields in the private CPU/GPU layout.
Placement initially uses the same fitted box for both; future authored volumes can tune them independently.
For each eligible receiver, the reflected ray intersects the proxy box; the vector from capture origin to
that hit supplies the corrected lookup direction. Receivers beyond a proxy face use the original ray
when its box exit is behind them, avoiding a reversed lookup. Influence bounds determine overlap weights.
Overlapping volumes blend before the single specular weight. Their weights combine the box-edge
fade with inverse squared distance to the capture. A finite core, one tenth of the shortest influence
half-extent, prevents a singularity at the capture origin. Capture distance keeps spatial preference
on shared floors and walls, where every touching box has the same minimum edge fade. Normalized
weights preserve HDR brightness and are independent of the world's coordinate units.

Irradiance and reflection assets publish together only for their matching input/settings generation.
Map replacement, cancellation, disable and in-flight descriptor ownership follow the same controller.
Probes remain available with SSR disabled. Setting `reflectionProbes` to zero disables capture; setting
both that value and `ssr` to zero avoids reflection guides and screen-resolution reflection targets.
These are low-resolution static approximations, not per-frame ray tracing or a production lighting preset.

## Water integration status

Measured water and glass share CPU optical conversion in `scene/optics.h` and GPU interface
math in `forward/shaders/dielectric.glsl`. Tint describes RGB transmission at the material's
measurement distance, in scene units; extinction is per scene unit. Converting both distances
to metres leaves transmission unchanged. Index of refraction supplies interface reflectance:
water is about two percent at normal incidence rather than the opaque BRDF's four percent.
The baker still selects an explicitly marked smooth mean interface. Live ripple normals are
a separate surface-shading input and must not alter volume absorption or the frozen boundary.

The optional late water pass accepts measured transmission bodies with valid IOR and constant
measurement colour/distance, ordinary depth testing/writes and opaque or source-over coverage.
Thin sheets, unusual blend/depth state, decals and viewmodels retain compatibility rendering.
Eligibility reads the resolved replacement material, as do baking and optional ray transport.
Water geometry does not enter opaque shadow-caster lists or surface-lightmap charts.

`water::Snapshot` supplies independent sampled inputs for the late pass. It stores RGBA16F
scene-linear HDR and R32F nearest opaque depth, about twelve bytes per pixel before allocation
alignment. It resolves depth by taking the minimum MSAA sample and preserves premultiplied HDR
coverage. Optional opaque reflections are added with that coverage before water samples the colour;
their provider may use GENERAL or SHADER_READ_ONLY layout. Outputs use SHADER_READ_ONLY layout.
This component does not modify the original opaque attachments or submit/wait for GPU work.
The caller must complete the selected descriptor slot's fence before recording, retain inputs until
completion, and retire all uses before resizing or destruction. Shared output images require the
same ordered queue. Direct output-view feedback is rejected.

The pass reuses the original MSAA attachments and owns its render pass, pipelines and fenced
descriptor slots. An additive overlay puts completed opaque specular into those attachments before
water shading; final composition skips its usual specular addition in that frame. Refraction reads
only the snapshots. Missing bent rays can use a known straight-through opaque layer behind the
surface; unknown background layers remain misses. Total internal reflection skips refraction queries.
Probe misses use sky radiance gated by baked visibility. Rough sky prefiltering and multiple
transparent layers remain future work.

Water SSR borrows the completed opaque depth hierarchy, nearest-sample normal guides and mipmapped
opaque HDR colour. Compute writes become visible to the late fragment pass before it samples them;
the borrowed targets stay alive through the frame fence. No additional reflection image set is allocated.
The shared tracer rejects back-facing hits, uses roughness-dependent colour footprints adapted from
s&box, and returns hit radiance weighted by screen confidence. Remaining confidence uses the filtered
parallax cubemap or visibility-gated sky. The water interface applies its IOR Fresnel once to the result.
The sampled opaque colour excludes its own reflection composition, avoiding recursive feedback.

The ordinary `ssr`, `ssrSteps`, `ssrDistance`, `ssrThickness` and `ssrRoughness` settings also control
water tracing. It runs at water-fragment resolution; `ssrScale` controls the opaque reflection dispatch
and does not reduce water fragment tracing.
GPU tests cover a current opaque emitter, removal, cutoff edits and opaque specular accounting at
1x/2x/4x MSAA and each opaque SSR scale. Resolved water roughness is also tested with a narrow emitter
to verify highlight broadening and reduced peak intensity rather than only rejecting rough surfaces.

With `rtReflections = 1`, supported hardware can continue missing water reflection confidence directly
from the water fragment. This uses the live scene's material-aware radiance transport, including
alpha coverage, current analytic lighting, emission and measured smooth dielectric boundaries. It
shares geometry, textures and acceleration structures with opaque reflections and live GI; no extra
guide render pass or full-screen reflection images are allocated. `rtReflectionDistance` and
`rtReflectionBounces` control this continuation. Diffuse GI and water reflection constants occupy
independent fenced descriptor sets, so their ranges and bounce budgets can differ in one submission.

A valid ray hit replaces remaining probe/sky confidence, including a black result. Misses and invalid
back faces preserve that fallback. SSR, hardware continuation and the fallback share one reflection
lobe, with the water interface's Fresnel applied once. Like the existing opaque ray fallback, this is
bounded sharp-direction transport below `ssrRoughness`, without a stochastic glossy-ray distribution
or temporal denoising. Rough SSR hits still use the shared filtered colour footprint.

The ray shader and fourth descriptor set are created only when ray features are enabled and the device
can fit the combined fragment resources. Water falls back to SSR/probes on an ordinary device or an
insufficient descriptor limit. The base water pipeline retains its Vulkan 1.1 feature requirements.
GPU tests cover hidden and moving emitters, removal, alpha rejection, live range/cutoff edits, shared
SSR/RT energy, valid black hits, probe fallback and simultaneous GI at 1x/2x/4x MSAA. Padded inactive
opaque reflection rays return before the containing-medium lookup, avoiding its six-axis queries.

The shared camera-medium API attenuates opaque geometry and visible water exits before tone mapping,
using the nearest depth and measured distance in scene units. The exit sheet skips duplicate body
absorption. Fog illumination currently assumes Source's world Z-up axis. Tests cover HDR transmission,
foreground occlusion, partial water MSAA coverage, repeated frame slots, underwater opaque geometry,
water exits and disabled-pass compatibility at 1x, 2x and 4x with ray-tracing features disabled.

## Settings

| Key under `rtx.native.forward.` | Default | Meaning |
| --- | --- | --- |
| `samples` | 4 | Requested MSAA count: 1, 2 or 4 |
| `shadows` | 1 | Enable sun shadow rendering |
| `shadowResolution` | 1024 | Power-of-two size per cascade, 64 to 4096 |
| `shadowBias` | 0.002 | Sun shadow comparison bias in metres; converted per cascade so distant skybox bounds do not erase small shadows |
| `shadowDistance` | 100 | Maximum sun-shadow distance in metres |
| `sceneScale` | 1 | Metres per world unit |
| `ambient` | 0.025 | Constant diffuse ambient fill |
| `exposure` | 1 | Display exposure multiplier |
| `water` | 1 | Late measured-transmission pass and camera-medium composition; zero selects compatibility water |
| `waterDistance` | 50 | Maximum screen refraction distance in metres |
| `waterThickness` | 0.15 | Screen refraction hit tolerance in metres |
| `lightCutoff` | 0.001 | Contribution threshold used to bound local-light influence |
| `ssr` | 1 | Enable current-frame screen-space reflections |
| `reflectionProbes` | 16 | Maximum baked box-projected cubemaps, 0 to 32 |
| `reflectionResolution` | 32 | Power-of-two cube face size, 8 to 128 |
| `lightmapPageSize` | 1024 | Maximum power-of-two surface atlas page size, 64 to 4096 |
| `lightmapTexels` | 4194304 | Surface atlas texel budget, including padding and unused page space |
| `lightmapDenoise` | 1 | Offline OIDN RTLightmap filtering of indirect surface SH where available; raw fallback without OIDN |
| `ssrScale` | 2 | Reflection resolution divisor: 1, 2 or 4 |
| `ssrSteps` | 128 | Maximum depth traversal iterations per reflection ray, 16 to 512 |
| `ssrDistance` | 50 | Maximum reflection ray distance in metres |
| `ssrThickness` | 0.15 | Depth hit tolerance in metres |
| `ssrRoughness` | 0.6 | Roughness cutoff for screen reflections |

Lighting, exposure and SSR trace-parameter edits update per-frame data without recreating render targets.
MSAA, water enablement, shadow-map resolution, SSR enablement or SSR resolution edits prepare replacement targets in the
background.

The Rendering panel's Lighting view and `rtx.native.forward.debugView` isolate opaque-world components:
0 is normal shaded output, 1 is neutral baked indirect irradiance (or probe/ambient fallback), 2 is
material-weighted direct diffuse, and 3 is direct specular. Diagnostic views exclude emission and
reflection composition; water, compatibility effects and UI retain their ordinary paths. Switching
views prepares a specialized shader while retaining the active bake; normal shading compiles out
diagnostic branches. Exposure/display mapping still applies. These views locate an artifact's lighting
component; they do not diagnose its sampling cause on their own.

For a fresh diagnostic bake, `REMIX_DEBUG_MASK_COSTS=1` together with
`REMIX_DEBUG_BAKE_CAPTURE_PREFIX` writes `.overflow-mask-costs.bin`. It measures every partial
overflow candidate before admission, including rejected masks: R8 texel populations, tile cardinality,
payload-size histogram, lossless cost, actual bounded-palette cost/error and estimated uniform 2/4-bit
costs and errors. Uniform quantization estimates do not enable those rendering formats. The audit adds CPU work only when
requested, retains aggregate counters rather than every mask, and does not change the memory budget
or allocation order. A cache hit has no candidate stream to measure; use a separate empty cache.

Backend values are stable: raster 0, pathtrace 1, forwardplus 2. Native capability bits use the matching
bit positions. Forward+ has independent preparation and settings; selecting it never implicitly selects
path tracing. Rendering options apply at a preparation boundary, and the previous renderer remains usable
while a changed forward target is prepared.

## Offline lightmap denoising

Configure Meson with `-Doidn_root=<OpenImageDenoise installation prefix>` to enable the separate
`lightmap/denoise` module. New settings enable `rtx.native.forward.lightmapDenoise = 1`;
an explicitly saved zero still selects raw transport. The Rendering panel exposes the setting.
The integration uses Open Image Denoise 2.5 APIs; the initial Windows dependency is the official
[OIDN 2.5.1 release](https://github.com/RenderKit/oidn/releases/tag/v2.5.1).
Builds without that dependency retain raw transport, including with the default setting enabled.
Availability/version participates in the cache key, so a raw unsupported-build cache cannot stand in
for denoised output in an OIDN-enabled build. OIDN-enabled
builds need its runtime library, core and selected device plugin available to the loader, together
with that plugin's dependencies. The SDK's license and third-party notices must accompany any
redistribution; `oidn_root` does not automatically package those files.
On Windows, `scripts/windows/package-runtime.py` bundles the configured SDK's DLLs
and notices alongside USD and the renderer; see [runtime packaging](../platforms.md#build-on-windows).

For bake-cache investigations, `REMIX_DEBUG_BAKE_INPUT_PREFIX` writes a JSON report
per frozen input hash. It records per-draw geometry/material/chart fingerprints
and cumulative checkpoints for the scene header, BSP partition, reflection hints,
members, sun and dome. An RGBA8 dome also writes its mip-zero pixels to a sibling
`.dome.bin` file. Use a fresh report prefix for each process; these files diagnose
input differences and do not change cache identity.

Filtering runs on the existing preparation worker after transport readback and before half-float
atlas encoding and persistent cache storage. OIDN chooses its default supported device, including
GPU backends where available. This is an offline stage with explicit transfers from the existing
CPU readback; it does not share live Vulkan atlas memory. Cancellation is checked between chart batches
and band submissions. CPU filters and GPU images of at least one megapixel also use OIDN's internal
progress callback for in-flight cancellation. Small GPU fields omit that callback because it adds
significant synchronization overhead; cancellation drains any queued bounded batch before cleanup.
Errors prevent publication.

Each chart is processed independently with its existing chart-local dilation donors. Packed atlas
neighbours cannot influence one another. L0 uses HDR RTLightmap filtering; the eight signed SH bands
are divided by twice their original L0, filtered in directional mode and reconstructed using the
filtered L0. Zero-energy inputs use zero normalized direction. A fixed input scale avoids automatic
exposure changing from chart to chart. Constant RGB fields bypass filtering to preserve their value
and avoid unnecessary dispatches. All SH alpha channels, transport moments, probes, per-light
direct SH and local/sun visibility remain unchanged. The filter cannot replace actual emitter
radius or angular-size sampling, and does not increase world-space luxel density.

The module holds original and filtered L0 plus bounded RGB coefficient packets for pending charts,
plus OIDN's reusable image buffer and internal working memory. Its 256 MiB `maxMemoryMB` request is a filter-memory target,
not a hard cap on total host/device allocation. Evaluating nine coefficient filters for many
small charts can still add substantial overhead. Chart boundaries are isolated rather than jointly
denoised across connected faces; seam quality and sample-count savings remain map-level visual
validation requirements. Denoising mode and compiled OIDN version participate in cache identity.

Each SH band retains a committed filter and a separate image binding. Charts are grouped by image
dimensions, and a filter is rebound only when dimensions, offset or the shared image buffer change.
With OIDN 2.5, each image explicitly matches the network's 16-pixel padding, so nearby chart sizes can
share committed bindings. Padding is zero in HDR data and minus one in signed directional data;
both become zero after the respective network input transforms. Only the original chart pixels are
reconstructed, and constant-field detection ignores the synthetic padding. Unknown OIDN versions
retain native chart dimensions. Padding is temporary denoiser scratch and consumes no atlas space.
Up to nine bands are packed into one packet. Equal-shaped charts with all nine bands in a packet can
queue ordered asynchronous uploads, filter executions and readbacks on one OIDN device. Their host
arrays stay alive until `oidnSyncDevice` completes. Queues flush on a shape change, before changing a
filter binding, at 32 charts, or at an 8 MiB host-image scratch target. One oversized chart remains
valid. Large charts retain the synchronous bounded-band path. All queued work is also drained before
cleanup on cancellation or error. This reuse does not join chart images or let one chart influence another.

Coefficient batches target at most 8 MiB of image data. Larger charts use fewer bands per batch;
a single field larger than that target still requires one image. This bounds the image-buffer
reservation by the larger of 8 MiB and the largest single field, without allocating a second atlas.
Each chart also retains original and filtered L0. Those arrays count toward the pending-chart scratch
target; an oversized single chart can exceed it. Diagnostics report actual executions, preparations,
upload/readback packets, device waits,
constant-field bypasses, internally monitored executions, peak host-image bytes, reserved image-buffer
bytes and transfer bytes; OIDN's internal allocations are additional. A separate host-time line
measures device/filter initialization, changed filter preparation, asynchronous upload, filter
execution submission, readback, explicit synchronization and chart encoding/reconstruction. These are disjoint wall intervals, not GPU
timestamps: asynchronous submission can itself block in OIDN or the driver, while work can overlap
chart processing. The remaining total includes ordering, constant-field scans and bookkeeping.
Bake status reports completed/total denoised charts
through worker-owned atomic counters so a long offline stage remains visible without blocking frames.
After denoising, status advances through atlas encoding, geometry preparation and cache saving rather
than continuing to display completed denoising. Reflection filtering also has its own status.

## Optional hardware reflections

`rtx.native.forward.rtReflections = 1` enables hardware continuation of unresolved SSR rays. It is off by
default and requires only the minimal ray-query features used by the bake, independently of full path
tracing. If those features are unavailable, Forward+ keeps rendering with SSR/probes and the UI reports
the fallback. Enable/disable changes prepare replacement renderer resources through the existing worker.

`ray/capture` combines complete authored geometry with current submitted opaque actors. Matching current
retained records override their authored counterpart rather than duplicating old transforms. Posed
vertices follow current bone transforms; sprite-sheet UV offsets and material/texture revisions affect
the capture key. Analytic lights merge by stable identity, with current values overriding authored ones.
Unsubmitted dynamic actors outside the authored snapshot are unavailable to this renderer. Blended,
refractive, viewmodel and unsupported compatibility geometry stays outside the ray scene.

`ray/live_scene` owns the live transport cache and each frame's query owner. The reflection pass consumes it. Complete unchanged packed
geometry/material inputs reuse their acceleration structures and atlas; changed geometry, poses, material
animation or texture revisions prepare a replacement. Analytic lights, sunlight, environment radiance/
transform and ray distance stream into fence-safe frame-slot buffers without rebuilding acceleration.
Old scene owners survive through querying fences.
Build inputs and uploads retire after their preparation fence. Shader pipelines warm during renderer
preparation. Geometry uploads, texture capture and acceleration builds record on the renderer queue.
There are no hidden submissions, CPU pixel readbacks or per-frame queue-idle waits.

The screen launcher reads the same resolved MSAA guides used by SSR. It traces a reflected direction
from each eligible receiver, skipping fully confident SSR hits, background and roughness-cutoff pixels.
A GPU-written point batch feeds the existing material-aware radiance transport. Rejected alpha and
sidedness candidates continue traversal; emissive hits, analytic-light visibility and diffuse secondary
bounces reuse the hardware bake implementation. This initial hit shader is diffuse transport: secondary
metallic/glossy lobes, secondary normal/roughness maps, glossy ray distributions and temporal denoising
are not implemented. The primary receiver still uses its mapped normal, metallic Fresnel and roughness
filter. The feature is therefore an experimental reflection continuation, not a complete path tracer.

A successful first hit fills only the remaining SSR confidence. A miss leaves that portion for the baked
cubemap. The common geometry-aware reconstruction then adds probe fallback and applies Fresnel once.
The composition is `SSR.rgb + RT.rgb * (1 - SSR.a) + probe.rgb * (1 - combinedConfidence)`.
Ray range misses also use probe fallback, avoiding an authoritative black result from a finite trace.
All inputs and radiance are current-frame data; changing or removing an actor leaves no history trail.

Ray work uses 16,384-point GPU batches, with inactive padded tails and compute dependencies between
launch, trace and merge. Batches share fixed slot constants; push constants provide each pixel offset.
Live transports set `deviceLocalBatches`: points written by the screen launcher and results consumed by
the merge stay in device-local memory. This is independent of the CPU baker's mapped upload and cached
readback buffers. GPU-only transports reject accidental CPU batch/read calls; explicit GPU copies are
available for diagnostics. Slot ownership, batch capacity and dependencies are unchanged.
The UI reports candidate ray slots and scene rebuilds, not an instrumented count of actual BVH queries.
Full/half/quarter resolution and roughness cutoff currently share the SSR quality controls.

Controls: `rtx.native.forward.rtReflectionDistance` (metres, default 50), `rtReflectionBounces` (1..8,
default 2), and `rtTextureResolution` (16..1024 power of two, default 256). Ray range and bounce edits
apply live. Texture capture uses a bounded 256 MiB atlas and may reduce resolution to fit. GPU point and
result buffers use about 9 MiB per live scene before alignment (two frame slots), plus shading,
texture and acceleration data. Retained older generations temporarily add to that memory.

`REMIX_DEBUG_REFLECTION_BATCH_TIMINGS=1` adds launch/transport/merge GPU scopes for the first 32 batches
per frame. The bound leaves timestamp space for other passes; larger targets report a diagnostic sample
of their batches. Leave this disabled for final performance measurements.

This initial cache repacks/hashes the current transport inputs each frame and rebuilds a monolithic
world-space acceleration scene on geometry/material capture changes. Lighting-only and ray-range edits
reuse it. CPU packing, render-thread allocation and rebuild cost need real-map profiling; separate
static/dynamic BLAS, refit, independent material updates and background resource preparation remain
performance work. Default
Forward+ with hardware reflections off keeps its existing inexpensive raster/baked path.

## Optional live diffuse GI

`rtx.native.forward.rtGI = 1` enables current-scene diffuse ray transport directly in the opaque material
shader. It is off by default. This variant replaces the surface-lightmap/probe/ambient indirect term;
it never adds another indirect term on top. A valid black result stays black. Direct analytic diffuse,
direct specular, per-light shadow masks, live shadow maps, emission and reflection composition remain
separate. Disabling GI or failing its capability/preparation checks uses the ordinary baked pipeline.

GI and RT reflections share one `ray/live_scene` and its acceleration/material/texture cache. GI-only
transport allocates tiny point/result buffers because fragment queries need no intermediate ray batch.
Optional fragment scene descriptors share immutable world data and the slot's current light buffer,
while dedicated fragment constants keep GI range, samples and bounces independent of the later compute
reflection dispatches. Upload, atlas and acceleration-build dependencies include fragment consumers.
All owners remain alive through querying frame fences, including obsolete generations.

The shader samples a cosine-weighted hemisphere around the receiver's mapped normal, with geometric
normal origin bias. Averaged radiance estimates E/Pi, matching the baked diffuse convention before
receiver albedo and metallic weighting. Rays see complete authored geometry plus current submitted
opaque actors, including offscreen emitters/occluders, skinning, animated materials and updated textures.
Ray misses sample the captured/current environment. Current analytic lighting is sampled at secondary
hits, giving indirect light; the primary direct-light calculation is unchanged. Shared material-aware
alpha/sidedness traversal and diffuse secondary shading come from `bake/shaders/radiance.glsl`.

The public material/mesh ABI and the baseline private forward bindings are unchanged. GI uses a third
private descriptor set and optional shader modules targeting Vulkan1.2. It requires ray-query support
plus at least eight fragment-stage SSBO descriptors and ten storage buffers across the pipeline layout.
If unavailable, the UI reports baked-lighting fallback. MSAA remains the primary geometry antialiasing;
no TAA or sample-rate-shading feature is introduced.

Controls: `rtx.native.forward.rtGISamples` (1..64, default16), `rtGIBounces` (1..8, default2), and
`rtGIDistance` (metres, default50). Enable changes prepare replacement pipelines; samples, bounces and
range update per completed slot without rebuilding geometry. `rtTextureResolution` controls the shared
live texture atlas. Changing or removing actors affects current shading without a retained GI history.

This first live GI implementation traces at shaded fragment resolution. Its deterministic stratified
samples have no temporal accumulation or spatial denoiser, so complex or small emitters can be noisy and
higher budgets can be expensive. Secondary metallic/glossy lobes and secondary normal/roughness maps
remain outside the shared diffuse transport. Reduced-resolution reconstruction, denoising and static/
dynamic acceleration optimizations remain performance/quality work; real GMod maps are not profiled.

## Hardware transport foundation

`forward::RayScene` owns a world-space triangle BLAS and a TLAS. It copies caller inputs, allocates
device-local acceleration/scratch storage, and records builds with query/build dependencies on the
caller's queue. Build inputs and scratch can retire after their fence; subsequent ray queries retain
only the acceleration structures. Empty scenes are supported. The querying shader decides sidedness
and confirms non-opaque candidates, allowing future alpha-tested transport to reject holes correctly.

This optional service requires Vulkan 1.2, acceleration structures, ray queries and buffer device
addresses, plus their KHR extensions. Runtime enablement is separate from full path-tracing capability:
it does not require shaderInt64, descriptor indexing or the tracer's large texture/G-buffer limits.
The baseline forward renderer remains usable without these features. Optional live reflection transport uses this service when requested.

## Diffuse bake transport

`bake::Transport` packs the frozen static scene into device-local shading buffers and a hardware ray scene.
It captures albedo, emission/mask, metallic and environment textures from the shared GPU provider into a
linear HDR texture array, avoiding descriptor indexing and preserving compressed-texture uploads already
owned by that provider. Texture resolution reduces when needed to fit the atlas allocation budget; the
actual resolution is queryable. Per-layer storage views isolate capture writes. Point/linear filtering,
repeat/mirror/clamp/border modes, texture stages, alpha tests and self-illumination use the same material
rules as forward shading.

The compute integrator traces diffuse paths through textured alpha coverage. It evaluates sun, shaped
sphere and triangle lights with hardware visibility, emissive ray hits and environment radiance. It samples
one explicit local light per bounce with its selection weight. Static snapshots reuse the scene
light hierarchy to propose nearby, high-power emitters in logarithmic work. One eighth of the
proposal remains uniform over captured lights; the combined PDF is used for both proposal branches,
so lights disfavoured by the bounds or hemisphere estimate retain nonzero probability. Leaf records
keep the original captured light IDs used by explicit visibility and mixed shadows. This is a
variance proposal, not an occlusion test: sampled emitters still use the same hardware visibility.
The hierarchy is compiled only into offline compute transport; streamed live-light queries retain
their uniform proposal. Bake SPIR-V is optimized and inlined by the shader generator before
embedding, while retaining its existing word-array loading contract. Baked radiance excludes analytic direct
lighting at the receiver, which the forward pass already evaluates. Environment visibility and emissive
geometry remain bake sources. Self-illumination emits its masked colour and reduces reflected diffuse
energy; suppressing generated emissive lights does not remove visible emission on ray hits.

Each point produces nine real spherical-harmonic coefficients convolved for cosine irradiance, six
directional distance-moment records, and coverage/sample diagnostics. Geometry/interpolated normals drive
diffuse transport; normal-map microdetail and glossy transport are outside this kernel. Small emissive
surfaces and bright environment texels currently rely on continuation sampling, so convergence still
needs map-level assessment. Ordinary source-over alpha blending is treated as thin stochastic coverage in
transport. Texture/vertex/factor alpha uses the same material operation as the
raster path; each ray candidate receives an independent coverage decision, so
stacked sheets multiply transmittance. Alpha tests run first. Each ray advances
the path's random stream once, even on a miss. Candidate decisions derive from
that ray's seed and primitive identity, so unspecified GPU traversal order cannot
advance or reorder the path's random stream. Opaque/cutout candidates consume no
additional random samples. This also applies to reflection
captures and live ray transport; primary MSAA shading remains unchanged.
Static modulating decals use a separate surface-colour policy. They never block visibility or
become diffuse bounce surfaces. At a real surface hit, a short hardware query along its geometric
normal samples aligned decal layers within 5 mm on the outward side. Their texture/vertex/tint colour
multiplies the receiver's diffuse and emissive response using the source blend's `2 * colour` factor;
diffuse reflectance remains bounded by one. Multiple attached layers multiply. This local attachment
test excludes remote sheets, but is not an authored receiver binding: very close parallel surfaces and
coincident overlapping geometry still require map-level review. Scenes without these layers skip the
extra query. The policy is shared by bake, capture and live ray transport; it leaves live raster
compositing unchanged. Additive blending, modulation without decal identity, and refractive materials
without an explicit bake boundary policy remain unsupported instead of becoming solid blockers. Static Source props can retain
alpha-blended antenna sheets, and retained terrain paint/overlays carry explicit
static identities.

The optional version-4 `remixapi_BakeSceneReflectionsInfoEXT` adds ordered
capture-site hints independently of irradiance-grid spacing. The snapshot owns
up to 4096 positions, validates finite coordinates inside playable bounds, and
includes their ordered values in content identity. `reflectionRevision` schedules
edits without becoming content itself. Versions 1-3 remain supported and clear
prior hint membership when a client downgrades.

Source's playable `env_cubemap` positions supply these hints. They seed the
existing strict-origin/tolerant-box fitter; they cannot override walls or author
arbitrary influence bounds. Duplicate rooms are merged, and unused capacity
falls back to automatic placement. Authored order resolves otherwise equal choices under the
16-default/32-maximum probe budget.

Automatic origins now come from clipped empty BSP leaves independently of the
irradiance grid. Conservative cell bounds are a search aid: midpoint/quarter
candidates must pass whole-volume clearance against the partition. Axial segment
queries rank nearby enclosure (opposite boundaries within 64 metres) before open
areas, with a capped clearance preference and deterministic ties. This is a
priority heuristic, not a semantic room classifier. Cyclic/invalid partitions fail
closed. Worker-local occupancy scratch avoids allocating per-node state for every
candidate test; candidate/cell and fitting-work limits bound map-load effort. The primary shortlist
allows 32 to 128 expensive fits; the existing irradiance fallback checks at most sixteen candidates
per coarse region. Larger capture counts do not increase the primary fitting limit.

Before fitting, the existing hardware transport validates authored and automatic
sites with 64 spherical first-hit queries. The geometry-only mode evaluates alpha
coverage and back-face fractions without lighting. It rejects origins inside
static props, which empty BSP leaves alone cannot detect. Results are consumed
only after their frame-slot fence; no hidden submits or waits are introduced.
The fitter receives the validated list, then fills remaining capacity from
already-tested irradiance probes. A low budget can still leave particular rooms
uncovered; larger maps can use 32 captures without changing irradiance-grid density. This increases
capture work, storage and per-pixel lookup cost; it does not make arbitrary concave rooms rectangular.

For inspection, `probes.request` in the profile directory writes `probes.json`
from the published frame's immutable metadata. It lists the active content hash,
world-unit scale, resolution, origins, projection boxes and influence boxes.
The additive `contentHashes` object fingerprints the exact packed grid,
reflection and surface-atlas payloads at publication. `captureRadiance` separately
fingerprints the raw cubemap RGB samples before filtering, excluding hit distances.
`captureInputs` fingerprints the submitted CPU ray origins and directions.
These decimal strings
preserve 64-bit values in JSON consumers. Zero marks absent data; an empty
reflection header still has a fingerprint. They are noncryptographic diagnostics
for comparing results from the same build, rather than persistent cache keys or
portable numerical-equivalence checks. Hashing runs once over existing CPU
staging data; it adds no per-frame GPU readback.
Metadata is retained with the GPU volume and cleared with it; generating a report
neither reads back GPU memory nor inspects a mutable fitting job.

For developer comparisons, `REMIX_DEBUG_BAKE_CAPTURE_PREFIX` enables an optional
raw capture dump at publication. Use a fresh absolute prefix for each process;
existing files are refused. `<prefix>.capture.bin` stores the eight-byte
`RMXCUB01` magic, little-endian scene hash (64 bits), probe count (32 bits), then
each probe's resolution (32 bits), XYZ origin (three floats), and six RGBA32F
faces in the usual order. RGB is captured radiance; A is first-hit distance.
After `<prefix>.frame.request` exists, the first frame writes raw reflection and
world uniform blocks plus draw/light arrays beside it. These auxiliary layouts
are specific to the producing build. BSP replay creates the request after its
measurement window, near the final screenshot. No captured CPU arrays are kept
alive just for diagnostics, and unavailable output never invalidates the bake.

With the same prefix, `REMIX_DEBUG_BAKE_SURFACE_SAMPLES=1` also writes the completed
surface transport inputs and results. `.surface-points.bin` contains 32-byte `bake::Point`
records, `.surface-results.bin` contains matching 256-byte `bake::Result` records, and
`.surface-owners.bin` maps each atlas texel to a 32-bit point index (`UINT32_MAX` is unused).
These private native layouts allow a changed atlas coefficient to be traced back to its
world position and raw GPU result. They add no tracing or GPU readback; output is opt-in
because the arrays can be large.

The same developer switch enables one-frame GPU readbacks when the HDR/depth
formats support transfer-source images. Once requested, the reflection pass
copies centre crops of opaque HDR, resolved normals/roughness, reflectance,
base depth, raw SSR and filtered reflection radiance. Each crop is at most
512 by 512 pixels; these are not full-frame comparisons. The pass retains its
readback buffers until the owning frame slot's fence completes. It adds no
queue submission or manual wait. BSP replay drains eight frames in this mode
so the slot can complete before shutdown; its ordinary drain remains three.
For comparisons that must include complete SSR trace inputs, additionally set
`REMIX_DEBUG_FULL_REFLECTION_INPUTS=1`. This replaces the centre crops with full
images and saves every reduced depth and colour level as `depth-mip<N>.image.bin`
and `color-mip<N>.image.bin`. The
reported dimensions describe that mip. The full option requires the capture
prefix and remains a one-frame capture, but uses substantially more memory and
disk space (about 332 MiB for the default 4K half-resolution SSR targets).

`<prefix>.<stage>.image.bin` begins with `RMXIMG01`, then seven little-endian
32-bit integers: Vulkan format, full width/height, crop X/Y and crop width/height.
Tightly packed pixels follow: RGBA16F for colour/guides and R32F for depth.
`<prefix>.probes-published.bin` holds the exact serialized CPU probe payload;
`<prefix>.probes.buffer.bin` holds the corresponding GPU descriptor range,
including its header and texels. The irradiance payload likewise has paired
`.irradiance-published.bin` and `.irradiance.buffer.bin` dumps. GPU uniform ranges
are saved as `.reflection-frame.buffer.bin` and `.sky-frame.buffer.bin` alongside
their CPU `.bin` counterparts. Compare only the declared uniform structure:
256 bytes for reflection constants and 160 for sky constants; an allocation's
descriptor range can include unused trailing padding. These permit a direct
upload comparison rather than inferring GPU contents from a CPU hash.
Transfer-source usage and GPU readback allocations are added only with the
developer switch enabled. Use fresh prefixes and keep diagnostic runs separate
from performance measurements.

`remixapi_MaterialBakeBoundaryEXT` explicitly opts a measured translucent body
into a smooth, constant optical boundary for baking. Capture excludes only its
normal texture/scrolling; base/emission animation, mutable optical maps and
moving geometry are still refused. Constant index, measurement color and
measurement distance belong to the snapshot and its content hash. Live normal
ripples keep their original textures and scroll rates. Source BSP water enables
this policy; its original material uses the late water pass when enabled and eligible.
USD transmission replacements retain their authored measurement distance and
color. Reusing the source albedo preserves its sampling and alpha state without
overwriting those optical coefficients. A valid replacement volume inherits the
source's explicit bake boundary policy, including when it supplies a new texture;
a thin-walled replacement does not inherit a volume boundary.
Hosts may provide up to 32 additional matching keys through
`remixapi_MaterialReplacementKeysEXT`. Matching prefers the material's own hash,
its single source hash, the ordered alternate keys, then its albedo texture hash.
The runtime copies and normalizes the list. An authored higher-priority match
keeps the original visible while loading; a lower-priority decoded material cannot
temporarily win because its textures happened to finish first. These keys are
matching metadata and do not alter an otherwise identical bake's transport hash.

The shared `bake/shaders/optics.glsl` implements dielectric Fresnel, Snell
refraction, total internal reflection and Beer-Lambert extinction per world
unit. Boundary events have a separate 32-step work bound and do not consume the
diffuse bounce budget. Radiance carries the index-squared transport factor.
Initial medium lookup uses first-hit exits along six axes once per shading
point; opaque geometry blocks those tests so an open pool does not extend below
its floor. This is a bounded approximation for disjoint bodies, not a general
nested-volume classifier. Enclosed underwater pockets without a visible exit
along those axes need explicit volume membership in a future extension.

Shadow connections use straight paths with colored extinction and Fresnel loss;
refractive caustic focusing is omitted. Existing scalar mixed-light channels use
the luminance of that transmission, while indirect/radiance paths retain RGB.
The ordinary depth-shadow maps do not gain colored dielectric shadowing through
this change. Primary ripple refraction uses the optional late Forward+ water pass.
Optional live ray transport shares the
same smooth optical policy without changing primary MSAA.

The render-thread controller schedules CPU packing and pipeline creation through the existing preparation
service. It traces bounded batches across frames after complete map inputs arrive. Slot buffers are
independent; a work-product limit bounds each dispatch. Static upload staging and acceleration build
inputs retire after the preparation fence. The bake module performs no queue waits. Ordinary rendering
continues while the bake prepares and traces.

The grid uses metre-based spacing and coarsens to its probe budget. When requested
density does not fit, placement brackets a fitting spacing and searches between
the bounds instead of jumping directly to the next power of two. Cell counts use
the rounded float spacing actually published to the GPU, retaining interpolation
borders and the same allocation cap. Already fitting requested spacing is retained.
The changed locations participate through the CPU cache algorithm revision, so
older caches are preserved and a new bake is required for this layout.
Version-2 scene descriptors may supply
playable bounds independently of transport geometry. A closed BSP partition excludes solid/outside leaves
from tracing. Backface coverage rejects unusable probes. The forward fragment shader reconstructs nine
SH coefficients from eight neighbours with trilinear, normal-facing and distance-moment visibility weights.
This replaces diffuse ambient fill where coverage is usable; direct lighting remains separate. Probe
relocation and adaptive placement are not implemented, so thin walls and poorly sampled spaces still need
map-level evaluation.

Only a completed bake matching current input content and settings is uploaded and activated. Submitted
frames retain their previous jobs/volumes until their fences complete. Map changes, unload and disabling
baking invalidate future use immediately. Preparation failures keep direct rendering available and expose
their reason in the Rendering tab and `remixapi_GetBakeStatisticsEXT`. The runtime retains one
map-bake controller across target recreation, including MSAA/SSR edits and swapchain resize.
Only the active renderer advances it; a preparation worker may retain its owner but does not
read or modify its mutable state. Replacement targets therefore reuse a matching published
volume and can continue an unfinished bake through the existing fenced frame slots. Scene,
bake-setting and light-cutoff changes still invalidate it before subsequent shading. Map
unload clears the volume. Leaving Forward+ releases this retained session, and device teardown
releases it before the pipeline service and Vulkan device. Swapchain rebuilding can
still temporarily use the compatibility renderer while replacement targets prepare; the
retained bake is available on the replacement Forward+ target's first rendered frame.
Finished lighting is also cached on disk; a matching reload skips transport and surface sampling.

```ini
rtx.native.forward.bake = 1
rtx.native.forward.bakeCache = 1
rtx.native.forward.bakeSpacing = 2
rtx.native.forward.bakeProbes = 32768
rtx.native.forward.bakeSamples = 256
rtx.native.forward.bakeBounces = 3
rtx.native.forward.bakeBatch = 64
rtx.native.forward.bakeGpuBudget = 8
rtx.native.forward.bakeTextureResolution = 256
```

Baking requires acceleration structures, ray queries and buffer device addresses, independently of the
path-tracing backend. The basic Forward+ renderer remains available without them. Batches bound ray work,
not elapsed GPU time; these defaults are not a measured GMod performance preset.

### Bake throughput

`bakeGpuBudget` targets GPU milliseconds per bake batch (default 8, range 0..16). Higher values favor
first-bake throughput and can reduce rendering frame rate during the bake. With a usable queue
timestamp clock, the controller starts from `bakeBatch`, subject to occupancy/work limits, and adjusts
the point count using completed dispatch timings. Grid irradiance, surface irradiance, local visibility,
sunlight and probe-site validation learn
separate costs. Growth is limited to doubling, slow batches reduce work immediately, and small queue
tails do not distort the estimate. Sample counts and bounce quality stay fixed. There is still one
bake dispatch per submitted render frame; no extra queue submission or CPU wait is introduced.

The target is approximate: scene complexity and several submitted frame slots can cause an overrun.
Automatic work keeps a 64-point occupancy floor, subject to the remaining points and hard work limit;
this avoids collapsing throughput when a nearly empty GPU dispatch still has appreciable latency.
Timed dispatches remain bounded to 65535 points and 16777216 point/sample/bounce work units. A setting
of zero uses the configured fixed batch size and the previous 1048576-work ceiling; missing timestamp
support also uses that fallback. Transport receives the same hard ceiling as the scheduler.
Reflection captures retain their existing 1024-point batching to preserve their sampling sequence.
`REMIX_DEBUG_BAKE_TIMING=1` prints per-stage point/batch counts, total GPU milliseconds, largest batch
and longest dispatch after activation. These are completed queue timestamps, rather than wall time
or an estimate from frame rate. Cached jobs have no tracing timings.
Bake results prefer CPU-cached host-visible memory (with invalidation on noncoherent heaps), and the
controller reuses its CPU readback storage between completed slots. Live GPU reflection/GI consumers
use device-local point/result batches. Profiling also prints total CPU readback/validation/scatter time.
It reports the actual number of bytes read back as well.

Parallel CPU local/sun visibility uses a dedicated kernel and a 48-byte result: one visibility scalar,
nine unit direct SH coefficients and two padding floats. This replaces the 256-byte RGB irradiance/moment
transfer for those queries, without changing sample counts or quantizing lighting. The kernel omits
diffuse/radiance work and reserves about 3 KiB of shared reduction storage instead of nearly 16 KiB.
The controller consumes these compact results directly; the general transport read API reconstructs
its original full result for callers that need it. Serial transport and live GPU consumers retain the
full format. Finished lightmaps and the persistent cache keep their existing representation.

Diffuse and local/sun visibility transport use one 64-lane workgroup per point. Lanes trace disjoint
sample indices and reduce signed SH, visibility weights and distance moments in shared memory.
Containing-medium classification happens once per point. The total sample count and bounce depth
remain unchanged; sample counts below or not divisible by 64 leave only the appropriate lanes active.
Local/sun visibility reduces only its nine scalar SH coefficients and visibility weights, avoiding
the full irradiance/moment reduction for each chart/light connection.
The reduction keeps exact blocked/clear decisions separate from weighted visibility arithmetic.
Its shared arrays fit below 16 KiB. Serial radiance and probe-site queries keep their existing paths.
Parallel lanes use independent random streams, so the noise realization and floating-point reduction
order differ from the old serial bake. The sampling streams do not depend on batch size. Fixture
results reproduce across frame slots, but real-map outputs can retain small precision differences;
byte identity across process runs or with older serial lighting data is not promised.

Timestamp pairs belong to frame slots and are read without waiting after the owning fence. Host batch
buffers start at 1024 points and grow only for the completed slot, updating that slot's descriptors.
Other submitted slots keep their buffers. Timings are private to static baking; live GI and reflection
transports do not request them. Budget and batch-size edits are execution changes, so they preserve
unfinished jobs and published lighting. Sample, bounce, texture, placement and surface-quality changes
still invalidate their affected bake generation.

## Extending the backend

The versioned `remix_bake_ext.h` contract accepts explicit retained-instance membership and static light
handles. `remixapi_SetBakeSceneEXT` publishes LOADING or atomically captures READY inputs; null unloads.
The core interface table is unchanged. BSP parsing stays in the host wrapper. Camera visibility and
automatic settling do not select bake membership. Geometry and material state are copied; only immutable
texture assets are shared. Published frames retain their exact snapshot through map changes.

Repeated requests compare dependency revisions and reuse unchanged snapshots. Republish after included
resource edits; replay retained instances after material/mod replacement first. Fingerprints cover actual
geometry, transforms, material parameters, texture contents, explicit static lights, scene units and input
schema. Runtime handles, submission order and the host's scheduling revision do not defeat content reuse.
The disk cache combines this input fingerprint with bake quality, light cutoff, transport shader bytes,
GPU/driver identity, device allocation limits and a versioned CPU-algorithm revision. Batch size,
GPU timing budget, cache preference and camera movement do not change its lighting identity.

The in-process GMod wrapper publishes after its retained BSP scene is ready. It invalidates inputs at map
changes and material/mesh rebuilds, excludes sky-camera polygons, and supplies unstyled map lights plus the
map-load sky snapshot. The wrapper owns a separate copy of the first complete sky capture for baking;
later live sky animation does not restart it. A map change releases that snapshot. Time-of-day changes
are therefore not reflected in this static sky contribution. Both the wrapper's ordered engine marker
and its external map-load entry point reset the live/baked sky before loading or unloading the BSP.
Dynamic engine lights and flashlights stay out of the bake. Missing brush groups or failed/
partial static prop decodes keep the scene pending. The native capture refuses skinned/deforming inputs,
animated materials and dynamic textures. Maps with those unresolved transport inputs cannot bake yet;
direct lighting remains available. The cross-process bridge does not forward this new contract yet.

Selected chart lights also publish separable direct diffuse.
Finished lighting assets publish at a synchronized frame boundary, replacing only the matching scene generation.

### Persistent bake data

`rtx.native.forward.bakeCache` defaults on. Cache files live under
`<configuration directory>/cache/forwardplus/`; `REMIX_BAKE_CACHE_DIR` can select a separate directory.
Each file contains the completed irradiance grid, directional surface atlas, render chart UVs,
mixed-light ownership/constant-visibility metadata, and raw plus filtered reflection captures.
It stores no resource handles, GPU pointers or surface-sampling work lists. Reload reconstructs
geometry against the current frozen source and uses the ordinary fenced volume upload/publication.
The status begins `Cached lighting active` on a hit. Hardware ray-query support is still required
by the current bake controller, even on a disk hit.

Loading checks the version, identity, exact file length and payload checksum before using array counts.
Counts, atlas rectangles, UVs, light indices and numeric values are validated before upload; file
payloads are bounded to 4 GiB and atlas/probe allocations must fit the requested/device limits.
Missing, corrupt or incompatible data triggers a fresh bake. I/O runs on the preparation worker;
saving uses a fresh temporary file and commits it only after completion. Failed/cancelled writes
preserve the previous committed file. Cache files are uncompressed and old keys are not automatically
pruned. Removing files from this cache directory causes them to be regenerated on demand.

Cache policy changes affect subsequent jobs and preserve active lighting. Raw surface-sample diagnostics
explicitly bypass disk reuse so their GPU results remain available. Transport shader regeneration changes
the cache key automatically; changes to CPU sampling, material conversion or cache semantics must bump
`algorithmRevision` in `bake/cache.cpp`. The `RMXBAK02` wire layout includes 32 reflection metadata slots;
the previous 16-slot wire layout is rejected before decoding and must be baked again.

Keep diffuse bake sampling separate from direct lighting. Reflection sources should share one
confidence-bearing specular interface so probes, SSR and RT replace/blend the same term. Optional RT GI
must replace the corresponding baked indirect term or explicitly provide only dynamic indirect light.
The baseline forward renderer must remain usable without per-frame acceleration structures.

## Surface lightmaps and mixed lighting

The runtime now traces and publishes directional surface lightmaps alongside irradiance probes and
static reflection cubemaps. Charted retained world surfaces use their baked texels; uncharted triangles
and moving objects use the existing probe reconstruction. Charted surfaces also have bounded per-light
static visibility for captured local emitters, retaining live direct colour and specular. Independent dynamic
caster maps update mixed visibility per light. Selected lights retain separate baked direct-diffuse coefficients.

Version 3 of `remix_bake_ext.h` extends the bounded bake request with optional surface charts. Charts name
a retained instance, replica, surface and triangle range, with one UV pair per indexed corner in luxel
units. Integer coordinates address texel centres; dimensions include both endpoint samples. The runtime
copies the charts into the owned bake snapshot, validates dimensions/ranges/UVs and positive-area triangle
overlap, and fingerprints chart data with its owning draw. Shared UV edges are allowed, including fan
triangles; collinear triangles own no area. This preserves seams without modifying core vertex layouts.
`chartRevision` is a scheduling promise: callers advance it after any chart edit. Its value is excluded
from the content fingerprint. Versions 1 and 2 remain accepted; they describe uncharted scenes.

The GMod wrapper preserves BSP face lightmap vectors, minima and dimensions as indexed-corner charts.
Displacements use flat-grid coordinates before displacement offsets, while baking samples the displaced
world surface. Invalid, degenerate or out-of-range host mappings remain uncharted. Clipped 3D-skybox
geometry, props and decal sheets currently remain uncharted as well. These use probe lighting until
additional mappings are available. Version-3 publication requires the matching native runtime.

`forward/lightmap/parameterization` maps planar faces onto a world-space orthonormal grid. Its
orientation is shared by faces with the same facing plane normal, and its phase is anchored in world
coordinates. Authored face scale, chart rotation and nonuniform instance placement therefore cannot
change the requested metre-space texel density. Material UVs remain unchanged.

`lightmap/curved` measures nonplanar charts on their actual displaced triangles. For a four-sided disk,
`lightmap/harmonic` maps the boundary to a rectangle using surface arc lengths, then solves interior
coordinates with a positive-weight Dirichlet Laplacian. When that mapping fails, `lightmap/fan` can
unfold an open triangle fan around its shared vertex using actual edge lengths and triangle angles.
This preserves boundary warps that collapse to a line in the authored UVs. It rejects disconnected,
degenerate and overlapping fans. Other charts retain their authored topology. These paths correct the average surface metric
and scale by the largest triangle stretch. A unit step in any chart direction therefore covers at most
one metre on the piecewise-linear surface before the requested luxel spacing is applied. Developable
patches can unfold nearly uniformly; general curvature can require oversampling to enforce this bound.
This is not an assertion of distortion-free flattening. Positions and material UVs are never flattened.
Collinear authored BSP fan triangles already own no atlas area. Rounding slivers in those fans are
excluded from metric fitting only when their world altitude is within the 0.1 mm geometric tolerance;
a collapsed mapping over larger geometry must be unwrapped successfully or fails explicitly. A small
relative density margin accommodates final float-coordinate rounding on narrow triangles.
The positive-weight approach follows the convex-combination parameterization principles described by
[Floater and Hormann](https://www.inf.usi.ch/hormann/papers/Floater.2002.POT.pdf).

Surface vertex records include the chart's normalized first/last texel-centre bounds. The fragment
shader uses centroid interpolation for chart coordinates under MSAA and clamps them to that explicit
subrectangle before every indirect, per-light visibility, direct diffuse or sun lookup. Surface atlases
sample only mip zero, with bilinear filtering and no anisotropy. Outward-rounded chart grids therefore
already contain all required taps, including verified seam donors. The runtime allocates no additional
outer border; CPU callers may still request one. Padding never substitutes for a neighbouring chart's
light ownership. A future mipmapped/anisotropic atlas would need a different footprint policy.

`forward/lightmap/packing` owns bounded shelf packing and reuses gaps in every previous row.
`lightmap/fit` first brackets a fitting global
spacing multiplier, then probes intermediate integer multipliers between the last failure and a known
fit. It retains a verified fit throughout; shelf ordering means this is not a globally optimal packing
search. Density-independent geometry preparation, including the curved solve, runs once per plan.
After choosing density, it tries smaller power-of-two pages and retains them only when total allocated
texels decreases. This respects padding, chart extents and the device layer limit without discarding
surfaces or changing luxel spacing. The configured page size remains a ceiling.
Budget probes reuse those shapes and build only mappings and rectangle allocations. `lightmap/layout`
rasterizes the selected layout and completes its seams once, with conservative coverage, barycentric surface samples and
inverse-transpose normals. Planar chart bounds round outward onto that grid instead of stretching their
endpoints independently. Curved chart extents round outward in their measured metric. Legacy authored
charts (zero metre spacing) retain their endpoints when coarsened. Tiny faces
keep real covered-centroid samples. Invalid/folded charts or an exhausted minimum-size budget fail
explicitly; no chart is silently dropped. Planning supports cancellation.

`lightmap/seams` identifies connected coplanar geometry from shared or overlapping edge segments,
including T-junctions. Separate atlas allocations use the same integer world-grid samples at a join.
Only the conservative bilinear footprint can import a neighbouring donor, and that donor must lie on
real geometry. `lightmap/coverage` clips the transfer segment against spatially indexed triangles:
a connection elsewhere in a patch cannot bridge an open hole. Opposite-facing, separated parallel
and disconnected faces do not share donors. The geometric joining tolerance is 0.1 mm. This handles
coplanar joins.

`lightmap/seam_mesh` also builds exact chart-local triangle adjacency and indexes chart border edges
in a private CPU hierarchy. Cross-chart links require overlapping collinear world edges within 0.1 mm,
compatible outward facing and opposite triangle interiors. Subsegments support T-junctions. Stacked,
opposite-facing and disconnected surfaces cannot link; sharp turns beyond 60 degrees keep independent
padding.

`lightmap/surface_walk` temporarily unfolds adjacent triangles around their actual shared edge to find
a curved gutter sample. It follows a straight segment in the unfolded surface, advancing through real
triangle links. An open edge stops the walk, even if another connected route exists elsewhere; a
256-triangle bound also stops pathological micro-mesh walks. The resulting position and normal are
interpolated on the actual neighbouring triangle. Geometry and the chart's stored UVs remain unchanged.
`lightmap/curved_seams` uses these donors only for outside-centre bilinear taps within the receiving
chart's conservative footprint. Existing interior samples stay authoritative. Each new filtering-only
sample retains the receiving chart and its light ownership; original receiver samples stay in the
blocked/clear proof set. Baked status reports the curved donor count. This supports smoothly bent
surfaces and compatible noncoplanar joins; it does not assert exact reconstruction of arbitrary
high-frequency lighting from a coarse atlas.

Filtering samples retain the receiving chart's local-light channel ownership. The original samples
on the face remain visibility checks even when a border tap uses a neighbouring grid centre. Compact
blocked/clear classification uses those receiving-face checks, so an illuminated neighbour cannot
turn every blocked light into overflow. Candidate bounds also include the extended footprint.
Unused atlas space remains unowned; padding allocation never overlaps another chart's allocation.

`rtx.native.forward.lightmapFitBudget` defaults to enabled. The planner derives a consistent world-space
density from atlas capacity, refining small maps as well as coarsening large ones. Packing probes include
chart rounding, padding, page allocations and device layer limits; surface rays are generated only once.
The published spacing is a measured result, rather than an input quality ceiling. Shelf packing is bounded
and heuristic, so this does not promise optimal utilization of every texel. A single large chart can still
limit density before all pages fill. `rtx.native.forward.lightmapLuxelSize` is a manual override only when
automatic fitting is disabled; existing saved spacing values do not cap automatic detail.
`rtx.native.forward.lightmapPageSize` selects a maximum power-of-two page size (64-4096, default 1024).
Pages shrink when a single requested page would exceed the total texel budget.
`rtx.native.forward.lightmapTexels` bounds allocated atlas texels including padding and unused page space
(65536-16777216, default 4194304). Device image dimensions and array-layer limits also constrain packing.
The UI reports the memory limit in MiB and exposes page size alongside the texel budget.
These are load-time quality/memory controls; bake spacing continues to control only the irradiance grid.

The controller traces bounded surface and per-light visibility batches after the irradiance grid and
before reflection capture.
Fence-completed results feed a cancellable worker that encodes nine signed RGB SH coefficients into
RGBA16F array layers (`page * 20 + coefficient`); layer 9 stores four direct-visibility channels, and
layers 10..18 store nine unit-radiance direct coefficients in four light channels. Indirect-layer alpha
stores nine unit sun coefficients; layer 19 stores static sun visibility. Image allocation,
staging, transfer and shader visibility
precede activation. The atlas, probes and cubemaps publish together for one completed snapshot/settings
generation; in-flight submissions retain their exact volume and chart-geometry owners. No per-frame
acceleration structure is needed to sample the finished atlas.

`lightmap/data` creates indexed chart geometry, duplicating only vertices whose atlas coordinates differ.
A separate vertex pipeline consumes those private coordinates; core mesh layouts and the existing shadow
geometry stay unchanged. `lightmap/geometry` binds that data only when retained identity, replica, surface,
geometry/material ownership, transform, index range and alpha state still match the captured draw. Weak
source references are used only as identity checks, never as transport inputs. A changed bake snapshot
currently starts a fresh runtime job even when its transport hash matches, preserving binding safety
until equivalent-snapshot rebinding is implemented.

Fractional or coarsened triangles use conservative texel-footprint coverage. A texel whose centre lies
on the face keeps that centre; otherwise the planner clips the triangle to the texel footprint and
samples its covered centroid. Multiple triangles sharing a texel retain one real surface donor,
preferring a contained centre and then the nearest clipped centroid. Rays therefore remain on actual
geometry, including slivers that cross texels without containing any integer centres. This avoids
collapsing an entire fractional face to one fallback sample. Chart-local dilation fills remaining unsupported
texels; verified coplanar completion replaces only supported border taps. Runtime atlas packing uses one texel of padding, sufficient
for its explicit mip-zero bilinear sampling, to preserve more coverage within the fixed memory budget.

The surface shader samples the chart's coefficients at mip zero with a dedicated linear sampler when
the HDR format supports it (nearest fallback otherwise), evaluates them with the
current mapped normal, and replaces the probe/ambient indirect diffuse term. Valid black texels remain
black. Normal-map and specular shading keep surface resolution. Selected local lights combine their
separate baked diffuse term with current specular; other lights use the ordinary Forward+ path. Atlas allocations remain isolated even where their lighting samples match. Original and charted vertex buffers currently
coexist; this increases static geometry memory and still needs real-map profiling.

### Per-light static visibility

`forward/mixed` assigns up to four captured local lights to each chart. Conservative world-space chart
bounds and the current Forward+ influence cutoff select candidates; importance and stable light identity
break allocation ties. This limit applies per chart, rather than limiting the whole map to four lights.
The planner retains each work item's surface point, captured-light index and destination channel.
The tested-light identity/shape table and compact occlusion survive activation; per-texel work
lists and untested candidate queues stay with the temporary job and are released after its final owners finish.

The hardware transport has a dedicated visibility mode using the same material-aware ray traversal and
emitter sampler as indirect lighting. Sphere emitters sample solid angle; triangles sample area with
emitter-facing/distance weights. Spotlight cone shaping and focus participate in the weights. A channel
stores visible weight divided by total weight, independently of receiver albedo, mapped normal and the
current light colour. This approximates soft static penumbrae for the renderer's centre-direction BRDF;
it does not exactly integrate a varying glossy BRDF across an area emitter.
The transport preserves an exactly clear endpoint from unattenuated RGB connections, avoiding
luminance/weighted-sum rounding. Any attenuated connection keeps its result below one, even if the
ratio would round to one; the planner uses exact endpoints rather than an epsilon threshold.

A chart's four captured-light indices accompany its private vertices as integer attributes, with flat
shader interpolation. They never pass through filtered colour textures. Visibility values use the same
chart-local donors and padding as the irradiance atlas, so adjacent charts cannot exchange light ownership.
The ordinary material and light layouts remain compatible, and the scene descriptor's storage-buffer
count does not increase. The frame light's private `shaping.w` stores captured index + 1 (zero means none).
CPU matching uses stable identity plus position, radius, triangle corners, direction, cone, softness and
focus before issuing that tag; the per-frame light array's ordering is unrelated to channel ownership.

The forward shader applies visibility only to the matched local light's direct BRDF, including specular,
and evaluates its current radiance per pixel. A colour edit can therefore reuse geometry-valid visibility;
the old indirect bounce still needs a newly published bake scene. Moved/reshaped lights reject their old
mask. Indirect irradiance, emission and other lights are never multiplied by that mask.

After each completed visibility round, chart/light pairs with exactly constant sampled visibility
move into compact two-bit metadata: unknown, blocked or clear. Their filtered channels test the next
candidates in the original importance/identity order. A single sample above zero prevents blocked
classification; a single sample below one prevents clear classification. Charts without samples
cannot be classified. Filtered-channel allocation stops when four channels retain partial visibility
or the candidate queue is exhausted. Remaining candidates then receive a streaming visibility audit,
one light per chart per round. Exactly blocked or clear receiver samples use compact metadata;
filtering donors cannot supply chart-wide classifications. Partial pairs retain a scalar visibility
mask at the receiving chart's existing luxel density, including its verified filtering donors. Values
are quantized to normalized bytes and sampled bilinearly within the chart bounds. These masks shade
both direct diffuse and specular and combine with independent moving-caster shadows. The audit preserves existing
filtered visibility and directional direct-light channels. This can increase map-load ray work, but avoids spending filtered channels on
constant masks. Classification remains limited by spatial/sample resolution: tiny unsampled openings
or occluders can be missed, just as in the corresponding constant filtered chart. These states are
not geometric proofs of visibility over every point on a surface.

Sparse masks allocate only their chart regions, with no extra full-atlas layers. An R8 mask chooses
between raw bytes and adaptive 8x8 tiles. Constant tiles store their actual byte and binary tiles store
64 exact coverage bits. Soft tiles try local four-, eight- and sixteen-colour palettes, then retain
all 64 original bytes if fitting exceeds eight R8 units of error at any sample. Zero and 255 get
reserved colours; intermediate samples never select those endpoints. The bound applies to quantized
R8 visibility, not to radiance or unsampled geometry. Local palettes preserve faint/near-clear gradients
better than uniformly quantizing the whole visibility range at the same index precision.

Four/eight/sixteen-colour payloads use 20/32/48 bytes; raw soft tiles use 64 bytes, each with a
four-byte descriptor. Palettes are fit on the bake worker using bounded iterations. The lowest-cost
palette satisfying the error limit wins; a whole chunk uses raw bytes when those are smaller.
Dimensions, luxel density and bilinear filtering remain unchanged. The decoder interpolates across
tile boundaries, including three-bit index word crossings. Cache import validates palette mode,
sorted colours, canonical indices, contiguous payload offsets and lengths before publication.
CPU codec/fitting and shader decoder live in `mixed/mask_codec.*`, `mixed/mask_palette.*` and
`shaders/overflow-mask.glsl` respectively. Algorithm cache identity invalidates older allocation results.

Chart-major tables
sorted by captured-light index support binary search in the existing occlusion texture. Tables plus
quantized masks have a 16 MiB encoded budget and must fit the device texture limit. Byte-valued half
storage uses twice that size on the GPU, plus row padding. Preparation runs on a cancellable bake
worker after the round's GPU fences complete. Partial pairs that exceed either budget remain counted
as overflow and require the realtime shadow fallback; their illumination is never silently removed.
No per-frame visibility rays or extra GPU descriptors are required. Cache wire version `RMXBAK03`
retains masks, exact identities and dimensions; older layouts rebake.

The metadata retains exact chart and captured-light identity for compacted channels. The shader
rejects blocked direct diffuse and specular before the expensive BRDF/shadow work. Clear lights still
receive independent moving-caster shadows; only their static visibility is constant. A moved light
rejects the captured tag, colour changes retain it, and uncharted draws never read another chart's bits.
Chart identity is encoded exactly in the private vertex's coordinate W (chart index + 1, zero invalid)
and converted to a flat integer varying before interpolation. The authored scene contract limits chart
counts to one million, within exact float-integer range. Block words use an unfiltered RGBA16F texture: each component stores an exact byte value from
0 to 255. Half precision represents those integers exactly, so decoding needs neither normalization
nor rounding. This reuses the existing HDR format and avoids another storage buffer or optional
integer-image requirement. Dimensions and chart stride live in the private frame constants; each
lookup reads only its data texel, rather than sampling texture headers for every light.
The fragment stage uses five scene samplers plus the five material samplers; its four storage-buffer
bindings stay unchanged. The texture also retains its header (words per chart and chart count) for inspection.
The CPU bitset has a 64 MiB budget; GPU byte-valued half storage uses twice that size before padding,
and the device texture limit also applies. Candidate queues have a four-million-link
budget. Exceeding these bounds reports a bake failure and preserves ordinary direct-light fallback.
The completion message separately counts compactly blocked pairs, clear pairs and untested overflow. The additive
`contentHashes.staticOcclusion` diagnostic fingerprints the published compact texture bytes.

Overflow progress counts completed surface/light pairs across the entire stage, rather than resetting
at each light round. Receiver membership is prepared once for the immutable layout. Visibility rays
without dielectric boundaries terminate at their first accepted blocker; opaque coverage skips alpha
texture reads. Cutouts and source-over coverage still run before hit acceptance, while dielectric
connections retain ordered closest-boundary traversal. These paths preserve sample and bounce budgets.

Untested overlap overflow is counted in completion status. These remaining overflow lights, uncharted receivers, dynamic lights
and unmatched shapes use local depth maps when selected within the independent shadow budget. Sunlight
still uses the existing CSM path. Autogenerated emission proxies without a matching captured light
identity also use this ordinary local-light fallback.

### Separable baked direct diffuse

`rtx.native.forward.bakeDirectDiffuse` defaults on. The same selected chart/light work that traces soft
visibility also integrates unit-radiance directional direct irradiance. The transport reuses emitter
samples and visibility rays, accumulating nine cosine-convolved SH coefficients separately from the
visibility ratio. Receiver albedo, current light colour, view direction and BRDF are not baked into
those coefficients. Each coefficient's RGBA components retain four stable chart-light owners,
independent of the four filtered visibility owners. Completed positive candidates fill direct slots in
importance order. Their nine signed coefficients are copied from temporary tracing channels before
those channels are reused; clear masks therefore do not discard their baked direct irradiance.
Both owner vectors accompany private chart geometry as flat integer attributes. Chart-local padding
preserves normal response and avoids channel exchange. Both filtered masks and direct SH remain
bounded to four owners each; other tested clear lights use analytic diffuse with known static visibility.

The surface shader reconstructs all four direct irradiances from nine additional atlas reads once per
shaded fragment, rather than nine reads per light. Pure metals, uncharted receivers and charts without
assigned direct channels skip this work. For a matching light it replaces only the analytic diffuse
lobe with the selected unit irradiance, applying current albedo/metallicity, diffuse Fresnel weight,
light radiance and influence-range fade. Specular keeps the current per-pixel GGX evaluation and the
separate baked visibility mask. The current normal map affects reconstructed diffuse irradiance.

Static occlusion is already inside baked direct irradiance, so it is not multiplied by the static mask
again. Independent moving-caster visibility multiplies that light's baked diffuse and its current static-
masked specular. Indirect lighting, emission and other lights are untouched. Unmatched, moved/reshaped,
dynamic and overflow lights use the existing analytic fallback (with local depth maps when budgeted).
Sunlight uses the mixed policy described below. Turning this policy off rebakes visibility without direct
coefficients and restores the analytic diffuse path; it does not disable local lighting or specular.

This is directional unit-light storage, not a single flattened RGB direct map. Colour edits remain live
while matching position/shape and per-channel ownership are required before reuse. A changed light's
indirect bounce still needs a newly published snapshot. No view-dependent specular term is baked.
The nine-coefficient representation smooths angular detail and can ring near tangency; results clamp
to nonnegative irradiance, and an analytic emitter/receiver-plane test rejects ringing from an emitter
wholly below the surface horizon. This approximates area-light diffuse response and centre-direction Fresnel;
it does not exactly integrate an arbitrary glossy BRDF across an emitter. Combining static area-light
visibility with centre-based dynamic PCF is still an approximation.

Pages use 20 RGBA16F layers: indirect RGB SH plus unit sun SH in 0..8, local visibility in 9,
unit local direct SH in 10..18 and sun visibility in 19. That is 160 bytes per allocated page texel
before device alignment. The default 4194304-texel budget is about 640 MiB for a fully allocated surface
atlas. A full 4096-square page uses 2.5 GiB; page dimensions do not replace the total texel budget.
Smaller pages avoid allocating a complete large page for partially occupied maps. The directional
coefficient representation is not texture-compressed. Local direct coefficients use two temporary 144-byte arrays per sampled point (working and
retained); sunlight adds 40 bytes per point when a distant light is captured. Device array-layer limits
participate in chart coarsening. Finished atlases still require no
per-frame acceleration structures. Finished visibility refinement releases its scratch coefficients.
After atlas encoding, completed transport arrays are released before upload (raw surface results are
retained only when the sample diagnostic is requested). Publication writes separate source arrays
directly into their staging offsets, avoiding an additional atlas-sized CPU concatenation buffer.
No core mesh/material/public ABI layout changes are introduced.

### Mixed sunlight

Captured distant lights bake a soft static sun mask using the source's angular disc and the same
material-aware ray transport as other lightmaps. Unit-radiance directional sun SH is stored separately
when baked direct diffuse is enabled. It shares the existing nine indirect-layer fetches through alpha;
one additional atlas layer holds sun visibility. Surface albedo, light colour and view-dependent
specular remain live. Direct SH already contains static occlusion, so it is not multiplied by the mask
again. Specular uses the soft mask and the current sun's per-pixel BRDF.
Static sun rays cover at least the complete packed geometry's diagonal, independently of the shorter
indirect/radiance trace distance. Distant authored occluders therefore remain eligible to shadow a
surface sample inside those bounds.

The sun depth array contains two groups of four cascades: complete frozen bake geometry, then current
casters outside that exact snapshot. Matching charted receivers use their baked static term plus only
the moving-caster cascades, allowing narrow realtime shadows over coarse soft static lighting.
Uncharted, moved or unmatched receivers combine both depth groups. Complete frozen geometry is
submitted independently of current camera draw membership. Sun colour changes preserve the bake;
direction or angular diameter changes reject it until a matching snapshot is published. A changed
sun's indirect bounce still requires a new bake. Disabling sun shadows selects unshadowed analytic
sunlight. Map unload releases both the atlas bindings and frozen caster membership.

The four cascade transforms and their split distances are shared by both groups. The second group
adds 16 MiB of depth at the default 1024-square resolution; each caster is drawn in its owning group,
rather than redrawn in both. Cascades remain camera-dependent and are rendered each frame. Beyond
the configured cascade distance, charted static sun shadows remain baked, while moving shadows have
the same finite realtime coverage as other cascades. Angular SH truncation and centre-direction
specular/dynamic PCF remain approximations to a finite sun disc.

### Dynamic local shadows

`forward/shadows/plan` owns the CPU light selection, six-face perspective matrices, cache invalidation
and exact authored-caster membership. `forward/shadows/maps` owns two D32 array images and their layer
views/framebuffers. Both use ordinary Vulkan descriptors and six ordinary raster passes per light;
there is no cube-array, geometry-shader, descriptor-indexing or ray-query feature requirement.

Each selected light has a cached static map from the complete immutable bake snapshot, including
retained geometry absent from current camera submissions. It refreshes when the snapshot, light origin
or depth range changes. Separate maps clear and render current moving casters each frame. Exact unchanged
snapshot members are excluded from the moving pass; a settled actor outside the authored membership
remains a moving caster. Changing authored static geometry requires republishing the scene because its
old baked lighting and static shadows belong to that snapshot.

For a chart/light with a baked visibility channel, analytic/specular lighting multiplies that soft
static visibility by the current dynamic map. Baked direct diffuse already includes static visibility
and multiplies only the moving map. It never applies the static depth map again on top of the baked penumbra.
Uncharted or unassigned receivers combine static and dynamic depth comparisons per tap before averaging.
Only the corresponding light's direct diffuse and specular terms are attenuated; indirect lighting,
emission and other lights remain independent. Dynamic shadows therefore have their own resolution,
independent of the chart's luxel density. Dynamic penumbrae currently use a fixed 3x3 PCF footprint from
the emitter centre, an approximation rather than area-emitter integration or contact-hardening shadows.

The maps store linear radial depth; comparison bias is expressed in metres and converted using scene
scale. PCF taps reproject across cube seams using the same face convention as reflection captures.
All layers initialize before descriptor sampling, including inactive slots. Queue dependencies protect
previous-frame reads before writes; per-frame commands retain geometry/material owners through fences.

Default quality is four shadowed lights at 512 texels per face. The bounded selector prioritizes camera-relative
influence (range squared over eye distance squared), breaks ties by stable light identity and preserves
surviving cache slots. The UI reports selected lights, budget overflow and static-map refreshes. Lights
outside the budget or configured shadow distance retain direct illumination without local-map shadows;
a valid baked channel can still shadow such a light statically. This fallback is visible and configurable,
not a claim that every local light receives dynamic shadows. Distance is currently a hard cutoff.

Profile controls: `rtx.native.forward.localShadows`, `localShadowLights` (0..8),
`localShadowResolution` (64..1024, power of two), `localShadowDistance` (metres), and
`localShadowBias` (metres). Count/resolution changes prepare replacement targets; enable, range and bias
can update live. Two arrays cost approximately `2 * lights * 6 * resolution^2 * 4` bytes before device
allocation overhead: 48 MiB at the default, plus cached frozen geometry. Budget zero uses initialized
tiny descriptors. The current all-face raster approach and influence-only selection need real-map GPU
profiling and may benefit from face culling, spot-specific maps, update budgets and selection hysteresis.

Static world surfaces should sample surface lightmaps, with irradiance probes retained for moving and
uncharted objects. The GMod BSP already contains face lightmap vectors, extents and dimensions; preserve
that mapping through an optional extension rather than changing the core Remix vertex/mesh ABI. Other
static geometry needs validated non-overlapping chart data or generated charts. Atlas padding, chart
ownership, transformed instance placement and material/geometry revisions must be part of the bake.
Hardware baking must sample the surface texels rather than reconstruct surface lighting from probes.

Keep lighting components separable:

- Indirect irradiance stays independent of direct light and of albedo. Directional storage should retain
  normal-map response. Dynamic shadows must not arbitrarily darken this whole term.
- Fully baked direct diffuse can be accumulated for lights that do not need runtime shadow changes.
  Preserve the relevant light identities and shadow information wherever runtime direct specular remains.
- Mixed/stationary lights keep stable identity, position, shape, radiance and per-surface baked static
  visibility. Evaluate their direct BRDF per pixel, including specular. An independently resolved shadow
  map from dynamic casters supplies current visibility. The low-resolution lightmap does not set that
  shadow map's resolution.
- If a mixed light also stores baked direct diffuse, retain that contribution separately so its dynamic
  shadow affects only that light. Never multiply the combined indirect-plus-direct lightmap by a generic
  dynamic shadow or add an unshadowed duplicate of already baked direct diffuse.

An area-light bake can integrate static penumbrae using the emitter radius/shape. Combining separate
static and dynamic soft visibility is an approximation; it does not recover correlated per-emitter
visibility exactly. Keep that combination isolated so the quality policy can evolve without changing
lightmap ownership or the BRDF. Normal maps and specular should remain evaluated at surface resolution.

Use explicit authored bake membership to identify static casters. A temporarily stationary actor must
not become a baked caster merely because its transform stopped changing. Mixed shadow maps draw current
dynamic casters, with alpha coverage shared with the main material path. Moving a mixed light invalidates
its baked static visibility; changing a light's colour can update direct shading while its old indirect
bounce still requires a rebake.

Per-light surface data needs a bounded allocation: pack compatible non-overlapping mixed lights into
mask channels or pages, track stable light-to-channel ownership, and provide a visible/runtime fallback
for overlap overflow. Do not silently drop a light or collapse all provenance into one RGB atlas. Cache
keys must cover charts, geometry, lights, units, bake options and the lighting/storage algorithm version.
The inspected local s&box `common/classes/Light.hlsl` provides a relevant reference: its lightmap source
reads light indices and strengths, reconstructs the indexed lights, and evaluates runtime visibility
separately. That is the intended first mixed-light layout here. Indices need discrete sampling; strength
filtering and chart padding must preserve which light owns each channel, including at atlas boundaries.
The broad stationary-light separation is documented by [Epic](https://dev.epicgames.com/documentation/en-us/unreal-engine/2.2---stationary-light?application_version=4.27);
this is our proposed architecture, not a claim to reproduce undocumented Source 2 internals exactly.

## Submission culling and GPU timing

World draws use conservative world-space bounds against the Vulkan camera clip
volume. Sun casters use each cascade's clip volume independently, so an object
outside the camera can still cast a visible shadow. Bounds include the posed
geometry and model transform. Missing bounds stay submitted, and touching or
crossing bounds survive with a small floating-point margin. The test does not
infer occlusion, room visibility or caster membership from camera visibility.

The existing `rtx.native.gpuPassTimers` switch exposes separate Forward+ scopes
for bake transport/publication, live ray scene preparation, frame uploads, light
clustering, sun cascades, local shadow maps, world shading and reflections.
These nest inside the runtime's Forward+ scope; inclusive times overlap with
their parent and should not be summed with it. Timers are inactive by default.
World-space bounds tests themselves run on the CPU and are not GPU timestamps.

The compute-produced cluster index list uses device-local memory and is never
CPU-mapped. Each fenced frame slot owns its list; growth replaces only that
slot's buffer, and the existing compute-to-fragment dependency makes writes
visible before world shading. CPU-uploaded light and draw data keep their
host-visible allocation path. GPU scratch storage does not inherit that path.

## Verification

Static displacement paint submitted with the terrain and decal categories uses a lit HDR
source-over pass after opaque geometry. Its surface charts receive baked diffuse lighting and
static visibility; dynamic lighting uses the same PBR material path. The pass tests the opaque
depth without writing it and preserves the underlying receiver's reflection guides. Blended
normal/roughness guides for those layers remain a limitation. Ordinary decals retain their
compatibility path, and paint layers are excluded from opaque shadow-caster lists.

Fitted cubemap projection boxes remain inset from solid room boundaries. Receiver influence
can snap outward to nearby axis-aligned BSP planes, so a floor or wall at the boundary can use
its room's capture. Each snap is checked against whole-volume occupancy with the existing
bounded trim tolerance; proximity alone never permits crossing a wall. This does not solve
rotated-room fitting or guarantee coverage when the capture budget is exhausted.

CPU policy coverage lives in `tests/forward`. GPU readback extends the existing raster fixture to check
local-light contribution, sun-shadow occlusion, MSAA silhouette coverage, frame-slot reuse and HUD
composition. SSR readback checks reflected emissive geometry at odd target dimensions across 1x, 2x
and 4x MSAA and all three reflection resolutions. It also checks roughness rejection, removed objects
and alpha-tested hit coverage. Probe checks cover face orientation, cross-face filtering, parallax,
GGX roughness, overlapping HDR volumes, cancellation, and CPU/GPU sampling agreement. Surface readback
isolates cubemap lighting on a rough metal with SSR disabled, then checks SSR hit replacement and probe
fallback across the same MSAA/resolution combinations. Mixed-light readback checks narrow live shadows against a
coarse static chart, unrelated-light isolation, alpha rejection, movement/removal without trails,
static-map reuse, complete offscreen captured geometry, enable switches, emitter motion and unload at
1x/2x/4x MSAA (including SSR). CPU shadow tests cover six-face projection, radial bounds, stable slots,
explicit membership and cache invalidation. Hardware reflection readback additionally covers hidden dynamic emitters at odd 191x127 dimensions,
multiple GPU batches, SSR/RT brightness conservation, blue-probe fallback on misses, authored offscreen
geometry and unload, distance edits without rebuilds, optional-feature fallback and runtime RT toggling.
Baked direct readback checks analytic-diffuse replacement without duplicate brightness, live colour,
static occlusion and narrow dynamic shadows at 1x/2x/4x MSAA. Hardware transport checks unit direct
irradiance against an analytic sphere and its directional response independently of specular visibility.
Live GI readback checks replacement of an active red surface lightmap by green current environment,
valid black occlusion, hidden blue emissive actors, direct sunlight preservation, movement/removal,
independent GI/reflection uniforms, range edits, disable restoration and hardware-unavailable fallback.
These run at 1x/2x/4x MSAA with RT reflections off/on. CPU capture checks include posed geometry,
material animation, texture revisions, live overrides and
lighting-independent cache reuse. GPU transport checks stream light colours, movement, removal and
buffer growth through completed slots. Run with Vulkan synchronization validation when available:

```text
REMIX_VULKAN_VALIDATION=1
VK_LAYER_VALIDATE_SYNC=1
meson test -C <build> --print-errorlogs
python scripts/shaders/generate.py forward --check
python scripts/shaders/generate.py forward --validate
python scripts/shaders/generate.py bake --check
python scripts/shaders/generate.py bake --validate
python scripts/shaders/generate.py live_ray --check
python scripts/shaders/generate.py live_ray --validate
python scripts/shaders/generate.py live_gi --check
python scripts/shaders/generate.py live_gi --validate
```

Synthetic readback does not establish GMod map compatibility or performance. Test dense lights, moving
casters, map changes, late materials, masks, water, 3D skyboxes and backend/resize transitions in the host.

For real-window validation with layers that do not recognize the provisional NVIDIA present-metering
feature, set `REMIX_FG_PRESENT_METERING=0` before device creation. This omits that optional device
feature and uses the existing timed-present fallback; Vulkan validation remains enabled.

## Inspecting published lightmaps

`scripts/experiments/lightmap_inspect.py` reads existing bake capture dumps without
opening the game or changing a cache. It requires NumPy and Pillow. Use a fresh
output directory and the same page size as the captured bake:

```text
python scripts/experiments/lightmap_inspect.py --prefix <capture-prefix> --output <new-directory> --page-size 1024 --metres-per-unit 0.0254
```

The overview shows chart rectangles, an exposed RGB view of the constant indirect
SH coefficient, and sun visibility. Individual page PNGs retain the original
resolution. The constant coefficient is orientation-independent; it does not show
the complete directional irradiance, attributed direct lighting or material BRDF.
Rectangle coverage includes padding and is not a measurement of used luxels.
Metadata records storage size, preview exposure and, if the irradiance-grid dump
is available, its dimensions, spacing and valid probe count. These are inspection
views of the current twenty-layer atlas, not an export to Source 2's texture format.
