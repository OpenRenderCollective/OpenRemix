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
    if (token == "base") {
      result.baseLightingOnly = true;
      input >> token;
    }
    result.geometry = token == "geometry";
    result.hemisphere = token == "hemisphere";
    result.sphere = token == "sphere" || token == "sphere-offsets";
    result.surface = token == "surface" || token == "surface-offsets";
    result.incomingIrradiance = token == "incoming-sh" || token == "incoming-sh-offsets";
    const bool offsets = token == "sh-offsets" || token == "surface-offsets" || token == "incoming-sh-offsets" || token == "sphere-offsets";
    if (result.usesRadianceRays() || result.surface || offsets || result.incomingIrradiance) input >> token;
    if (result.geometry) result.samples = 1;
    else {
      const auto parsed = std::from_chars(token.data(), token.data() + token.size(), result.samples);
      if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() ||
          result.samples < 64 || result.samples > 8192)
        throw std::invalid_argument("Reference capture requires 64..8192 samples");
    }
    while (input >> std::ws && input.peek() != std::char_traits<char>::eof()) {
      Point point{};
      if (result.points.size() == 16 || !(input >> point.position[0] >> point.position[1] >> point.position[2]))
        throw std::invalid_argument("Reference capture requires at most 16 XYZ positions");
      for (float value : point.position)
        if (!std::isfinite(value)) throw std::invalid_argument("Non-finite reference position");
      if (result.hemisphere || result.surface || result.geometry) {
        if (!(input >> point.normal[0] >> point.normal[1] >> point.normal[2]))
          throw std::invalid_argument(result.geometry ? "Geometry reference requires XYZ unit directions"
                                                     : "Surface reference requires XYZ unit normals");
        const Vec3 normal{point.normal[0], point.normal[1], point.normal[2]};
        const float lengthSquared = dot(normal, normal);
        if (!std::isfinite(lengthSquared) || std::abs(lengthSquared - 1) > 1e-4f)
          throw std::invalid_argument("Invalid reference unit direction or normal");
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
    if (!result.usesRadianceRays()) result.m_results.resize(result.points.size());
    result.m_completed.resize(result.tasks());
    if (result.hemisphere) result.m_sums.resize(result.points.size());
    if (result.sphere) result.m_sphereProjection.resize(result.points.size());
    return result;
  }
  std::vector<Point> ReferenceCapture::batch(uint32_t first, uint32_t count) const {
    if (first > tasks() || count > tasks() - first) throw std::out_of_range("Reference task range");
    std::vector<Point> result;
    result.reserve(count);
    for (uint32_t task = first; task < first + count; ++task) {
      auto point = points[usesRadianceRays() ? task / samples : task];
      if (usesRadianceRays()) {
        const auto ordinal = task % samples;
        const Vec3 normal = hemisphere ? normalized({point.normal[0], point.normal[1], point.normal[2]})
                                       : Vec3{0, 0, 1};
        const Vec3 tangent = normalized(cross(std::abs(normal[2]) < .99f ? Vec3{0, 0, 1} : Vec3{1, 0, 0}, normal));
        const Vec3 bitangent = cross(normal, tangent);
        const float cosine = hemisphere ? std::sqrt((float(ordinal) + .5f) / samples)
                                        : 1 - 2 * (float(ordinal) + .5f) / samples;
        const float radius = std::sqrt(1 - cosine * cosine);
        const uint32_t rotation = std::bit_cast<uint32_t>(point.position[0]) * 73856093u ^
                                  std::bit_cast<uint32_t>(point.position[1]) * 19349663u ^
                                  std::bit_cast<uint32_t>(point.position[2]) * 83492791u;
        const uint32_t phaseOffset = sampleOffsets.empty() ? 0 : sampleOffsets.at(task / samples);
        const double phase = double((ordinal + phaseOffset) * 0x61c88647u + rotation) / 4294967296.0;
        const double phi = phase * 2 * std::numbers::pi;
        if (sphere) {
          point.normal = {radius * float(std::cos(phi)), radius * float(std::sin(phi)), cosine, 0};
        } else {
          for (uint32_t c = 0; c < 3; ++c)
            point.normal[c] = normal[c] * cosine + radius *
                (tangent[c] * float(std::cos(phi)) + bitangent[c] * float(std::sin(phi)));
        }
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
      if (!usesRadianceRays()) {
        for (const auto &moment : values[i].moments)
          for (float value : moment)
            if (!std::isfinite(value)) throw std::invalid_argument("Non-finite reference distance moments");
        for (float value : values[i].statistics)
          if (!std::isfinite(value)) throw std::invalid_argument("Non-finite reference statistics");
      }
    }
    const auto directions = sphere ? batch(first, uint32_t(values.size())) : std::vector<Point>{};
    for (size_t i = 0; i < values.size(); ++i) {
      if (sphere) {
        const auto &direction = directions[i].normal;
        const auto &value = values[i].irradiance[0];
        m_sphereProjection[(first + i) / samples].add(
            {direction[0], direction[1], direction[2]}, {value[0], value[1], value[2]});
      } else if (hemisphere) {
        for (uint32_t c = 0; c < 3; ++c)
          m_sums[(first + i) / samples][c] += values[i].irradiance[0][c];
      } else m_results[first + i] = values[i];
      m_completed[first + i] = true;
    }
    received += uint32_t(values.size());
    if (received != tasks()) return;
    std::ofstream output(m_output, std::ios::trunc);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    const char *mode = geometry ? "geometry" : incomingIrradiance ? "incoming-sh" : hemisphere ? "hemisphere"
                           : sphere ? "sphere" : surface ? "surface" : "sh";
    output << std::setprecision(9) << "{\"version\":1,\"sceneHash\":\"" << sceneHash
           << "\",\"samples\":" << samples << ",\"bounces\":" << bounces
           << ",\"combinedLighting\":" << (combinedLighting ? "true" : "false")
           << ",\"analyticReceiverDirectPolicy\":" << (analyticReceiverDirectPolicy ? "true" : "false")
           << ",\"baseLightingOnly\":" << (baseLightingOnly ? "true" : "false")
           << ",\"mode\":\"" << mode << "\",\"points\":[";
    for (size_t i = 0; i < points.size(); ++i) {
      if (i) output << ',';
      output << "{\"partitionCluster\":" << (m_partitionClusters.empty() ? -2 : m_partitionClusters.at(i))
             << ",\"position\":[";
      for (uint32_t c = 0; c < 3; ++c) output << (c ? "," : "") << points[i].position[c];
      if (geometry) {
        const bool miss = m_results[i].statistics[1] >= .5f;
        output << "],\"direction\":[" << points[i].normal[0] << ',' << points[i].normal[1] << ',' << points[i].normal[2]
               << "],\"maximumDistance\":" << maximumGeometryDistance << ",\"firstHitDistance\":";
        if (miss) output << "null"; else output << m_results[i].irradiance[0][3];
        output << ",\"miss\":" << (miss ? "true" : "false") << ",\"opaqueBackface\":"
               << (m_results[i].statistics[0] >= .5f ? "true" : "false") << '}';
        continue;
      }
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
      const auto projected = sphere ? m_sphereProjection[i].sphericalHarmonics() : RadianceProjection::Sh{};
      for (uint32_t j = 0; j < 9; ++j) {
        output << (j ? ",[" : "[");
        for (uint32_t c = 0; c < 3; ++c)
          output << (c ? "," : "") << (sphere ? projected[j][c]
                                             : m_results[i].irradiance[j][c]);
        output << ']';
      }
      if (sphere) {
        output << "],\"ambientCubeFaceOrder\":\"+X,-X,+Y,-Y,+Z,-Z\",\"ambientCubeIrradiance\":[";
        const auto cube = m_sphereProjection[i].ambientCube();
        for (uint32_t face = 0; face < 6; ++face) {
          output << (face ? ",[" : "[") << cube[face][0] << ',' << cube[face][1] << ',' << cube[face][2] << ']';
        }
        output << "]}";
        continue;
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
