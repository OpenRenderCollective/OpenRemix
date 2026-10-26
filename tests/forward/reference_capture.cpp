#include "forward/bake/reference_capture.h"
#include "scene/fixtures.h"
#include "forward/bake/spherical_harmonics.h"
#include <sstream>
#include <numbers>
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
  remix::scene::BakeScene region;
  auto partition = std::make_shared<remix::scene::VisibilityPartition>();
  partition->closed = true; partition->clusterCount = 1;
  partition->nodes = {{{1, 0, 0, 0}, {-1, -2}, {}}};
  partition->leafClusters = {0, -1};
  region.partition = partition;
  capture.classify(region);
  capture.analyticReceiverDirectPolicy = true;
  require(capture.samples == 4096 && capture.points.size() == 2 && capture.sampleOffsets.empty() &&
              !capture.baseLightingOnly &&
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
  require(text.find("\"analyticReceiverDirectPolicy\":true") != std::string::npos &&
              text.find("\"samples\":4096") != std::string::npos &&
              text.find("\"partitionCluster\":0,\"position\":[1,2,3]") != std::string::npos &&
              text.find("\"partitionCluster\":-1,\"position\":[-4,5,6]") != std::string::npos &&
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
  {
    std::ofstream sphereInput(path); sphereInput << "base sphere 8192\n1 2 3\n";
  }
  auto sphere = ReferenceCapture::load(path);
  require(sphere.sphere && sphere.usesRadianceRays() && !sphere.hemisphere && sphere.tasks() == 8192 && sphere.baseLightingOnly,
          "Sphere references expand XYZ sites into incoming-radiance rays without receiver normals");
  const auto sphereRays = sphere.batch(0, sphere.tasks());
  std::vector<Result> sphereValues(sphereRays.size());
  double zMean = 0, zSquaredMean = 0;
  for (size_t i = 0; i < sphereRays.size(); ++i) {
    const auto &direction = sphereRays[i].normal;
    require(std::abs(direction[0]*direction[0]+direction[1]*direction[1]+direction[2]*direction[2]-1) < 1e-5f,
            "Uniform sphere rays retain unit directions");
    zMean += direction[2]; zSquaredMean += direction[2]*direction[2];
    sphereValues[i].irradiance[0] = {1+.2f*direction[0],2,0,0};
  }
  require(std::abs(zMean / sphereRays.size()) < 1e-8 &&
              std::abs(zSquaredMean / sphereRays.size()-1./3) < 1e-6,
          "Sphere quadrature covers both hemispheres with the uniform second moment");
  const auto sphereSplit = sphere.batch(13, 37);
  require(std::equal(sphereSplit.begin(), sphereSplit.end(), sphereRays.begin()+13,
                    [](const auto &a, const auto &b) { return a.position == b.position && a.normal == b.normal; }),
          "Sphere directions are independent of GPU batch boundaries");
  sphere.consume(4097, std::span(sphereValues).subspan(4097));
  sphere.consume(0, std::span(sphereValues).first(4097));
  {
    std::ifstream json(output);
    const std::string result((std::istreambuf_iterator<char>(json)), {});
    const auto start = result.find("\"irradiance\":[");
    require(start != std::string::npos && result.find("\"mode\":\"sphere\"") != std::string::npos &&
                result.find("\"analyticReceiverDirectPolicy\":false") != std::string::npos &&
                result.find("distanceMoments") == std::string::npos && result.find("statistics") == std::string::npos,
            "Sphere output identifies CPU SH projection without inventing visibility statistics");
    std::istringstream coefficients(result.substr(start+13));
    std::array<std::array<double,3>,9> encoded{};
    char separator;
    coefficients >> separator;
    require(separator == '[', "Sphere SH starts with a coefficient array");
    for (uint32_t j = 0; j < 9; ++j) {
      coefficients >> separator; require(separator == '[', "Sphere SH retains coefficient tuples");
      for (uint32_t c = 0; c < 3; ++c) {
        coefficients >> encoded[j][c] >> separator;
        require(separator == (c == 2 ? ']' : ','), "Sphere SH tuples contain exactly RGB");
      }
      coefficients >> separator; require(separator == (j == 8 ? ']' : ','), "Sphere SH has exactly nine bands");
    }
    for (const auto &normal : std::array<remix::forward::Vec3,6>{{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}}}) {
      const auto sh = irradianceBasis(normal);
      std::array<double,3> projected{};
      for (uint32_t j = 0; j < 9; ++j) for (uint32_t c = 0; c < 3; ++c) projected[c] += sh[j]*encoded[j][c];
      require(std::abs(projected[0]-std::numbers::pi*(1+.2*2./3*normal[0])) < .001 &&
                  std::abs(projected[1]-2*std::numbers::pi) < .001 && projected[2] == 0,
              "Radiance-to-SH reconstructs analytic constant and linear lighting on all six axes");
    }
  }
  {
    std::ifstream json(output);
    const std::string result((std::istreambuf_iterator<char>(json)), {});
    const std::string key = "\"ambientCubeIrradiance\":";
    const auto start = result.find(key);
    require(start != std::string::npos &&
                result.find("\"ambientCubeFaceOrder\":\"+X,-X,+Y,-Y,+Z,-Z\"") != std::string::npos,
            "Matched sphere output declares its ambient-face order");
    std::istringstream faces(result.substr(start + key.size()));
    char separator;
    faces >> separator; require(separator == '[', "Ambient cube begins with an array");
    for (uint32_t face = 0; face < 6; ++face) {
      faces >> separator; require(separator == '[', "Ambient cube faces contain RGB tuples");
      std::array<double,3> rgb;
      for (uint32_t c = 0; c < 3; ++c) {
        faces >> rgb[c] >> separator;
        require(separator == (c == 2 ? ']' : ','), "Ambient face has exactly three channels");
      }
      const double x = face == 0 ? 1 : face == 1 ? -1 : 0;
      require(std::abs(rgb[0] - std::numbers::pi * (1 + .2 * 2./3 * x)) < .001 &&
                  std::abs(rgb[1] - 2 * std::numbers::pi) < .001 && rgb[2] == 0,
              "Positive face integration reproduces constant and linear lighting from the same SH rays");
      faces >> separator; require(separator == (face == 5 ? ']' : ','), "Ambient cube has exactly six faces");
    }
  }
  { std::ofstream input(path); input << "sphere 64\n1 2 3\n"; }
  const auto originalSphere = ReferenceCapture::load(path).batch(0,64);
  { std::ofstream input(path); input << "base sphere-offsets 64\n1 2 3 0\n1 2 3 8192\n"; }
  auto phasedSphere = ReferenceCapture::load(path);
  const auto phasedRays = phasedSphere.batch(0,128);
  require(phasedSphere.sphere && !phasedSphere.incomingIrradiance && phasedSphere.baseLightingOnly &&
              phasedSphere.sampleOffsets == std::vector<uint32_t>{0,8192},
          "Sphere phases preserve incoming-only projection and explicit component policy");
  require(std::equal(originalSphere.begin(),originalSphere.end(),phasedRays.begin(),
                    [](const auto &a,const auto &b) { return a.position == b.position && a.normal == b.normal; }) &&
              phasedRays[0].normal != phasedRays[64].normal && phasedRays[0].position == phasedRays[64].position,
          "Phase zero preserves old rays; another phase changes directions at the identical origin");
  const auto crossed = phasedSphere.batch(55,21);
  require(std::equal(crossed.begin(),crossed.end(),phasedRays.begin()+55,
                    [](const auto &a,const auto &b) { return a.position == b.position && a.normal == b.normal; }),
          "Phased sphere rays remain stable when GPU batches cross site boundaries");
  for (const char *row : {"1 2 3", "1 2 3 -1", "1 2 3 4294967295"}) {
    { std::ofstream input(path); input << "sphere-offsets 64\n" << row << '\n'; }
    bool invalid = false;
    try { ReferenceCapture::load(path); } catch (const std::invalid_argument &) { invalid = true; }
    require(invalid, "Sphere phases reject missing, negative and overflowing sample offsets");
  }
  { std::ofstream bad(path); bad << "hemisphere 64\n1 2 3 0 0 0\n"; }
  rejected = false;
  try { ReferenceCapture::load(path); }
  catch (const std::invalid_argument &) { rejected = true; }
  require(rejected, "Invalid receiver normals cannot create hemisphere rays");
  { std::ofstream geometryInput(path); geometryInput << "geometry\n1 2 3 0 0 1\n4 5 6 -1 0 0\n"; }
  auto geometry = ReferenceCapture::load(path);
  geometry.maximumGeometryDistance = 100;
  require(geometry.geometry && geometry.tasks() == 2 && geometry.samples == 1 && !geometry.usesRadianceRays(),
          "Geometry requests dispatch exact unit rays instead of lighting quadrature");
  const auto geometryRays = geometry.batch(0,2);
  require(geometryRays[0].normal == remix::forward::Vec4{0,0,1,0} &&
              geometryRays[1].position == remix::forward::Vec4{4,5,6,0},
          "Geometry batches retain exact origin/direction identities");
  Result geometryMiss{}; geometryMiss.irradiance[0][3] = 100; geometryMiss.statistics = {0,1,1,0};
  Result geometryHit{}; geometryHit.irradiance[0][3] = 2; geometryHit.statistics = {1,0,1,0};
  geometry.consume(1,std::span(&geometryMiss,1));
  geometry.consume(0,std::span(&geometryHit,1));
  {
    std::ifstream json(output);
    const std::string result((std::istreambuf_iterator<char>(json)),{});
    require(result.find("\"mode\":\"geometry\"") != std::string::npos &&
                result.find("\"maximumDistance\":100,\"firstHitDistance\":2,\"miss\":false,\"opaqueBackface\":true") != std::string::npos &&
                result.find("\"firstHitDistance\":null,\"miss\":true") != std::string::npos &&
                result.find("\"irradiance\"") == std::string::npos && result.find("distanceMoments") == std::string::npos,
            "Geometry output distinguishes hit/miss and traversal limit without fabricated lighting");
  }
  { std::ofstream bad(path); bad << "geometry\n1 2 3 0 0 0\n"; }
  rejected = false;
  try { ReferenceCapture::load(path); } catch (const std::invalid_argument &) { rejected = true; }
  require(rejected,"Geometry rays reject nonunit directions");
  { std::ofstream sites(path); sites << "surface 8192\n1 2 3 0 0 -1\n"; }
  auto surface = ReferenceCapture::load(path);
  require(surface.surface && !surface.hemisphere && surface.tasks() == 1 && surface.samples == 8192,
          "Surface references dispatch one full irradiance estimate per site, not primary radiance rays");
  const auto sites = surface.batch(0, 1);
  require(sites[0].position == remix::forward::Vec4{1, 2, 3, 0} &&
              sites[0].normal == remix::forward::Vec4{0, 0, -1, 0},
          "Surface reference transport retains receiver positions and normals for direct-light integration");
  Result surfaceValue{};
  surfaceValue.irradiance[2][0] = -2;
  surface.consume(0, std::span(&surfaceValue, 1));
  {
    std::ifstream json(output);
    const std::string result((std::istreambuf_iterator<char>(json)), {});
    require(result.find("\"mode\":\"surface\"") != std::string::npos &&
                result.find("\"normal\":[0,0,-1],\"diffuseIrradiance\":[0.977205") != std::string::npos,
            "Surface reference reconstructs irradiance in the supplied normal, preserving directional SH");
  }
  { std::ofstream bad(path); bad << "surface 64\n1 2 3 0 0 2\n"; }
  rejected = false;
  try { ReferenceCapture::load(path); }
  catch (const std::invalid_argument &) { rejected = true; }
  require(rejected, "Surface references reject non-unit normals before transport");
  {
    std::ofstream sites(path);
    sites << "surface-offsets 256\n1 2 3 0 0 -1 0\n1 2 3 0 0 -1 1024\n";
  }
  auto surfaceStreams = ReferenceCapture::load(path);
  require(surfaceStreams.surface && surfaceStreams.samples == 256 && surfaceStreams.tasks() == 2 &&
              surfaceStreams.sampleOffsets == std::vector<uint32_t>{0, 1024},
          "Surface convergence streams retain independent sample offsets");
  const auto streamSites = surfaceStreams.batch(0, 2);
  require(streamSites[0].position == streamSites[1].position &&
              streamSites[0].normal == remix::forward::Vec4{0, 0, -1, 0} &&
              streamSites[0].normal == streamSites[1].normal,
          "Changing the surface stream never moves or rotates the receiver");
  surfaceStreams.consume(0, std::span(&surfaceValue, 1));
  surfaceStreams.consume(1, std::span(&surfaceValue, 1));
  {
    std::ifstream json(output);
    const std::string result((std::istreambuf_iterator<char>(json)), {});
    require(result.find("\"mode\":\"surface\"") != std::string::npos &&
                result.find("\"firstSample\":1024") != std::string::npos &&
                result.find("\"diffuseIrradiance\":[0.977205") != std::string::npos,
            "Surface stream output retains the estimator, normal reconstruction and sample provenance");
  }
  for (const auto *row : {"1 2 3 0 0 -1", "1 2 3 0 0 -1 -1", "1 2 3 0 0 -1 4294967040",
                          "1 2 3 0 0 2 0"}) {
    { std::ofstream bad(path); bad << "surface-offsets 256\n" << row << '\n'; }
    rejected = false;
    try { ReferenceCapture::load(path); }
    catch (const std::invalid_argument &) { rejected = true; }
    require(rejected, "Surface streams reject missing/invalid offsets, range overflow and non-unit normals");
  }
  {
    std::ofstream sites(path);
    sites << "base surface-offsets 256\n1 2 3 0 0 -1 1024\n";
  }
  auto baseSurface = ReferenceCapture::load(path);
  require(baseSurface.baseLightingOnly && baseSurface.surface && !baseSurface.hemisphere &&
              baseSurface.sampleOffsets == std::vector<uint32_t>{1024} &&
              baseSurface.batch(0, 1)[0].normal == remix::forward::Vec4{0, 0, -1, 0},
          "Base-only references retain surface normals and sample-stream provenance");
  baseSurface.consume(0, std::span(&surfaceValue, 1));
  {
    std::ifstream json(output);
    const std::string result((std::istreambuf_iterator<char>(json)), {});
    require(result.find("\"baseLightingOnly\":true") != std::string::npos &&
                result.find("\"combinedLighting\":false") != std::string::npos &&
                result.find("\"firstSample\":1024") != std::string::npos,
            "Base reference output identifies its requested and effective lighting components");
  }
  { std::ofstream sites(path); sites << "base hemisphere 64\n1 2 3 0 0 1\n"; }
  require(ReferenceCapture::load(path).hemisphere && ReferenceCapture::load(path).baseLightingOnly,
          "The base prefix also preserves independent hemisphere integration");
  { std::ofstream sites(path); sites << "base 64\n1 2 3\n"; }
  require(ReferenceCapture::load(path).baseLightingOnly && !ReferenceCapture::load(path).surface,
          "The base prefix preserves the original normal-free SH reference format");
  for (const auto *input : {"base", "base base 64\n1 2 3", "base surface 64\n1 2 3 0 0 0",
                             "incoming-sh-offsets 64\n1 2 3", "incoming-sh-offsets 64\n1 2 3 4294967295"}) {
    { std::ofstream bad(path); bad << input; }
    rejected = false;
    try { ReferenceCapture::load(path); }
    catch (const std::invalid_argument &) { rejected = true; }
    require(rejected, "Malformed reference prefixes, normals and incoming sample ranges are rejected");
  }
  for (const auto *mode : {"incoming-sh", "incoming-sh-offsets"}) {
    { std::ofstream sites(path); sites << "base " << mode << " 256\n1 2 3";
      if (std::string(mode) == "incoming-sh-offsets") sites << " 1024";
      sites << '\n'; }
    auto incoming = ReferenceCapture::load(path);
    require(incoming.incomingIrradiance && incoming.baseLightingOnly && !incoming.usesRadianceRays() &&
                incoming.tasks() == 1 && incoming.batch(0, 1)[0].normal == remix::forward::Vec4{},
            "Incoming SH preserves normal-free bounded transport and base component selection");
    require((incoming.sampleOffsets.empty() && std::string(mode) == "incoming-sh") ||
                incoming.sampleOffsets == std::vector<uint32_t>{1024},
            "Incoming SH retains explicit sample streams without inventing an offset");
    Result value{};
    value.irradiance[0][0] = 1;
    incoming.consume(0, std::span(&value, 1));
    std::ifstream receipt(output);
    const std::string text((std::istreambuf_iterator<char>(receipt)), {});
    require(text.find("\"mode\":\"incoming-sh\"") != std::string::npos &&
                text.find("\"analyticReceiverDirectPolicy\":false") != std::string::npos,
            "Incoming SH output explicitly identifies its estimator and excluded receiver-direct policy");
  }
  std::filesystem::remove(path);
  std::filesystem::remove(output);
}
