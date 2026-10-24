#pragma once
#include "sky/shaders/embedded.h"
#include <cstddef>
#include <cstdint>

namespace remix::sky {
  // SPIR-V of an embedded NUMOS pass or adapter stage.
  struct ShaderCode {
    const uint32_t *code = nullptr;
    size_t bytes = 0;
  };
  // A numos::PassId; decompressed on first use, from any thread, and kept for
  // the process lifetime.
  ShaderCode numosShader(uint32_t pass);
  ShaderCode shader(Stage stage);
} // namespace remix::sky
