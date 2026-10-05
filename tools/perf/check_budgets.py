#!/usr/bin/env python3
"""Compare a measurement document against the recorded budgets.

Roadmap section 9: "Perf regression gate: CI job runs the perf harness against a
scripted scenario and fails if PSS or wakeups exceed budgets by >10%".

Budgets live in docs/perf-baseline.json. Each entry has:
    { "value": <number>, "unit": "kB"|"/s"|"ms", "budget": <number>, "higher_is_worse": true }

`value` is the baseline that was accepted; `budget` is the roadmap's hard limit.
A regression is "actual > budget OR actual > value * (1 + tolerance/100)", but
only when the baseline itself already met the budget -- an over-budget baseline
should not silently bless further growth without being reported.

Exit codes: 0 = within tolerance, 1 = regression, 2 = bad input.
"""

from __future__ import annotations

import argparse
import json
import sys
from typing import Any


def flatten(doc: dict[str, Any], prefix: str = "") -> dict[str, float]:
    out: dict[str, float] = {}
    for key, value in doc.items():
        if key in ("env", "notes", "record", "command", "niri_version", "kernel", "cpu"):
            continue
        path = f"{prefix}.{key}" if prefix else key
        if isinstance(value, dict):
            out.update(flatten(value, path))
        elif isinstance(value, (int, float)):
            out[path] = float(value)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--actual", required=True)
    ap.add_argument("--tolerance", type=float, default=10.0)
    args = ap.parse_args()

    try:
        with open(args.baseline) as fh:
            baseline = json.load(fh)
        with open(args.actual) as fh:
            actual = json.load(fh)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"perf-gate: cannot read inputs: {exc}", file=sys.stderr)
        return 2

    budgets = baseline.get("budgets") or baseline.get("scenarios") or {}
    if not budgets:
        print("perf-gate: baseline has no budgets section", file=sys.stderr)
        return 2

    base_flat = flatten(baseline)
    actual_flat = flatten(actual)

    failures: list[str] = []
    warnings: list[str] = []
    checked = 0

    for key, spec in budgets.items():
        if not isinstance(spec, dict):
            continue
        budget = spec.get("budget")
        base_value = spec.get("value")
        if budget is None or base_value is None:
            continue

        got = actual_flat.get(key)
        if got is None:
            warnings.append(f"{key}: not measured in this run")
            continue

        checked += 1
        unit = spec.get("unit", "")
        base_value = float(base_value)
        budget = float(budget)
        higher_is_worse = spec.get("higher_is_worse", True)

        # Roadmap section 0: budgets are absolute.
        if higher_is_worse and got > budget:
            failures.append(
                f"{key}: {got:g}{unit} exceeds the budget of {budget:g}{unit}"
            )
            continue
        if not higher_is_worse and got < budget:
            failures.append(
                f"{key}: {got:g}{unit} is below the required minimum of {budget:g}{unit}"
            )
            continue

        # Section 9: fail if we regressed against the recorded baseline by >10%.
        allowed = base_value * (1.0 + args.tolerance / 100.0)
        if higher_is_worse and got > allowed and base_value > 0:
            pct = (got - base_value) / base_value * 100.0
            failures.append(
                f"{key}: {got:g}{unit} is {pct:.1f}% above the recorded "
                f"{base_value:g}{unit} (allowed {allowed:g}{unit})"
            )
        elif not higher_is_worse and base_value > 0 and got < base_value * (1.0 - args.tolerance / 100.0):
            warnings.append(f"{key}: {got:g}{unit} dropped from {base_value:g}{unit}")

    for w in warnings:
        print(f"perf-gate: note: {w}")

    if not checked:
        print("perf-gate: nothing was comparable; the harness shape changed?")
        return 2

    if failures:
        print("", file=sys.stderr)
        print("perf-gate: FAILED", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        return 1

    print(f"perf-gate: OK ({checked} measurements within budget and tolerance)")
    return 0


if __name__ == "__main__":
    sys.exit(main())