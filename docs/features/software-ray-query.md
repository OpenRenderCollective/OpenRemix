# Path tracing without ray queries

On a device without `VK_KHR_acceleration_structure` and `VK_KHR_ray_query`
(GTX 16 series and Quadro T class, older AMD and Intel GPUs) the path tracer
runs the same passes over acceleration structures built on the CPU and
traversed by the shaders. `runtime/device.cpp` selects it; nothing else
(Forward+ ray effects, traced particles, the NUMOS sky, opacity micromaps)
uses it, and those stay off there.

- `REMIX_SOFTWARE_RAY_QUERY=1` uses it on a device with ray queries too
  (comparison and diagnosis); `=0` never does, so such a device rasterizes.

## How it works

- **Structures** (`pathtrace/software_bvh.h`): TinyBVH (`third_party/tinybvh`)
  builds a two-child BVH per mesh and one over the instances; both are packed
  into 16-byte words whose nodes hold their children's bounds. A bottom
  level's leaves are triangles (vertex, two edges, primitive and geometry
  index, opaque flag); the top level's are instances (both transforms,
  custom index, mask, flags, the bottom level's buffer address).
- **Bottom levels** are built with the mesh conversion on the workers
  (`convertMesh`). A deforming view keeps its tree
  (`MeshView::softwareBuilder`) and refits it while its topology stays the
  same, a skin without limit and other deformations 60 times in a row, as
  the hardware policy; a refit moves the tree's bounds in a fiftieth of a
  build's time. Skinning stays on the CPU (`REMIX_GPU_SKINNING` has no
  effect).
- **Top levels** are built on the render thread from the same
  `VkAccelerationStructureInstanceKHR` records a hardware build takes, whose
  reference is the bottom level's buffer address, and uploaded whole whenever
  the TLAS policy asks for a build or an update; an update keeps the tree
  and moves its bounds (`Tlas::softwareBuilder`).
- **Shaders**: every stage built from `pathtrace.comp` has a "software" twin
  (`scripts/shaders/generate.py`, `SOFTWARE_RAY_QUERY`), which
  `trace::shader()` returns in this mode. `shaders/software_ray_query.glsl`
  defines the `rayQuery*EXT` names over the structures, so the query sites are
  the hardware ones: candidates for non-opaque triangles, opaque ones
  committed in traversal, instance masks and flags, ray flags, front faces.
  Bindings 0 and 1 are the top levels' first words instead of acceleration
  structures.
- One traversal stack serves every query of a shader. A stack in each query
  gave each of the megakernel's 132 inlined query sites its own storage: on a
  Quadro T1000 the tracing passes took 31.8 ms with it and 13.2 ms without.
- The twins are not optimized: `spirv-opt -O` runs out of IDs and memory on
  them (27 GB reached). The driver's first compile of a stage takes minutes
  and is cached; frames are rasterized until the requested configuration is
  ready.
- The static and the dynamic scene share one query call site
  (`SOFTWARE_SCENES` in `pathtrace.comp`): a loop over the structure mask
  where the hardware stages query each scene in turn. The traversal is
  inlined at every site, and the driver's compile time grows faster than the
  code (below).
- No lean lighting variants (the stages without paint and subsurface
  scattering): one compiles for as long as the full stage and traces no
  faster here.

## Defaults on such a device

They are defaults (`Profiles::setDefault`): a profile or the game overrides
them.

- `rtx.native.pathtrace.dlssRayReconstruction` 0 and
  `rtx.native.pathtrace.upscaler` `fsr31`: the DLSS networks run without
  matrix hardware, slowly (below).
- `rtx.native.pathtrace.precompileConfigurations` 0: the other
  configurations' pipelines compile when asked for, not ahead.

## Measurements

Quadro T1000 (4 GB, driver 580.196), `remix-bench ... pathtrace grid=16
foliage=300 lights=4 window=480x270` (912x513 output on a HiDPI display),
GPU pass timers:

| | GPU frame | of which |
|---|---|---|
| DLSS Ray Reconstruction (transformer) | 116 ms | RR 81 ms, tracing 34 ms |
| NRD + DLSS Super Resolution | 68 ms | SR 26 ms, NRD 1.3 ms |
| NRD + FSR 3.1 | 40 ms | tracing 32 ms, FSR 0.7 ms |
| the same, one traversal stack | 16.5 ms | tracing 12.1 ms |

Eight meshes of 20,000 triangles re-created every frame (`grid=8 foliage=0
lights=4 churn=8 churnTris=20000 window=912x513`): the CPU geometry step took
35.9 ms building each tree again and 9.5 ms refitting them (frame 43.3 to
26.5 ms).

First compile, with neither the runtime's nor the driver's cache (`REMIX_CONFIG_DIR`
and `__GL_SHADER_DISK_CACHE_PATH` at empty directories; `pipelines.request`
in the configuration directory writes each pipeline's compile time to
`pipelines.txt`), same scene at 912x513:

| | first traced frame | slowest stages | tracing |
|---|---|---|---|
| a query site for each scene | 402 s | single-sheet transparency 398 s, indirect 165 s | 12.4 ms |
| one site for both | 185 s | single-sheet transparency about 184 s, indirect 81 s | 9.9 ms |
| and one query and one Hit site in `closest()` | 72 s | indirect and local lighting 32 s | 8.6 ms |

Six workers compile for all of that time; ten made it slower (429 s). The
lean variants added 39 minutes of compile summed over the workers after the
first traced frame (7.5 minutes of six busy cores) for the same frame time.
A stage that is small once its unused functions are dropped compiles in
0.1 to 3 s, so stripping the modules ahead (`spirv-opt` without the inliner)
would gain nothing. `closest()` was inlined seven times in the single-sheet
stage, each with three `nearest()` and three `surfaceHit()` sites, 60% of
that stage's instructions: it is now one loop with one site of each, for the
hardware stages too (one optimizes and validates as before; they were not
run, on a device without ray queries). Screenshots of the bench scene, plain,
with glass and with paint, match the stages before both changes to the
noise (mean difference under 0.1 of 255).

Tried and not kept, at the same settings: a compressed eight-wide bottom
level (TinyBVH's `BVH8_CWBVH`) traced 40% more rays in a kernel of its own
(41 against 29 million a second) but no more in the path tracer's passes
(tracing 13.1 ms against 12.1 on the small meshes above, 12.9 against 12.7
on the large ones), and refitting it took 17.8 ms where the two-child tree
takes 9.5; spatial splits (`BuildHQ`) traced no faster for two to three
times the build. Rays that stop at their first hit already run a fifth
faster than nearest-hit rays; a separate loop for them was not written.

`tests/pathtrace/software_bvh.cpp` checks the structures against direct
intersection of every triangle, with a CPU traversal written as the shader's,
through builds and refits of both levels.

## Building for such a device only

`-Dpathtrace_hardware_shaders=false` builds the ray-query stages as stubs
(each costs `spirv-opt` minutes and about 6 GB); such a build cannot trace on
a device with ray queries unless `REMIX_SOFTWARE_RAY_QUERY=1`. `-Dforward=false`
leaves the Forward+ backend out (it reports itself unsupported).
