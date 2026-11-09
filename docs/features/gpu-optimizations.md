# Ray-tracing GPU optimizations

The Performance tab's **GPU optimizations** panel holds path-tracing options
that do not change what is rendered. Opacity micromaps need an optional Vulkan
extension and are greyed out without it.

## Opacity micromaps

`rtx.native.pathtrace.opacityMicromaps` (0/1, default 0) and
`rtx.native.pathtrace.opacityMicromapLevel` (1-8, default 8) attach
`VK_EXT_opacity_micromap` data to alpha-tested geometry. Traversal settles
micro-triangles that are always opaque or always cut out; only the rest reach
the shader's alpha test (`acceptCandidate`). The device needs
`VK_EXT_opacity_micromap` and `VK_KHR_synchronization2`
(`runtime/device.cpp`); Metal has no equivalent.

- **Proof, not approximation.** `scene::OpacityCache` already proves whole
  triangles opaque from the CPU texture copy, the alpha combiner, vertex alpha,
  tint and the compare function, with filtering, wrapping and rounding margins.
  `OpacityCache::bakeMicromap` applies the same interval proof to each
  micro-triangle's texel footprint, and also proves "always fails" from the
  footprint's alpha range (a min/max pyramid of mip 0). Anything unproved is
  stored as unknown-opaque, so the shader still decides it. Textures kept
  block-compressed ("Keep BC textures compressed") have no decoded copy:
  `compressTexture` records each mip-0 4x4 block's alpha range instead
  (`CompressedPixels::alphaBlocks`, widened by one step for hardware decoding
  differences), and the pyramid starts at block cells. Dynamic textures and
  moving texture coordinates prove nothing from texels.
- **Eligible surfaces** are those the opaque-triangle split already handles:
  double-sided (or one-sided with hardware face culling), not blended, no
  transmission, compare LESS/LEQUAL/GREATER/GEQUAL. Decals are excluded because
  visibility rays skip them before the alpha test.
- **Subdivision** is chosen per triangle from its texel area (about two
  micro-triangles per texel, since every footprint is widened by half a texel
  of filtering; up to the level setting and the device limit). Proved
  regions stop subdividing early; uniform triangles use the special indices and
  store no data; identical micro-triangle arrays are shared. A partition with
  nothing proved gets no micromap.
- **Identity.** A micromap belongs to its immutable opacity partition and is
  part of the cache key (the level) and of the texture check: a replaced texture
  always proves it again. Mesh view keys and the BLAS input content include its
  hash and indices, so the geometry cache, refits and texture replacement never
  reuse a stale one. A refit keeps its BLAS's micromap; a changed micromap
  rebuilds the BLAS.
- **GPU objects** (`pathtrace/geometry.cpp`, `pathtrace/blas.cpp`). Each BLAS
  has one micromap holding its surfaces' micro-triangles; every non-opaque
  geometry points at it with an index per triangle, stored in the BLAS input.
  Micromaps are built in the same command buffer just before the BLAS builds,
  and every acceleration structure built with one keeps it alive (including
  compacted copies).
- **Order.** `scene::microTriangleIndex` is the specification's hierarchical
  ("bird curve") order. `remix-micromap-test` checks it against anchors
  measured on an RTX 5080 with `tools/vulkan-probes/shaders/micromap.comp`
  (the probe tool has no host mode for it yet).

- **Level.** Foliage cards span many texels per triangle, so low levels leave
  most micro-triangles straddling a cutout edge. On `ep2_outland_06a`'s forest
  (Garry's Mod, RTX 5080) level 4 left 76% of micro-triangles unknown and level
  8 34%, which removed 66% of the shader alpha tests; level 8 added no
  measurable VRAM. Hence the default of 8.
- **Background baking.** The triangle partition is proved on the frame path
  as before, but the tracer bakes micromaps on two background threads
  (`OpacityCache::setAsyncMicromaps`): `partition()` returns the partition at
  once, marked `micromapPending` and traced with the shader's alpha test, and
  `collectMicromaps()` (start of `prepareScene`) installs finished bakes as
  replacement partitions. Frame draws find them on their next lookup; units of
  the persistent static set holding a pending partition take the replacement
  and are rebuilt (`StaticScene::sync`), so their BLAS gets the micromap a few
  frames later. A level change drops queued bakes (an epoch); a pending entry
  whose bake was dropped counts as a miss for `find()` and is queued again by
  `partition()`. At level 8 the forest bakes in about 7 s without lowering the
  frame rate, where the synchronous bake stalled the game for about 3 s.
  `REMIX_SYNC_MICROMAPS` bakes within the frame (the GPU micromap test).
- **Content reuse.** Bakes are also keyed by surface content (indices, texture
  coordinates, vertex alpha) with the cache key and partition. A mesh uploaded
  again with new positions only (swaying foliage, every frame) takes the
  micromap of an earlier copy, or waits for its bake in flight, instead of
  baking again: in the background, a bake per copy never caught up with the
  next copy.

Baking runs on the CPU when a surface's partition is first proved, so enabling
micromaps (or a new alpha-tested mesh) costs one proof per surface. Changing
either setting proves the kept static set again. `REMIX_OPACITY_DIAGNOSTICS`
prints the frame's micromap counts (micro-triangles opaque, transparent and
unknown) and the bakes queued and installed so far.

One difference from the shader-only path: subsurface probe rays
(`subsurface.glsl`) count every surface crossing without an alpha test, so a
cut-out micro-triangle is no longer counted there.

`rtx.native.pathtrace.shadowMicromap2State` (0-2, default 0) lets shadow-type
rays (sun, sky and local light visibility) skip the shader's alpha test on
micromapped geometry altogether: the bake stores each unproved micro-triangle
as unknown-opaque or unknown-transparent by the alpha estimated at its centre,
and these rays force the two-state reading of the micromap. 1 applies it to
the indirect pass, the radiance cache and every sky ray; 2 also to the primary
surface's sun and local light shadows. Camera and bounce rays keep the exact
test. A cutout's shadow edge then follows the micro-triangles (about half a
texel at the level the triangle got) instead of the texels. Geometry without
a micromap (paint, moving texture coordinates, dynamic textures, anything the
bake proved nothing about) is traced as before.

## Wavefront indirect lighting

`rtx.native.pathtrace.wavefrontIndirect` (0/1, default 1) runs the separate
indirect pass (one sample a pixel, denoised lighting) as small kernels
(`pathtrace.comp`, `WAVEFRONT_PASS`) instead of one kernel that carries a
whole path. Each compiles alone, with fewer registers than the whole path
needs, and the bounce kernels launch only for the paths still going:

- **start** (over the screen): the primary surface, the first direction and
  the sky rays' directions and weights; a pixel without a path (sky,
  blended) gets its result here.
- **sky**: the sky rays' shadow rays (and sky-guide training) over the paths
  that have them.
- per split bounce (`rtx.native.pathtrace.wavefrontBounces`, 1-16, default
  1): **arguments** (one invocation: the indirect dispatch over the bounce's
  list of live paths), **trace** (the bounce ray's receiver query, and
  whether paint lies before the receiver) and **shade** (that bounce,
  appending the paths that go on to the next list): the paths with paint in
  front of their receiver in a kernel that peels it, the rest in one with the
  paint peel compiled out (`shade painted`, `shade unpainted`). A shade
  kernel stops at a blended layer; the next trace kernel or the tail takes
  the layer behind it.
- **tail**: the remaining bounces of the paths left, in one kernel (a kernel
  per bounce of the long roulette tail costs more in launches than its few
  paths do).
- **finish** (over the screen): the denoiser inputs and the opaque pass's
  transport, as the single kernel writes them.

`rtx.native.pathtrace.wavefrontShadowQueue` (0/1, default 1) moves the split
bounces' sun, sky and local light shadow rays into a **shadow** kernel over
their own list (the local light's sample is still drawn in the shade kernel).

The path state between kernels (origin, direction, throughput, radiance and
random state exact, the rest as halves) is in per-pixel records, allocated
while the option is on: 220 bytes a pixel, plus 16 a sky sample or 48 with
the shadow queue, whichever is more (268 by default: 133 MiB at 960x540,
942 MiB at 2560x1440). Neither the receiver query nor the alpha test draws
random numbers, so a path continues as the single kernel's would; images
match within noise. The kernels compile with the temporal lighting passes, or
in the background when the option is turned on later; until all have, the
single kernel runs. GPU timings name them `Trace indirect (wavefront ...)`.
On `ep2_outland_06a`'s forest (Garry's Mod, RTX 5080, 960x540 render, paint)
the indirect pass took 3.30 ms as one kernel and 2.18 ms as wavefront
kernels with the shadow queue (frame 8.28 to 7.33 ms).

## Tests

- `remix-micromap-test`: hardware-measured order anchors, table nesting, and a
  brute-force check that every point of an opaque (transparent) micro-triangle
  passes (fails) the shader's alpha model.
- `remix-opacity-micromap-test` (GPU): identical images with micromaps off, on
  and off again, for ray-traced primary visibility, sun shadows and a refitted
  skinned mesh, and fewer shader alpha tests in the ray profile.
- `remix-shader-execution-reordering-test` (GPU): the SER pass runs and matches
  compute within rounding; `REMIX_TEST_SER_BENCHMARK=1` prints 1080p timings,
  and `REMIX_TEST_SER_MATERIALS=legacy|pbr|mixed` selects the material classes
  (legacy and PBR are the only classes with different shading code, so a mixed
  scene costing no more than the pure ones means there is no divergence for
  per-class hit shaders to remove).
  `REMIX_PATH_FIXED_SEED` makes every frame draw the same samples.
