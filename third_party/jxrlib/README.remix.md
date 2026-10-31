# jxrlib

Unmodified sources from [jxrlib](https://github.com/4creators/jxrlib), the JPEG XR
reference library Microsoft published under the BSD 2-Clause license (`LICENSE`),
commit `f7521879862b9085318e814c6157490dd9dbbdb4` (2017-06-15; upstream is otherwise
inactive). Only the codec core (`image/sys`, `image/encode`, `image/decode`), the glue
library (`jxrgluelib`) and `common/include` are vendored; the apps, test library, x86
asm paths and Visual Studio projects are not. `sources.sha256` lists the checksums.

`meson.build` builds the ANSI path (`__ANSI__`, `DISABLE_PERF_MEASUREMENT`) on every
platform. On Windows it adds `__LP64__=1` (pointer-sized integers) and force-includes
`remix_compat.h`, our file, for the two clashes with the Windows SDK headers. The
runtime uses it to write scRGB JPEG XR screenshots (`src/native/runtime/jxr_writer.cpp`).
