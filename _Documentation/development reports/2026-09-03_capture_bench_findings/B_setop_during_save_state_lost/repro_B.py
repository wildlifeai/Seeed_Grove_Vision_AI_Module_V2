#!/usr/bin/env python3
"""Reproduce finding B on demand: a `setop` that lands after Save State is acknowledged but
never written, so the device wakes with the old value.

The window is the one finding A's sweep hits: after the last activity the idle detector
fires (op8, 1000 ms here), the image task saves state and unmounts the card, and a moment
later the IF task sends `Sleep` and the Himax enters DPD. A `setop` processed in between
gets `Set OpParam N = V` back, posts SAVE_CONFIG to a FatFS task whose `mounted` flag is
stale, and the save fails with FR_NOT_ENABLED (12). The RAM copy is lost in DPD.

Mechanics as in repro_A.py: anchor on one command's reply, type the probe while the reply
is on its way, tap send at a swept delay, read the outcome from the three-way bench log.
op9 (LED brightness percent) is the value flipped; it is read first and restored at the
end.

    python repro_B.py                # sweep 600..900 ms in 50 ms steps
    python repro_B.py 700 750        # chosen delays
"""
import re
import subprocess
import sys
import time

import os

LOG = os.environ.get("REPRO_B_LOG") or (
    r"C:\Users\Victor\AppData\Local\Temp\claude\C--Users-ww"
    r"\2c78acd7-f4c0-4115-a1cf-a222e9aae63a\scratchpad\bench\2026-09-04_finding_B\finding_B_bench.log")
SEND = (985, 1289)
OP = 9
ANCHOR_CMD = "AI slots"

ANCHOR = re.compile(r"nrf .*BLE out: Sent .*'Active slot")
GETOP = re.compile(r"nrf .*BLE out: Sent .*'OpParam %d = (\d+)'" % OP)
SETOP_REPLY = re.compile(r"nrf .*BLE out: Sent .*'Set OpParam %d = (\d+)'" % OP)
PROBE_RX = re.compile(r"himax .*command received: 'setop %d " % OP)
SAVED = re.compile(r"himax .*Saved state to SD card")
ERR12 = re.compile(r"himax .*Error 12 saving config")
A_HIT = re.compile(r"IF Task unhandled event 'Inactivity' in 'I2C TX State'")
DPD = re.compile(r"himax .*Entering DPD")
INACTIVE = re.compile(r"himax .*Inactive for ")


def adb(*args):
    subprocess.run(["adb", "shell", *args], check=False, capture_output=True)


def type_text(text):
    adb("input", "text", text.replace(" ", "%s"))


def tap():
    adb("input", "tap", str(SEND[0]), str(SEND[1]))


def send(text):
    """Type, wait for the injected text to land (slow when the console is busy), then send."""
    type_text(text)
    time.sleep(1.5)
    tap()


class Tail:
    def __init__(self, path):
        self.f = open(path, "r", encoding="utf-8", errors="replace")
        self.f.seek(0, 2)

    def lines(self):
        while True:
            line = self.f.readline()
            if not line:
                return
            yield line.rstrip("\n").replace("\0", "")

    def wait(self, patterns, timeout):
        end = time.time() + timeout
        while time.time() < end:
            for line in self.lines():
                for i, p in enumerate(patterns):
                    if p.search(line):
                        return i, line
            time.sleep(0.01)
        return None, None


def getop(tail):
    """Send AI getop OP and return the value the device reports, waking it if needed."""
    send(f"AI getop {OP}")
    i, line = tail.wait([GETOP], 15)
    return int(GETOP.search(line).group(1)) if line else None


def settle(tail):
    """Wait for the device to go to sleep after the last activity."""
    tail.wait([DPD], 6)
    time.sleep(0.3)


def verify(tail, new, original):
    """After the device has slept (or been power-cycled): read op back, then restore it."""
    after = getop(tail)
    print(f"after the wake op{OP} = {after}; the acknowledged value was {new}, the value before was {original}")
    if after is not None and after != new:
        print("PROOF: the acknowledged setop was not saved")
    settle(tail)
    send(f"AI setop {OP} {original}")
    i, line = tail.wait([SETOP_REPLY], 15)
    print(f"restored: {line[12:80] if line else 'no reply'}")


def main():
    tail = Tail(LOG)
    if sys.argv[1:2] == ["verify"]:
        verify(tail, int(sys.argv[2]), int(sys.argv[3]))
        return 0
    delays = [int(a) for a in sys.argv[1:]] or list(range(600, 901, 50))

    orig = getop(tail)
    if orig is None:
        print("no reply to getop; is the console open with the input focused?")
        return 2
    original = orig
    print(f"op{OP} is {orig}")
    settle(tail)
    candidates = [v for v in (orig + 1, orig + 2) if v != orig]

    for n, d in enumerate(delays):
        new = candidates[n % 2]
        # Anchor, and pre-type the probe while the reply is on its way.
        type_text(ANCHOR_CMD)
        tap()
        time.sleep(0.4)
        type_text(f"AI setop {OP} {new}")
        i, line = tail.wait([ANCHOR, A_HIT], 15)
        if i is None:
            print(f"d={d}: no anchor reply within 15 s, stopping")
            return 2
        if i == 1:
            print(f"d={d}: A hit before the probe went out; power-cycle the device")
            return 3
        t0 = time.time()
        remaining = t0 + d / 1000 - time.time()
        if remaining > 0:
            time.sleep(remaining)
        tap()

        # Classify what happened, in order.
        seen = []
        end = time.time() + 8
        while time.time() < end:
            for l in tail.lines():
                for p in (PROBE_RX, SAVED, ERR12, A_HIT, DPD, INACTIVE, SETOP_REPLY):
                    if p.search(l):
                        seen.append((p, l))
                        break
            if any(p is A_HIT for p, _ in seen) or any(p is DPD for p, _ in seen) and any(p is PROBE_RX for p, _ in seen):
                break
            time.sleep(0.01)
        order = [p for p, _ in seen]
        a_hit = A_HIT in order
        b_hit = ERR12 in order
        if PROBE_RX not in order:
            outcome = "late: the device slept before the probe arrived (it woke it instead)"
        elif b_hit:
            outcome = "B HIT: setop acknowledged after Save State, save failed with error 12"
        elif SAVED in order and order.index(PROBE_RX) < order.index(SAVED):
            outcome = "early: setop processed before Save State, so it was saved normally"
        else:
            outcome = "unclear, see the lines"
        print(f"\nd={d} ms, setop {OP} {new}: {outcome}" + (" and A hit (device stuck awake)" if a_hit else ""))
        for _, l in seen:
            print("    " + l[:120])

        if a_hit:
            print(f"\nThe device is stuck awake (finding A). Power-cycle it, then run:"
                  f"  python repro_B.py verify {new} {orig}")
            return 3
        if b_hit:
            settle(tail)
            print()
            verify(tail, new, original)
            return 0

        # Not in the window: the value was saved normally; wait for sleep and try the next delay.
        settle(tail)
        orig = new if PROBE_RX in order else orig
        candidates = [v for v in (orig + 1, orig + 2) if v != orig]

    print("\nno hit in this sweep")
    if orig != original:
        type_text(f"AI setop {OP} {original}")
        tap()
        i, line = tail.wait([SETOP_REPLY], 15)
        print(f"restored: {line[12:80] if line else 'no reply'}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
