#pragma once
#include "scene/types.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <type_traits>

namespace remix::forward::bake::cache::detail {
  static_assert(std::endian::native == std::endian::little);
  constexpr uint64_t maximumFileBytes = 4ull << 30;
  struct Header {
    char magic[8] = {'R', 'M', 'X', 'B', 'A', 'K', '0', '2'};
    uint64_t key = 0, bytes = 0, checksum = 0;
  };
  static_assert(sizeof(Header) == 32);
  inline void check(std::stop_token cancelled) {
    if (cancelled.stop_requested())
      throw std::runtime_error("Bake cache operation superseded");
  }
  class Writer {
  public:
    Writer(const std::filesystem::path &path, uint64_t key, std::stop_token cancelled)
        : output(path, std::ios::binary), cancelled(cancelled) {
      output.exceptions(std::ios::failbit | std::ios::badbit);
      header.key = key;
      output.write(reinterpret_cast<const char *>(&header), sizeof(header));
    }
    template <typename T> void pod(const T &value) {
      static_assert(std::is_trivially_copyable_v<T>);
      bytes(&value, sizeof(value));
    }
    template <typename T> void array(std::span<const T> values) {
      static_assert(std::is_trivially_copyable_v<T>);
      pod(uint64_t(values.size()));
      bytes(values.data(), values.size_bytes());
    }
    template <typename T> void array(const std::vector<T> &values) { array(std::span(values)); }
    void finish() {
      check(cancelled);
      header.checksum = checksum;
      output.seekp(0);
      output.write(reinterpret_cast<const char *>(&header), sizeof(header));
      output.close();
    }

  private:
    std::ofstream output;
    std::stop_token cancelled;
    Header header;
    uint64_t checksum = 1469598103934665603ull;
    void bytes(const void *data, size_t count) {
      if (count > maximumFileBytes - header.bytes)
        throw std::length_error("Bake cache exceeds its file budget");
      const auto *p = static_cast<const char *>(data);
      while (count) {
        check(cancelled);
        const size_t chunk = std::min(count, size_t(1u << 20));
        checksum = scene::hashBytes(p, chunk, checksum);
        output.write(p, std::streamsize(chunk));
        header.bytes += chunk;
        count -= chunk;
        p += chunk;
      }
    }
  };
  class Reader {
  public:
    Reader(const std::filesystem::path &path, uint64_t key, std::stop_token cancelled)
        : input(path, std::ios::binary), cancelled(cancelled) {
      input.exceptions(std::ios::failbit | std::ios::badbit);
      Header header;
      input.read(reinterpret_cast<char *>(&header), sizeof(header));
      const Header expected;
      if (std::memcmp(header.magic, expected.magic, sizeof(header.magic)) || header.key != key ||
          header.bytes > maximumFileBytes ||
          std::filesystem::file_size(path) != sizeof(Header) + header.bytes)
        throw std::runtime_error("Incompatible bake cache header");
      // Verify integrity before allocating from any counts in the payload.
      std::vector<char> block(1u << 20);
      uint64_t checksum = 1469598103934665603ull, left = header.bytes;
      while (left) {
        check(cancelled);
        const size_t chunk = size_t(std::min(left, uint64_t(block.size())));
        input.read(block.data(), std::streamsize(chunk));
        checksum = scene::hashBytes(block.data(), chunk, checksum);
        left -= chunk;
      }
      if (checksum != header.checksum)
        throw std::runtime_error("Bake cache checksum mismatch");
      input.seekg(sizeof(Header));
      remaining = header.bytes;
    }
    template <typename T> T pod() {
      static_assert(std::is_trivially_copyable_v<T>);
      T value{};
      bytes(&value, sizeof(value));
      return value;
    }
    template <typename T> std::vector<T> array(uint64_t maximum) {
      static_assert(std::is_trivially_copyable_v<T>);
      const auto count = pod<uint64_t>();
      if (count > maximum || count > remaining / sizeof(T))
        throw std::runtime_error("Invalid bake cache array length");
      std::vector<T> result(size_t(count), T{});
      bytes(result.data(), result.size() * sizeof(T));
      return result;
    }
    void finish() {
      if (remaining)
        throw std::runtime_error("Unexpected bake cache payload");
      check(cancelled);
    }

  private:
    std::ifstream input;
    std::stop_token cancelled;
    uint64_t remaining = 0;
    void bytes(void *data, size_t count) {
      check(cancelled);
      if (count > remaining)
        throw std::runtime_error("Truncated bake cache payload");
      if (count)
        input.read(static_cast<char *>(data), std::streamsize(count));
      remaining -= count;
    }
  };
} // namespace remix::forward::bake::cache::detail
