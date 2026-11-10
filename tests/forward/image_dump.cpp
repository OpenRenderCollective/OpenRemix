#include "forward/image_dump.h"
#include "gpu_fixture.h"
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>

using namespace remix::forward;
using namespace remix::forward::test;
int main(int argc, char **argv) {
  require(argc == 2, "Image dump test needs an output directory");
  const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto directory = std::filesystem::path(argv[1]) / std::to_string(unique);
  std::filesystem::create_directories(directory);
  const auto prefix = (directory / "frame").string();
#ifdef _WIN32
  _putenv_s("REMIX_DEBUG_BAKE_CAPTURE_PREFIX", prefix.c_str());
#else
  setenv("REMIX_DEBUG_BAKE_CAPTURE_PREFIX", prefix.c_str(), 1);
#endif
  Fixture fixture(false);
  Buffer source;
  source.create(fixture.physical, fixture.device, 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  const std::array<uint32_t, 4> first{1, 2, 3, 4}, second{5, 6, 7, 8};
  source.write(first.data(), sizeof(first));
  ImageDump capture(fixture.physical, fixture.device);
  capture.selectFrame(0);
  fixture.begin();
  capture.recordBuffer(fixture.command, 0, "early", source.descriptor());
  std::ofstream(prefix + ".frame.request").close();
  capture.recordBuffer(fixture.command, 0, "late", source.descriptor());
  fixture.finish();
  capture.selectFrame(0);
  require(!std::filesystem::exists(prefix + ".late.buffer.bin"),
          "A request arriving mid-record cannot capture only trailing buffers");
  source.write(second.data(), sizeof(second));
  fixture.begin();
  capture.recordBuffer(fixture.command, 0, "early", source.descriptor());
  capture.recordBuffer(fixture.command, 0, "late", source.descriptor());
  fixture.finish();
  capture.selectFrame(0);
  for (const char *name : {"early", "late"}) {
    const auto path = prefix + "." + name + ".buffer.bin";
    require(std::filesystem::exists(path) && std::filesystem::file_size(path) == sizeof(second),
            "Both buffers from the next complete frame are published");
    std::array<uint32_t, 4> actual{};
    std::ifstream input(path, std::ios::binary);
    input.read(reinterpret_cast<char *>(actual.data()), sizeof(actual));
    require(bool(input) && actual == second, "Readbacks belong to the same completed submission");
  }
  fixture.begin();
  capture.recordBuffer(fixture.command, 0, "after", source.descriptor());
  fixture.finish();
  capture.selectFrame(0);
  require(!std::filesystem::exists(prefix + ".after.buffer.bin"), "A completed capture remains one-shot");
  require(validationErrors == 0, "Image dump request timing is Vulkan validation clean");
  std::puts("Image dump request latching preserved complete, fenced, one-shot readbacks");
}
