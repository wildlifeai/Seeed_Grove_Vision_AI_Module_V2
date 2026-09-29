#!/usr/bin/env python3
"""Reproduce finding A on demand (inactivity event landing in the IF task's I2C TX state).

Mechanism, from the 18:55 log: the idle-hook detector fires op8 ms after the last
activity and posts Inactivity to the image task first; that task's Save State runs
before the detector posts to the IF task. A command arriving in that gap puts the
IF task into I2C TX exactly when its Inactivity is dequeued, which it does not
handle, and the device never sleeps again.

So: anchor on the reply to one command (the Himax's last activity), then send a
probe command at a swept delay so that some attempt lands inside the Save State.

The phone is driven over adb: the Engineer Console input is focused, so `input
text` types into it and a tap on the send button sends. The probe is typed while
the anchor's reply is still on its way, so the only timing-critical action is one
tap. Observation is the three-way bench log the logger is already writing.

Result, 3 September 2026 19:54: sweep from 600 ms in 50 ms steps, first attempt
missed, second (650 ms) hit. See logs/repro_A_sweep.txt.
"""
import re
import subprocess
import sys
import time

LOG = r"C:\Users\Victor\AppData\Local\Temp\claude\C--Users-ww\2c78acd7-f4c0-4115-a1cf-a222e9aae63a\scratchpad\bench\2026-09-03_capture-keep-awake\capture_keepawake_06_retest.log"
SEND = (985, 1289)          # send button, from uiautomator dump, keyboard open (it stays open)
CMD = "AI slots"            # 116-byte reply, and it wakes the device if asleep

HIT = re.compile(r"IF Task unhandled event 'Inactivity' in 'I2C TX State'")
ANCHOR = re.compile(r"nrf .*BLE out: Sent .*'Active slot")
DPD = re.compile(r"himax .*Entering DPD")
PROBE_IN = re.compile(r"nrf .*BLE in: Received .*'AI slots'")
INACTIVE = re.compile(r"himax .*Inactive for ")


def adb(*args):
    subprocess.run(["adb", "shell", *args], check=False, capture_output=True)


def type_text(text):
    adb("input", "text", text.replace(" ", "%s"))


def tap(xy):
    adb("input", "tap", str(xy[0]), str(xy[1]))


class Tail:
    def __init__(self, path):
        self.f = open(path, "r", encoding="utf-8", errors="replace")
        self.f.seek(0, 2)

    def lines(self):
        while True:
            line = self.f.readline()
            if not line:
                return
            yield line.rstrip("\n")

    def wait(self, patterns, timeout):
        """Return (index of the first pattern matched, line, time) or (None, None, None)."""
        end = time.time() + timeout
        while time.time() < end:
            for line in self.lines():
                for i, p in enumerate(patterns):
                    if p.search(line):
                        return i, line, time.time()
            time.sleep(0.01)
        return None, None, None


def main():
    delays = [int(a) for a in sys.argv[1:]] or list(range(600, 1400, 50))
    tail = Tail(LOG)
    print(f"sweeping {len(delays)} delays: {delays}")
    for d in delays:
        # Anchor: send once, and type the probe while the reply is on its way.
        type_text(CMD)
        tap(SEND)
        time.sleep(0.4)   # let the send clear the field before the probe is typed
        type_text(CMD)
        i, line, t0 = tail.wait([ANCHOR, HIT], 12)
        if i is None:
            print(f"d={d}: no anchor reply within 12 s, stopping")
            return 2
        if i == 1:
            print(f"d={d}: HIT before the probe: {line}")
            return 0
        # Probe at t0 + d.
        remaining = t0 + d / 1000 - time.time()
        if remaining > 0:
            time.sleep(remaining)
        t_tap = time.time()
        tap(SEND)
        # Watch what the device does with it.
        seen = []
        end = time.time() + 7
        outcome = "?"
        while time.time() < end:
            for l in tail.lines():
                if HIT.search(l):
                    outcome = "HIT"
                    seen.append(l)
                    break
                if PROBE_IN.search(l) or INACTIVE.search(l) or DPD.search(l) or ANCHOR.search(l):
                    seen.append(l)
            if outcome == "HIT":
                break
            if any(DPD.search(s) for s in seen) and any(ANCHOR.search(s) for s in seen):
                outcome = "miss"
                break
            time.sleep(0.01)
        print(f"d={d}: tap at +{(t_tap - t0) * 1000:.0f} ms after the anchor line -> {outcome}")
        for s in seen:
            print("    " + s[:110])
        if outcome == "HIT":
            print("Device is stuck awake. Power-cycle it.")
            return 0
        # Let it settle asleep before the next attempt.
        if not any(DPD.search(s) for s in seen):
            tail.wait([DPD], 5)
        time.sleep(0.5)
    print("no hit in this sweep")
    return 1


if __name__ == "__main__":
    sys.exit(main())
