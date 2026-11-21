#include "pathtrace/shader_code.h"
#include "runtime/pipelines.h"
#include <xz.h>
#include <cctype>
#include <mutex>
#include <stdexcept>
#include <string>
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

  namespace {
    bool software = false;
    // The stage named "software" + the stage's capitalized name, or the stage.
    Stage softwareTwin(Stage stage) {
      static const std::vector<Stage> twins = [] {
        std::vector<Stage> out(static_cast<size_t>(Stage::Count));
        for (size_t i = 0; i < out.size(); ++i) {
          out[i] = Stage(i);
          std::string name = std::string("software") + stageNames[i];
          name[8] = char(std::toupper(static_cast<unsigned char>(name[8])));
          for (size_t j = 0; j < out.size(); ++j)
            if (name == stageNames[j])
              out[i] = Stage(j);
        }
        return out;
      }();
      return twins[size_t(stage)];
    }
  } // namespace

  void setSoftwareRayQuery(bool enabled) { software = enabled; }
  bool softwareRayQuery() { return software; }

  ShaderCode shader(Stage stage) {
    if (software)
      stage = softwareTwin(stage);
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
