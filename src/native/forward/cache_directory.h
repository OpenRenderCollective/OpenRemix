#pragma once
#include <cstdlib>
#include <filesystem>

namespace remix::forward::cache_io {
inline std::filesystem::path directory() {
  const char *override = std::getenv("REMIX_BAKE_CACHE_DIR");
  const char *config = std::getenv("REMIX_CONFIG_DIR");
  return override && *override
             ? std::filesystem::u8path(override)
             : (config && *config
                    ? std::filesystem::u8path(config)
                    : std::filesystem::current_path() / "openremix") /
                   "cache" / "forwardplus";
}
} // namespace remix::forward::cache_io
