#include "forward/probes/capture_dump.h"
#include "forward/probes/receiver_audit.h"
#include "forward/probes/receiver_sites.h"
#include "forward/probes/receivers.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace p = remix::forward::probes;
namespace s = remix::scene;
namespace {
  void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
  }
  void environment(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
  }
  struct Environment {
    const char *name;
    std::optional<std::string> previous;
    explicit Environment(const char *key) : name(key) {
      if (const char *value = std::getenv(name)) previous = value;
      environment(name, nullptr);
    }
    ~Environment() { environment(name, previous ? previous->c_str() : nullptr); }
  };
  s::BakeScene fixture() {
    s::BakeScene source;
    source.sceneScale = 1;
    auto surface = std::make_shared<s::Surface>();
    const float points[][3] = {{0, 0, 0}, {2, 0, 0}, {0, 1, 0}};
    for (const auto &point : points) {
      s::Vertex vertex{};
      std::copy_n(point, 3, vertex.position);
      surface->vertices.push_back(vertex);
    }
    surface->indices = {0, 1, 2};
    s::Draw draw;
    draw.surface = surface;
    source.draws.push_back(draw);
    s::BakeSurfaceChart chart;
    chart.width = chart.height = 2;
    chart.texcoords.resize(3);
    source.charts.push_back(chart);
    return source;
  }
}
int main(int argc, char **argv) {
  require(argc == 2, "Expected owned report path");
  Environment local("REMIX_DEBUG_PROBE_LOCAL_CAPTURE"), report("REMIX_DEBUG_PROBE_RECEIVER_VISIBILITY");
  const auto source = fixture();
  std::vector<p::Capture> captures(8);
  for (auto &capture : captures) {
    capture.position = {0, 0, 10};
    capture.resolution = 8;
    capture.bounds.include({-1, -1, 9});
    capture.bounds.include({1, 1, 11});
  }
  require(!p::prepareReceiverAudit(source, captures), "Unrequested preparation allocated an audit");
  environment(local.name, "0");
  require(!p::prepareReceiverAudit(source, captures), "Disabled local capture allocated an audit");
  environment(local.name, "1");
  const auto prepared = p::prepareReceiverAudit(source, captures);
  require(prepared && prepared->rays().size() == 32768,
          "Local capture needs a bounded audit without a report filename");

  const auto receivers = p::sampleReceivers(source, 8192);
  const auto ray = p::receiverRay(receivers[0], captures[0].position, .002f);
  require(bool(ray), "Fixture receiver has no connection ray");
  std::vector<p::ReceiverPair> pairs;
  for (uint32_t rank = 0; rank < 8; ++rank) pairs.push_back({0, rank, *ray, rank});
  p::ReceiverAudit blocked(std::move(pairs), .001f);
  std::vector<remix::forward::bake::Result> hits(8);
  for (auto &hit : hits) { hit.irradiance[0][3] = 1; hit.statistics[2] = 1; }
  require(blocked.observe(0, hits), "Fixture visibility did not complete");
  auto sites = p::prepareReceiverSites(source, blocked);
  require(sites && sites->sites().size() == 1, "Local sites require no report filename");

  const std::filesystem::path path(argv[1]);
  const auto sitePath = path.string() + ".sites.json";
  require(!std::filesystem::exists(path) && !std::filesystem::exists(sitePath), "Report fixture path already exists");
  { std::ofstream output(path); output << "preserve-audit"; }
  { std::ofstream output(sitePath); output << "preserve-sites"; }
  environment(report.name, argv[1]);
  require(bool(p::prepareReceiverAudit(source, captures)), "Existing report blocked retained audit evidence");
  require(bool(p::prepareReceiverSites(source, blocked)), "Existing report blocked retained site evidence");
  p::dumpReceiverAudit(source, captures, blocked);
  std::ifstream input(path);
  std::string contents;
  std::getline(input, contents);
  require(contents == "preserve-audit", "Diagnostic overwrote an existing report");
  input.close();
  environment(local.name, "0");
  require(!p::prepareReceiverAudit(source, captures) && !p::prepareReceiverSites(source, blocked),
          "Report-only request must retain fresh-output protection");
  std::filesystem::remove(path);
  std::filesystem::remove(sitePath);
  require(bool(p::prepareReceiverAudit(source, captures)), "Fresh report-only request no longer works");
  std::stop_source cancelled;
  cancelled.request_stop();
  require(!p::prepareReceiverAudit(source, captures, cancelled.get_token()), "Cancelled preparation produced work");
}
