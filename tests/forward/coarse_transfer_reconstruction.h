void coarseTransferReconstructionTests(bake::Grid grid) {
  const Vec3 position{1,2,2}, normal{0,0,1};
  const auto original = bake::probeStencil(grid,position,normal);
  const auto baseline = bake::sample(grid,position,normal);
  auto plan = bake::CoarseTransfer::plan(grid,{});
  std::vector<float> clear(plan->rays(),-1);
  plan->consume(0,clear);
  std::vector<uint32_t> words(plan->words().begin(),plan->words().end());
  auto attach = [&](const auto &data) { grid.coarseTransferWords = std::make_shared<const std::vector<uint32_t>>(data); };
  attach(words);
  require(bake::probeStencil(grid,position,normal).weights == original.weights,
          "Clear transfer preserves the exact ordinary weights");
  const auto patch = words[4], fields = words[patch+2], count = words[patch+1];
  for (uint32_t node=0; node<count; ++node) {
    words[fields+node*3] = words[fields+node*3+1] = 0;
    words[fields+node*3+2] = 255;
  }
  attach(words);
  const auto corrected = bake::sample(grid,position,normal);
  require(baseline && corrected && (*corrected)[0] > (*baseline)[0] &&
              std::abs((*corrected)[0]-10.f/3) < 1e-5f,
          "Geometry correction changes the actual CPU reconstruction with common normalization");
  const auto left = bake::sample(grid,{4-1e-4f,2,2},normal), right = bake::sample(grid,{4+1e-4f,2,2},normal);
  require(left && right && std::abs((*left)[0]-(*right)[0]) < 1e-6f,
          "Corrected reconstruction returns continuously to coarse fallback at the patch edge");
  auto missing = words;
  for (uint32_t node=0; node<count; ++node) missing[fields+node*3+2] = 0;
  attach(missing);
  require(bake::probeStencil(grid,position,normal).weights == original.weights,
          "Unknown zero bytes retain exact ordinary weights");
  auto malformed = words; malformed[3]++;
  attach(malformed);
  require(bake::probeStencil(grid,position,normal).weights == original.weights,
          "Malformed field shape falls back without changing weights");
  auto unmatched = words;
  for (uint32_t c=0; c<8; ++c) unmatched[patch+4+c] = 0;
  attach(unmatched);
  require(bake::probeStencil(grid,position,normal).weights == original.weights,
          "Coarse donor identity mismatch cannot darken the field");
  auto wrongFine = original;
  for (uint32_t f=0; f<8; ++f) wrongFine.indices[8+f] = uint32_t(grid.probes.size());
  const auto wrongWeights = wrongFine.weights;
  bake::applyCoarseTransfer(words,grid,wrongFine);
  require(wrongFine.weights == wrongWeights, "Fine donor identity mismatch retains unknown fallback");
  attach(words);
  for (auto &probe : grid.probes) probe.irradiance[0] = {.7f/.2820947918f,0,0,0};
  const auto constant = bake::sample(grid,position,normal);
  require(constant && std::abs((*constant)[0]-.7f) < 1e-5f,
          "Constant irradiance energy survives the integrated CPU path");
  for (auto &probe : grid.probes) for (auto &coefficient : probe.irradiance) coefficient = {};
  const auto black = bake::sample(grid,position,normal);
  require(black && *black == Vec3{}, "Corrected black remains covered rather than becoming missing");
}
