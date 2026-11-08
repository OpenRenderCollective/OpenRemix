# Metal backend

macOS arm64 builds `libremix` with a native Metal runtime instead of the Vulkan one. The
exported API and ABI are unchanged; the backend follows the target platform and a build
contains exactly one backend. macOS builds link no Vulkan.

## Layout

`src/native/metal` holds the backend; nothing outside it uses Metal.

| File | Role |
| --- | --- |
| `runtime.h`, `runtime.mm` | `RemixNativeRuntime` for Metal builds: lifecycle, settings, statistics |
| `presentation.mm` | `CAMetalLayer` presentation, frame slots, frame generation, screenshots |
| `raster.h`, `raster.mm`, `raster_gpu.h` | `metal::Raster`, the API's scene (`NativeResources`) and raster renderer |
| `raster_overlays.mm` | halo outlines, masked overlays and role CUT coverage, soft sprites' scene depth |
| `tracer.h`, `tracer.mm`, `tracer_geometry.mm`, `tracer_gpu.h` | `metal::PathTracer`: BLAS/TLAS, tables, passes, SHARC, ReGIR, light guiding, volumetrics, sparse rendering |
| `frame.h` | the shaders' `Frame` blocks, checked against the translated shaders' sizes |
| `denoise.h`, `denoise.mm` | NRD REBLUR/SIGMA dispatches, confidence filter, composition and TAA |
| `reconstruction.h`, `reconstruction.mm` | MetalFX upscaling and denoising, the display composite |
| `post.h`, `post.mm` | automatic exposure and bloom |
| `timers.h`, `timers.mm` | GPU pass timers (counter sample buffers at encoder boundaries) |
| `shaders.mm`, `gpu.h` | embedded libraries, pipelines (compiled now or in the background) and argument buffers |
| `ui.mm` | Dear ImGui on Metal, and the Metal runtime's answers to the panel's queries (`ui/status.h`) |
| `diagnostics.h`, `host_passes.h` | the API layer's diagnostics and host passes, as Metal provides them |
| `shaders/embedded.{h,bin}` | generated Metal libraries |

Shared with Vulkan builds: the API layer (`api/state.h` includes `metal/runtime.h` when
`REMIX_METAL`), the CPU scene library, the SDL window (`metalLayer()` replaces the Vulkan
surface), settings and profiles, the panel, profile and mods UI, the path tracer's frame tables
(`pathtrace/frame_tables.h`), TLAS policy, opacity proofs, light hierarchies and the volumetric and
sparse policies. The Metal host code ports the Vulkan renderer's passes; comments name the Vulkan
function each mirrors.

The developer panel is shared: both builds compile the same tabs (`ui/*.cpp`). What differs
between the backends (device, swapchain or drawables, reconstruction, frame generation, features
one backend lacks) reaches the tabs only through the queries in `ui/status.h`, which the Vulkan
runtime answers in `runtime/ui_status.cpp` and the Metal runtime in `metal/ui.mm`. A tab never
reads a backend's members directly; a new control that needs backend state adds a query that both
answer. Features the Metal backend does not have yet report unavailable, and the tabs hide or
explain them.

HDR10 output (`metal/hdr_output.mm`) follows `runtime/hdr_output.h`: while the display has
extended dynamic range the layer is BT.2100 PQ with 10 bits, the frame draws into a float back
buffer, the HUD and developer UI are scaled to their own brightness, and an encode pass writes PQ
into the drawable (`HdrNits` is shared in `runtime/hdr_nits.h`). With frame generation the frame
stays SDR. Screenshots and screenshot sets are `runtime/screenshot.cpp` itself, built with the
Metal readback (`metal/screenshot.mm`), so the PNG, HDR PNG, JPEG XR and EXR files are the Vulkan
backend's. The path inspector records the clicked pixel's paths as on Vulkan.

The NUMOS sky is the Vulkan backend's `sky/renderer.cpp`, built a second time as Objective-C++
(`metal/sky_renderer.mm`) over `metal/sky_bridge.h`, the same `sky::Bridge` interface on the Metal
device; `sky/host.h` names the GPU objects the renderer takes on each backend. A sky change made
for Vulkan reaches Metal without a port, and one that needs a bridge call Metal lacks fails the
macOS build. The Slang passes are translated with the other shaders (stages `sky0` onward). Both
the path tracer and the raster backend draw it (`metal/raster_sky.mm`, as `runtime/raster_sky.cpp`).

Particle emitters (`metal/particles.mm`) run the particle shaders translated from the same SPIR-V,
with `particles/renderer.cpp`'s pool, frame constants and passes: the light volume and SHELTER
spawns trace the world TLAS, and the draws sample the captured scene depth. With frame generation
they are drawn before the frame is taken (the UI pass clears depth), so they are interpolated.
Light cookies take the frame's texture table as on Vulkan.

## Shaders

The Metal backend runs the same shaders as the Vulkan one. The build generates the Vulkan SPIR-V
and then, with `scripts/shaders/generate.py metal`, translates each stage with the pinned
upstream SPIRV-Cross (`toolchain.lock.json`, `spirv-cross-msl`) with OpenRemix's MSL fixes
(`scripts/shaders/patches/spirv-cross-msl.patch`), built by `scripts/shaders/toolchain.py --metal`, and compiles it with Xcode's `metal` into one
XZ-compressed library per stage, which `embedded_data.cpp` embeds. Path tracing stages are translated
from glslang's SPIR-V before `spirv-opt -O`: Apple's compiler service fails on the fully inlined
lighting kernels. Descriptor sets become argument buffers at `buffer(set)`; `embedded.h` maps
each stage's Vulkan `(set, binding)` to argument buffer IDs, so the host binds by the Vulkan
numbering. Specialization constants are Metal function constants at their `constant_id`.

`denoise.comp` switches on a push constant, so every image some mode writes is a `read_write`
texture in its translation, and Apple GPUs read those through a slower, coherent path. Each mode
therefore also has a variant of its own (`denoiseMode0`..`5` and the sigma/local families), with
the mode fixed and dead code removed, in which the images it only reads are plain textures.

A changed shader regenerates the Metal libraries with the Vulkan SPIR-V in the same build. The
std140 sizes of the blocks the host writes come from reflection, and `metal/frame.h` checks its
copies of them at compile time: a field added to `scene.glsl`'s `Frame` fails the build.

## Path tracing

The passes and their pipelines follow `pathtrace/rendering.cpp` and `local_lights.cpp`:

- One lighting family: SHARC runs on Metal, so the cache-querying stages (`sharcQuery`,
  `sharcTemporal`, ...) light every frame and the query is gated per frame, as on a
  SHARC-capable Vulkan device. Metal has no 64-bit atomic compare-exchange: SHARC's update pass
  uses SHARC's lock-buffer insertion (`HASH_GRID_ENABLE_64_BIT_ATOMICS=0`); the Vulkan SPIR-V is
  unchanged.
- Variants: a pass's base pipeline takes local lights, the shadow early-out setting and its
  separate-indirect role; the scene's other variants (guiding, the small set, resampling, the ray
  profile, sparse launches) are keyed as on Vulkan and compile in the background, several at a
  time within the compile slots' memory budget, while the base renders. The per-dispatch switches
  (decals, roulette, angular emitters, first opaque hit, the multi-sheet fallback) are the
  `features` push constant.
- A frame records sky raster, the primary G-buffer (view models after the scene on a cleared
  depth), ReGIR presampling, the volumetric update, SHARC update and resolve, sparse compaction,
  the separate indirect pass (`REMIX_SPLIT_INDIRECT=0` traces the bounces inline), the opaque
  pass, confidence, NRD, sheet lighting, the single- and multi-sheet transparency passes (the
  multi-sheet pass compiles in the background; the single-sheet pass composes layers meanwhile),
  composition and TAA, then composites as the Vulkan renderer does.
- BLASes are built per converted mesh view; static and dynamic TLASes use
  `MTLAccelerationStructureUserIDInstanceDescriptor` with the custom index as user ID, and the
  instance masks of `pathtrace/tlas.cpp` (blended layers, view models). Vulkan's instance flags
  have the same values as Metal's instance options. Buffers the shaders reach by address are made
  resident from the tracer's registry each frame.
- Volumetrics (`pathtrace/volume.cpp`) and sparse rendering (`pathtrace/sparse.cpp`) run as on
  Vulkan. Sparse rendering needs the MetalFX denoised scaler (Ray Reconstruction's place); its
  lighting launch is an indirect dispatch.

The NRD library is shared with Vulkan builds; its DXC SPIR-V is translated like the GLSL stages
and dispatched by the library's dispatch descriptions (`denoise/nrd.cpp`'s bindings and settings).

The resources a tracer needs (size, REBLUR, TAA, SIGMA, local-light denoising, SHARC,
reconstruction, the shadow early-out and separate indirect settings) form its configuration. A
changed configuration prepares a new tracer on a thread while the current one, or raster, keeps
presenting, as the Vulkan runtime prepares configurations, with the same progress notice. The
first use of a configuration compiles its lighting kernels (tens of seconds); later starts load
them from Metal's cache. A preparation still compiling at shutdown is abandoned (a Metal compile
cannot be cancelled); the pipeline cache outlives static destruction.

## Raster overlays

The raster renderer mirrors `raster/rendering.cpp`: frustum culling of index ranges
(`REMIX_RASTER_FRUSTUM_CULL=0` turns it off), host-ordered blended draws, wireframe instances
(triangles drawn as lines), sprite overlays (`spriteVert`/`spriteFrag`, the mesh's sprite vertices
as a second vertex buffer), masked overlays (`maskOverlay`, `maskWithin`) and halo outlines
(`haloMask`, `haloBlur`, `haloComposite`). Silhouettes are drawn into R8 coverage targets at
target resolution in passes of their own; `presentation.mm` splits the frame's pass around them
as `runtime/presentation.cpp` does, keeping the depth attachment for the passes that resume it.
Role CUT coverage is drawn ahead of the path tracer, which samples it through its texture table
(`frame.cutConfig`, `cutSlots`). Soft sprites sample a blit copy of the frame's depth taken after
the composite. Dynamic textures with generated mips are uploaded whole on each update.

## MetalFX

FSR and DLSS are not available on Metal; MetalFX takes their places, through the same settings.

| Setting | Effect |
| --- | --- |
| `rtx.native.pathtrace.upscaler` (any mode but `off`) | MetalFX temporal scaler (FSR/DLSS-SR's place), with NRD |
| `rtx.native.pathtrace.dlssRayReconstruction` `1` | MetalFX temporal denoised scaler (macOS 26): replaces NRD and TAA, as Ray Reconstruction does |
| `rtx.native.pathtrace.dlssQuality` (`fsrQuality` with `fsr31`) | render size: native AA, quality 1.5x, balanced 1.7x, performance 2x, ultra performance 3x |
| `rtx.native.frameGeneration` `1` | MetalFX frame interpolation (macOS 26): presents a generated frame before each rendered one |

The tracer renders at the preset's size; `upscale.comp` prepares reversed device depth, pixel
motion and the reactive mask from the denoiser's guides as for FSR, and `upscale.frag` composites
the display-size output with post-processing and the stable depth for raster overlays. The
stable depth pass runs with raster primary visibility, or with reconstruction when a raster draw
of the frame tests the scene's depth. The denoised scaler also takes the Ray Reconstruction
guides (diffuse and specular albedo, normal, roughness, specular hit distance).
`GetReconstructionDiagnosticsEXT` reports MetalFX upscaling as backend 1 and MetalFX denoising
as 3.

Frame generation needs a temporal configuration (not accumulation). The scene renders into an
offscreen color target without the view model and HUD layers, which have no motion vectors of
their own; the interpolator takes it with the render-size depth and motion and writes the frame
between the previous one and this one. The view model, HUD and UI are drawn over each frame, not
interpolated. As in RTGL-D2-Metal, a present queue acquires the drawables once the frame's work
completes and presents the generated frame, then the rendered one half the frame's GPU time
later, so the render thread never waits for the display. Presented screenshots capture the
rendered frame.

## Diagnostics

- `rtx.native.gpuPassTimers` and the Diagnostics tab: GPU time per pass. MetalFX's own work is
  timed by a marker pass after it.
- `REMIX_GPU_PASS_LOG=N`: every N frames, logs the passes' mean GPU times (any host).
- `REMIX_RAY_PROFILE=file`: the ray profile (`pathtrace/ray_profile.cpp`'s CSV columns).
- `REMIX_PIPELINE_THREADS=N`: concurrent pipeline compiles (default: cores but two, at most one
  per 2 GB of memory).
- `pipelines.request` (the API's dev requests): the compiled pipelines.

## Differences from the Vulkan backend

- The scene signature leaves out `Frame::animationTime`, so accumulation and the reuse of an
  unchanged frame's output continue while water scrolls.
- Exiting without `Shutdown` leaves the GPU state to the process's end: Apple's frameworks have
  torn down their statics by then.

## Not on Metal

Split local lighting (torch and emissive families, the split direct pass and the split denoiser
with its emissive NRD signal) and ReSTIR DI run as on Vulkan; with the MetalFX denoiser, as with
Ray Reconstruction, the families are not denoised apart.

Host passes (`remixapi_HostPassEXT`) are `runtime/host_passes.cpp`'s API side with the passes
replayed in `metal/host_passes.mm`: a host's SPIR-V is translated to MSL when it is registered, by
SPIRV-Cross built into the library (`third_party/spirv-cross`, the build-time translation's
revision and patch), and passes draw, copy and clear directly into the frame's texture.

Asynchronous presentation (`rtx.native.asyncPresent`) follows the Vulkan runtime: Present waits
for the render thread's previous frame, builds the next on the caller's thread (settings, the frame
capture with `NativeResources::publishFrame` into a second `metal::Raster` that holds the GPU state,
particles, the developer UI, the published statistics) and hands it to the render thread, which
records, commits and presents it from a copy of the frame's settings. While it works the
statistics getters return the last completed frame's values.

Gradient-mapped albedo (`remixapi_MaterialGradientMapEXT`) and wound cuts
(`remixapi_InstanceInfoWoundCutEXT`) are path traced as on Vulkan (the wound vertices are
evaluated by `vertex-wound.comp`); the raster backend does not draw them on Metal yet.

Not ported: low latency (NVIDIA Reflex has no Metal counterpart); its setting is accepted and
reported as unavailable. ORCA cannot be translated: its 64-bit `atomicAdd` has no MSL equivalent. USD capture
and mods (`-Dusd=true`) are untested on macOS.
