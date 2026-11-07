# Ray-tracing GPU optimizations

The Performance tab's **GPU optimizations** panel holds path-tracing options
that do not change what is rendered. Opacity micromaps need an optional Vulkan
extension and are greyed out without it.

## Opacity micromaps

`rtx.native.pathtrace.opacityMicromaps` (0/1, default 0) and
`rtx.native.pathtrace.opacityMicromapLevel` (1-8, default 4) attach
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
  stored as unknown-opaque, so the shader still decides it. Compressed and
  dynamic textures and moving texture coordinates prove nothing from texels.
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

Baking runs on the CPU when a surface's partition is first proved, so enabling
micromaps (or a new alpha-tested mesh) costs one proof per surface. Changing
either setting proves the kept static set again.

One difference from the shader-only path: subsurface probe rays
(`subsurface.glsl`) count every surface crossing without an alpha test, so a
cut-out micro-triangle is no longer counted there.

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
