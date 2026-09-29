#!/usr/bin/env python3
"""Verify the fix for Seeed #205 (Inactivity event landing in the IF task's I2C TX state).

Same drive as repro_A.py: anchor on the reply to `AI slots`, send a probe command at a
swept delay so that one attempt lands inside the image task's Save State (about 300 ms
wide, op8 after the last activity). Before the fix the IF task dropped its Inactivity
event ("IF Task unhandled event 'Inactivity' in 'I2C TX State'") and the device never
slept again. After the fix (ae_review 4bcb722c) it must print "Deferring event 0x070e"
in that state, replay it in Idle, send its Sleep line and enter DPD.

Outcomes per attempt:
  FIXED     "Deferring event 0x070e" seen, then "Entering DPD" within the wait
  BROKEN    the old "unhandled event 'Inactivity'" line seen (device stuck awake)
  DEFER-NO-DPD  deferred but no DPD within the wait (investigate)
  miss      the probe missed the window (normal reply, then DPD)

Usage: repro_A_fix.py <log path> [delays ms ...]
"""
import re
import subprocess
import sys
import time

SEND = (985, 1289)          # Engineer Console send button, keyboard open
CMD = "AI slots"            # 116-byte reply; wakes the device if asleep

OLD_HIT = re.compile(r"IF Task unhandled event 'Inactivity'")
DEFER = re.compile(r"himax .*Deferring event 0x070e")
ANCHOR = re.compile(r"nrf .*BLE out: Sent .*'Active slot")
DPD = re.compile(r"himax .*Entering DPD")
PROBE_IN = re.compile(r"nrf .*BLE in: Received .*'AI slots'")
INACTIVE = re.compile(r"himax .*Inactive for ")
SLEEPLINE = re.compile(r"himax .*Sending 116 bytes.*'Sleep ")
PROBE_HIMAX = re.compile(r"himax .*MKL62BA command received: 'slots'")
NRF_SLEEP_REPLY = re.compile(r"nrf .*BLE out: Sent +6 bytes: 'Sleep'")


def adb(*args):
    subprocess.run(["adb", "shell", *args], check=False, capture_output=True)


def type_text(text):
    adb("input", "text", text.replace(" ", "%s"))


def tap(xy):
    adb("input", "tap", str(xy[0]), str(xy[1]))


def clear_field():
    adb("input", "keyevent", "KEYCODE_MOVE_END")
    adb("input", "keyevent", "--longpress", *(["KEYCODE_DEL"] * 20))


class Tail:
    def __init__(self, path):
        self.f = open(path, "r", encoding="utf-8", errors="replace")
        self.f.seek(0, 2)

    def lines(self):
        while True:
            line = self.f.readline()
            if not line:
                return
            yield line.rstrip("\n").replace("\x00", "")

    def wait(self, patterns, timeout):
        end = time.time() + timeout
        while time.time() < end:
            for line in self.lines():
                for i, p in enumerate(patterns):
                    if p.search(line):
                        return i, line, time.time()
            time.sleep(0.01)
        return None, None, None


def main():
    log = sys.argv[1]
    delays = [int(a) for a in sys.argv[2:]] or list(range(600, 1400, 50))
    tail = Tail(log)
    print(f"sweeping {len(delays)} delays: {delays}")
    hits = 0
    for d in delays:
        i = None
        for attempt in range(3):
            clear_field()
            type_text(CMD)
            tap(SEND)
            time.sleep(0.4)
            type_text(CMD)
            i, line, t0 = tail.wait([ANCHOR, OLD_HIT], 12)
            if i is not None:
                break
            print(f"d={d}: no anchor reply within 12 s (attempt {attempt + 1}), device busy? retrying")
            clear_field()
            time.sleep(3)
        if i is None:
            print(f"d={d}: no anchor reply after 3 attempts, stopping")
            return 2
        if i == 1:
            print(f"d={d}: BROKEN before the probe: {line}")
            return 3
        remaining = t0 + d / 1000 - time.time()
        if remaining > 0:
            time.sleep(remaining)
        t_tap = time.time()
        tap(SEND)
        seen, outcome = [], "?"
        deferred = inactive = window = False
        end = time.time() + 8
        while time.time() < end and outcome == "?":
            for l in tail.lines():
                if OLD_HIT.search(l):
                    outcome = "BROKEN"
                    seen.append(l)
                    break
                if DEFER.search(l):
                    deferred = True
                    seen.append(l)
                elif INACTIVE.search(l):
                    inactive = True
                    seen.append(l)
                elif PROBE_HIMAX.search(l):
                    if inactive:
                        window = True
                    seen.append(l)
                elif PROBE_IN.search(l) or SLEEPLINE.search(l) or ANCHOR.search(l) or NRF_SLEEP_REPLY.search(l):
                    seen.append(l)
                elif DPD.search(l):
                    seen.append(l)
                    outcome = "FIXED" if deferred else ("WINDOW-slept" if window else "miss")
                    break
            time.sleep(0.01)
        if outcome == "?":
            outcome = "DEFER-NO-DPD" if deferred else "no DPD in 8 s"
        print(f"d={d}: tap at +{(t_tap - t0) * 1000:.0f} ms after the anchor line -> {outcome}")
        for s in seen:
            print("    " + s[:120])
        if outcome == "BROKEN":
            print("Device is stuck awake. Power-cycle it.")
            return 3
        if outcome in ("FIXED", "WINDOW-slept"):
            hits += 1
            if hits >= 3:
                print(f"{hits} window hits, all slept. Done.")
                return 0
        time.sleep(0.5)
    print(f"sweep finished: {hits} window hits, all slept" if hits else "no window hit in this sweep")
    return 0 if hits else 1


if __name__ == "__main__":
    sys.exit(main())
