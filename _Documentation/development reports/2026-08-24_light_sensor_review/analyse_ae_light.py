"""Test which HM0360 AE register best identifies darkness, against an INDEPENDENT label.

Why this exists
---------------
AE_Light_Sensor_Roadmap.md section 3.2 labelled each image dark or light "using a
composite scoring heuristic" built from the AE registers, then in 3.4 scored each
register against that label. That is circular: it measures how much each register
agrees with a blend of itself and its neighbours, not how well it detects darkness.
Analog gain "winning" at 99.7% may only mean analog gain dominated the heuristic.

Here the label comes from the clock instead, which knows nothing about the sensor.
Time-lapse frames carry a real timestamp, so hours near solar noon are day and hours
near midnight are night, with dawn and dusk deliberately left unlabelled rather than
guessed. Every register is then scored on frames whose class is not in doubt, and the
transition band is characterised separately, which is where the flash decision is
actually hard. The mean-based decision that shipped until ee65771f and the gain-based
one that replaced it are then both simulated on the same frames, including how often
each flips between consecutive frames.

The CSV comes from _Tools/jpegAE-batch.py. Its timestamps are whatever the camera
wrote into EXIF; the June 2026 time-lapse is in UTC while the camera sat in New
Zealand, so pass --utc-offset 12 and give the windows in local hours. This is the
command behind every table in light_sensor_validation_and_app_contract.md:

    python analyse_ae_light.py logs/ae_303_frames_june2026.csv --utc-offset 12 --day 8-16 --night 18-6
"""
import argparse
import csv
import sys
from collections import Counter

REGISTERS = ["Integration time", "Analog gain", "Digital gain", "AE Mean"]
# Direction: True means "higher value implies darker".
DARK_WHEN_HIGHER = {"Integration time": True, "Analog gain": True,
                    "Digital gain": True, "AE Mean": False}

# The gain-based rule on ae_review since ee65771f (lightSensor.c, AE_DECISION_GAIN_BASED):
# dark when the AE loop has not converged, or analog gain is above this.
DARK_ANALOG_GAIN_THRESHOLD = 2


def parse_window(spec):
    lo, hi = (int(x) for x in spec.split("-"))
    return lo, hi


def in_window(hour, window):
    lo, hi = window
    return lo <= hour < hi if lo <= hi else (hour >= lo or hour < hi)


def load(path, utc_offset):
    rows = []
    with open(path, newline="", encoding="utf-8") as fh:
        for r in csv.DictReader(fh):
            try:
                t = r["FileTime"]
                hour = (int(t.split(" ")[1].split(":")[0]) + utc_offset) % 24
                rec = {"file": r["FileName"], "time": t, "hour": hour,
                       "converged": r.get("AEConverged", "").strip().upper().startswith("Y")}
                for k in REGISTERS:
                    rec[k] = int(r[k])
                rows.append(rec)
            except (KeyError, ValueError, IndexError):
                continue
    rows.sort(key=lambda r: r["time"])
    return rows


def sweep(rows, reg):
    """Best threshold for one register, plus its accuracy on the labelled set."""
    dark_high = DARK_WHEN_HIGHER[reg]
    vals = sorted({r[reg] for r in rows})
    # Start below zero so a register that never beats chance still reports the
    # threshold it settled on, rather than None.
    best = (-1.0, vals[0] if vals else 0)
    for v in vals:
        correct = 0
        for r in rows:
            pred_dark = (r[reg] > v) if dark_high else (r[reg] < v)
            if pred_dark == r["is_dark"]:
                correct += 1
        acc = correct / len(rows)
        if acc > best[0]:
            best = (acc, v)
    return best[1], best[0]


def evaluate(rows, predicate):
    tp = sum(1 for r in rows if predicate(r) and r["is_dark"])
    tn = sum(1 for r in rows if not predicate(r) and not r["is_dark"])
    return (tp + tn) / len(rows) if rows else 0.0


def flips(rows, predicate):
    """Decision changes between consecutive frames, over the whole time series."""
    count = 0
    prev = None
    for r in rows:
        d = predicate(r)
        if prev is not None and d != prev:
            count += 1
        prev = d
    return count


def confusion(day, night, predicate):
    """(day frames called dark, night frames called bright, overall accuracy)."""
    fp = sum(1 for r in day if predicate(r))
    fn = sum(1 for r in night if not predicate(r))
    acc = ((len(day) - fp) + (len(night) - fn)) / (len(day) + len(night))
    return fp, fn, acc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--utc-offset", type=int, default=0,
                    help="hours to add to the CSV timestamps before labelling (12 for NZST)")
    ap.add_argument("--day", default="10-16", help="local hours counted as unambiguous day")
    ap.add_argument("--night", default="22-04", help="local hours counted as unambiguous night")
    ap.add_argument("--again-max", type=int, default=4,
                    help="analog gain ceiling, for the 'railed' test (default: the ceiling seen in the June data)")
    ap.add_argument("--dgain-max", type=int, default=192,
                    help="digital gain ceiling, for the 'railed' test (default: as above)")
    args = ap.parse_args()

    day_w, night_w = parse_window(args.day), parse_window(args.night)
    rows = load(args.csv, args.utc_offset)
    if not rows:
        print("no usable rows", file=sys.stderr)
        return 1

    for r in rows:
        r["is_dark"] = None
        if in_window(r["hour"], night_w):
            r["is_dark"] = True
        elif in_window(r["hour"], day_w):
            r["is_dark"] = False
        # "Railed" as lightSensor.c defines it: both gains at their ceiling, so AE
        # can amplify no further. The firmware reads the ceilings from the sensor;
        # here they are the maxima the data reached.
        r["railed"] = (r["Analog gain"] >= args.again_max
                       and r["Digital gain"] >= args.dgain_max)
    labelled = [r for r in rows if r["is_dark"] is not None]
    twilight = [r for r in rows if r["is_dark"] is None]
    night = [r for r in labelled if r["is_dark"]]
    day = [r for r in labelled if not r["is_dark"]]

    print(f"{len(rows)} frames, {rows[0]['time']} to {rows[-1]['time']} "
          f"(timestamps shifted by {args.utc_offset:+d} h)")
    print(f"labelled by clock: {len(night)} night ({args.night}), {len(day)} day ({args.day}), "
          f"{len(twilight)} left unlabelled (dawn/dusk)\n")

    if not night or not day:
        print("STOP: the labelled set contains only one class, so every accuracy below\n"
              "would be meaningless. This dataset does not span a day/night cycle.\n"
              "Needs frames from both --day and --night windows.", file=sys.stderr)
        return 2

    print("Per-register, scored against the CLOCK (not against each other):")
    print(f"  {'register':<18} {'threshold':>9} {'accuracy':>9}")
    results = {}
    for reg in REGISTERS:
        thr, acc = sweep(labelled, reg)
        results[reg] = (thr, acc)
        arrow = ">" if DARK_WHEN_HIGHER[reg] else "<"
        print(f"  {reg:<18} {arrow}{thr:>8} {acc:>8.1%}")

    # The composite Charles is reaching for: dark if the sensor is working hard,
    # whatever the resulting mean happens to be.
    print("\nComposite predictors:")
    athr = results["Analog gain"][0]
    mthr = results["AE Mean"][0]
    cands = {
        "integration railed OR analog gain high":
            lambda r: r["Integration time"] >= 376 or r["Analog gain"] > athr,
        "analog gain alone":
            lambda r: r["Analog gain"] > athr,
        "AE Mean alone":
            lambda r: r["AE Mean"] < mthr,
        "integration railed AND mean below threshold":
            lambda r: r["Integration time"] >= 376 and r["AE Mean"] < mthr,
        "integration railed OR gain high OR mean low":
            lambda r: (r["Integration time"] >= 376 or r["Analog gain"] > athr
                       or r["AE Mean"] < mthr),
    }
    for name, fn in cands.items():
        print(f"  {evaluate(labelled, fn):>7.1%}  {name}")

    # The decision that shipped until ee65771f (decideDarkBright(), still in the
    # source behind the define): dark = gainRailed OR meanAE < op23, hysteresis 0.
    print("\nMean-based decision, dark = railed OR (AE Mean < op23), AE_HYSTERESIS 0:")
    print(f"  railed (AGain >= {args.again_max} and DGain >= {args.dgain_max}) on "
          f"{sum(1 for r in night if r['railed'])}/{len(night)} night frames, "
          f"{sum(1 for r in day if r['railed'])}/{len(day)} day frames")
    print(f"  {'op23':>5} {'day called dark':>16} {'night called bright':>20} {'overall':>8} {'flips':>6}")
    for t in (65, 60, 56, 50, 45, 40, 35, 30):
        rule = lambda r, t=t: r["railed"] or r["AE Mean"] < t
        fp, fn, acc = confusion(day, night, rule)
        print(f"  {t:>5} {fp:>8}/{len(day)} ({fp/len(day):>5.1%}) "
              f"{fn:>10}/{len(night)} ({fn/len(night):>5.1%}) {acc:>7.1%} {flips(rows, rule):>6}")
    print(f"  flips = decision changes between consecutive frames over all {len(rows)}; "
          "a real day/night cycle needs two per day")

    # The decision on ae_review since ee65771f (decideDarkBrightGainBased()).
    gain_rule = lambda r: (not r["converged"]) or r["Analog gain"] > DARK_ANALOG_GAIN_THRESHOLD
    fp, fn, acc = confusion(day, night, gain_rule)
    print(f"\nGain-based decision (ee65771f), dark = not converged OR AGain > {DARK_ANALOG_GAIN_THRESHOLD}:")
    print(f"  day called dark {fp}/{len(day)} ({fp/len(day):.1%}), "
          f"night called bright {fn}/{len(night)} ({fn/len(night):.1%}), "
          f"overall {acc:.1%}, {flips(rows, gain_rule)} flips")
    nc_day = sum(1 for r in day if not r["converged"])
    print(f"  of the day frames called dark, {sum(1 for r in day if not r['converged'] and r['Analog gain'] <= DARK_ANALOG_GAIN_THRESHOLD)} "
          f"are on 'not converged' alone ({nc_day}/{len(day)} day frames had not converged)")

    # Where the decision is actually hard.
    print("\nTwilight frames (the band that matters):")
    if twilight:
        for reg in REGISTERS:
            vs = [r[reg] for r in twilight]
            print(f"  {reg:<18} min {min(vs):>4}  max {max(vs):>4}  "
                  f"mean {sum(vs)/len(vs):>6.1f}")
        rail = sum(1 for r in twilight if r["Integration time"] >= 376)
        print(f"  integration railed at 376 in {rail}/{len(twilight)} twilight frames")
        nc = sum(1 for r in twilight if not r["converged"])
        print(f"  AE not converged in {nc}/{len(twilight)} twilight frames")

    # Is AE Mean actually flat once AE has settled? That is the mechanism claim.
    conv = [r for r in labelled if r["converged"]]
    if conv:
        dm = [r["AE Mean"] for r in conv if r["is_dark"]]
        lm = [r["AE Mean"] for r in conv if not r["is_dark"]]
        print("\nAE Mean on CONVERGED frames only (tests the 'mean is an AE output' claim):")
        if dm:
            print(f"  night: n={len(dm):>3}  mean {sum(dm)/len(dm):>5.1f}  range {min(dm)}-{max(dm)}")
        if lm:
            print(f"  day:   n={len(lm):>3}  mean {sum(lm)/len(lm):>5.1f}  range {min(lm)}-{max(lm)}")
        if dm and lm:
            overlap = max(0, min(max(dm), max(lm)) - max(min(dm), min(lm)))
            print(f"  overlapping span: {overlap} AE-Mean units "
                  f"(the wider this is, the worse AE Mean is as a discriminator)")

    print("\nConverged flag overall:",
          dict(Counter("converged" if r["converged"] else "hunting" for r in rows)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
