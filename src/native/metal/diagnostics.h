#pragma once
// The tracer's frame-pacing diagnostics that the API layer reads by name
// (api/lifecycle.cpp timings.request). The Vulkan tracer declares the first
// three in pathtrace/renderer.h beside its Vulkan types; these are copies of
// them for Metal builds, which time none of those Vulkan steps (they stay
// zero). The sparse and volumetric states come from the shared headers.
#include "pathtrace/block_costs.h"
#include "pathtrace/volume_policy.h"
#include <cstdint>

namespace remix::trace {
  struct TableCosts {
    double total = 0, grow = 0, write = 0, compare = 0;
    uint64_t bytes = 0, capacity = 0, grown = 0, copied = 0;
  };
  inline TableCosts g_tableCosts;
  struct BlockCosts {
    uint32_t ready = 0, awaited = 0, none = 0, large = 0;
    double slowest = 0;
    uint64_t slowestBuffer = 0;
    uint32_t slowestType = 0, slowestHow = 0, slowestBlocks = 0;
    bool slowestHost = false;
    double slowestBind = 0, slowestMap = 0, slowestCreate = 0;
    bool bindFirst = false;
  };
  inline BlockCosts g_blockCosts;
  struct AccelCosts {
    double total = 0, bottom = 0, top = 0, create = 0, record = 0, queryPool = 0, instances = 0, sizes = 0;
    double decide = 0, fill = 0, write = 0, storage = 0, topRecord = 0, storageInstances = 0, storageAccel = 0;
    uint64_t written = 0;
    uint32_t created = 0, bottomBuilds = 0, topInstances = 0, staticFilled = 0;
    bool update = false, staticChanged = false, keptPrefix = false;
  };
  inline AccelCosts g_accelNow, g_accelCosts;
} // namespace remix::trace
