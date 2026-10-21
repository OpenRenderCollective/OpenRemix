#include "forward/bake/reference_capture.h"
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <stdexcept>

namespace remix::forward::bake {
  std::optional<ReferenceCapture> ReferenceCapture::fromEnvironment() {
    const char *path = std::getenv("REMIX_DEBUG_BAKE_REFERENCE_POINTS");
    if (!path || !*path) return {};
    return load(std::filesystem::u8path(path));
  }
  ReferenceCapture ReferenceCapture::load(const std::filesystem::path &path) {
    if (std::filesystem::file_size(path) > 4096)
      throw std::invalid_argument("Reference capture input exceeds 4 KiB");
    std::ifstream input(path);
    ReferenceCapture result;
    if (!(input >> result.samples) || result.samples < 64 || result.samples > 8192)
      throw std::invalid_argument("Reference capture requires 64..8192 samples");
    while (input >> std::ws && input.peek() != std::char_traits<char>::eof()) {
      Point point{};
      if (result.points.size() == 16 || !(input >> point.position[0] >> point.position[1] >> point.position[2]))
        throw std::invalid_argument("Reference capture requires at most 16 XYZ positions");
      for (float value : point.position)
        if (!std::isfinite(value)) throw std::invalid_argument("Non-finite reference position");
      result.points.push_back(point);
    }
    if (result.points.empty()) throw std::invalid_argument("Empty reference capture");
    result.m_output = path;
    result.m_output += ".results.json";
    result.m_results.resize(result.points.size());
    result.m_completed.resize(result.points.size());
    return result;
  }
  void ReferenceCapture::consume(uint32_t first, std::span<const Result> values) {
    if (first > points.size() || values.size() > points.size() - first)
      throw std::out_of_range("Reference capture readback range");
    for (size_t i = 0; i < values.size(); ++i) {
      if (m_completed[first + i]) throw std::logic_error("Duplicate reference readback");
      for (const auto &coefficient : values[i].irradiance)
        for (float value : coefficient)
          if (!std::isfinite(value)) throw std::invalid_argument("Non-finite reference irradiance");
    }
    for (size_t i = 0; i < values.size(); ++i) {
      m_results[first + i] = values[i];
      m_completed[first + i] = true;
    }
    received += uint32_t(values.size());
    if (received != points.size()) return;
    std::ofstream output(m_output, std::ios::trunc);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output << std::setprecision(9) << "{\"version\":1,\"sceneHash\":\"" << sceneHash
           << "\",\"samples\":" << samples << ",\"bounces\":" << bounces
           << ",\"combinedLighting\":" << (combinedLighting ? "true" : "false") << ",\"points\":[";
    for (size_t i = 0; i < points.size(); ++i) {
      if (i) output << ',';
      output << "{\"position\":[";
      for (uint32_t c = 0; c < 3; ++c) output << (c ? "," : "") << points[i].position[c];
      output << "],\"irradiance\":[";
      for (uint32_t j = 0; j < 9; ++j) {
        output << (j ? ",[" : "[");
        for (uint32_t c = 0; c < 3; ++c) output << (c ? "," : "") << m_results[i].irradiance[j][c];
        output << ']';
      }
      output << "]}";
    }
    output << "]}\n";
    output.flush();
  }
}
