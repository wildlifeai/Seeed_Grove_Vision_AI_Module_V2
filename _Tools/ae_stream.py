#!/usr/bin/env python3
"""Continuous 'light' command stream for the WW500 bench.

Complements ae_monitor.py (which drives full 'capture' cycles on a timer) with a
much simpler tool: repeatedly send the on-demand 'light' CLI command
(see EPII_CM55M_APP_S/app/ww_projects/ww500_md/doc/light_sensor.md sec.6.4) as fast
as the device answers, and print each reading. No NN, no file save - only a
throwaway single-frame capture, the same one the periodic AE-check-interval timer
wake already uses, so this is a much lower-latency way to watch the raw light-sensor
value change (e.g. while covering/uncovering the sensor) than waiting for the normal
wake-cycle gating.

'light' replies to the console immediately ("Checking light level...") without
waiting for the reading - the actual result is reported asynchronously, on the
"[LS] AE light check: ..." console line (this script's LIGHT_RE parses that line,
stripped of the "[LS] " marker and any colour codes) once the ~2s sampling window
finishes.

Each 'light' command keeps the device out of DPD sleep for another 60s
(INACTIVITYTIMEOUTCLI), same as ae_monitor.py's captures, so a continuous stream
holds the device awake indefinitely. If the device is currently asleep when this
script starts, it waits for a wake (RTC timer or motion) first, exactly like
ae_monitor.py.

--capture switches from the throwaway 'light' check to a real 'capture 1 1'
(one image, saved to SD, same as ae_monitor.py but sent back-to-back as fast
as the device answers rather than on a fixed timer). The same "[LS] AE light
check: ..." line is parsed either way - lightSensor_takeReading() runs after
a real capture too - but it is only printed if the AE decision is actually
consumed: enable one of, over the console, first:

    setop 13 1     # OP_PARAMETER_FLASH_LED = visible LED -> FLASH_MODE_AE
    setop 13 2     # same, but IR LED
    setop 26 1     # OP_PARAMETER_SLOT_SWITCH -> automatic day/night switching (op26)

('light' does not need this - it forces the reading regardless.) A real
capture also takes noticeably longer (JPEG encode, NN processing, disk write)
than the ~2s 'light' sampling window, so --capture uses a longer reply
timeout.

READING THE OUTPUT - each line is one 'light' command's result. analog gain and
converged (AE_CONVERGED) are the sensor's own state on the last sampled frame -
useful for judging how much to trust a given mean-AE reading:

    [19:25:35] #12  Light level:  71 (DARK  ) gain= 12 conv=Y |###########                             |

BENCH NOTES - at any time (no need to stop the stream), type free text and
press Enter to log it as a timestamped line alongside the readings, e.g. while
covering the sensor by hand to mark what you just did:

    [19:26:02] NOTE: covered sensor with hand

Typing is silent - keystrokes are not echoed to the console and are never
sent to the device - only the finished note appears, once you press Enter.
Backspace still edits the note before then. Because it does not interrupt
the 'light' command stream, reading lines keep appearing while you type; the
note itself is unaffected and is logged correctly once you press Enter.

Press ESC to stop (Ctrl+C also works). The script always closes the serial port
before exiting, however it exits - normal ESC/Ctrl+C, a device timeout, or any
other error - see the try/finally in main() and the _RawKeys context manager below.

Usage:
    python ae_stream.py --port COM4
    python ae_stream.py --port COM4 --verbose
    python ae_stream.py --port COM4 --capture   # real 'capture 1 1' cycles instead of 'light'
"""

import argparse
import os
import re
import sys
import time

import serial

ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
# Anchored on the '[LS] ' marker: decideDarkBright() (lightSensor.c) prints the
# same text twice on the console - once itself, prefixed '[LS] ', and again
# (unprefixed) wherever the outgoing message to the BLE processor gets echoed.
# Without the anchor both copies match and every reading is reported twice.
LIGHT_RE = re.compile(
    r"^\[LS\]\s.*mean AE\s*=\s*(\d+).*?"
    r"analog gain\s*=\s*(\d+).*?converged\s*=\s*(yes|no).*?"
    r"->\s*(DARK|BRIGHT)"
)
BOOT_MARKERS = ("Image sensor and data path initialised", "Inactivity period set",
                "available commands")

# Give up waiting for one reply after this long and re-send, rather than
# hanging forever if a reply is missed. 'light's own sampling window is ~2s;
# a real 'capture' additionally encodes/writes/runs NN, so it gets longer.
COMMAND_TIMEOUT = 8.0
COMMAND_TIMEOUT_CAPTURE = 20.0


# --- Cross-platform, non-blocking keyboard polling --------------------------
#
# Windows: msvcrt.kbhit()/getch() need no setup/teardown of their own.
# POSIX: the terminal must be put into cbreak mode to read a key without the
# user pressing Enter, and MUST be restored afterward - this context manager
# guarantees that restoration on every exit path, the same way the serial port
# itself is guaranteed to close (see main()'s try/finally).
#
# poll_keys() returns every keystroke waiting right now, as a plain string
# (possibly empty) - ESC, Enter, Backspace and printable characters all come
# back as normal characters ('\x1b', '\r'/'\n', '\x08'/'\x7f', ...) for main()
# to interpret; it is what lets main() both watch for ESC and accumulate the
# free-text notes described in the module docstring.
if os.name == "nt":
    import msvcrt

    class _RawKeys:
        def __enter__(self):
            return self

        def __exit__(self, *exc_info):
            return False

    def poll_keys() -> str:
        chars = []
        while msvcrt.kbhit():
            ch = msvcrt.getch()
            if ch in (b"\x00", b"\xe0"):
                msvcrt.getch()  # discard 2nd byte of an extended key (arrows, F-keys, ...)
                continue
            chars.append(ch.decode("utf-8", "replace"))
        return "".join(chars)

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

    def poll_keys() -> str:
        chars = []
        while select.select([sys.stdin], [], [], 0)[0]:
            chars.append(sys.stdin.read(1))
        return "".join(chars)


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
    ap.add_argument("--capture", action="store_true",
                     help="send real 'capture 1 1' cycles instead of the throwaway 'light' check")
    args = ap.parse_args()

    command = "capture 1 1" if args.capture else "light"
    command_timeout = COMMAND_TIMEOUT_CAPTURE if args.capture else COMMAND_TIMEOUT

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
            print("Press ESC to stop. Type notes and press Enter to log them.")

            buf = b""
            awake = False
            reading_num = 0
            awaiting_reply = False
            command_deadline = 0.0
            note_buf = []
            stopping = False

            while True:
                for ch in poll_keys():
                    if ch == "\x1b":
                        stopping = True
                    elif ch in ("\r", "\n"):
                        if note_buf:
                            note_text = "".join(note_buf)
                            note_buf.clear()
                            print(f"[{wallclock()}] NOTE: {note_text}", flush=True)
                        # blank Enter (no text typed) - ignore
                    elif ch in ("\x08", "\x7f"):  # Backspace / DEL
                        if note_buf:
                            note_buf.pop()
                    elif ch.isprintable():
                        note_buf.append(ch)

                if stopping:
                    print(f"\n[{wallclock()}] ESC pressed - stopping.")
                    break

                now = time.monotonic()

                if awake and not awaiting_reply:
                    send(command)
                    awaiting_reply = True
                    command_deadline = now + command_timeout
                elif awaiting_reply and now >= command_deadline:
                    print(f"[{wallclock()}] No reply within {command_timeout:.0f}s - retrying.")
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
                            print(f"[{wallclock()}] Device is awake - starting the '{command}' stream.")

                        m = LIGHT_RE.search(line)
                        if m:
                            reading_num += 1
                            awaiting_reply = False
                            ae = int(m.group(1))
                            analog_gain = int(m.group(2))
                            converged = m.group(3) == "yes"
                            state = m.group(4)
                            bar = "#" * min(40, ae * 40 // 255)
                            print(f"[{wallclock()}] #{reading_num:<4} Light level: {ae:3d} ({state:<6}) "
                                  f"gain={analog_gain:3d} conv={'Y' if converged else 'N'} |{bar:<40}|",
                                  flush=True)

            print(f"[{wallclock()}] Session finished ({reading_num} readings).")
            return 0
        finally:
            port.close()


if __name__ == "__main__":
    sys.exit(main())
