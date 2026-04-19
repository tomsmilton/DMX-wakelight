"""Replays the firmware ramp against a chosen schedule and dumps every
DMX frame the sender_task would emit.

Ports these pieces of src/ verbatim (so the sim matches what the lamp sees):

  schedule.c : pct_to_b255, schedule_eval (segment interpolation in byte space
               for brightness, Kelvin space for CCT)
  dmx_out.c  : cct_to_byte (2500K..10000K -> 0..255), frame layout
               [mode=0, bright, cct_byte, gm_byte] at slots 1..4
  main.c     : ramp_task — runs at 1 Hz but `mod = hour*60 + min`, so the
               interpolation input only advances once per minute

Outputs:
  logs/ramp_capture.txt    one row per sender_task iteration (30 ms)
                           showing the raw 4-slot DMX payload
  logs/ramp_changes.txt    one row per frame CHANGE, with delta in
                           DMX units and % — this is the step-size view
"""

from __future__ import annotations

import pathlib
import sys
from dataclasses import dataclass


# ------------ ported from src/ -------------------------------------------------

def pct_to_b255(pct: int) -> int:
    if pct >= 100:
        return 255
    return (pct * 255) // 100  # integer division, matches C uint32_t cast


def cct_to_byte(cct_k: int) -> int:
    if cct_k <= 2500:
        return 0
    if cct_k >= 10000:
        return 255
    return ((cct_k - 2500) * 255) // 7500


@dataclass
class Waypoint:
    minute_of_day: int
    brightness_pct: int
    cct_k: int


@dataclass
class Schedule:
    enabled: bool
    points: list[Waypoint]


def schedule_eval(s: Schedule, sod: int) -> tuple[bool, int, int]:
    """Returns (active, brightness_byte, cct_k) — mirrors schedule_eval() with
    second-of-day granularity (waypoints are still stored as minute-of-day)."""
    if not s.enabled or not s.points:
        return False, 0, 2700
    first_sod = s.points[0].minute_of_day * 60
    last_sod = s.points[-1].minute_of_day * 60
    if sod < first_sod:
        return False, 0, 2700
    if sod >= last_sod:
        last = s.points[-1]
        return True, pct_to_b255(last.brightness_pct), last.cct_k
    for a, b in zip(s.points, s.points[1:]):
        a_sod = a.minute_of_day * 60
        b_sod = b.minute_of_day * 60
        if a_sod <= sod < b_sod:
            span = b_sod - a_sod
            pos = sod - a_sod
            ab = pct_to_b255(a.brightness_pct)
            bb = pct_to_b255(b.brightness_pct)
            bright = ab + ((bb - ab) * pos) // span
            cct = a.cct_k + ((b.cct_k - a.cct_k) * pos) // span
            return True, bright & 0xFF, cct
    return False, 0, 2700


def build_frame(bright: int, cct_k: int, gm: int = 128) -> tuple[int, int, int, int]:
    """Slots 1..4 as laid out in dmx_out.c sender_task."""
    return (0, bright, cct_to_byte(cct_k), gm)


# ------------ default schedule (from src/schedule.c default_schedule) ----------

DEFAULT = Schedule(
    enabled=True,  # default_schedule sets enabled=false but we want to see the ramp
    points=[
        Waypoint(minute_of_day=6 * 60 + 30, brightness_pct=0,   cct_k=2500),
        Waypoint(minute_of_day=6 * 60 + 50, brightness_pct=30,  cct_k=3000),
        Waypoint(minute_of_day=7 * 60 + 0,  brightness_pct=100, cct_k=5000),
    ],
)


# ------------ sim loop ---------------------------------------------------------

SENDER_PERIOD_MS = 30   # dmx_out.c sender_task
RAMP_PERIOD_MS = 1000   # main.c ramp_task


def simulate(sched: Schedule, start_min: int, end_min: int, out_dir: pathlib.Path):
    out_dir.mkdir(exist_ok=True)
    dense = out_dir / "ramp_capture.txt"
    changes = out_dir / "ramp_changes.txt"

    total_ms = (end_min - start_min) * 60_000

    # ramp_task recomputes the setpoint at 1 Hz against second-of-day; the
    # sender reads the packed frame every 30 ms.
    current_frame = (0, 0, 0, 128)
    current_b_pct = 0.0
    current_cct_k = 2700
    current_state = "idle"
    last_change_frame = None
    last_change_b_pct = 0.0

    n_sender_iters = 0
    n_changes = 0

    with dense.open("w") as fd, changes.open("w") as fc:
        fd.write("# t_ms  sod  state  slot1(mode) slot2(bright) slot3(cct) slot4(gm)  bright_pct  cct_k_interp\n")
        fc.write("# t_ms  sod  bright(dmx)  dbright(dmx)  bright_pct  dbright_pct  cct_byte  dcct  cct_k_interp\n")

        for t_ms in range(0, total_ms, SENDER_PERIOD_MS):
            sec_of_sim = t_ms // 1000
            cur_sod = start_min * 60 + sec_of_sim

            if t_ms % RAMP_PERIOD_MS == 0:
                active, bright, cct_k = schedule_eval(sched, cur_sod)
                if not active:
                    bright, cct_k = 0, 2700
                    state = "idle"
                elif cur_sod >= sched.points[-1].minute_of_day * 60:
                    state = "holding"
                else:
                    state = "ramping"
                frame = build_frame(bright, cct_k)
                current_frame = frame
                current_b_pct = bright * 100.0 / 255
                current_cct_k = cct_k
                current_state = state

            n_sender_iters += 1
            fd.write(
                f"{t_ms:>8d}  {cur_sod:>5d}  {current_state:<8s}  "
                f"{current_frame[0]:>3d} {current_frame[1]:>3d} {current_frame[2]:>3d} {current_frame[3]:>3d}  "
                f"{current_b_pct:>6.2f}%  {current_cct_k:>5d}K\n"
            )

            if current_frame != last_change_frame:
                if last_change_frame is None:
                    dbright = current_frame[1]
                    dcct = current_frame[2]
                    dpct = current_b_pct
                else:
                    dbright = current_frame[1] - last_change_frame[1]
                    dcct = current_frame[2] - last_change_frame[2]
                    dpct = current_b_pct - last_change_b_pct
                fc.write(
                    f"{t_ms:>8d}  {cur_sod:>5d}  "
                    f"b={current_frame[1]:>3d}  Δb={dbright:+4d}  "
                    f"{current_b_pct:>6.2f}%  Δ%={dpct:+6.2f}  "
                    f"cct={current_frame[2]:>3d}  Δcct={dcct:+4d}  "
                    f"({current_cct_k}K)\n"
                )
                last_change_frame = current_frame
                last_change_b_pct = current_b_pct
                n_changes += 1

    return {
        "sender_iters": n_sender_iters,
        "changes": n_changes,
        "dense_file": dense,
        "changes_file": changes,
    }


def summarise_changes(changes_file: pathlib.Path) -> None:
    """Print the biggest brightness steps — where chunkiness hides."""
    steps = []
    with changes_file.open() as f:
        for line in f:
            if line.startswith("#"):
                continue
            # parse Δ% value
            for tok in line.split():
                if tok.startswith("Δ%="):
                    try:
                        steps.append(float(tok.split("=")[1]))
                    except ValueError:
                        pass
    if not steps:
        return
    # skip the very first step which is just "first frame after idle"
    real_steps = steps[1:]
    if not real_steps:
        return
    biggest = max(real_steps, key=abs)
    avg = sum(abs(s) for s in real_steps) / len(real_steps)
    print(f"  brightness step count: {len(real_steps)}")
    print(f"  biggest |Δ%|: {abs(biggest):.2f}%")
    print(f"  average |Δ%|: {avg:.2f}%")


def main():
    repo = pathlib.Path(__file__).resolve().parent.parent
    out = repo / "logs"

    # Full default schedule: 06:30 -> 07:00 plus a minute of hold
    start = 6 * 60 + 29   # start one minute before first waypoint to see the kick-in
    end   = 7 * 60 + 2    # one minute into hold

    print(f"simulating default schedule from {start // 60:02d}:{start % 60:02d} "
          f"to {end // 60:02d}:{end % 60:02d}")
    r = simulate(DEFAULT, start, end, out)
    print(f"  sender_task iterations (30 ms): {r['sender_iters']}")
    print(f"  distinct frames emitted:         {r['changes']}")
    print(f"  dense capture: {r['dense_file']}")
    print(f"  change log:    {r['changes_file']}")
    summarise_changes(r["changes_file"])


if __name__ == "__main__":
    main()
