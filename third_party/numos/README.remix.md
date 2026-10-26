# NUMOS sky, atmosphere and volumetric clouds

NUMOS: https://github.com/Kim2091/numos-sky (revision in `upstream.json`).
Copyright 2026 Kim2091 (Sparkles); Apache License 2.0. NUMOS includes MIT and zlib
portions (NVIDIA RTX Remix, community dxvk-remix forks, the Blender sky model, DXVK),
listed in its `THIRD_PARTY_NOTICES.md` with their licence texts in `licenses/`.

Sky and clouds: NUMOS by Kim2091 (Sparkles).

`numos-sky` is a git submodule and is never modified. Check it out without recursion:

    git submodule update --init third_party/numos/numos-sky

Its own submodules (`external/imgui`, `external/glfw`) belong to the NUMOS viewer and
are neither built nor shipped. `meson.build` here builds the CPU core library only
(no viewer, no reference Vulkan backend). `-Dsky=auto` builds without NUMOS when the
submodule is absent.

Packages carry `NUMOS.txt` (licence), `NUMOS-NOTICE.txt`,
`NUMOS-THIRD_PARTY_NOTICES.md` and the `NUMOS-LICENSE-*.txt` portions. The night sky
data in `numos-sky/assets/nightsky` is not shipped.
