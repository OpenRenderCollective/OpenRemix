#!/usr/bin/env python3
"""Compare fitted proxies on common, observed-visible receivers; no renderer changes."""
import argparse
import csv
import json
import math
import statistics
from pathlib import Path
from reflection_regions import inside, propose_seeded_influence, support


def load(path):
    if path.stat().st_size > 64 << 20:
        raise ValueError(f"Report exceeds 64 MiB: {path}")
    return json.loads(path.read_text(encoding="utf-8"))


def vector(values):
    if len(values) != 3 or not all(math.isfinite(x) for x in values):
        raise ValueError("Expected three finite coordinates")
    return values


def quantile(values, fraction):
    ordered = sorted(values)
    index = (len(ordered) - 1) * fraction
    low = math.floor(index)
    high = math.ceil(index)
    return ordered[low] + (ordered[high] - ordered[low]) * (index - low)


def summary(values):
    if not values:
        return {"count": 0}
    return {"count": len(values), "meanMetres": statistics.mean(values),
            "medianMetres": statistics.median(values), "p90Metres": quantile(values, .9),
            "p99Metres": quantile(values, .99), "maximumMetres": max(values)}


def proxy_error(origin, position, minimum, maximum):
    """Distance between receiver and far box hit along the origin-to-receiver ray."""
    direction = [p - o for p, o in zip(position, origin)]
    length = math.hypot(*direction)
    if length == 0:
        return None  # No defined projection direction, rather than perfect agreement.
    exit_parameter = math.inf
    for axis, component in enumerate(direction):
        if component != 0:
            plane = maximum[axis] if component > 0 else minimum[axis]
            exit_parameter = min(exit_parameter, (plane - origin[axis]) / component)
    return abs(exit_parameter - 1) * length


def candidate(specification):
    fields = specification.split(":")
    if len(fields) != 2 or any(not field.isdecimal() for field in fields):
        raise argparse.ArgumentTypeError("Candidate must be ATTEMPT:VARIANT")
    return tuple(map(int, fields))


def compare(sites, audit, candidates, fit_error=None, clearance_metres=.1):
    if sites["sceneHash"] != audit["sceneHash"] or sites["sceneScale"] != audit["sceneScale"]:
        raise ValueError("Reports must belong to the same frozen scene and unit scale")
    scale = sites["sceneScale"]
    if not math.isfinite(scale) or scale <= 0:
        raise ValueError("Invalid scene scale")
    positions = {}
    for pair in audit["pairs"] + sites["localCoverage"]["pairs"]:
        if "position" not in pair:
            continue
        receiver, position = pair["receiver"], vector(pair["position"])
        if receiver in positions and positions[receiver] != position:
            raise ValueError("Receiver positions disagree within the report")
        positions[receiver] = position
    results, metadata = {}, {}
    selected = {(row["attempt"], row["variant"]) for row in sites["coveragePlan"]["replacements"]}
    for attempt, variant in candidates:
        fit = sites["projectionFits"]["attempts"][attempt]
        record = fit["variants"][variant]
        origin = vector(sites["sites"][fit["site"]]["position"])
        minimum, maximum = vector(record["minimum"]), vector(record["maximum"])
        lower, upper = vector(record["influenceMinimum"]), vector(record["influenceMaximum"])
        if not all(minimum[c] < origin[c] < maximum[c] and lower[c] < upper[c] for c in range(3)):
            raise ValueError("Projection requires an interior origin and valid influence bounds")
        values = {}
        for pair in sites["localCoverage"]["pairs"]:
            if pair["attempt"] != attempt or pair["visibility"] != "visible":
                continue
            position = positions.get(pair["receiver"])
            if position is None or not all(lower[c] <= position[c] <= upper[c] for c in range(3)):
                continue  # Missing or unobserved data cannot establish coverage.
            error = proxy_error(origin, position, minimum, maximum)
            if error is not None:
                values[pair["receiver"]] = error * scale
        name = f"{attempt}:{variant}"
        results[name] = values
        metadata[name] = {"origin": origin, "selectedByDiagnosticPlan": (attempt, variant) in selected,
                          "observedVisible": summary(list(values.values()))}
        if fit_error is not None:
            cohort = {}
            for pair in sites["localCoverage"]["pairs"]:
                index = pair["receiver"]
                if pair["attempt"] != attempt or index not in positions or not inside(positions[index], lower, upper):
                    continue
                label = "unknown"
                if pair["visibility"] == "blocked":
                    label = "bad"
                elif pair["visibility"] == "visible" and index in values:
                    label = "good" if values[index] <= fit_error else "bad"
                cohort[index] = (positions[index], label)
            training = [sample for index, sample in sorted(cohort.items()) if index % 2 == 0]
            held_out = [sample for index, sample in sorted(cohort.items()) if index % 2 == 1]
            proposal = propose_seeded_influence(origin, lower, upper, training, clearance_metres / scale, .002 / scale)
            if proposal:
                checks = support(held_out, proposal["minimum"], proposal["maximum"])
                proposal["heldOutSupport"] = checks
                proposal["minimumHeldOutGood"] = 8
                proposal["heldOutChecksPass"] = checks["bad"] == 0 and checks["good"] >= 8
            metadata[name]["influenceExperiment"] = {
                "maximumErrorMetres": fit_error, "originClearanceMetres": clearance_metres,
                "trainingSamples": len(training), "heldOutSamples": len(held_out),
                "proposal": proposal, "scope": "Sampled proposal only; occupancy and rendering unverified"}

    common = sorted(set.intersection(*(set(values) for values in results.values())))
    if not common:
        raise ValueError("Candidates have no common observed-visible receivers")
    for name, values in results.items():
        metadata[name]["commonReceivers"] = summary([values[index] for index in common])
        metadata[name]["strictlyBestCommonReceivers"] = sum(
            all(values[index] < other[index] for key, other in results.items() if key != name)
            for index in common)
    report = {"sceneHash": sites["sceneHash"], "sceneScale": scale,
              "commonReceiverCount": len(common), "candidates": metadata,
              "scope": "Sampled receiver-to-box spatial residual; not radiance, medium transport, or full-volume coverage"}
    rows = [[index, *positions[index], *(values[index] for values in results.values())] for index in common]
    return report, rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sites", type=Path, help="Receiver .sites.json report")
    parser.add_argument("audit", type=Path, help="Matching receiver visibility .json report")
    parser.add_argument("--candidate", action="append", type=candidate, required=True)
    parser.add_argument("--output", type=Path, required=True, help="Owned output prefix for JSON and CSV")
    parser.add_argument("--fit-error-metres", type=float, help="Optionally propose local influence with this residual bound")
    parser.add_argument("--origin-clearance-metres", type=float, default=.1)
    args = parser.parse_args()
    if len(set(args.candidate)) < 2 or len(set(args.candidate)) != len(args.candidate):
        parser.error("Choose at least two distinct candidates")
    if (not math.isfinite(args.origin_clearance_metres) or args.origin_clearance_metres <= 0 or
            (args.fit_error_metres is not None and (not math.isfinite(args.fit_error_metres) or args.fit_error_metres <= 0))):
        parser.error("Error and clearance limits must be finite and positive")
    try:
        report, rows = compare(load(args.sites), load(args.audit), args.candidate,
                               args.fit_error_metres, args.origin_clearance_metres)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        outputs = [Path(str(args.output) + suffix) for suffix in [".json", ".csv"]]
        if any(path.exists() for path in outputs):
            raise ValueError("Output already exists; choose a fresh report prefix")
        outputs[0].write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        with outputs[1].open("w", newline="", encoding="utf-8") as stream:
            writer = csv.writer(stream)
            writer.writerow(["receiver", "x", "y", "z", *(f"{a}:{v} error metres" for a, v in args.candidate)])
            writer.writerows(rows)
        print(json.dumps(report, indent=2))
    except (OSError, ValueError, KeyError, IndexError, TypeError) as error:
        parser.exit(1, f"Proxy comparison unavailable: {error}\n")


if __name__ == "__main__":
    main()
