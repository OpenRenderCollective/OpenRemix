#include "forward/bake/data.h"
#include "forward/material.h"
#include "scene/optics.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace remix::forward::bake {
  Inputs pack(const scene::BakeScene &scene, double seconds, MaterialUse use) {
    if (!scene.ready)
      throw std::invalid_argument("Baking needs a complete static scene");
    Inputs result;
    std::unordered_map<const scene::Texture *, uint32_t> textures;
    auto texture = [&](const std::shared_ptr<scene::Material> &material,
                       const std::shared_ptr<scene::Texture> &image) {
      if (!image)
        return 0u;
      if (auto found = textures.find(image.get()); found != textures.end())
        return found->second;
      const uint32_t layer = uint32_t(result.textures.size()) + 1;
      textures.emplace(image.get(), layer);
      result.textures.push_back({material, image});
      return layer;
    };
    for (const auto &draw : scene.draws) {
      const auto &surface = *draw.surface;
      const auto &source = *draw.material;
      const auto &blend = draw.blend;
      const bool sourceOver = sourceOverCoverage(blend);
      const bool modulate = modulatingDecal(draw);
      const auto medium = source.bakeSmoothBoundary ? scene::measuredMedium(source) : std::nullopt;
      const bool dielectric = medium.has_value();
      if ((source.transmission && !dielectric) || (source.translucent && !sourceOver && !dielectric) ||
          (blend.enabled && !sourceOver && !modulate))
        throw std::invalid_argument(
            "Unsupported bake coverage: draw " + std::to_string(result.materials.size()) +
            ", material " + std::to_string(source.sourceHash) + ", blend " +
            std::to_string(blend.enabled) + "/" + std::to_string(blend.src) + "/" +
            std::to_string(blend.dst) + ", transmission " + std::to_string(source.transmission) +
            ", translucent " + std::to_string(source.translucent));
      if (result.vertices.size() + surface.vertices.size() > UINT32_MAX)
        throw std::length_error("Bake geometry exceeds 32-bit addressing");
      const uint32_t vertexBase = uint32_t(result.vertices.size());
      const uint32_t material = uint32_t(result.materials.size());
      const auto shading = materialData(draw, seconds);
      result.materials.push_back(
          {shading,
           {texture(draw.material, source.texture), texture(draw.material, source.emissionTexture),
            texture(draw.material, source.metallicTexture), 0},
           {source.wrapU, source.wrapV, source.filter,
            (source.suppressEmissionLight ? suppressEmission : 0u) |
                (sourceOver ? sourceOverCoverageFlag : 0u) | (dielectric ? dielectricBoundaryFlag : 0u) |
                (dielectric && source.thinWallThickness > 0 ? thinDielectricFlag : 0u) |
                (draw.reflectionOnly ? reflectionOnlyFlag : 0u) |
                (use == MaterialUse::StaticBake && (source.bakeTintAxes & 1u) ? averageTintUFlag : 0u) |
                (use == MaterialUse::StaticBake && (source.bakeTintAxes & 2u) ? averageTintVFlag : 0u) |
                (modulate ? modulatingDecalFlag : 0u)}});
      if (dielectric) {
        // Live rays use the streamed clock; static transport can average its
        // explicitly selected UV axes from this same immutable rate payload.
        auto &mapped = result.materials.back().shading;
        mapped.remap[0][3] = source.transmittanceScroll[0];
        mapped.remap[1][3] = source.transmittanceScroll[1];
        auto &optics = result.materials.back().optics;
        std::copy(medium->extinction.begin(), medium->extinction.end(), optics.begin());
        optics[3] = medium->refractiveIndex;
        if (source.thinWallThickness > 0) {
          const auto sheet = scene::measuredSheet(source).value();
          std::copy(sheet.normalDepth.begin(), sheet.normalDepth.end(), optics.begin());
          result.materials.back().textures[3] =
              std::bit_cast<uint32_t>(sheet.thicknessRatio);
        }
      }
      std::vector<Matrix> boneNormals;
      if (surface.bonesPerVertex) {
        if (!draw.bones)
          throw std::invalid_argument("Posed transport geometry needs bone transforms");
        for (const auto &bone : *draw.bones)
          boneNormals.push_back(normalMatrix(bone));
      }
      for (size_t index = 0; index < surface.vertices.size(); ++index) {
        const auto &v = surface.vertices[index];
        Vec3 position{v.position[0], v.position[1], v.position[2]},
            normal{v.normal[0], v.normal[1], v.normal[2]};
        if (surface.bonesPerVertex) {
          Vec3 posed{}, posedNormal{};
          for (uint32_t b = 0; b < surface.bonesPerVertex; ++b) {
            const auto offset = index * surface.bonesPerVertex + b;
            const auto bone = surface.bones.at(offset);
            const auto p = transformPoint(draw.bones->at(bone), position);
            const auto n = transformVector(boneNormals.at(bone), normal);
            for (uint32_t c = 0; c < 3; ++c) {
              posed[c] += p[c] * surface.weights.at(offset);
              posedNormal[c] += n[c] * surface.weights.at(offset);
            }
          }
          position = posed;
          normal = posedNormal;
        }
        const auto p = transformPoint(draw.model, position);
        const auto n = transformVector(shading.normal, normal);
        result.vertices.push_back(
            {{p[0], p[1], p[2], 1},
             {n[0], n[1], n[2], 0},
             {v.uv[0] * shading.uv[0] + shading.uv[2], v.uv[1] * shading.uv[1] + shading.uv[3], 0, 0},
             {v.color[0], v.color[1], v.color[2], v.color[3]}});
      }
      const size_t first = draw.firstIndex;
      const size_t count = draw.indexCount ? draw.indexCount : surface.indices.size();
      if (first > surface.indices.size() || count > surface.indices.size() - first || count % 3)
        throw std::invalid_argument("Bake draw index range is not a valid triangle list");
      for (size_t i = first; i < first + count; i += 3) {
        UInt4 triangle{};
        for (uint32_t c = 0; c < 3; ++c) {
          const int64_t index = int64_t(surface.indices[i + c]) + draw.vertexOffset;
          if (index < 0 || uint64_t(index) >= surface.vertices.size())
            throw std::invalid_argument("Bake draw references a vertex outside its surface");
          triangle[c] = vertexBase + uint32_t(index);
        }
        triangle[3] = material;
        result.triangles.push_back(triangle);
      }
    }
    for (const auto &source : scene.lights) {
      TransportLight light;
      light.positionRadius = {source.position[0], source.position[1], source.position[2], source.radius};
      light.radiance = {source.radiance[0], source.radiance[1], source.radiance[2], float(source.triangle)};
      light.directionCone = {source.direction[0], source.direction[1], source.direction[2], source.coneCos};
      light.shaping = {source.softness, source.focus, 0, float(source.style)};
      for (uint32_t i = 0; i < 3; ++i)
        std::copy_n(source.corners[i], 3, light.corners[i].begin());
      result.lights.push_back(light);
    }
    if (scene.dome.texture) {
      auto sampler = std::make_shared<scene::Material>();
      sampler->texture = scene.dome.texture;
      result.domeLayer = texture(sampler, scene.dome.texture);
    }
    return result;
  }
} // namespace remix::forward::bake
