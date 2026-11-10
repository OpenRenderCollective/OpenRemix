#pragma once

void sparseDirectWorkTests() {
  std::vector<uint32_t> offsets{0, 7, 7, 9};
  std::vector<m::DirectWork::Owner> owners{{0, 17}, {1, 20}, {2, 3}, {3, 4}, {4, 1},
                                        {5, 40}, {6, 0}, {7, 52}, {8, 90}};
  lm::Layout layout;
  layout.pointCharts = {0, 1, 2, 0, 2, 1};
  layout.points.resize(layout.pointCharts.size());
  const m::DirectWork work(9, offsets, owners, layout);
  require(work.size() == 18, "Seven overlapping owners and empty charts retain all receiver tasks");
  std::vector<m::Task> expected;
  for (uint32_t point = 0; point < layout.pointCharts.size(); ++point) {
    const auto chart = layout.pointCharts[point];
    for (uint32_t i = offsets[chart]; i < offsets[chart + 1]; ++i)
      expected.push_back({point, owners[i].light, owners[i].output});
  }
  require(expected[6].light == 6 && expected[6].channel == 0 &&
              expected[7].point == 2 && expected[7].light == 7 && expected[7].channel == 52,
          "Sparse output tags are independent of legacy RGBA channels and empty receivers");
  for (uint32_t first = 0; first <= work.size(); ++first)
    for (uint32_t count = 0; count <= work.size() - first; ++count) {
      const auto batch = work.batch(first, count);
      for (uint32_t i = 0; i < count; ++i)
        require(batch[i].point == expected[first+i].point && batch[i].light == expected[first+i].light &&
                    batch[i].channel == expected[first+i].channel,
                "Arbitrary split batches preserve every sparse receiver, light and output tag");
    }
  auto invalid = [&](auto action) {
    bool refused = false;
    try { action(); } catch (const std::exception &) { refused = true; }
    require(refused, "Malformed sparse direct work is rejected before submission");
  };
  const std::vector<uint32_t> descending{0, 8, 7, 9}, truncated{0, 7, 7, 8};
  invalid([&] { m::DirectWork bad(9, descending, owners, layout); });
  invalid([&] { m::DirectWork bad(9, truncated, owners, layout); });
  auto duplicate = owners;
  duplicate[1].light = duplicate[0].light;
  invalid([&] { m::DirectWork bad(9, offsets, duplicate, layout); });
  duplicate = owners; duplicate[1].output = duplicate[0].output;
  invalid([&] { m::DirectWork bad(9, offsets, duplicate, layout); });
  invalid([&] { m::DirectWork bad(8, offsets, owners, layout); });
  duplicate = owners; duplicate[1].output = m::none;
  invalid([&] { m::DirectWork bad(9, offsets, duplicate, layout); });
  std::stop_source stop;
  stop.request_stop();
  invalid([&] { m::DirectWork bad(9, offsets, owners, layout, stop.get_token()); });
  owners[0] = {8, 999}; offsets[1] = 0; layout.pointCharts.assign(layout.points.size(), 1);
  const auto snapshot = work.batch(0, 1);
  require(snapshot[0].point == 0 && snapshot[0].light == 0 && snapshot[0].channel == 17,
          "Queued work owns its sparse tables independently of caller mutation");
  std::vector<m::DirectWork::Owner> many(50000);
  for (uint32_t i = 0; i < many.size(); ++i) many[i] = {i, i};
  const uint32_t denseOffsets[] = {0, uint32_t(many.size())};
  layout.points.resize(100000); layout.pointCharts.assign(layout.points.size(), 0);
  bool overflow = false;
  try { m::DirectWork bad(uint32_t(many.size()), denseOffsets, many, layout); }
  catch (const std::length_error &) { overflow = true; }
  require(overflow, "Sparse ownership cannot wrap a 32-bit receiver-task prefix");
}
