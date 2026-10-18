#pragma once
// Floating-point std::from_chars/to_chars, which Apple's libc++ provides only
// from macOS 26 (from_chars) and 13.3 (to_chars), later than the macOS builds'
// deployment target. Apple builds use the C locale's strtod/snprintf, which
// read and write the same text; other platforms use <charconv>.
#include <charconv>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#if defined(__APPLE__)
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <xlocale.h>
#endif

namespace remix {
  // Parses all of text as a float in from_chars's format (no leading
  // whitespace, '+' or hexadecimal prefix).
  template <typename T> bool parseFloat(std::string_view text, T &value) {
#if defined(__APPLE__)
    if (text.empty() || text[0] == '+' || std::isspace(static_cast<unsigned char>(text[0])) ||
        text.find_first_of("xX") != std::string_view::npos)
      return false;
    std::string copy(text);
    char *end = nullptr;
    errno = 0;
    T parsed;
    if constexpr (std::is_same_v<T, float>)
      parsed = strtof_l(copy.c_str(), &end, nullptr);
    else
      parsed = strtod_l(copy.c_str(), &end, nullptr);
    // Out of range, as from_chars reports it.
    if (end != copy.c_str() + copy.size() || (errno == ERANGE && std::isinf(parsed)))
      return false;
    value = parsed;
    return true;
#else
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
#endif
  }

  // Writes value with max_digits10 significant digits (chars_format::general).
  template <typename T> std::string formatFloat(T value) {
    char buffer[64];
#if defined(__APPLE__)
    int length = snprintf_l(buffer, sizeof(buffer), nullptr, "%.*g", std::numeric_limits<T>::max_digits10,
                            static_cast<double>(value));
    return {buffer, length > 0 ? size_t(length) : 0};
#else
    auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general,
                                std::numeric_limits<T>::max_digits10);
    return {buffer, result.ptr};
#endif
  }
} // namespace remix
