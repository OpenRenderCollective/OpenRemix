#pragma once
#include <cstdint>
namespace remix::forward::bake {
  // Algorithmic contents of the RGB irradiance field, not a reflection domain.
  enum class IrradianceContents : uint8_t { Combined, ReceiverDirect };
}
