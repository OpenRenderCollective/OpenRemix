// Force-included into the jxrlib C sources on Windows, where the ANSI path meets
// the Windows SDK. Not an upstream file.
#pragma once
// UCRT declares _byteswap_ulong(unsigned long); strcodec.c defines its own U32
// fallback. Declare the UCRT one first, then give jxrlib's a private name.
#include <stdlib.h>
#define _byteswap_ulong remix_jxr_byteswap_ulong
// The SDK's sal.h defines __specstrings, which hides wmsal.h and its *_win annotations.
#ifndef __in_win
#define __in_win
#endif
#ifndef __out_win
#define __out_win
#endif
