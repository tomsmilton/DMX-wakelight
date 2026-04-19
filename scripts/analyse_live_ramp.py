"""Summarise logs/live_ramp.txt — the status poll captured during a live ramp
against the real micro. Emits per-change step sizes to quantify smoothness.
"""

import pathlib
import sys
from collections import Counter


def main():
    p = pathlib.Path("logs/live_ramp.txt")
    if not p.exists():
        print("no log file"); sys.exit(1)

    rows = []
    for line in p.read_text().splitlines():
        if line.startswith("#") or not line.strip():
            continue
        parts = line.split()
        # ts now_date now_time mod state bp cct active
        ts, d, t, mod, state, bp, cct, a = parts[0], parts[1], parts[2], int(parts[3]), parts[4], int(parts[5]), int(parts[6]), parts[7]
        rows.append((ts, f"{d} {t}", mod, state, bp, cct, a))

    if not rows:
        print("empty log"); sys.exit(1)

    print(f"rows: {len(rows)}")
    states = Counter(r[3] for r in rows)
    print(f"states: {dict(states)}")
    first_ramp_idx = next((i for i, r in enumerate(rows) if r[3] == "ramping"), None)
    first_hold_idx = next((i for i, r in enumerate(rows) if r[3] == "holding"), None)
    if first_ramp_idx is not None:
        print(f"first ramping sample: idx={first_ramp_idx}  ts={rows[first_ramp_idx][0]}  now={rows[first_ramp_idx][1]}  bp={rows[first_ramp_idx][4]}  cct={rows[first_ramp_idx][5]}")
    if first_hold_idx is not None:
        print(f"first holding sample: idx={first_hold_idx}  ts={rows[first_hold_idx][0]}  now={rows[first_hold_idx][1]}  bp={rows[first_hold_idx][4]}  cct={rows[first_hold_idx][5]}")

    # Steps during ramp only
    ramp_rows = [r for r in rows if r[3] == "ramping"]
    if ramp_rows:
        steps = []
        for a, b in zip(ramp_rows, ramp_rows[1:]):
            steps.append((b[0], b[4] - a[4], a[4], b[4]))
        sizes = [s[1] for s in steps]
        print(f"\nramp samples: {len(ramp_rows)}")
        print(f"brightness_pct span: {ramp_rows[0][4]}% -> {ramp_rows[-1][4]}%")
        print(f"step-size distribution (Δ% between consecutive samples):")
        hist = Counter(sizes)
        for k in sorted(hist):
            print(f"   Δ%={k:+3d}  count={hist[k]}")
        print(f"max |Δ%|: {max(abs(x) for x in sizes)}")

        # show first 10 + last 10 ramp rows
        print("\nfirst 10 ramp samples:")
        for r in ramp_rows[:10]:
            print(f"  {r[0]}  now={r[1]}  bp={r[4]:3d}  cct={r[5]}")
        print("last 10 ramp samples:")
        for r in ramp_rows[-10:]:
            print(f"  {r[0]}  now={r[1]}  bp={r[4]:3d}  cct={r[5]}")


if __name__ == "__main__":
    main()
