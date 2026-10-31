#!/usr/bin/env python3
"""Measure the AI processor's sleep timer from a Tera Term log (issue #245).

Reads one or more saved console logs, with no connection to the board. The log must
have Tera Term's timestamps on every line ("[2026-10-30 17:49:17.335] ..."): in Tera Term,
File > Log > Timestamp.

For every sleep that ended with a timer wake it finds:
  - the setting in force: "RTC correction N ppm" or "RTC correction none" (rtc_cal.c, since
                          31 Oct 2026), or "RC32K trim N" in older logs. Shown as "corr"
  - the sleep asked for:  "Sleep Ns = M RTC s" (since 31 Oct 2026: N real seconds asked, M set
                          on the RTC after the correction), else ">>> Entering DPD at <RTC>" to
                          "Will wake at <RTC>", in RTC seconds
  - the real sleep:       the PC time from ">>> Entering DPD" to the next "1st BL" (bootloader) line
and prints one row per sleep, then a summary per setting and interval. Error % is
(real / asked - 1) x 100: negative means the sleep was short.

The alarm is set in whole RTC seconds, from some point inside the second when the
processor went to sleep, so each sleep is only good to about +-0.5 s (+-0.8 % at 60 s,
+-0.1 % at 10 minutes), and on average lasts half an RTC second less than asked. The
summary's "rate %" allows for that (real / (asked - 0.5) - 1): the best estimate of how
fast the timer runs. Long sleeps give the accurate rate.

Sleeps ended by anything but the timer (motion, BLE, a reset) are listed as skipped.

Usage:
    python sleep_timing.py LOG [LOG ...] [--csv OUT.csv]
"""

import argparse
import csv
import re
import sys
from collections import defaultdict
from datetime import datetime

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
STAMP = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]\s?(.*)$")
TRIM = re.compile(r"RC32K trim (\d+)")
CORRECTION = re.compile(r"RTC correction (-?\d+ ppm|none)")
SLEEP_ASKED = re.compile(r"Sleep (\d+)s = (\d+) RTC s")
DPD = re.compile(r">>> Entering DPD at (\d{4}:\d\d:\d\d \d\d:\d\d:\d\d)")
WAKE_AT = re.compile(r"Will wake at (\d{4}:\d\d:\d\d \d\d:\d\d:\d\d)")
BOOT = re.compile(r"1st BL")
WAKE_EVENT = re.compile(r"Wakeup_event = ")

# Lines after the bootloader line within which the wake reason must appear
WAKE_EVENT_SEARCH_LINES = 40


def rtc(text):
    return datetime.strptime(text, "%Y:%m:%d %H:%M:%S")


def pc(text):
    return datetime.strptime(text, "%Y-%m-%d %H:%M:%S.%f")


def read_lines(path):
    """Return [(pc_datetime, text)], dropping lines with no timestamp."""
    lines = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            m = STAMP.match(ANSI.sub("", raw.rstrip("\r\n")))
            if m:
                lines.append((pc(m.group(1)), m.group(2)))
    return lines


def sleeps(path):
    """Yield one dict per sleep found in a log."""
    lines = read_lines(path)
    if not lines:
        print(f"{path}: no timestamped lines (turn on Tera Term's log timestamps)", file=sys.stderr)
        return
    trim = None
    i = 0
    while i < len(lines):
        t, text = lines[i]
        m = TRIM.search(text)
        if m:
            trim = "trim " + m.group(1)
        m = CORRECTION.search(text)
        if m:
            trim = m.group(1)
        m = DPD.search(text)
        if not m:
            i += 1
            continue

        row = {"log": path, "trim": trim, "dpd_pc": t, "asked_s": None, "real_s": None,
               "error_pct": None, "result": ""}
        rtc_dpd = rtc(m.group(1))
        trim = None		# each sleep must print its own setting

        # The alarm, then the next boot
        asked_real = None
        j = i + 1
        while j < len(lines) and not BOOT.search(lines[j][1]):
            m = SLEEP_ASKED.search(lines[j][1])
            if m:
                asked_real = int(m.group(1))
            m = WAKE_AT.search(lines[j][1])
            if m:
                row["asked_s"] = int((rtc(m.group(1)) - rtc_dpd).total_seconds())
            if DPD.search(lines[j][1]):
                break	# another sleep with no boot between: the log has a gap
            j += 1
        if row["asked_s"] is not None and asked_real is not None:
            row["asked_s"] = asked_real
        if j >= len(lines) or not BOOT.search(lines[j][1]):
            row["result"] = "no boot after it"
            yield row
            i = j
            continue
        if row["asked_s"] is None:
            row["result"] = "skipped: no timer alarm"
            yield row
            i = j
            continue

        # A stray character at DPD entry can start the bootloader's line early, so Tera Term stamps it
        # with the time it went to sleep: then take the time of the line after it
        boot_t = lines[j][0]
        if not lines[j][1].lstrip().startswith("1st BL") and j + 1 < len(lines):
            boot_t = lines[j + 1][0]
        row["real_s"] = (boot_t - t).total_seconds()

        # Was it the timer that woke it?
        reason = ""
        for k in range(j, min(j + WAKE_EVENT_SEARCH_LINES, len(lines))):
            if WAKE_EVENT.search(lines[k][1]):
                reason = lines[k][1]
                break
        if "RTC Timer" not in reason:
            row["result"] = "skipped: not a timer wake"
        else:
            row["error_pct"] = (row["real_s"] / row["asked_s"] - 1.0) * 100.0
            row["result"] = "ok"
        yield row
        i = j


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("logs", nargs="+", help="Tera Term log files, with timestamps")
    parser.add_argument("--csv", help="also write every sleep to this CSV file")
    args = parser.parse_args()

    rows = []
    for path in args.logs:
        rows.extend(sleeps(path))
    if not rows:
        print("No sleeps found.")
        return 1

    print(f"{'log':<14} {'PC time at DPD':<23} {'corr':>12} {'asked s':>8} {'real s':>9} {'error %':>8}  result")
    for r in rows:
        name = r["log"].replace("\\", "/").split("/")[-1][:14]
        trim = "?" if r["trim"] is None else r["trim"]
        asked = "" if r["asked_s"] is None else str(r["asked_s"])
        real = "" if r["real_s"] is None else f"{r['real_s']:.3f}"
        err = "" if r["error_pct"] is None else f"{r['error_pct']:+.2f}"
        print(f"{name:<14} {r['dpd_pc'].strftime('%Y-%m-%d %H:%M:%S.%f')[:23]:<23} {trim:>12} {asked:>8} {real:>9} {err:>8}  {r['result']}")

    # Summary per log, setting and interval
    groups = defaultdict(list)
    for r in rows:
        if r["result"] == "ok":
            groups[(r["log"], r["trim"], r["asked_s"])].append(r)
    if groups:
        print()
        print(f"{'log':<14} {'corr':>12} {'asked s':>8} {'sleeps':>6} {'mean real s':>12} {'mean error %':>13} {'min %':>7} {'max %':>7} {'rate %':>7}")
        for (log, trim, asked) in sorted(groups, key=lambda k: (k[0], "" if k[1] is None else k[1], k[2])):
            g = groups[(log, trim, asked)]
            errs = [r["error_pct"] for r in g]
            mean_real = sum(r["real_s"] for r in g) / len(g)
            rate = (mean_real / (asked - 0.5) - 1.0) * 100.0 if asked > 0.5 else float("nan")
            name = log.replace("\\", "/").split("/")[-1][:14]
            print(f"{name:<14} {'?' if trim is None else trim:>12} {asked:>8} {len(g):>6} {mean_real:>12.3f} "
                  f"{sum(errs) / len(errs):>+13.2f} {min(errs):>+7.2f} {max(errs):>+7.2f} {rate:>+7.2f}")
        print("\nNegative: the sleep was short (the timer runs fast). Each sleep is good to about +-0.5 s;")
        print("'rate %' allows for the half second lost on average to the whole-second alarm.")

    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["log", "pc_time_at_dpd", "correction", "asked_s", "real_s", "error_pct", "result"])
            for r in rows:
                w.writerow([r["log"], r["dpd_pc"].isoformat(sep=" "), r["trim"], r["asked_s"],
                            "" if r["real_s"] is None else f"{r['real_s']:.3f}",
                            "" if r["error_pct"] is None else f"{r['error_pct']:.3f}", r["result"]])
        print(f"Wrote {args.csv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
