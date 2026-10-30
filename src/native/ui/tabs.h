#pragma once
// The runtime the developer panel's tabs draw, whichever backend the build has
// (one per build): Metal's (metal/runtime.h) or Vulkan's (runtime/runtime.h).
// What differs between them reaches the tabs through ui/status.h's queries.
#if defined(REMIX_METAL)
#include "metal/runtime.h"
#else
#include "runtime/runtime.h"
#endif
#include "ui/status.h"
