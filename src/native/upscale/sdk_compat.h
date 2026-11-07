#pragma once
// The unmodified SDK uses Microsoft's bounded CRT functions. These shims are
// private to its translation units; no platform-specific API reaches the
// renderer.

#include <algorithm>
#include <bit>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <locale>
// Keep the vendor utility header after its standard-library dependencies.
#include <FidelityFX/host/ffx_util.h>
using std::abs;
#ifndef _WIN32
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#define sprintf_s std::snprintf
#define swprintf_s std::swprintf
inline int wcscpy_s(wchar_t *out, size_t size, const wchar_t *in) {
  if (!out || !in || !size)
    return EINVAL;
  if (std::wcslen(in) >= size) {
    out[0] = 0;
    return ERANGE;
  }
  std::wcscpy(out, in);
  return 0;
}
template <size_t N> int wcscpy_s(wchar_t (&out)[N], const wchar_t *in) { return wcscpy_s(out, N, in); }
inline int strcpy_s(char *out, size_t size, const char *in) {
  if (!out || !in || !size)
    return EINVAL;
  if (std::strlen(in) >= size) {
    out[0] = 0;
    return ERANGE;
  }
  std::strcpy(out, in);
  return 0;
}
inline int wcstombs_s(size_t *converted, char *out, size_t size, const wchar_t *in, size_t count) {
  if (!out || !in || !size)
    return EINVAL;
  size_t result = std::wcstombs(out, in, std::min(size - 1, count));
  if (result == size_t(-1)) {
    out[0] = 0;
    return EILSEQ;
  }
  out[result] = 0;
  if (converted)
    *converted = result + 1;
  return 0;
}
#endif
