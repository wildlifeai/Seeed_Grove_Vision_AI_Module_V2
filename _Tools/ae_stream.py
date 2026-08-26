#!/usr/bin/env python3
"""Continuous 'light' command stream for the WW500 bench.

Complements ae_monitor.py (which drives full 'capture' cycles on a timer) with a
much simpler tool: repeatedly send the on-demand 'light' CLI command
(see EPII_CM55M_APP_S/app/ww_projects/ww500_md/doc/light_sensor.md sec.6.4) as fast
as the device answers, and print each reading. No NN, no file save, no capture -
'light' calls lightSensor_takeReadingForced() directly, so this is the lowest-latency
way to watch the raw light-sensor value change (e.g. while covering/uncovering the
sensor) without waiting for the normal wake-cycle gating.

Each 'light' command keeps the device out of DPD sleep for another 60s
(INACTIVITYTIMEOUTCLI), same as ae_monitor.py's captures, so a continuous stream
holds the device awake indefinitely. If the device is currently asleep when this
script starts, it waits for a wake (RTC timer or motion) first, exactly like
ae_monitor.py.

READING THE OUTPUT - each line is one 'light' command's reply:

    [19:25:35] #12  Light level: 71 (DARK)

Press ESC to stop (Ctrl+C also works). The script always closes the serial port
before exiting, however it exits - normal ESC/Ctrl+C, a device timeout, or any
other error - see the try/finally in main() and the _RawKeys context manager below.

Usage:
    python ae_stream.py --port COM4
    python ae_stream.py --port COM4 --verbose
"""

import argparse
import os
import re
import sys
import time

import serial

ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
LIGHT_RE = re.compile(r"Light level:\s*(\d+)\s*\((DARK|BRIGHT)\)")
BOOT_MARKERS = ("Image sensor and data path initialised", "Inactivity period set",
                "available commands")

# Give up waiting for one 'light' reply after this long (its own sampling window
# is ~2s) and re-send, rather than hanging forever if a reply is missed.
COMMAND_TIMEOUT = 8.0


# --- Cross-platform, non-blocking single-key check for ESC -----------------
#
# Windows: msvcrt.kbhit()/getch() need no setup/teardown of their own.
# POSIX: the terminal must be put into cbreak mode to read a key without the
# user pressing Enter, and MUST be restored afterward - this context manager
# guarantees that restoration on every exit path, the same way the serial port
# itself is guaranteed to close (see main()'s try/finally).
if os.name == "nt":
    import msvcrt

    class _RawKeys:
        def __enter__(self):
            return self

        def __exit__(self, *exc_info):
            return False

    def esc_pressed() -> bool:
        pressed = False
        while msvcrt.kbhit():
            if msvcrt.getch() == b"\x1b":
                pressed = True
        return pressed

else:
    import termios
    import tty
    import select

    class _RawKeys:
        def __enter__(self):
            self.fd = sys.stdin.fileno()
            self.old_settings = termios.tcgetattr(self.fd)
            tty.setcbreak(self.fd)
            return self

        def __exit__(self, *exc_info):
            termios.tcsetattr(self.fd, termios.TCSADRAIN, self.old_settings)
            return False

    def esc_pressed() -> bool:
        pressed = False
        while select.select([sys.stdin], [], [], 0)[0]:
            if sys.stdin.read(1) == "\x1b":
                pressed = True
        return pressed


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except AttributeError:
        pass

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="COM14")
    ap.add_argument("--baud", type=int, default=921600)
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

    with _RawKeys():
        try:
            port.reset_input_buffer()
            print(f"[{wallclock()}] Waiting for the device to wake (RTC timer ~2 min, or wave to trigger motion)...")
            print("Press ESC to stop.")

            buf = b""
            awake = False
            reading_num = 0
            awaiting_reply = False
            command_deadline = 0.0

            while True:
                if esc_pressed():
                    print(f"\n[{wallclock()}] ESC pressed - stopping.")
                    break

                now = time.monotonic()

                if awake and not awaiting_reply:
                    send("light")
                    awaiting_reply = True
                    command_deadline = now + COMMAND_TIMEOUT
                elif awaiting_reply and now >= command_deadline:
                    print(f"[{wallclock()}] No reply within {COMMAND_TIMEOUT:.0f}s - retrying.")
                    awaiting_reply = False  # loop will resend immediately

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
                            print(f"[{wallclock()}] Device is awake - starting the light stream.")

                        m = LIGHT_RE.search(line)
                        if m:
                            reading_num += 1
                            awaiting_reply = False
                            ae = int(m.group(1))
                            state = m.group(2)
                            bar = "#" * min(40, ae * 40 // 255)
                            print(f"[{wallclock()}] #{reading_num:<4} Light level: {ae:3d} ({state:<6}) |{bar:<40}|",
                                  flush=True)

            print(f"[{wallclock()}] Session finished ({reading_num} readings).")
            return 0
        finally:
            port.close()


if __name__ == "__main__":
    sys.exit(main())
