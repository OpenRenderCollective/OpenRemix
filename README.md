> This project uses AI-generated code
# OpenRemix
﻿
A performance-focused, multi-platform alternative to [dxvk-remix](https://github.com/NVIDIAGameWorks/dxvk-remix). 
﻿
## Features
- Raster rendering and path tracing, with raster or ray-traced primary visibility, this is used to pass through raster effects/overlays into the Path-traced view 
- PBR materials, normal maps, decals, alpha cutouts, and translucent surfaces.
- Mesh instancing, skinning, and animated textures.
- NRD/SIGMA denoising.
- SHARC radiance caching and progressive accumulation.
- Native TAA, FSR 3.1, DLSS Super Resolution and Ray Reconstruction.
- FSR FG and DLSS FG
- Autoexposure, threshold-free bloom, and Prism tonemapping.
- OpenUSD support for loading existing Remix replacement mods.
- Remix compatible render API

## Projects Using OpenRemix
- [BetaRT](https://github.com/sambow23/BetaRT/tree/openremix)
- [garrys-mod-rtx-remixed-2](https://github.com/sambow23/garrys-mod-rtx-remixed-2)
- [UnityRTX](https://github.com/Veitaserum4/UnityRTX/tree/unity-bridge-openremix)
- [xash3d-fwgs (from Veitaserum4)](https://github.com/Veitaserum4/xash3d-fwgs-openremix)

## Building

See [platforms.md](https://github.com/OpenRenderCollective/OpenRemix/blob/main/docs/platforms.md)

## Credits

- NVIDIA: NRD, SIGMA, SHARC and DLSS. AMD: FidelityFX FSR. Pixar and OpenUSD contributors.
- SDL, Vulkan/MoltenVK, Dear ImGui, stb and bcdec contributors.
- Jane, TheGreatHmmmmm, and surething on the `RTX Remix Showcase` server for guiding me in the right direction.
- TheGreatHmmmmm for the Prism tonemapper, Bloom, Gamma 2.2, and HDR implementations.
- [sparkles✨](https://github.com/Kim2091) for their Numos implementation, please [donate to them when you get the chance](https://ko-fi.com/kim20913944)

See [third-party notices](ThirdPartyLicenses.txt)
