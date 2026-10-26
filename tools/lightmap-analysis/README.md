# Offline directional lightmap controls

`directional.py` provides double-precision mathematical controls for evaluating
compact lightmap candidates against captured cosine-convolved RGB SH. It requires
NumPy. It is not imported by the renderer and changes no bake or runtime format.

Run the mathematical checks from the repository root:

```powershell
python -m unittest discover -s tools/lightmap-analysis -p test_directional.py -v
```

`from_sh` converts the stored nine RGB coefficients into a world-space quadratic
polynomial. `fit_surface` stores fifteen scalars and uses the geometric normal
supplied at decode time. It preserves value and angular gradient at that normal,
before clamping. The quadratic residual identity is documented in the function.

This local fit can lose substantial energy away from the encode normal. Linear
filtering commutes with encoding only for a fixed geometric normal. Varying
normals, chart seams, normal maps, colored light, displacement interpolation and
rough specular response require separate qualification. Corrected constants can
be signed; the current positive-HDR L0 denoising contract cannot simply be reused.
Scalar count alone establishes neither texture memory nor performance savings.

These controls do not reproduce Valve's encoder, packed texture formats or bake
implementation. Store captured inputs, study scripts and results under
`_work/reports`, with source hashes and the scope of each comparison.
