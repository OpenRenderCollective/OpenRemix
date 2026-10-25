#include "forward/bake/coarse_transfer.h"
void coarseTransferPlanTests(bake::Grid grid) {
  bake::CoarseTransfer::Options options;
  auto limited = options;
  limited.maximumBytes = 1;
  require(!bake::CoarseTransfer::plan(grid, limited), "Transfer planning declines an insufficient byte budget");
  limited = options; limited.maximumRays = 1;
  require(!bake::CoarseTransfer::plan(grid, limited), "Transfer planning declines an insufficient ray budget");
  auto planned = bake::CoarseTransfer::plan(grid, options);
  require(planned && planned->rays() > 2 && planned->scratchBytes() <= options.maximumBytes &&
              planned->maximumDistance() > 0,
          "Adaptive transfer planning fits bounded scratch without growing the probe field");
  auto &plan = *planned;
  const auto rays = plan.batch(0,plan.rays());
  for (const auto &ray : rays)
    require(std::abs(ray.normal[0]*ray.normal[0] + ray.normal[1]*ray.normal[1] +
                     ray.normal[2]*ray.normal[2]-1) < 1e-6f,
            "Prepared transfer rays have unit directions");
  bool rejected = false;
  try { plan.words(); } catch (const std::logic_error &) { rejected = true; }
  require(rejected, "Incomplete transfer readback cannot publish a packed field");
  const float unknown = -2, blocked = 0;
  plan.consume(0,{&unknown,1});
  std::vector<float> clear(plan.rays()-2,-1);
  plan.consume(2,clear);
  require(!plan.complete(), "Out-of-order readback retains the outstanding transfer slot");
  const float invalid = std::numeric_limits<float>::quiet_NaN();
  rejected = false;
  try { plan.consume(1,{&invalid,1}); } catch (const std::invalid_argument &) { rejected = true; }
  require(rejected && !plan.complete(), "Nonfinite readback cannot complete a field");
  plan.consume(1,{&blocked,1});
  const auto words = plan.words();
  require(words[0] == bake::CoarseTransfer::magic && words[1] == 12 && words[2] == 1 && words[3] == words.size(),
          "Packed transfer header describes the complete bounded directory");
  const uint32_t patch = words[4], count = words[patch+1], fields = words[patch+2];
  require(words[patch] == 2 && count == 27, "Patch fields preserve actual local refinement density");
  uint32_t known = 0, zero = 0, unknownEntries = 0;
  for (uint32_t node=0; node<count; ++node) for (uint32_t c=0; c<8; ++c) {
    const auto field = fields+node*3;
    const bool valid = (words[field+2] & (1u<<c)) != 0;
    const auto value = (words[field+c/4] >> ((c%4)*8)) & 255u;
    known += valid; zero += valid && value == 0;
    if (!valid) { ++unknownEntries; require(value == 255, "Unknown transfer stores ordinary fallback"); }
  }
  require(known == 27*8-1 && zero == 1 && unknownEntries == 1,
          "Shared endpoints are clear without rays; fenced results retain known and unknown separately");
  rejected = false;
  try { plan.consume(1,{&blocked,1}); } catch (const std::logic_error &) { rejected = true; }
  require(rejected, "Duplicate transfer completion cannot silently overwrite a field");
  auto transactional = bake::CoarseTransfer::plan(grid,options);
  const std::array<float,2> invalidBatch{0,invalid};
  rejected = false;
  try { transactional->consume(0,invalidBatch); } catch (const std::invalid_argument &) { rejected = true; }
  require(rejected, "A readback batch validates before mutating any transfer entries");
  std::vector<float> allClear(transactional->rays(),-1);
  transactional->consume(0,allClear);
  require(transactional->complete(), "Rejected batch leaves every slot available for a valid retry");
  auto unsupportedGrid = grid;
  unsupportedGrid.probes[12].statistics[3] = 0;
  auto unsupported = bake::CoarseTransfer::plan(unsupportedGrid,options);
  std::vector<float> supportedClear(unsupported->rays(),-1);
  unsupported->consume(0,supportedClear);
  const auto partial = unsupported->words();
  const auto unknownNode = partial[partial[4]+2];
  require(partial[unknownNode] == ~0u && partial[unknownNode+1] == ~0u && partial[unknownNode+2] == 0,
          "Unavailable fine support stays explicitly unknown rather than becoming an opaque blocker");
  grid.header.settings[2] = 5;
  grid.probes[0].irradiance[0][3] = .1f;
  const auto relocated = bake::CoarseTransfer::plan(grid,options);
  const auto first = relocated->batch(0,1).front();
  require(first.position[0] > .09f && first.position[0] < .11f && first.normal[0] < -.99f,
          "Transfer endpoints include actual retained probe relocation offsets");
  std::stop_source stop; stop.request_stop();
  rejected = false;
  try { bake::CoarseTransfer::plan(grid,options,stop.get_token()); }
  catch (const std::runtime_error &) { rejected = true; }
  require(rejected, "Superseded transfer planning stops without publishing partial state");
}
