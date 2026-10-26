#include "sky/shader_code.h"
#include "runtime/pipelines.h"
#include <xz.h>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace remix::sky {
  namespace {
    // embeddedData holds the XZ streams listed by embeddedShaders (generated
    // by the build: scripts/shaders/sky.py).
    constexpr size_t shaderCount = std::size(embeddedShaders);
    struct Decoded {
      std::once_flag once;
      std::vector<uint32_t> words;
    };
    Decoded decoded[shaderCount];

    void decompress(const EmbeddedShader &source, uint8_t *out) {
      static std::once_flag crc;
      std::call_once(crc, [] { xz_crc32_init(); });
      if (source.offset + source.compressedBytes > embeddedDataBytes)
        throw std::runtime_error("Embedded sky data table is inconsistent");
      xz_dec *decoder = xz_dec_init(XZ_SINGLE, 0);
      if (!decoder)
        throw std::bad_alloc();
      xz_buf buffer{.in = embeddedData + source.offset,
                    .in_pos = 0,
                    .in_size = source.compressedBytes,
                    .out = out,
                    .out_pos = 0,
                    .out_size = source.bytes};
      const auto result = xz_dec_run(decoder, &buffer);
      xz_dec_end(decoder);
      if (result != XZ_STREAM_END || buffer.out_pos != source.bytes)
        throw std::runtime_error("Embedded sky data failed to decompress");
    }

    ShaderCode decode(size_t index) {
      if (index >= shaderCount)
        throw std::out_of_range("Embedded sky shader index out of range");
      auto &entry = decoded[index];
      std::call_once(entry.once, [&] {
        const auto &source = embeddedShaders[index];
        if (source.bytes % 4)
          throw std::runtime_error("Embedded sky shader table is inconsistent");
        std::vector<uint32_t> words(source.bytes / 4);
        decompress(source, reinterpret_cast<uint8_t *>(words.data()));
        entry.words = std::move(words);
        pipeline::nameCode(entry.words.data(), shaderNames[index]);
      });
      return {entry.words.data(), entry.words.size() * sizeof(uint32_t)};
    }
  } // namespace

  ShaderCode numosShader(uint32_t pass) {
    if (pass >= numosPassCount)
      throw std::out_of_range("NUMOS pass index out of range");
    return decode(pass);
  }

  ShaderCode shader(Stage stage) { return decode(numosPassCount + size_t(stage)); }

  std::vector<uint8_t> nightSkyAsset(size_t index) {
    if (index >= std::size(embeddedNightSky))
      throw std::out_of_range("Night sky asset index out of range");
    std::vector<uint8_t> bytes(embeddedNightSky[index].bytes);
    if (!bytes.empty())
      decompress(embeddedNightSky[index], bytes.data());
    return bytes;
  }
} // namespace remix::sky
