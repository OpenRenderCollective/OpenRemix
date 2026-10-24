#include "forward/bake/reference_capture.h"
#include "scene/fixtures.h"
#include <fstream>
#include <limits>
#include <cmath>

using namespace remix::forward::bake;
using remix::test::require;
int main(int argc, char **argv) {
  require(argc == 2, "Reference fixture has an isolated output path");
  const std::filesystem::path path(argv[1]), output(std::string(argv[1]) + ".results.json");
  std::filesystem::remove(output);
  { std::ofstream file(path); file << "4096\n1 2 3\n-4 5 6\n"; }
  auto capture = ReferenceCapture::load(path);
  require(capture.samples == 4096 && capture.points.size() == 2 && capture.sampleOffsets.empty() &&
              capture.points[0].normal == remix::forward::Vec4{},
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
  for (bool moments : {true, false}) {
    auto &field = moments ? value.moments[2][1] : value.statistics[0];
    field = std::numeric_limits<float>::quiet_NaN();
    rejected = false;
    try { capture.consume(0, std::span(&value, 1)); }
    catch (const std::invalid_argument &) { rejected = true; }
    require(rejected && capture.received == 1 && !std::filesystem::exists(output),
            "Non-finite visibility data rejects before consuming a reference batch");
    field = 0;
  }
  value.moments[0] = {2, 5, .5f, 4096};
  value.statistics = {.125f, .25f, 4096, 0};
  capture.consume(0, std::span(&value, 1));
  std::ifstream file(output);
  const std::string text((std::istreambuf_iterator<char>(file)), {});
  require(text.find("\"samples\":4096") != std::string::npos &&
              text.find("\"firstSample\"") == std::string::npos &&
              text.find("\"position\":[-4,5,6]") != std::string::npos,
          "Completed output contains sampling budget and original positions");
  require(text.find("\"distanceMoments\":[[2,5,0.5,4096]") != std::string::npos &&
              text.find("\"statistics\":[0.125,0.25,4096,0]") != std::string::npos,
          "SH reference preserves raw visibility moments and transport statistics");
  file.close();
  {
    std::ofstream shifted(path);
    shifted << "sh-offsets 256\n1 2 3 0\n1 2 3 1024\n";
  }
  auto shifted = ReferenceCapture::load(path);
  require(!shifted.hemisphere && shifted.samples == 256 && shifted.tasks() == 2 &&
              shifted.sampleOffsets == std::vector<uint32_t>{0, 1024} &&
              shifted.points[0].position == shifted.points[1].position,
          "Offset SH references preserve identical sites and distinct transport streams");
  shifted.consume(1, std::span(&value, 1));
  shifted.consume(0, std::span(&value, 1));
  {
    std::ifstream json(output);
    const std::string result((std::istreambuf_iterator<char>(json)), {});
    require(result.find("\"firstSample\":0") != std::string::npos &&
                result.find("\"firstSample\":1024") != std::string::npos,
            "Out-of-order reference output attributes each stream to its original site");
  }
  for (const auto *offset : {"", "-1", "1.5", "4294967296", "4294967040", "nan"}) {
    { std::ofstream bad(path); bad << "sh-offsets 256\n1 2 3 " << offset; }
    rejected = false;
    try { ReferenceCapture::load(path); }
    catch (const std::invalid_argument &) { rejected = true; }
    require(rejected, "Malformed or overflowing reference sample ranges reject");
  }
  { std::ofstream boundary(path); boundary << "sh-offsets 256\n1 2 3 4294967039\n"; }
  require(ReferenceCapture::load(path).sampleOffsets[0] == UINT32_MAX - 256,
          "Largest non-overflowing reference sample range remains representable");
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
  {
    std::ofstream hemisphere(path);
    hemisphere << "hemisphere 64\n1 2 3 0 0 1\n-4 5 6 -1 0 0\n";
  }
  auto cosine = ReferenceCapture::load(path);
  require(cosine.hemisphere && cosine.tasks() == 128, "Hemisphere sites expand to bounded radiance rays");
  const auto rays = cosine.batch(0, cosine.tasks());
  double cosineSquared = 0;
  for (uint32_t i = 0; i < rays.size(); ++i) {
    const remix::forward::Vec3 direction{rays[i].normal[0], rays[i].normal[1], rays[i].normal[2]};
    require(std::abs(remix::forward::dot(direction, direction) - 1) < 1e-5f &&
                (i < 64 ? direction[2] : -direction[0]) > 0,
            "Every cosine ray is unit length and stays above its oriented receiver plane");
    const float receiverCosine = i < 64 ? direction[2] : -direction[0];
    cosineSquared += receiverCosine * receiverCosine;
  }
  require(std::abs(cosineSquared / rays.size() - .5) < 1e-6,
          "Stratified cosine rays integrate the cosine-squared moment to one half");
  const auto split = cosine.batch(57, 19);
  require(std::equal(split.begin(), split.end(), rays.begin() + 57,
                    [](const auto &a, const auto &b) { return a.position == b.position && a.normal == b.normal; }),
          "Splitting ray batches across site boundaries preserves deterministic directions");
  std::vector<Result> constant(128);
  for (uint32_t i = 0; i < constant.size(); ++i)
    for (uint32_t c = 0; c < 3; ++c) constant[i].irradiance[0][c] = float(c + 1 + (i / 64) * 3);
  cosine.consume(73, std::span(constant).subspan(73));
  cosine.consume(0, std::span(constant).first(73));
  {
    std::ifstream json(output);
    const std::string result((std::istreambuf_iterator<char>(json)), {});
    require(result.find("\"diffuseIrradiance\":[3.14159265,6.28318531,9.42477796]") != std::string::npos &&
                result.find("\"diffuseIrradiance\":[12.5663706,15.7079633,18.8495559]") != std::string::npos,
            "Cosine sampling integrates constant radiance to Pi times radiance for each site");
    require(result.find("\"distanceMoments\"") == std::string::npos &&
                result.find("\"statistics\"") == std::string::npos,
            "Hemisphere output does not invent SH visibility moments from individual radiance rays");
  }
  { std::ofstream bad(path); bad << "hemisphere 64\n1 2 3 0 0 0\n"; }
  rejected = false;
  try { ReferenceCapture::load(path); }
  catch (const std::invalid_argument &) { rejected = true; }
  require(rejected, "Invalid receiver normals cannot create hemisphere rays");
  std::filesystem::remove(path);
  std::filesystem::remove(output);
}
