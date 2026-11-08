# SPIRV-Cross

Sources from [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) (Apache License 2.0,
`LICENSE`), commit `57e4efcc84112abcd426dc5ec248b5339bee04f2`, the revision
`scripts/shaders/toolchain.lock.json` pins for the build-time translation of the Metal
shaders. Only the parser, IR, CFG, GLSL base and MSL backend are vendored; the CLI, the C API
and the other backends are not. `sources.sha256` lists the checksums.

They carry OpenRemix's three MSL fixes, `openremix-msl.patch` (the same file as
`scripts/shaders/patches/spirv-cross-msl.patch`): physical pointer members copied as values,
atomics on one vector lane, and `ray` reserved. Update both together.

`meson.build` builds it for Metal builds only, in a namespace of its own
(`SPIRV_CROSS_NAMESPACE_OVERRIDE`). The Metal runtime translates the SPIR-V shaders a host
registers for its host passes (`src/native/metal/host_passes.mm`).
