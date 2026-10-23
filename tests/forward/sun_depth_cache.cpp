#include "forward/bake/sun_depth_cache.h"
#include "scene/fixtures.h"
#include <bit>
#include <fstream>
#include <limits>
#include <random>
using namespace remix::forward;
using remix::test::require;
namespace io = bake::cache::detail;
namespace disk = bake::sun_depth_cache;
int main(int argc, char **argv) {
  require(argc == 2, "Cache test has an isolated output directory");
  const auto root = std::filesystem::path(argv[1]) / std::to_string(std::random_device{}());
  std::filesystem::create_directories(root);
  const auto file = root / "sun.depth";
  bake::SunDepthLayoutOptions options;
  options.pageSize = 8; options.tileTexels = 2; options.border = 1;
  options.maximumPages = 2; options.maximumBytes = 1024;
  const std::array<std::array<int32_t,2>,5> tiles{{{-2,0},{-1,0},{0,0},{1,0},{2,0}}};
  const std::array<Vec3,3> basis{Vec3{1,0,0},Vec3{0,1,0},Vec3{0,0,-1}};
  for (size_t tileCount : {size_t(3),size_t(5)}) {
    const auto planned = bake::planSunDepthLayout(std::span(tiles).first(tileCount), options);
    require(bool(planned), "Cache fixture fits its memory limit");
    const auto &layout = *planned;
    const auto header = bake::sunDepthHeader(layout, basis, 4, -2, 6, .002f);
    std::vector<float> values(size_t(layout.depthBytes / 4), std::numeric_limits<float>::quiet_NaN());
    std::vector<bool> captured(values.size());
    for (uint32_t slot = 0; slot < tileCount; ++slot) {
      const auto a = layout.allocation(slot);
      for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 4; ++x) {
        const auto index = (a.page * 8 + a.y - 1 + y) * 8 + a.x - 1 + x;
        captured[index] = true;
        values[index] = (x+y)%3 ? float(slot) + .25f : -1;
      }
    }
    values[0] = -0.f;
    values[1] = -2; // Explicit unresolved-transmission fallback survives persistence.
    auto store = [&](std::span<const float> data) {
      io::Writer output(file, 17, {});
      disk::write(output, layout, header, data);
      output.finish();
    };
    auto load = [&](const bake::SunDepthHeader &expected) {
      io::Reader input(file, 17, {});
      auto result = disk::read(input, layout, expected);
      input.finish();
      return result;
    };
    store(values);
    const auto restored = load(header);
    for (size_t i = 0; i < values.size(); ++i)
      require(captured[i] ? std::bit_cast<uint32_t>(restored[i]) == std::bit_cast<uint32_t>(values[i]) : restored[i] == -1,
              "Captured depth bits survive exactly; unused pages never serialize GPU contents");
    const uint64_t expectedBytes = sizeof(io::Header) + 8 + sizeof(header) + 8 + layout.directory.size()*4 + 8 + tileCount*16*4;
    require(std::filesystem::file_size(file) == expectedBytes,
            "Cache payload stores captured rows without per-row counts or unused atlas padding");
    auto rejects = [&](auto action, const char *message) {
      bool rejected = false;
      try { action(); } catch (const std::exception &) { rejected = true; }
      require(rejected, message);
    };
    auto changed = header; changed.sampling[3] += .001f;
    rejects([&] { load(changed); }, "Changed projection/shape policy cannot reuse a stale cache");
    rejects([&] { io::Reader wrong(file, 18, {}); }, "Source cache identity is independently checked");
    std::stop_source stop; stop.request_stop();
    rejects([&] { io::Reader cancelled(file, 17, stop.get_token()); }, "Cancelled cache reads stop during integrity verification");
    auto bad = values; bad[0] = std::numeric_limits<float>::quiet_NaN();
    rejects([&] { store(bad); }, "Nonfinite captured depth cannot enter a persistent payload");
    bad[0] = -3;
    rejects([&] { store(bad); }, "Unknown negative depth sentinel rejects");
    bad[0] = 9;
    rejects([&] { store(bad); }, "Depth beyond the captured caster span rejects");
    rejects([&] { store(std::span(values).first(values.size()-1)); }, "Incomplete atlas rejects rather than caching partial capture");
    for (uint32_t mode = 0; mode < 5; ++mode) {
      {
        io::Writer output(file, 17, {});
        output.pod(uint32_t(0x53444e50)); output.pod(uint32_t(mode == 0 ? 1 : 2));
        output.pod(header);
        auto directory = layout.directory;
        if (mode == 1) directory[0] = 0;
        if (mode == 4) output.pod(UINT64_MAX);
        else output.array(directory);
        output.pod(mode == 2 ? UINT64_MAX : uint64_t(tileCount*16));
        output.finish();
      }
      rejects([&] { load(header); }, "Checksummed but incompatible version, directory, count or truncated depth rejects");
    }
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(), -3.f, 9.f}) {
      {
        io::Writer output(file, 17, {});
        output.pod(uint32_t(0x53444e50)); output.pod(uint32_t(2)); output.pod(header);
        output.array(layout.directory); output.pod(uint64_t(tileCount*16));
        std::vector<float> malicious(tileCount*16, .5f);
        malicious[0] = invalid;
        output.raw(std::span<const float>(malicious)); output.finish();
      }
      rejects([&] { load(header); }, "Semantic depth validation is independent of file checksum and producer validation");
    }
    store(values);
    std::filesystem::resize_file(file, std::filesystem::file_size(file)-1);
    rejects([&] { load(header); }, "Truncated cache container rejects before GPU allocation");
    store(values);
    {
      std::fstream corrupt(file, std::ios::binary | std::ios::in | std::ios::out);
      corrupt.seekp(-1, std::ios::end); char byte=42; corrupt.write(&byte,1);
    }
    rejects([&] { load(header); }, "Changed raw depth bytes fail container checksum");
  }
}
