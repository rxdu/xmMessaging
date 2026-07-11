#!/usr/bin/env python3
"""bench/compare.py — M9-A5 regression comparison (report-only by default).

Compares a bench/results/*.json report against bench/reference.json using
per-metric tolerance bands (ratio current/reference). Until the reference is
pinned (``"pinned": true``, done by CI on the designated reference runner),
the comparison always runs in report-only mode: findings are printed, the
exit code stays 0. With ``--gate`` AND a pinned reference, any regression
beyond tolerance exits 1 — the telemetry S1 overhead-gate discipline.

Usage: compare.py <result.json> <reference.json> [--gate]
"""

import json
import sys


def main() -> int:
    args = [a for a in sys.argv[1:] if a != "--gate"]
    gate_requested = "--gate" in sys.argv[1:]
    if len(args) != 2:
        print(__doc__, file=sys.stderr)
        return 2

    with open(args[0], encoding="utf-8") as f:
        result = json.load(f)
    with open(args[1], encoding="utf-8") as f:
        reference = json.load(f)

    pinned = bool(reference.get("pinned", False))
    tolerance = reference.get(
        "default_tolerance", {"p50": 1.5, "p99": 2.0, "max": 5.0}
    )
    ref_benchmarks = reference.get("benchmarks", {})
    current = {b["name"]: b for b in result.get("benchmarks", [])}

    gating = gate_requested and pinned
    mode = "GATING" if gating else "report-only"
    if gate_requested and not pinned:
        print(
            "compare: --gate requested but reference is not pinned — "
            "staying report-only (pin the reference on the CI runner first)"
        )
    print(f"compare: {len(ref_benchmarks)} reference entries, mode: {mode}")

    regressions = []
    compared = 0
    for name, ref in ref_benchmarks.items():
        cur = current.get(name)
        if cur is None:
            print(f"  MISSING   {name}: in reference but not in this run")
            continue
        for metric, band in tolerance.items():
            ref_value = ref.get(metric)
            cur_value = cur.get(metric)
            if ref_value is None or cur_value is None or ref_value <= 0:
                continue
            compared += 1
            ratio = cur_value / ref_value
            if ratio > band:
                regressions.append(
                    f"{name} {metric}: {cur_value:.1f} ns vs reference "
                    f"{ref_value:.1f} ns (x{ratio:.2f} > tolerance x{band})"
                )

    if compared == 0:
        print(
            "compare: no pinned reference values yet — nothing to compare "
            "(M9-A5 gate armed, values TO BE PINNED by CI)"
        )
        return 0

    if regressions:
        print(f"compare: {len(regressions)} regression(s) beyond tolerance:")
        for line in regressions:
            print(f"  REGRESSION {line}")
        return 1 if gating else 0

    print(f"compare: PASS — {compared} metric(s) within tolerance")
    return 0


if __name__ == "__main__":
    sys.exit(main())
