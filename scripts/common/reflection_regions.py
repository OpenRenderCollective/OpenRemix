"""Bounded, sampled influence proposals. These are not certified free-space volumes."""
from bisect import bisect_left, bisect_right
from itertools import permutations
import math


def inside(position, minimum, maximum):
    return all(minimum[c] <= position[c] <= maximum[c] for c in range(3))


def support(samples, minimum, maximum):
    counts = dict(good=0, bad=0, unknown=0)
    for position, label in samples:
        if inside(position, minimum, maximum):
            counts[label] += 1
    return counts


def propose_seeded_influence(origin, minimum, maximum, samples, clearance, margin):
    """Grow at most 32 origin-anchored seeds in six axis orders.

    Unlike inward greedy cuts, growth cannot discard the seed while removing an
    obstruction. This is a bounded search, not a proof of optimality or free space.
    Held-out observations must never participate in choosing the resulting box.
    """
    # Reuse the same input validation, including strict origin clearance.
    initial = propose_influence(origin, minimum, maximum, samples, clearance, margin)
    if initial is not None:
        return dict(initial, strategy="inwardCuts")
    if not all(minimum[c] + clearance < origin[c] < maximum[c] - clearance for c in range(3)):
        return None
    good = sorted({tuple(p) for p, label in samples if label == "good" and inside(p, minimum, maximum)})
    bad = [p for p, label in samples if label == "bad" and inside(p, minimum, maximum)]
    if not good:
        return None
    # Evenly spaced spatial seeds keep runtime independent of report density.
    seed_count = min(32, len(good))
    indices = [i * (len(good) - 1) // (seed_count - 1)
               for i in range(seed_count)] if seed_count > 1 else [0]
    best = None
    for index in indices:
        seed = good[index]
        lower = [max(minimum[c], min(origin[c] - clearance, seed[c] - margin)) for c in range(3)]
        upper = [min(maximum[c], max(origin[c] + clearance, seed[c] + margin)) for c in range(3)]
        if any(inside(p, lower, upper) for p in bad):
            continue
        for axes in permutations(range(3)):
            lo, hi = list(lower), list(upper)
            for axis in axes:
                blockers = [p[axis] for p in bad if all(
                    lo[c] <= p[c] <= hi[c] for c in range(3) if c != axis)]
                below = [x for x in blockers if x < lo[axis]]
                above = [x for x in blockers if x > hi[axis]]
                lo[axis] = min(lo[axis], max(minimum[axis], max(below) + margin)) if below else minimum[axis]
                hi[axis] = max(hi[axis], min(maximum[axis], min(above) - margin)) if above else maximum[axis]
            counts = support(samples, lo, hi)
            if counts["bad"] or not counts["good"]:
                continue
            score = (counts["good"], math.prod(hi[c] - lo[c] for c in range(3)))
            if best is None or score > best[0]:
                best = (score, {"minimum": lo, "maximum": hi, "trainingSupport": counts,
                                "strategy": "seedGrowth", "seedBudget": 32})
    return best[1] if best else None


def propose_influence(origin, minimum, maximum, samples, clearance, margin, maximum_cuts=24):
    """Greedy inward cuts retain the most good samples, then remove the most bad.

    Axis prefix counts bound each iteration by O(N log N), rather than scanning
    every sample for every proposed cut. Unknown samples cannot earn support.
    A caller must independently validate occupancy, held-out support and rendering.
    """
    if (len(origin) != 3 or len(minimum) != 3 or len(maximum) != 3 or
            not all(math.isfinite(x) for x in [*origin, *minimum, *maximum, clearance, margin]) or
            clearance <= 0 or margin <= 0 or type(maximum_cuts) is not int or
            not 0 < maximum_cuts <= 24 or len(samples) > 8192):
        raise ValueError("Invalid influence proposal bounds, clearance or budget")
    if not all(minimum[c] + clearance < origin[c] < maximum[c] - clearance for c in range(3)):
        return None
    for position, label in samples:
        if len(position) != 3 or not all(math.isfinite(x) for x in position) or label not in {"good", "bad", "unknown"}:
            raise ValueError("Invalid regional support sample")
    lower, upper = list(minimum), list(maximum)
    for iteration in range(maximum_cuts + 1):
        retained = [(position, label) for position, label in samples if inside(position, lower, upper)]
        good = sum(label == "good" for _, label in retained)
        bad = sum(label == "bad" for _, label in retained)
        if not good:
            return None
        if not bad:
            return {"minimum": lower, "maximum": upper, "cuts": iteration,
                    "trainingSupport": support(samples, lower, upper)}
        if iteration == maximum_cuts:
            return None
        best = None
        for axis in range(3):
            ordered = sorted(retained, key=lambda sample: sample[0][axis])
            coordinates = [position[axis] for position, _ in ordered]
            good_prefix, bad_prefix = [0], [0]
            for _, label in ordered:
                good_prefix.append(good_prefix[-1] + (label == "good"))
                bad_prefix.append(bad_prefix[-1] + (label == "bad"))
            boundaries = sorted({position[axis] for position, label in retained if label == "bad"})
            for coordinate in boundaries:
                for lower_face in [True, False]:
                    cut = coordinate + margin if lower_face else coordinate - margin
                    if lower_face:
                        if not lower[axis] < cut < origin[axis] - clearance:
                            continue
                        count = bisect_left(coordinates, cut)
                        lost_good, removed_bad = good_prefix[count], bad_prefix[count]
                    else:
                        if not origin[axis] + clearance < cut < upper[axis]:
                            continue
                        count = bisect_right(coordinates, cut)
                        lost_good, removed_bad = good - good_prefix[count], bad - bad_prefix[count]
                    if not removed_bad or good == lost_good:
                        continue
                    candidate_lower, candidate_upper = list(lower), list(upper)
                    (candidate_lower if lower_face else candidate_upper)[axis] = cut
                    volume = math.prod(candidate_upper[c] - candidate_lower[c] for c in range(3))
                    score = (good - lost_good, removed_bad, volume)
                    if best is None or score > best[0]:
                        best = (score, candidate_lower, candidate_upper)
        if best is None:
            return None
        _, lower, upper = best
    return None
