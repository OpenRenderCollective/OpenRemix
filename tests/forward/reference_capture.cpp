#include "forward/bake/reference_capture.h"
#include "scene/fixtures.h"
#include <fstream>
#include <limits>

using namespace remix::forward::bake;
using remix::test::require;
int main(int argc, char **argv) {
  require(argc == 2, "Reference fixture has an isolated output path");
  const std::filesystem::path path(argv[1]), output(std::string(argv[1]) + ".results.json");
  std::filesystem::remove(output);
  { std::ofstream file(path); file << "4096\n1 2 3\n-4 5 6\n"; }
  auto capture = ReferenceCapture::load(path);
  require(capture.samples == 4096 && capture.points.size() == 2 && capture.points[0].normal == remix::forward::Vec4{},
          "Reference sites retain exact world positions and unbiased normals");
  Result value{};
  value.irradiance[0][0] = .25f;
  capture.consume(1, std::span(&value, 1));
  require(capture.received == 1 && !std::filesystem::exists(output),
          "Out-of-order fenced readback does not publish a partial reference");
  bool rejected = false;
  try { capture.consume(1, std::span(&value, 1)); }
  catch (const std::logic_error &) { rejected = true; }
  require(rejected && capture.received == 1, "Duplicate readback cannot complete the reference early");
  value.irradiance[3][2] = std::numeric_limits<float>::infinity();
  rejected = false;
  try { capture.consume(0, std::span(&value, 1)); }
  catch (const std::invalid_argument &) { rejected = true; }
  require(rejected && capture.received == 1, "Non-finite transport cannot contaminate JSON");
  value.irradiance[3][2] = 0;
  capture.consume(0, std::span(&value, 1));
  std::ifstream file(output);
  const std::string text((std::istreambuf_iterator<char>(file)), {});
  require(text.find("\"samples\":4096") != std::string::npos &&
              text.find("\"position\":[-4,5,6]") != std::string::npos,
          "Completed output contains sampling budget and original positions");
  file.close();
  for (const auto *input : {"63\n1 2 3", "8193\n1 2 3", "4096\n", "4096\n1 2 broken"}) {
    { std::ofstream bad(path); bad << input; }
    rejected = false;
    try { ReferenceCapture::load(path); }
    catch (const std::invalid_argument &) { rejected = true; }
    require(rejected, "Malformed or unbounded reference requests reject");
  }
  {
    std::ofstream oversized(path);
    oversized << "4096\n" << std::string(4096, ' ');
  }
  rejected = false;
  try { ReferenceCapture::load(path); }
  catch (const std::invalid_argument &) { rejected = true; }
  require(rejected, "Oversized input rejects before allocating reference sites");
  {
    std::ofstream excessive(path);
    excessive << "4096\n";
    for (int i = 0; i < 17; ++i) excessive << "1 2 3\n";
  }
  rejected = false;
  try { ReferenceCapture::load(path); }
  catch (const std::invalid_argument &) { rejected = true; }
  require(rejected, "Reference point count remains bounded independently of file size");
  std::filesystem::remove(path);
  std::filesystem::remove(output);
}
