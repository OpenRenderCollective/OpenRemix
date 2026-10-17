#include "forward/probes/regions.h"
#include "forward/probes/partition_cells.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace remix::forward::probes {
  namespace {
    bool valid(const Bounds &box) {
      if (!box.valid)
        return false;
      for (uint32_t c = 0; c < 3; ++c)
        if (!std::isfinite(box.minimum[c]) || !std::isfinite(box.maximum[c]) ||
            box.minimum[c] >= box.maximum[c])
          return false;
      return true;
    }
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Reflection region fitting superseded");
    }
    using detail::Box;
    using detail::clip;
    class EmptySpace {
    public:
      explicit EmptySpace(const scene::VisibilityPartition &partition, std::stop_token cancelled = {})
          : m_partition(partition), m_cancelled(cancelled), m_active(partition.nodes.size()) {}
      bool contains(const Bounds &box, RegionTolerance tolerance = {}) {
        if (!valid(box) || !m_partition.closed || m_partition.nodes.empty())
          return false;
        if (++m_generation == 0) {
          std::fill(m_active.begin(), m_active.end(), 0);
          ++m_generation;
        }
        m_stack.clear();
        m_solids.clear();
        Box initial;
        for (uint32_t c = 0; c < 3; ++c) {
          initial.minimum[c] = box.minimum[c];
          initial.maximum[c] = box.maximum[c];
        }
        m_stack.push_back({0, false, initial});
        size_t visited = 0;
        while (!m_stack.empty()) {
          if (++visited > m_partition.nodes.size() * 4 + 1024)
            return false;
          if ((visited & 255u) == 0)
            check(m_cancelled);
          const auto entry = m_stack.back();
          m_stack.pop_back();
          if (entry.index < 0) {
            const auto leaf = uint64_t(-1 - int64_t(entry.index));
            if (leaf >= m_partition.leafClusters.size())
              return false;
            const int32_t cluster = m_partition.leafClusters[leaf];
            if (cluster >= 0) {
              if (uint32_t(cluster) >= m_partition.clusterCount)
                return false;
            } else {
              if (tolerance.detailDepth <= 0 || m_solids.size() == 256)
                return false;
              m_solids.push_back(entry.box);
            }
            continue;
          }
          if (size_t(entry.index) >= m_partition.nodes.size())
            return false;
          if (entry.completed) {
            m_active[entry.index] = 0;
            continue;
          }
          if (m_active[entry.index] == m_generation)
            return false;
          const auto &node = m_partition.nodes[entry.index];
          double normalSquared = 0;
          for (uint32_t c = 0; c < 4; ++c) {
            if (!std::isfinite(node.plane[c]))
              return false;
            if (c < 3)
              normalSquared += double(node.plane[c]) * node.plane[c];
          }
          if (!normalSquared)
            return false;
          m_active[entry.index] = m_generation;
          m_stack.push_back({entry.index, true, entry.box});
          for (uint32_t child = 0; child < 2; ++child) {
            auto fragment = entry.box;
            if (clip(fragment, node.plane, child ? -1 : 1))
              m_stack.push_back({node.children[child], false, fragment});
          }
        }
        return detailsFit(initial, tolerance);
      }

    private:
      bool detailsFit(const Box &region, RegionTolerance tolerance) {
        // Reassemble rectangular wall fragments before classification. Avoid
        // filling the empty corner of an L-shaped trim union: connected
        // ceiling bevels must not become one room-sized solid bounding box.
        bool merged;
        do {
          check(m_cancelled);
          merged = false;
          for (size_t i = 0; !merged && i < m_solids.size(); ++i)
            for (size_t j = i + 1; j < m_solids.size(); ++j) {
              bool touches = true;
              for (uint32_t c = 0; c < 3; ++c)
                touches &= m_solids[i].minimum[c] <= m_solids[j].maximum[c] &&
                           m_solids[j].minimum[c] <= m_solids[i].maximum[c];
              if (!touches)
                continue;
              double a = 1, b = 1, overlap = 1, joined = 1;
              for (uint32_t c = 0; c < 3; ++c) {
                a *= m_solids[i].maximum[c] - m_solids[i].minimum[c];
                b *= m_solids[j].maximum[c] - m_solids[j].minimum[c];
                overlap *= std::max(0., std::min(m_solids[i].maximum[c], m_solids[j].maximum[c]) -
                                            std::max(m_solids[i].minimum[c], m_solids[j].minimum[c]));
                joined *= std::max(m_solids[i].maximum[c], m_solids[j].maximum[c]) -
                          std::min(m_solids[i].minimum[c], m_solids[j].minimum[c]);
              }
              if (a + b - overlap < joined * .9)
                continue;
              for (uint32_t c = 0; c < 3; ++c) {
                m_solids[i].minimum[c] = std::min(m_solids[i].minimum[c], m_solids[j].minimum[c]);
                m_solids[i].maximum[c] = std::max(m_solids[i].maximum[c], m_solids[j].maximum[c]);
              }
              m_solids.erase(m_solids.begin() + j);
              merged = true;
              break;
            }
        } while (merged);
        double ignored = 0, total = 1;
        for (uint32_t c = 0; c < 3; ++c)
          total *= region.maximum[c] - region.minimum[c];
        for (const auto &solid : m_solids) {
          uint32_t thinAxes = 0, interiorAxes = 0;
          double volume = 1;
          for (uint32_t c = 0; c < 3; ++c) {
            const double width = solid.maximum[c] - solid.minimum[c];
            const double extent = region.maximum[c] - region.minimum[c];
            thinAxes += width <= std::min(double(tolerance.detailDepth), extent * .25);
            interiorAxes += width <= extent * .5 && solid.minimum[c] > region.minimum[c] &&
                            solid.maximum[c] < region.maximum[c];
            volume *= width;
          }
          if (thinAxes < 2 && interiorAxes < 2)
            return false;
          ignored += volume;
        }
        return ignored <= total * tolerance.maxIgnoredFraction;
      }
      struct Entry {
        int32_t index;
        bool completed;
        Box box;
      };
      const scene::VisibilityPartition &m_partition;
      std::stop_token m_cancelled;
      uint32_t m_generation = 0;
      std::vector<uint32_t> m_active;
      std::vector<Entry> m_stack;
      std::vector<Box> m_solids;
    };
    double volume(const Bounds &box) {
      double result = 1;
      for (uint32_t c = 0; c < 3; ++c)
        result *= double(box.maximum[c]) - box.minimum[c];
      return result;
    }
  } // namespace
  struct RegionQuery::State {
    EmptySpace space;
    State(const scene::VisibilityPartition &partition, std::stop_token cancelled)
        : space(partition, cancelled) {}
  };
  RegionQuery::RegionQuery(const scene::VisibilityPartition &partition, std::stop_token cancelled)
      : m_state(std::make_unique<State>(partition, cancelled)) {
  }
  RegionQuery::~RegionQuery() = default;
  bool RegionQuery::contains(const Bounds &bounds) {
    return m_state->space.contains(bounds);
  }
  bool emptyRegion(const scene::VisibilityPartition &partition, const Bounds &bounds) {
    return EmptySpace(partition).contains(bounds);
  }
  Bounds receiverRegion(const scene::VisibilityPartition &partition, const Bounds &projection,
                         float snapDistance, std::stop_token cancelled, RegionTolerance tolerance) {
    check(cancelled);
    if (!valid(projection) || !std::isfinite(snapDistance) || snapDistance < 0)
      throw std::invalid_argument("Invalid reflection receiver bounds");
    EmptySpace space(partition, cancelled);
    if (!snapDistance || !space.contains(projection, tolerance))
      return projection;
    std::array<std::vector<float>, 6> candidates;
    for (const auto &node : partition.nodes) {
      uint32_t axis = 3;
      for (uint32_t c = 0; c < 3; ++c)
        if (node.plane[c] != 0) {
          if (axis != 3) { axis = 3; break; }
          axis = c;
        }
      if (axis == 3)
        continue; // an oblique plane is not a constant axis coordinate
      const float boundary = node.plane[3] / node.plane[axis];
      if (!std::isfinite(boundary))
        continue;
      if (boundary <= projection.minimum[axis] && projection.minimum[axis] - boundary <= snapDistance)
        candidates[axis * 2].push_back(boundary);
      if (boundary >= projection.maximum[axis] && boundary - projection.maximum[axis] <= snapDistance)
        candidates[axis * 2 + 1].push_back(boundary);
    }
    Bounds result = projection;
    for (uint32_t face = 0; face < 6; ++face) {
      check(cancelled);
      auto &coordinates = candidates[face];
      std::sort(coordinates.begin(), coordinates.end(), [face](float a, float b) {
        return face & 1u ? a > b : a < b;
      });
      coordinates.erase(std::unique(coordinates.begin(), coordinates.end()), coordinates.end());
      for (float coordinate : coordinates) {
        auto candidate = result;
        (face & 1u ? candidate.maximum : candidate.minimum)[face / 2] = coordinate;
        if (space.contains(candidate, tolerance)) {
          result = candidate;
          break;
        }
      }
    }
    return result;
  }
  std::optional<Bounds> fitRegion(const scene::VisibilityPartition &partition, const Bounds &limit, Vec3 site,
                                  float margin, std::stop_token cancelled, RegionTolerance tolerance) {
    check(cancelled);
    if (!valid(limit) || !std::isfinite(margin) || margin <= 0 || !std::isfinite(tolerance.detailDepth) ||
        tolerance.detailDepth < 0 || !std::isfinite(tolerance.maxIgnoredFraction) ||
        tolerance.maxIgnoredFraction < 0 || tolerance.maxIgnoredFraction > .25f)
      return {};
    EmptySpace space(partition, cancelled);
    Bounds initial;
    initial.valid = true;
    for (uint32_t c = 0; c < 3; ++c) {
      if (!std::isfinite(site[c]) || site[c] - 2 * margin < limit.minimum[c] ||
          site[c] + 2 * margin > limit.maximum[c])
        return {};
      initial.minimum[c] = site[c] - 2 * margin;
      initial.maximum[c] = site[c] + 2 * margin;
    }
    if (!space.contains(initial))
      return {};
    Bounds best = initial;
    for (uint32_t c = 0; c < 3; ++c) {
      best.minimum[c] += margin;
      best.maximum[c] -= margin;
    }
    std::array<uint32_t, 3> order{0, 1, 2};
    do {
      for (bool reverse : {false, true}) {
        auto candidate = initial;
        for (uint32_t pass = 0; pass < 2; ++pass)
          for (uint32_t axis : order)
            for (uint32_t face = 0; face < 2; ++face) {
              check(cancelled);
              const bool upper = (face != 0) ^ reverse;
              auto &coordinate = upper ? candidate.maximum[axis] : candidate.minimum[axis];
              const float start = coordinate, target = upper ? limit.maximum[axis] : limit.minimum[axis];
              coordinate = target;
              if (space.contains(candidate, tolerance))
                continue;
              float accepted = start, rejected = target;
              for (uint32_t step = 0; step < 24; ++step) {
                check(cancelled);
                const float middle = std::midpoint(accepted, rejected);
                if (middle == accepted || middle == rejected ||
                    std::abs(accepted - rejected) < margin * .125f)
                  break;
                coordinate = middle;
                if (space.contains(candidate, tolerance))
                  accepted = middle;
                else
                  rejected = middle;
              }
              coordinate = accepted;
            }
        // Validate the published, inset box as well: shrinking a box can
        // increase its occupied fraction past the detail budget.
        for (uint32_t c = 0; c < 3; ++c) {
          candidate.minimum[c] += margin;
          candidate.maximum[c] -= margin;
        }
        if (volume(candidate) > volume(best) && space.contains(candidate, tolerance))
          best = candidate;
      }
    } while (std::next_permutation(order.begin(), order.end()));
    for (uint32_t c = 0; c < 3; ++c) {
      if (best.minimum[c] >= best.maximum[c] || site[c] < best.minimum[c] || site[c] > best.maximum[c])
        return {};
    }
    return best;
  }
} // namespace remix::forward::probes
