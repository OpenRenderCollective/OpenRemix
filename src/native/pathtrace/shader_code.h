#pragma once
#include "pathtrace/shaders/embedded.h"
#include <cstddef>
#include <cstdint>

namespace remix::trace {
  // SPIR-V of an embedded stage.
  struct ShaderCode {
    const uint32_t *code = nullptr;
    size_t bytes = 0;
  };
  // Decompressed on first use, from any thread, and kept for the process
  // lifetime: the pointer stays valid for lazily compiled variants.
  ShaderCode shader(Stage stage);
  // Devices without ray queries (PathTracer::initialize): shader() then
  // returns a stage's "software" twin where it has one, which traverses the
  // CPU-built structures of pathtrace/software_bvh.h. Set before any use.
  void setSoftwareRayQuery(bool enabled);
  bool softwareRayQuery();
} // namespace remix::trace
