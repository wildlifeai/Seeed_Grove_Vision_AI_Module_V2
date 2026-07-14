#!/usr/bin/env python3
"""Live AE (light sensor) monitor for the WW500 bench.

Holds the device awake and repeatedly triggers an on-demand capture, printing
the firmware's own light-sensor decision each time:

    AE light check: AE Mean = NN, threshold = 65 -> flash ON/OFF

Every 'capture' command is a console keystroke sequence, which resets the CLI
inactivity timer to 60 s (INACTIVITYTIMEOUTCLI), so sending one every <60 s
keeps the device out of deep sleep for the whole session. This makes the
cover/uncover test interactive instead of waiting for the 2-minute timer wake.

Because the device only listens on its UART while awake, the monitor first
waits for a wake (RTC timer or motion), grabs it, then holds it.

Usage:
    python ae_monitor.py                 # default COM14, ~4 min session
    python ae_monitor.py --duration 360  # longer session
"""

import argparse
import re
import sys
import time

import serial

ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
# Matches both the legacy single-frame line ("AE Mean = 46, threshold = 65 -> flash ON")
# and the new aggregated line ("mean AE = 34 (min 3, max 66) over 8 frames,
# threshold = 65, gain railed = yes -> flash ON").
AE_RE = re.compile(r"(?:AE Mean|mean AE)\s*=\s*(\d+).*?threshold\s*=\s*(\d+).*?flash\s*(ON|OFF)")
RAILED_RE = re.compile(r"gain railed\s*=\s*(yes|no)")
INTEG_RE = re.compile(r"Integration time\s*=\s*(\d+)")
AGAIN_RE = re.compile(r"Analog gain\s*=\s*(\d+)")
DGAIN_RE = re.compile(r"Digital gain\s*=\s*(\d+)")
BOOT_MARKERS = ("Image sensor and data path initialised", "Inactivity period set",
                "available commands")


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except AttributeError:
        pass

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="COM14")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--duration", type=float, default=240.0, help="total session seconds")
    ap.add_argument("--interval", type=float, default=20.0, help="seconds between captures (<60)")
    ap.add_argument("--char-delay", type=float, default=0.03, help="inter-character send delay")
    ap.add_argument("--verbose", action="store_true", help="also stream raw console lines")
    args = ap.parse_args()

    eol = b"\r\n"

    try:
        port = serial.Serial(args.port, args.baud, timeout=0.05)
    except serial.SerialException as e:
        print(f"Cannot open {args.port}: {e}", file=sys.stderr)
        return 2

    def send(cmd: str) -> None:
        data = cmd.encode("ascii", "replace") + eol
        for i in range(len(data)):
            port.write(data[i:i + 1])
            port.flush()
            time.sleep(args.char_delay)

    def wallclock() -> str:
        return time.strftime("%H:%M:%S")

    try:
        port.reset_input_buffer()
        print(f"[{wallclock()}] Waiting for the device to wake (RTC timer ~2 min, or wave to trigger motion)...")

        deadline = time.monotonic() + args.duration
        buf = b""
        awake = False
        next_capture = 0.0
        last_ae = None
        reading_num = 0
        integ = again = dgain = None  # most recent gain regs seen

        while time.monotonic() < deadline:
            now = time.monotonic()

            # Once awake, keep the device awake and sample on a cadence
            if awake and now >= next_capture:
                send("capture 1 500")
                next_capture = now + args.interval

            data = port.read(4096)
            if data:
                buf += data
                while True:
                    nl = buf.find(b"\n")
                    if nl < 0:
                        break
                    line = ANSI_RE.sub("", buf[:nl].rstrip(b"\r").decode("utf-8", "replace"))
                    buf = buf[nl + 1:]

                    if args.verbose:
                        print(f"    | {line}")

                    if not awake and any(m in line for m in BOOT_MARKERS):
                        awake = True
                        print(f"[{wallclock()}] Device is awake - holding it up and starting captures.")
                        # first capture almost immediately; small settle so the
                        # CLI task is ready to accept the command
                        next_capture = time.monotonic() + 1.0

                    mi = INTEG_RE.search(line)
                    if mi:
                        integ = int(mi.group(1))
                    ma = AGAIN_RE.search(line)
                    if ma:
                        again = int(ma.group(1))
                    md = DGAIN_RE.search(line)
                    if md:
                        dgain = int(md.group(1))

                    m = AE_RE.search(line)
                    if m:
                        reading_num += 1
                        ae = int(m.group(1))
                        thr = int(m.group(2))
                        flash = m.group(3)
                        mr = RAILED_RE.search(line)
                        railed = (mr.group(1) == "yes") if mr else False
                        state = "DARK " if ae < thr else "BRIGHT"
                        arrow = ""
                        if last_ae is not None:
                            arrow = "up  " if ae > last_ae else ("down" if ae < last_ae else "=   ")
                        last_ae = ae
                        bar = "#" * min(40, ae * 40 // 255)
                        # gain/integration rise in the dark - a more robust dark signal
                        # than AE Mean, which the sensor's own AE re-converges upward
                        gains = f"integ={integ if integ is not None else '?':>4} " \
                                f"aGain={again if again is not None else '?':>2} " \
                                f"dGain={dgain if dgain is not None else '?':>3}"
                        railtag = "RAILED " if railed else "       "
                        print(f"[{wallclock()}] #{reading_num:<3} AE={ae:3d}(thr{thr}) "
                              f"{state} flash {flash:<3} {arrow} {railtag}{gains}  |{bar:<40}|", flush=True)

        print(f"[{wallclock()}] Session finished ({reading_num} readings).")
        return 0
    finally:
        port.close()


if __name__ == "__main__":
    sys.exit(main())
