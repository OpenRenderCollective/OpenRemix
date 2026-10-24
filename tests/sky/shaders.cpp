#include "sky/shader_code.h"
#include <numos/shaders.h>
#include <cstdio>
#include <cstdlib>

namespace {
void require(bool value, const char *message, const char *pass) {
  if (!value) {
    std::fprintf(stderr, "%s: %s\n", pass, message);
    std::exit(1);
  }
}
} // namespace

int main() {
  require(remix::sky::numosPassCount == numos::passCount(), "embedded pass count differs from the pass table", "all");
  // Each embedded pass is the SPIR-V of the table entry with the same index:
  // a valid module whose workgroup size and binding count match the table.
  for (uint32_t pass = 0; pass < numos::passCount(); ++pass) {
    const auto &info = numos::passInfo(pass);
    const auto code = remix::sky::numosShader(pass);
    require(code.bytes > 20 && code.code[0] == 0x07230203u, "not a SPIR-V module", info.name);
    uint32_t local[3] = {}, bindings = 0;
    for (size_t i = 5; i < code.bytes / 4;) {
      const uint32_t op = code.code[i] & 0xffffu, count = code.code[i] >> 16;
      require(count != 0 && i + count <= code.bytes / 4, "truncated instruction", info.name);
      if (op == 16 && code.code[i + 2] == 17) // OpExecutionMode LocalSize
        for (int axis = 0; axis < 3; ++axis)
          local[axis] = code.code[i + 3 + axis];
      if (op == 71 && code.code[i + 2] == 33) // OpDecorate Binding
        ++bindings;
      i += count;
    }
    for (int axis = 0; axis < 3; ++axis)
      require(local[axis] == info.localSize[axis], "workgroup size differs from the pass table", info.name);
    require(bindings == info.bindingCount, "binding count differs from the pass table", info.name);
  }
  return 0;
}
