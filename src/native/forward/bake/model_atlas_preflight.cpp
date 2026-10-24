#include "forward/bake/model_preflight.h"
#include "forward/bake/model_charts.h"
#include "forward/bake/model_triangles.h"
#include "forward/lightmap/data.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace remix::forward::bake {
  void writeModelAtlasPreflight(const scene::BakeScene &source, lightmap::Options options,
      const std::filesystem::path &path, std::stop_token cancelled) {
    using Clock = std::chrono::steady_clock;
    const auto check = [&] {
      if (cancelled.stop_requested()) throw std::runtime_error("Model atlas preflight superseded");
    };
    check();
    if (std::filesystem::exists(path)) throw std::runtime_error("Model atlas preflight output already exists");
    if (!source.ready || source.draws.size() > 32768 || options.padding > 16 ||
        options.pageSize < 8 || options.pageSize > 4096 || !std::has_single_bit(options.pageSize) ||
        !options.maxPages || options.maxPages > 256 || !options.maxTexels || options.maxTexels > (64u << 20))
      throw std::invalid_argument("Invalid model atlas preflight source or budget");
    const uint64_t capacity = std::min(options.maxTexels,
        uint64_t(options.maxPages) * options.pageSize * options.pageSize);
    const uint64_t minimumSide = 2 + 2 * options.padding;
    const uint64_t maximumCharts = capacity / (minimumSide * minimumSide);
    if (source.charts.size() > maximumCharts)
      throw std::length_error("Authored charts leave no model chart capacity");
    const auto modelLimit = uint32_t(std::min<uint64_t>(1048576, maximumCharts - source.charts.size()));
    std::fprintf(stderr, "Model atlas preflight: counting eligible triangles\n");
    const auto started = Clock::now();
    uint64_t triangles = 0;
    forEachModelTriangle(source, [&](const ModelTriangle &) {
      if (++triangles > 4000000) throw std::length_error("Model atlas preflight triangle budget exceeded");
    }, cancelled);
    const auto counted = Clock::now();
    std::fprintf(stderr, "Model atlas preflight: generating charts for %llu triangles\n",
                 static_cast<unsigned long long>(triangles));
    if (triangles && !modelLimit) throw std::length_error("No model chart capacity in atlas preflight");
    auto charts = triangles ? modelCharts(source, modelLimit, cancelled) : std::vector<scene::BakeSurfaceChart>{};
    const auto generated = Clock::now();
    const auto modelChartsCount = charts.size();
    std::fprintf(stderr, "Model atlas preflight: fitting %zu authored + %zu model charts\n",
                 source.charts.size(), modelChartsCount);
    auto derived = source;
    derived.charts.insert(derived.charts.end(), std::make_move_iterator(charts.begin()), std::make_move_iterator(charts.end()));
    const auto layout = lightmap::fit(derived, options, cancelled);
    const auto fitted = Clock::now();
    std::fprintf(stderr, "Model atlas preflight: fit complete, %u pages at %.6g m spacing\n",
                 layout.pages, layout.luxelMetres);
    uint64_t rectangleTexels = 0, modelRectangleTexels = 0;
    for (size_t i = 0; i < layout.charts.size(); ++i) {
      const auto &a = layout.charts[i];
      const auto area = uint64_t(a.width) * a.height;
      rectangleTexels += area;
      if (i >= source.charts.size()) modelRectangleTexels += area;
    }
    const uint64_t allocated = uint64_t(layout.pages) * layout.pageSize * layout.pageSize;
    if (allocated > options.maxTexels || layout.pages > options.maxPages)
      throw std::runtime_error("Model atlas preflight exceeded requested capacity");
    const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    std::ostringstream text;
    text.imbue(std::locale::classic()); text.precision(12);
    text << "{\"version\":1,\"sceneHash\":\"" << source.contentHash << "\",\"authoredCharts\":" << source.charts.size()
         << ",\"eligibleModelTriangles\":" << triangles << ",\"generatedModelCharts\":" << modelChartsCount
         << ",\"studyModelChartLimit\":" << modelLimit << ",\"requestedTexels\":" << options.maxTexels
         << ",\"requestedPageSize\":" << options.pageSize << ",\"maximumPages\":" << options.maxPages
         << ",\"fillBudget\":" << (options.fillBudget ? "true" : "false")
         << ",\"allocatedTexels\":" << allocated << ",\"pageSize\":" << layout.pageSize << ",\"pages\":" << layout.pages
         << ",\"rectangleTexels\":" << rectangleTexels << ",\"modelRectangleTexels\":" << modelRectangleTexels
         << ",\"surfacePoints\":" << layout.points.size() << ",\"seamSamples\":" << layout.seamSamples
         << ",\"luxelMetres\":" << layout.luxelMetres
         << ",\"raw20LayerAtlasBytes\":" << allocated * lightmap::atlasBytesPerTexel
         << ",\"countMs\":" << ms(started, counted) << ",\"modelChartMs\":" << ms(counted, generated)
         << ",\"copyAndFitMs\":" << ms(generated, fitted)
         << ",\"chartAlgorithm\":\"current consecutive patches\",\"scope\":\"CPU layout only with authored and generated model charts. "
            "No lighting transport, GPU allocation, cache publication or live draw validation. "
            "Raw atlas bytes exclude additional lighting fields and are not measured VRAM use.\"}\n";
    check();
    if (std::filesystem::exists(path)) throw std::runtime_error("Model atlas preflight output already exists");
    std::ofstream output(path);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output << text.str(); output.close();
  }
}
