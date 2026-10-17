# Forward+ delivery roadmap

The intended result is a cheaper, Source-2-style rendering backend for GMod: forward PBR shading with
MSAA, hardware map-load surface lightmaps and probes, attributed baked direct lighting, soft static
shadows combined with dynamic-object shadows, automatic tolerant parallax cubemaps, rough SSR, and
optional hardware reflections/GI. Replacement materials must be shared with path tracing. Water must
remain compatible with the same authored optical materials.

This roadmap tracks delivery requirements. Detailed measurements and run evidence belong in local
reports. An implementation or synthetic test alone does not close an integration or visual-quality gate.

## Current capabilities and open requirements

| Requirement | Current state | Remaining proof or work |
| --- | --- | --- |
| Modular forward PBR, clustered lights and 1x/2x/4x MSAA | Implemented | Refresh current-build engine coverage through the integration milestone below |
| World-metric surface charts, atlas budgets and irradiance probes | Implemented; live pool bake and cached reload exercised | Inspect connected-face continuity, seam donors and budget coarsening in representative map views |
| Soft finite emitters, attributed direct diffuse and live specular | Implemented | Compare visible penumbrae and light separation in the live engine |
| Dynamic-object shadows combined with baked static shadows | Implemented; captured static casters excluded; world-metric sun bias and one indoor live case exercised | Local-light receiver quality, characters and static membership after map changes |
| Automatic parallax cubemap bounds with tolerance for room details | Implemented | Review room transitions, bevels, interior props and reflection leaks in map views |
| Rough SSR, optional hardware reflections and live GI | Implemented | Refresh actual-engine toggles and shared-scene lifetime coverage; continue measured performance work where needed |
| Shared USD/PBR material replacements | Implemented; plain Source roughness fallback corrected | Refresh late material arrival and broader material/BRDF parity in the current engine build |
| Water optics, normals, HDR refraction, SSR/RT and underwater composition | Implemented and opt-in; above-water engine captures exercised | Underwater/lifecycle validation, noise and visual comparison with the path-traced material response |
| Optional OIDN indirect-lightmap filtering | Implemented and opt-in | Connected-seam review and lower-sample quality comparisons; distribution of optional dependencies |
| Usable build and deployment | Feature branches/remotes retained; Windows runtime bundler available | Remaining platform/dependency coverage, lifecycle smoke tests and final delivery instructions |

## Next milestones, in order

1. **Complete live GMod integration.** Pool startup, a fresh bake, an exact cached reload and a moving
   actor have been exercised in the isolated installation. Continue with a solid receiver for mixed
   shadows beyond the exercised indoor sun case, below-water views, renderer/RT switches and a map reload/change. Record binary hashes and
   dependencies. Require actual
   engine logs, active baked lighting and captures; investigate crashes, input restarts, missing assets
   or new validation errors before extending coverage. Earlier live-game logs are historical evidence.
2. **Close visible lighting-quality gaps.** Use fixed map views and a high-sample reference to inspect
   connected chart seams, static/dynamic shadow composition, indoor/outdoor probe transitions and
   parallax bounds. Compare lower indirect sample counts separately from direct-shadow sampling.
   Keep changes tied to an observed artifact and a repeatable comparison.
3. **Finish material and water parity.** Exercise authored/replaced normals, roughness, emission,
   transmission distance and IOR in the live engine. Document supported approximations and repair
   discrepancies that break compatibility with the shared material system.
4. **Package and deliver.** Check clean builds with and without OIDN, include required runtime plugins
   and notices, document settings and defaults, and repeat the integration smoke test from the delivered
   layout. Keep the upstream `main` branches and the user's feature-branch remotes intact.

## Work discipline

Each checkpoint records its changed files/commit, evidence, limits, and the next action. A failed
experiment remains a failed experiment. A running process is tracked by its actual handle until it
finishes. Performance improvements include before/after configuration and output comparisons.
Milestones close against their stated gates; newly useful features do not replace unfinished gates.
