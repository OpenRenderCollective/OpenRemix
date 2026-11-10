#!/usr/bin/env python3
"""Compare two published probe grids against one native receiver report.

Requires NumPy. Inputs are read-only. The candidate reconstruction must first
match the native report; uncovered receivers are reported, never treated as black.
"""
import argparse
import json
from pathlib import Path

import numpy as np


def basis(normals):
    x, y, z = normals.T
    return np.stack([
        np.full(len(x), .2820947918, dtype=np.float32),
        .4886025119 * y, .4886025119 * z, .4886025119 * x,
        1.092548431 * x * y, 1.092548431 * y * z,
        .3153915653 * (3 * z * z - 1), 1.092548431 * x * z,
        .5462742153 * (x * x - y * y),
    ], axis=1).astype(np.float32)


def reconstruct(path, positions, normals):
    if path.stat().st_size < 48 or (path.stat().st_size - 48) % 256:
        raise ValueError(f"Incomplete probe payload: {path}")
    header = np.fromfile(path, dtype="<f4", count=12)
    dimensions = np.fromfile(path, dtype="<u4", count=12)[4:8]
    count = (path.stat().st_size - 48) // 256
    if np.any(dimensions[:3] < 2) or int(np.prod(dimensions[:3].astype(np.uint64))) != count or dimensions[3] != count:
        raise ValueError("Probe dimensions disagree with the payload")
    if not np.isfinite(header[3]) or header[3] <= 0 or header[10] not in (0, 1):
        raise ValueError("Invalid probe spacing or relocation flag")
    probes = np.memmap(path, dtype="<f4", mode="r", offset=48, shape=(count, 16, 4))
    normals = normals / np.sqrt(np.sum(normals * normals, axis=1, dtype=np.float32))[:, None]
    sh = basis(normals)
    positions = positions + normals * header[8]
    q = (positions - header[:3]) / header[3]
    inside = np.all((q >= 0) & (q <= dimensions[:3].astype(np.float32) - 1), axis=1)
    base = np.minimum(np.maximum(q, 0).astype(np.uint32), dimensions[:3] - 2)
    fraction = q - base.astype(np.float32)
    value = np.zeros((len(positions), 3), dtype=np.float32)
    total = np.zeros(len(positions), dtype=np.float32)
    row = np.arange(len(positions))
    for corner in range(8):
        upper = np.array([corner & 1, (corner >> 1) & 1, (corner >> 2) & 1], dtype=np.uint32)
        coordinate = base + upper
        factors = np.where(upper.astype(bool), fraction, 1 - fraction)
        weight = factors[:, 0] * factors[:, 1] * factors[:, 2]
        index = (coordinate[:, 2] * dimensions[1] + coordinate[:, 1]) * dimensions[0] + coordinate[:, 0]
        probe = probes[index]
        delta = positions - (header[:3] + coordinate.astype(np.float32) * header[3])
        if header[10] > .5:
            delta -= probe[:, :3, 3]
        distance = np.sqrt(np.sum(delta * delta, axis=1, dtype=np.float32))
        axis = np.argmax(np.abs(delta), axis=1)
        face = axis * 2 + (delta[row, axis] < 0).astype(np.int64)
        moment = probe[row, 9 + face]
        weight = np.where(inside & (probe[:, 15, 3] >= .5) & (weight > 0) & (moment[:, 3] > 0), weight, 0)
        variance = np.maximum(header[9], moment[:, 1] - moment[:, 0] * moment[:, 0])
        difference = distance - moment[:, 0]
        visibility = (variance / np.maximum(variance + difference * difference, np.float32(1e-30))) ** 3
        weight *= np.where(distance > moment[:, 0], visibility, 1)
        facing = np.where(distance > 1e-6,
                          np.sum(normals * delta, axis=1, dtype=np.float32) / np.maximum(distance, np.float32(1e-30)), -1)
        weight *= np.maximum(np.float32(.05), .5 - .5 * facing) ** 2
        for coefficient in range(9):
            value += weight[:, None] * probe[:, coefficient, :3] * sh[:, coefficient, None]
        total += weight
    covered = inside & (total >= 1e-4)
    value = np.maximum(value / np.maximum(total[:, None], np.float32(1e-30)), 0)
    value[~covered] = 0
    return value, covered


def metrics(reference, sampled, mask):
    if not mask.any():
        return {"count": 0}
    luminance = np.array([.2126, .7152, .0722], dtype=np.float64)
    a, b = reference[mask] @ luminance, sampled[mask] @ luminance
    difference = np.abs(a - b)
    return dict(count=int(mask.sum()), mean_surface=float(a.mean()), mean_probe=float(b.mean()),
                mae=float(difference.mean()), rmse=float(np.sqrt(np.mean((a - b) ** 2))),
                p95_absolute_error=float(np.quantile(difference, .95)),
                dark_reference_count=int(np.sum(a < .01)), dark_leaks=int(np.sum((a < .01) & (b > .05))))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--receivers", required=True, type=Path)
    parser.add_argument("--baseline-grid", required=True, type=Path)
    parser.add_argument("--candidate-grid", required=True, type=Path)
    parser.add_argument("--baseline-receivers", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output exists; choose a fresh report path")
    report = json.loads(args.receivers.read_text())
    rows = report["receivers"]
    if not rows:
        parser.error("receiver report is empty")
    if args.baseline_receivers:
        baseline = json.loads(args.baseline_receivers.read_text())
        identity = lambda r: (r["chart"], r["position"], r["normal"])
        if baseline["sceneHash"] != report["sceneHash"] or list(map(identity, baseline["receivers"])) != list(map(identity, rows)):
            parser.error("receiver geometry or scene identity differs between reports")
    positions = np.array([r["position"] for r in rows], dtype=np.float32)
    normals = np.array([r["normal"] for r in rows], dtype=np.float32)
    reference = np.array([r["surface"] for r in rows], dtype=np.float32)
    if not np.all(np.isfinite(positions)) or not np.all(np.isfinite(normals)) or np.any(np.sum(normals * normals, axis=1) <= 0):
        parser.error("receiver positions and nonzero normals must be finite")
    variants = {"baseline": reconstruct(args.baseline_grid, positions, normals),
                "candidate": reconstruct(args.candidate_grid, positions, normals)}
    candidate, covered = variants["candidate"]
    native = np.array([r["probe"] for r in rows], dtype=np.float32)
    native_covered = np.array([r["covered"] for r in rows])
    error = np.abs(candidate - native)
    maximum_error = float(error.max())
    if not np.array_equal(covered, native_covered) or maximum_error > 2e-5:
        parser.error(f"candidate reconstruction disagrees with native output (maximum error {maximum_error})")
    playable = np.array([r.get("cluster") is not None and r["cluster"] >= 0 for r in rows])
    common = playable & variants["baseline"][1] & covered
    result = dict(scene_hash=report["sceneHash"], receivers=len(rows), native_maximum_error=maximum_error,
                  scope="Shared surface reference; indirect only; triangle-weighted, not screen/area-weighted. BSP-empty labels are provenance, not a complete gameplay visibility test.",
                  variants={})
    for name, (value, mask) in variants.items():
        result["variants"][name] = dict(all_covered=metrics(reference, value, mask),
                                         bsp_empty_covered=metrics(reference, value, mask & playable),
                                         bsp_empty_uncovered=int(np.sum(playable & ~mask)),
                                         common_bsp_empty=metrics(reference, value, common))
    old_mask = variants["baseline"][1]
    result["bsp_empty_coverage_gained"] = int(np.sum(playable & covered & ~old_mask))
    result["bsp_empty_coverage_lost"] = int(np.sum(playable & ~covered & old_mask))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x") as output:
        json.dump(result, output, indent=2)
        output.write("\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
