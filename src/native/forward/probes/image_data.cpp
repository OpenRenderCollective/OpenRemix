#include "forward/probes/image_data.h"
#include "forward/probes/priority_order.h"
#include "forward/half.h"
#include <bit>
#include <cmath>
#include <stdexcept>

namespace remix::forward::probes {
  ImageData prepareImage(const Data &source, std::stop_token cancelled) {
    const auto &[count, size, levels, stride] = source.header.parameters;
    if (cancelled.stop_requested())
      throw std::runtime_error("Reflection image preparation superseded");
    if (!count) {
      if (size || levels || stride || !source.texels.empty())
        throw std::invalid_argument("Nonempty reflection payload without captures");
      return {source.header, {}, {}};
    }
    if (count > maxProbes || size < 8 || size > 512 || !std::has_single_bit(size) ||
        levels != std::bit_width(size) || stride != levelOffset(size, levels) ||
        source.texels.size() != size_t(count) * stride)
      throw std::invalid_argument("Invalid reflection image dimensions or mip payload");
    ImageData result{source.header, {}, {}};
    const auto normalization = captureNormalization(source, cancelled);
    const auto order = priorityOrder(source.header);
    for (uint32_t i = 0; i < count; ++i) {
      result.header.probes[i] = source.header.probes[order[i]];
      result.normalization.coefficients[i] = normalization.coefficients[order[i]];
    }
    result.levels.reserve(levels);
    result.pixels.resize(source.texels.size() * 4);
    size_t destination = 0;
    for (uint32_t level = 0; level < levels; ++level) {
      const uint32_t faceSize = size >> level;
      result.levels.push_back({destination * sizeof(uint16_t), faceSize});
      const uint32_t values = 6 * faceSize * faceSize;
      for (uint32_t probe = 0; probe < count; ++probe) {
        if (cancelled.stop_requested())
          throw std::runtime_error("Reflection image preparation superseded");
        const size_t start = size_t(order[probe]) * stride + levelOffset(size, level);
        for (uint32_t pixel = 0; pixel < values; ++pixel) {
          const auto &value = source.texels[start + pixel];
          for (uint32_t channel = 0; channel < 3; ++channel) {
            if (!std::isfinite(value[channel]) || value[channel] < 0 || value[channel] > 65000)
              throw std::invalid_argument("Reflection image radiance outside finite HDR range");
            result.pixels[destination++] = half::encode(value[channel]);
          }
          // Raw base-level alpha can contain a geometry distance. It is not
          // sampled as radiance and must not overflow an unused half channel.
          result.pixels[destination++] = 0;
        }
      }
    }
    return result;
  }
}
