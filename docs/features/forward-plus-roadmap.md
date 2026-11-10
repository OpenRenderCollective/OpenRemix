# Forward+ roadmap

Forward+ is opt-in. Its default Source 2 preset uses baked surface lighting,
probe volumes for moving receivers, mixed shadows and parallax cubemaps. SSR
defaults off. See the [controls](forward-plus-controls.md) and
[lighting architecture](../architecture/forward-lighting.md).

The [implementation history](https://github.com/OpenRenderCollective/OpenRemix/blob/7c136aea33776297678613434d8abb5166a6b84e/docs/features/forward-plus-roadmap.md)
preserves earlier checkpoints, measured bake results and rejected variants.

## Review and release

- Merge [OpenRemix #42](https://github.com/OpenRenderCollective/OpenRemix/pull/42)
  before [SourcePT #123](https://github.com/sambow23/SourcePT/pull/123) so the
  wrapper finds the new public headers. Use the [fresh-clone setup](forward-plus-setup.md)
  and pin both revisions for verification.
- Finish the scope review and qualify the exact PR heads. Recheck live warm
  transitions and 1440p/4K visuals and timing on Construct and Crossfire.
  Earlier qualified replays matched fresh and cached images.
- Retain the indexed path for overlapping and styled lights. The compact RGB
  atlas, packed style composition, split clusters and viewmodel depth-prepass
  trials remain in the experiment history.
- Verify Office's planning-budget fix, the Construct grazing-angle wedge
  and the Dust II CT spawn leak. Renderer allocations stabilized in repeated
  transitions. Compare longer runs to find the owner of process commit growth.

## Visual quality

- Capture the original Trainstation playground overlay and leaf-bed angles.
  The sampled wall views retained the ivy and graffiti. Check detail-prop
  depth and grazing angles with the exact camera and map content.
- Fix the Trainstation storefront seam where a chart filters across an
  intersecting wall. Both neighbouring samples have valid visibility.
  Compare chart subdivision and discontinuity-aware sampling.
- In Construct, check the skylit hall on props, the two-light room behind
  spawn, the connector bend, white room and secret room. Compare sun edges,
  bicubic filtering, chart seams and lighting continuity.
- Concentrate finer probes near playable floors and typical receiver
  heights using shared placement hints. Keep sparse fallback coverage
  and independent diffuse lighting when reflections are unavailable.
- Verify styled-light fades, point-shadow filtering and shadow stability
  during camera motion. Keep static visibility baked and off-screen caster
  submission independent. Check probe-lit viewmodels, dynamic receiver
  shadows and isolated self-shadowing.
- Verify decals inherit receiver lighting. Track Forward+ support for
  projected textures and volumetric fog against the path-tracing features.
- Compare HDR exposure, sky lighting and reflection normalization with
  path tracing. Use fixed poses and compare raised blacks and washed-out
  interiors with matched output settings.
- Validate cubemap overlap, feathering, roughness and outdoor coverage.
  Use Construct, Black Mesa and available Half-Life port volumes to improve
  automatic fitting. Preserve authored-over-auto fallback and independent
  capture origins. Improve native editor usability and live sidecar checks.
  A wider C# Hammer-style replacement editor and water-only SSR are deferred.

## Performance and bake

- Target 300–400 FPS at the chosen quality settings. Native 4K at 360 FPS
  leaves 2.78 ms per frame. Report median and p95 with focus, warm-up,
  settings, resolution and weapon. Separate live frames from replay timings.
- Profile Trainstation at 1080p with and without `gmod_mcore_test 1`, then
  isolate native 4K GPU costs. Separate opaque and weapon shading, probe
  interpolation, mixed shadows, BRDF, reflection selection and water.
- Compare CS2 tile and depth-bin traversal, VRF visibility and batching, and
  s&box shading interfaces. Qualify GPU light culling and depth aggregation
  with active local lights and large visibility-limited maps.
- Evaluate a compact common case for indexed baked lights with exact overflow.
  Reuse scalar layers only for disjoint filtered footprints. Qualify scenes
  with more than four overlapping lights, styled fades and mixed casters.
  Check authored BSP cutoffs before enabling them by default.
- Profile overflow visibility, transport, denoising, probe and reflection
  publication. Compare cold bakes and restores. Track peak memory and cache
  size, and verify that walking and changing weapons keep the bake stable.
- Keep new lighting providers opt-in. Continue modular experiments with
  DDGI, radiance cascades, radiance volumes, virtual shadow maps and distance
  reflections separately from the default preset. Keep Source-specific
  materials in the adapter.

## Map coverage

Verify mounted content and the loaded BSP before accepting a capture.

| Map | Coverage |
| --- | --- |
| `gm_construct` | Skylit hall, white room, terrain, props, viewmodels and reflections. |
| `crossfire` | Light-heavy views, weapon changes, mixed shadows and host overhead. |
| `c1a0`, `c1a0d` | Interior lighting, point shadows, probes and bake stability. |
| `cs_office`, `de_dust2` | Planning limits, styled lights, sun seams and map transitions. |
| `gm_fork` | Large-world visibility, distant casters, bake budgets and memory. |
| `d1_trainstation_02` | Courtyard, foliage and overlay depth, transitions and 4K cost. |
| `bm_c1a1b` | Complex coverage and comparison with authored parallax bounds. |
| `d2_lostcoast`, `dm_steamlab` | Bake failures, transport, water and cache restore. |
| Combined Half-Life addon maps | Long corridors, connected volumes, bake cost and traversal stutter. |
