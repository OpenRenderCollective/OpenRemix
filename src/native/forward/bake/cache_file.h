#pragma once
#include <filesystem>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif
namespace remix::forward::bake::cache::detail {
  inline void replaceFile(const std::filesystem::path &temporary, const std::filesystem::path &destination) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      throw std::runtime_error("Bake cache rename failed");
#else
    std::filesystem::rename(temporary, destination);
#endif
  }
}
