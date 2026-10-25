#include "forward/bake/reference_capture.h"
#include "forward/bake/spherical_harmonics.h"
#include <algorithm>
#include <cmath>
#include <bit>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <numbers>
#include <stdexcept>

namespace remix::forward::bake {
  void ReferenceCapture::classify(const scene::BakeScene &source) {
    m_partitionClusters.assign(points.size(), -2);
    if (!source.partition || !source.partition->closed) return;
    for (size_t i = 0; i < points.size(); ++i)
      m_partitionClusters[i] = source.partition->cluster(points[i].position.data());
  }
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
    std::string token;
    input >> token;
    result.hemisphere = token == "hemisphere";
    result.surface = token == "surface" || token == "surface-offsets";
    const bool offsets = token == "sh-offsets" || token == "surface-offsets";
    if (result.hemisphere || result.surface || offsets) input >> token;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), result.samples);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() ||
        result.samples < 64 || result.samples > 8192)
      throw std::invalid_argument("Reference capture requires 64..8192 samples");
    while (input >> std::ws && input.peek() != std::char_traits<char>::eof()) {
      Point point{};
      if (result.points.size() == 16 || !(input >> point.position[0] >> point.position[1] >> point.position[2]))
        throw std::invalid_argument("Reference capture requires at most 16 XYZ positions");
      for (float value : point.position)
        if (!std::isfinite(value)) throw std::invalid_argument("Non-finite reference position");
      if (result.hemisphere || result.surface) {
        if (!(input >> point.normal[0] >> point.normal[1] >> point.normal[2]))
          throw std::invalid_argument("Surface reference requires XYZ unit normals");
        const Vec3 normal{point.normal[0], point.normal[1], point.normal[2]};
        const float lengthSquared = dot(normal, normal);
        if (!std::isfinite(lengthSquared) || std::abs(lengthSquared - 1) > 1e-4f)
          throw std::invalid_argument("Invalid surface reference unit normal");
      }
      if (offsets) {
        std::string offset;
        input >> offset;
        uint32_t firstSample = 0;
        const auto parsedOffset = std::from_chars(offset.data(), offset.data() + offset.size(), firstSample);
        if (parsedOffset.ec != std::errc{} || parsedOffset.ptr != offset.data() + offset.size() ||
            firstSample > UINT32_MAX - result.samples)
          throw std::invalid_argument("Invalid reference sample offset or overflowing sample range");
        result.sampleOffsets.push_back(firstSample);
      }
      result.points.push_back(point);
    }
    if (result.points.empty()) throw std::invalid_argument("Empty reference capture");
    result.m_output = path;
    result.m_output += ".results.json";
    if (!result.hemisphere) result.m_results.resize(result.points.size());
    result.m_completed.resize(result.tasks());
    if (result.hemisphere) result.m_sums.resize(result.points.size());
    return result;
  }
  std::vector<Point> ReferenceCapture::batch(uint32_t first, uint32_t count) const {
    if (first > tasks() || count > tasks() - first) throw std::out_of_range("Reference task range");
    std::vector<Point> result;
    result.reserve(count);
    for (uint32_t task = first; task < first + count; ++task) {
      auto point = points[hemisphere ? task / samples : task];
      if (hemisphere) {
        const auto ordinal = task % samples;
        const Vec3 normal = normalized({point.normal[0], point.normal[1], point.normal[2]});
        const Vec3 tangent = normalized(cross(std::abs(normal[2]) < .99f ? Vec3{0, 0, 1} : Vec3{1, 0, 0}, normal));
        const Vec3 bitangent = cross(normal, tangent);
        const float cosine = std::sqrt((float(ordinal) + .5f) / samples);
        const float radius = std::sqrt(1 - cosine * cosine);
        const uint32_t rotation = std::bit_cast<uint32_t>(point.position[0]) * 73856093u ^
                                  std::bit_cast<uint32_t>(point.position[1]) * 19349663u ^
                                  std::bit_cast<uint32_t>(point.position[2]) * 83492791u;
        const double phase = double(ordinal * 0x61c88647u + rotation) / 4294967296.0;
        const double phi = phase * 2 * std::numbers::pi;
        for (uint32_t c = 0; c < 3; ++c)
          point.normal[c] = normal[c] * cosine + radius *
              (tangent[c] * float(std::cos(phi)) + bitangent[c] * float(std::sin(phi)));
      }
      result.push_back(point);
    }
    return result;
  }
  void ReferenceCapture::consume(uint32_t first, std::span<const Result> values) {
    if (first > tasks() || values.size() > tasks() - first)
      throw std::out_of_range("Reference capture readback range");
    for (size_t i = 0; i < values.size(); ++i) {
      if (m_completed[first + i]) throw std::logic_error("Duplicate reference readback");
      for (const auto &coefficient : values[i].irradiance)
        for (float value : coefficient)
          if (!std::isfinite(value)) throw std::invalid_argument("Non-finite reference irradiance");
      if (!hemisphere) {
        for (const auto &moment : values[i].moments)
          for (float value : moment)
            if (!std::isfinite(value)) throw std::invalid_argument("Non-finite reference distance moments");
        for (float value : values[i].statistics)
          if (!std::isfinite(value)) throw std::invalid_argument("Non-finite reference statistics");
      }
    }
    for (size_t i = 0; i < values.size(); ++i) {
      if (hemisphere) {
        for (uint32_t c = 0; c < 3; ++c)
          m_sums[(first + i) / samples][c] += values[i].irradiance[0][c];
      } else m_results[first + i] = values[i];
      m_completed[first + i] = true;
    }
    received += uint32_t(values.size());
    if (received != tasks()) return;
    std::ofstream output(m_output, std::ios::trunc);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output << std::setprecision(9) << "{\"version\":1,\"sceneHash\":\"" << sceneHash
           << "\",\"samples\":" << samples << ",\"bounces\":" << bounces
           << ",\"combinedLighting\":" << (combinedLighting ? "true" : "false")
           << ",\"mode\":\"" << (hemisphere ? "hemisphere" : surface ? "surface" : "sh") << "\",\"points\":[";
    for (size_t i = 0; i < points.size(); ++i) {
      if (i) output << ',';
      output << "{\"partitionCluster\":" << (m_partitionClusters.empty() ? -2 : m_partitionClusters.at(i))
             << ",\"position\":[";
      for (uint32_t c = 0; c < 3; ++c) output << (c ? "," : "") << points[i].position[c];
      if (hemisphere) {
        output << "],\"normal\":[";
        for (uint32_t c = 0; c < 3; ++c) output << (c ? "," : "") << points[i].normal[c];
        output << "],\"diffuseIrradiance\":[";
        for (uint32_t c = 0; c < 3; ++c)
          output << (c ? "," : "") << m_sums[i][c] * std::numbers::pi / samples;
        output << "]}";
        continue;
      }
      output << ']';
      if (surface) {
        const Vec3 normal{points[i].normal[0], points[i].normal[1], points[i].normal[2]};
        output << ",\"normal\":[" << normal[0] << ',' << normal[1] << ',' << normal[2]
               << "],\"diffuseIrradiance\":[";
        const auto basis = irradianceBasis(normalized(normal));
        for (uint32_t c = 0; c < 3; ++c) {
          double value = 0;
          for (uint32_t j = 0; j < 9; ++j) value += basis[j] * m_results[i].irradiance[j][c];
          output << (c ? "," : "") << std::max(value, 0.0);
        }
        output << ']';
      }
      if (!sampleOffsets.empty()) output << ",\"firstSample\":" << sampleOffsets.at(i);
      output << ",\"irradiance\":[";
      for (uint32_t j = 0; j < 9; ++j) {
        output << (j ? ",[" : "[");
        for (uint32_t c = 0; c < 3; ++c) output << (c ? "," : "") << m_results[i].irradiance[j][c];
        output << ']';
      }
      output << "],\"distanceMoments\":[";
      for (uint32_t face = 0; face < 6; ++face) {
        output << (face ? ",[" : "[");
        for (uint32_t c = 0; c < 4; ++c) output << (c ? "," : "") << m_results[i].moments[face][c];
        output << ']';
      }
      output << "],\"statistics\":[";
      for (uint32_t c = 0; c < 4; ++c) output << (c ? "," : "") << m_results[i].statistics[c];
      output << "]}";
    }
    output << "]}\n";
    output.flush();
  }
}
