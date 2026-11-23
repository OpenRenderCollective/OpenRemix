// Precompiled for the renderer's larger targets (meson cpp_pch): the standard
// headers they use throughout and scene/types.h, which nearly every one of
// their files reaches. Parsing these once per target instead of once per file
// is most of a file's compile time. Nothing here depends on macros a file
// defines before its includes (no Vulkan or platform headers beyond
// remix_c.h's).
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "scene/types.h"
