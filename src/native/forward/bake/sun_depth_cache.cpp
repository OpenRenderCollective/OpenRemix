#include "forward/bake/sun_depth_cache.h"
#include <algorithm>
#include <cmath>
#include <cstring>
namespace remix::forward::bake::sun_depth_cache {
  namespace {
    constexpr uint32_t magic = 0x53444e50, version = 1;
    struct Shape { uint32_t extent; uint64_t packedWords, atlasWords; };
    Shape validate(const SunDepthLayout &layout, const SunDepthHeader &header) {
      const std::array<Vec3, 3> basis{Vec3{header.axisX[0],header.axisX[1],header.axisX[2]},
          Vec3{header.axisY[0],header.axisY[1],header.axisY[2]}, Vec3{header.axisZ[0],header.axisZ[1],header.axisZ[2]}};
      const auto expected = sunDepthHeader(layout, basis, header.sampling[0], header.sampling[1],
                                            header.sampling[2], header.sampling[3]);
      if (std::memcmp(&expected, &header, sizeof(header)) || layout.tiles.empty() ||
          layout.depthBytes > (128ull << 20)) throw std::invalid_argument("Invalid sun depth cache identity");
      const uint32_t extent = layout.options.tileTexels + 2 * layout.options.border;
      return {extent, uint64_t(layout.tiles.size()) * extent * extent, layout.depthBytes / sizeof(float)};
    }
    void finite(std::span<const float> values, float distance) {
      for (float value : values)
        if (!std::isfinite(value) || (value < 0 && value != -1) || value > distance)
          throw std::invalid_argument("Invalid cached sun depth sample");
    }
    void rows(const SunDepthLayout &layout, Shape shape, std::stop_token cancelled, auto visit) {
      for (uint32_t slot = 0; slot < layout.tiles.size(); ++slot) {
        cache::detail::check(cancelled);
        const auto a = layout.allocation(slot);
        for (uint32_t y = 0; y < shape.extent; ++y) {
          cache::detail::check(cancelled);
          const uint64_t offset = (uint64_t(a.page) * layout.options.pageSize + a.y - layout.options.border + y) *
                                    layout.options.pageSize + a.x - layout.options.border;
          if (offset > shape.atlasWords || shape.extent > shape.atlasWords - offset)
            throw std::invalid_argument("Sun depth cache tile exceeds atlas");
          visit(size_t(offset), shape.extent);
        }
      }
    }
  }
  void write(cache::detail::Writer &output, const SunDepthLayout &layout, const SunDepthHeader &header,
             std::span<const float> atlas, std::stop_token cancelled) {
    cache::detail::check(cancelled);
    const auto shape = validate(layout, header);
    if (atlas.size() != shape.atlasWords) throw std::invalid_argument("Incomplete sun depth cache atlas");
    output.pod(magic); output.pod(version); output.pod(header);
    output.array(layout.directory);
    output.pod(shape.packedWords);
    rows(layout, shape, cancelled, [&](size_t offset, uint32_t count) {
      const auto row = atlas.subspan(offset, count);
      finite(row, header.sampling[2] - header.sampling[1]);
      output.raw(row);
    });
  }
  std::vector<float> read(cache::detail::Reader &input, const SunDepthLayout &layout,
                         const SunDepthHeader &header, std::stop_token cancelled) {
    cache::detail::check(cancelled);
    const auto shape = validate(layout, header);
    if (input.pod<uint32_t>() != magic || input.pod<uint32_t>() != version)
      throw std::invalid_argument("Unsupported sun depth cache version");
    const auto stored = input.pod<SunDepthHeader>();
    if (std::memcmp(&header, &stored, sizeof(header))) throw std::invalid_argument("Sun depth cache sampling mismatch");
    if (input.array<uint32_t>(layout.directory.size()) != layout.directory)
      throw std::invalid_argument("Sun depth cache directory mismatch");
    if (input.pod<uint64_t>() != shape.packedWords) throw std::invalid_argument("Sun depth cache sample count mismatch");
    std::vector<float> result(size_t(shape.atlasWords), -1);
    rows(layout, shape, cancelled, [&](size_t offset, uint32_t count) {
      auto row = std::span(result).subspan(offset, count);
      input.raw(row);
      finite(row, header.sampling[2] - header.sampling[1]);
    });
    return result;
  }
}
