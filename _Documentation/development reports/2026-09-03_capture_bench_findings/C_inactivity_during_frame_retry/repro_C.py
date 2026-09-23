#!/usr/bin/env python3
"""Reproduce finding C on demand: the device sleeps in the middle of a capture sequence.

Mechanism (ae_review e8b7feb5): the inactivity detector counts idle time, and a capture
waiting for its next frame is idle. When it fires mid-capture the image task rightly
ignores it (image_task.c:1074, "Inactive - expect WDT timeout soon?"), but the IF task
does not: it sends `Sleep` to the nRF, sets `lastMessageSent` (if_task.c:926) and calls
`barrier_ready()`. `lastMessageSent` is never cleared, so the IF task calls
`barrier_ready()` again on its next transmission (if_task.c:1089), and the two-party
shutdown barrier (ww500_md.c:872) counts calls, not tasks (barrier.c:39). The second
call fires `image_sleepNow()` with the image task still capturing.

So: a capture of several images with a gap longer than op8 (1000 ms), and any message
the IF task sends after the detector has fired, ends the sequence in DPD. The per-image
`HM0360 AE regs` telemetry is such a message, so the capture alone should do it; the
optional probe adds a command reply as a second trigger.

    python repro_C.py                     # AI capture 3 3000, watch
    python repro_C.py --probe             # also AI slots 2.5 s in
    python repro_C.py --images 3 --interval 3000
"""
import argparse
import os
import re
import subprocess
import sys
import time

LOG = os.environ.get("REPRO_C_LOG") or (
    r"C:\Users\Victor\AppData\Local\Temp\claude\C--Users-ww"
    r"\2c78acd7-f4c0-4115-a1cf-a222e9aae63a\scratchpad\bench\2026-09-04_finding_C\finding_C_bench.log")
SEND = (985, 1289)

WATCH = [
    ("capture in", re.compile(r"nrf .*BLE in: Received .*'AI capture")),
    ("image", re.compile(r"himax .*Image capture (\d+)/(\d+) took")),
    ("inactive", re.compile(r"himax .*Inactive for \d+ms")),
    ("image task ignores", re.compile(r"himax .*Inactive - expect WDT")),
    ("IF ready", re.compile(r"himax .*IF task ready to sleep")),
    ("Sleep to nRF", re.compile(r"nrf .*BLE out: Sent .*'Sleep'")),
    ("probe in", re.compile(r"nrf .*BLE in: Received .*'AI slots'")),
    ("probe reply", re.compile(r"nrf .*BLE out: Sent .*'Active slot")),
    ("AE regs out", re.compile(r"himax .*Sending .*'HM0360 AE regs")),
    ("DPD", re.compile(r"himax .*Entering DPD")),
    ("Captured", re.compile(r"Captured \d+ images")),
    ("app", re.compile(r"app .*RAW_RX received .*(Sleep|Captured|Active slot)")),
]
STAMP = re.compile(r"^\[(\d\d):(\d\d)\.(\d\d\d)\]")


def adb(*args):
    subprocess.run(["adb", "shell", *args], check=False, capture_output=True)


def send(text):
    adb("input", "text", text.replace(" ", "%s"))
    time.sleep(1.5)
    adb("input", "tap", str(SEND[0]), str(SEND[1]))


def stamp(line):
    m = STAMP.match(line)
    return int(m.group(1)) * 60 + int(m.group(2)) + int(m.group(3)) / 1000 if m else None


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--images", type=int, default=3)
    ap.add_argument("--interval", type=int, default=3000, help="ms between images; keep it above op8")
    ap.add_argument("--probe", action="store_true", help="also send AI slots 2.5 s after the capture command")
    ap.add_argument("--log", default=LOG)
    args = ap.parse_args()

    tail = Tail(args.log)
    send(f"AI capture {args.images} {args.interval}")
    t_cmd = time.time()
    probe_sent = False
    events = []
    t0 = None
    end = time.time() + 20 + args.images * args.interval / 1000
    while time.time() < end:
        for line in tail.lines():
            for name, p in WATCH:
                if p.search(line):
                    events.append((name, line))
                    if name == "capture in":
                        t0 = stamp(line)
                    break
        if args.probe and not probe_sent and time.time() - t_cmd > 2.5:
            send("AI slots")
            probe_sent = True
        names = [n for n, _ in events]
        if "DPD" in names or "Captured" in names and names.count("image") >= args.images:
            time.sleep(1.0)
            for line in tail.lines():
                for name, p in WATCH:
                    if p.search(line):
                        events.append((name, line))
                        break
            break
        time.sleep(0.02)

    if t0 is None:
        print("the nRF never saw the capture command; is the console open with the input focused?")
        return 2
    print(f"AI capture {args.images} {args.interval}" + (" with AI slots at +2.5 s" if args.probe else "") + ":\n")
    for name, line in events:
        t = stamp(line)
        print(f"  [+{t - t0:6.3f}] {name:<19} {line[12:110]}")

    names = [n for n, _ in events]
    images = [int(WATCH[1][1].search(l).group(1)) for n, l in events if n == "image"]
    dpd = "DPD" in names
    captured = "Captured" in names
    print()
    if dpd and not captured:
        print(f"HIT: entered DPD after image {max(images) if images else 0} of {args.images}, "
              f"'Captured' never sent; IF task declared ready {names.count('IF ready')} times")
        return 0
    if dpd and captured and names.index("DPD") < names.index("Captured"):
        print("HIT: entered DPD before 'Captured'")
        return 0
    print("no hit: the sequence completed" + (" and then slept" if dpd else ""))
    return 1


if __name__ == "__main__":
    sys.exit(main())
