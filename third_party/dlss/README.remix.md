# NVIDIA DLSS SDK 310.9.1

Unmodified NGX headers, Linux x86-64 and Windows x86-64 release libraries from
[NVIDIA/DLSS](https://github.com/NVIDIA/DLSS/tree/374959484e79a640feaba44c93ac8cfb0a03f5b5),
revision `374959484e79a640feaba44c93ac8cfb0a03f5b5`.
`sources.sha256` records the downloaded artifacts; their Git blob hashes were
checked against that revision's tree. `LICENSE.txt` is the upstream license.
The Windows frame-generation library (`nvngx_dlssg.dll`) is included for DLSS Frame
Generation; the Linux one and the development libraries are excluded.

Linux links `libnvsdk_ngx.a`; native Windows clang-cl links `nvsdk_ngx_d.lib`
(the dynamic CRT variant). These are dispatch libraries, with no load-time NGX
runtime dependency. The renderer queries both features before creating the
Vulkan device, then explicitly searches the adjacent `dlss/310.9.1` directory.
Meson development builds use the adjacent `upscale` artifact directory. Both
paths require matching packaged release files before enabling the feature.

SR uses model preset K and RR uses model preset E by default.

Ray Reconstruction's CNN model (`rtx.native.pathtrace.dlssRayReconstructionModel =
cnn`, preset C by default) is no longer in this release: its last release, 310.2.1
([revision `af199869c51cf2d71cc64d3db5064788ff38eb02`](https://github.com/NVIDIA/DLSS/tree/af199869c51cf2d71cc64d3db5064788ff38eb02)),
supplies only its RR libraries, `lib/*/rel-310.2.1/`, unmodified (Git blob hashes
checked against that tree), with that release's license as `LICENSE-310.2.1.txt`.
They are installed in `dlss/310.2.1`, which NGX searches before `dlss/310.9.1`
while that model is selected (NGX restarts to change it). On an RTX 2080 at
1080p output, ultra performance, it costs about 2.8 ms against the transformer's
5.8 ms. The other new default models
in this release (SR L/M and RR F) produced internal Vulkan validation errors on
the tested Turing Linux driver. Quality modes and dimensions still come from
NGX's optimal-settings callbacks. Native Windows RTX validation remains a
separate release gate.
