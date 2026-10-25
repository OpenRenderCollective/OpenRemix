#include "pathtrace/shader_code.h"
#include "runtime/pipelines.h"
#include <xz.h>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace remix::trace {
  namespace {
    // embeddedData holds the XZ streams listed by embeddedShaders (generated
    // by the build: scripts/shaders/generate.py).
    struct Decoded {
      std::once_flag once;
      std::vector<uint32_t> words;
    };
    Decoded decoded[size_t(Stage::Count)];
  } // namespace

  ShaderCode shader(Stage stage) {
    static std::once_flag crc;
    std::call_once(crc, [] { xz_crc32_init(); });
    auto &entry = decoded[size_t(stage)];
    std::call_once(entry.once, [&] {
      const auto &source = embeddedShaders[size_t(stage)];
      if (source.offset + source.compressedBytes > embeddedDataBytes || source.bytes % 4)
        throw std::runtime_error("Embedded shader table is inconsistent");
      std::vector<uint32_t> words(source.bytes / 4);
      xz_dec *decoder = xz_dec_init(XZ_SINGLE, 0);
      if (!decoder)
        throw std::bad_alloc();
      xz_buf buffer{.in = embeddedData + source.offset,
                    .in_pos = 0,
                    .in_size = source.compressedBytes,
                    .out = reinterpret_cast<uint8_t *>(words.data()),
                    .out_pos = 0,
                    .out_size = source.bytes};
      const auto result = xz_dec_run(decoder, &buffer);
      xz_dec_end(decoder);
      if (result != XZ_STREAM_END || buffer.out_pos != source.bytes)
        throw std::runtime_error("Embedded shader failed to decompress");
      entry.words = std::move(words);
      pipeline::nameCode(entry.words.data(), stageNames[size_t(stage)]);
    });
    return {entry.words.data(), entry.words.size() * sizeof(uint32_t)};
  }
} // namespace remix::trace
