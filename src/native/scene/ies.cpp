#include "scene/ies.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace remix::scene {
  namespace {
    // LM-63's numeric part is whitespace- or comma-separated values over any
    // number of lines.
    struct Numbers {
      std::string_view text;
      size_t at = 0;
      bool next(double &value) {
        while (at < text.size() && (std::isspace(static_cast<unsigned char>(text[at])) || text[at] == ','))
          ++at;
        if (at >= text.size())
          return false;
        const size_t end = text.find_first_of(" \t\r\n,", at);
        const std::string token(text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
        at = end == std::string_view::npos ? text.size() : end;
        char *parsed = nullptr;
        value = std::strtod(token.c_str(), &parsed);
        return parsed && *parsed == 0 && std::isfinite(value);
      }
    };
  } // namespace

  bool parseIes(std::string_view text, IesTable &table) {
    // The keywords end at the TILT line; its value says whether lamp tilt
    // data follows the photometric header's first line (INCLUDE).
    const size_t tilt = text.find("TILT=");
    if (tilt == std::string_view::npos)
      return false;
    size_t lineEnd = text.find('\n', tilt);
    if (lineEnd == std::string_view::npos)
      return false;
    const std::string_view tiltValue = text.substr(tilt + 5, lineEnd - tilt - 5);
    Numbers numbers{text, lineEnd + 1};
    double v = 0;
    if (tiltValue.substr(0, 7) == "INCLUDE") {
      // Lamp-to-luminaire geometry, then angle/multiplier pairs.
      double geometry = 0, pairs = 0;
      if (!numbers.next(geometry) || !numbers.next(pairs) || pairs < 0 || pairs > 1e5)
        return false;
      for (double i = 0; i < 2 * pairs; ++i)
        if (!numbers.next(v))
          return false;
    }
    // Lamps, lumens per lamp, multiplier, vertical and horizontal angle
    // counts, photometric type, units, width, length, height; ballast factor,
    // future use, input watts.
    double header[13];
    for (double &h : header)
      if (!numbers.next(h))
        return false;
    const double verticalCount = header[3], horizontalCount = header[4];
    if (verticalCount < 1 || horizontalCount < 1 || verticalCount > 1e4 || horizontalCount > 1e4)
      return false;
    std::vector<double> vertical(static_cast<size_t>(verticalCount)),
        horizontal(static_cast<size_t>(horizontalCount));
    for (double &a : vertical)
      if (!numbers.next(a))
        return false;
    for (double &a : horizontal)
      if (!numbers.next(a))
        return false;
    // Candela per horizontal angle, each over the vertical angles; averaged.
    std::vector<double> candela(vertical.size(), 0);
    for (size_t h = 0; h < horizontal.size(); ++h)
      for (double &c : candela) {
        if (!numbers.next(v))
          return false;
        c += std::max(v, 0.) / horizontal.size();
      }
    if (!std::is_sorted(vertical.begin(), vertical.end()))
      return false;
    // Resample to the table's angles: linear between the file's, nothing
    // outside them.
    double peak = 0;
    for (unsigned i = 0; i < kIesBins; ++i) {
      const double angle = 180. * i / (kIesBins - 1);
      double value = 0;
      if (vertical.size() == 1)
        value = angle == vertical[0] ? candela[0] : 0;
      else if (angle >= vertical.front() && angle <= vertical.back()) {
        const size_t upper = std::min<size_t>(
            std::upper_bound(vertical.begin(), vertical.end(), angle) - vertical.begin(), vertical.size() - 1);
        const size_t lower = upper - 1;
        const double span = vertical[upper] - vertical[lower];
        const double t = span > 0 ? (angle - vertical[lower]) / span : 0;
        value = candela[lower] + (candela[upper] - candela[lower]) * std::clamp(t, 0., 1.);
      }
      table[i] = float(value);
      peak = std::max(peak, value);
    }
    if (!(peak > 0))
      return false;
    for (float &t : table)
      t = float(t / peak);
    return true;
  }
} // namespace remix::scene
