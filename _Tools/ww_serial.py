#!/usr/bin/env python3
"""Send a command to the WW500 console UART and print the response.

The WW500 (Himax HX6538) console runs the FreeRTOS CLI at 921600 baud.
This tool opens the port, optionally sends one command, then streams whatever
the device says until it goes quiet - one shot per invocation, which suits
scripted/agent use better than an interactive terminal.

The device's UART receive is lossy while the firmware is busy (no flow
control), so --on-boot commands are echo-verified: each command is resent
until its echo appears in the console stream, then the next one is sent.

Examples:
    python ww_serial.py ver                     # send 'ver', print reply
    python ww_serial.py "setop 23 65"           # arguments with spaces
    python ww_serial.py --listen 10             # just listen for 10 seconds
    python ww_serial.py --port COM13 status     # the other UART
    # catch a power-cycle: when the boot banner appears, keep it awake
    python ww_serial.py --listen 120 --on-boot "setop 8 60000"

NOTE: close TeraTerm first - Windows COM ports are exclusive.
"""

import argparse
import re
import sys
import time

import serial

ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

# Lines that show the firmware is up and the CLI task is accepting input.
BOOT_MARKERS = ("Inactivity period set", "Starting Image Task", "available commands")

ECHO_TIMEOUT = 2.5   # resend a command if its echo hasn't appeared in this long
MAX_ATTEMPTS = 5


def emit(line: str, raw: bool) -> None:
    if not raw:
        line = ANSI_RE.sub("", line)
    print(line, flush=True)


def main() -> int:
    # Windows consoles default to cp1252; device output may contain
    # replacement chars / box drawing that cp1252 cannot encode.
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except AttributeError:
        pass

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", nargs="*", help="command to send (omit with --listen to only listen)")
    ap.add_argument("--port", default="COM14")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--timeout", type=float, default=6.0, help="max seconds to wait for output")
    ap.add_argument("--quiet-ms", type=int, default=800, help="stop after this long with no new output")
    ap.add_argument("--listen", type=float, metavar="SECS", help="listen-only for SECS seconds (no quiet cutoff)")
    ap.add_argument("--on-boot", metavar="CMDS",
                    help="when a boot banner is seen, send these ';'-separated commands, "
                         "echo-verified with retries (e.g. \"setop 8 60000;ver\")")
    ap.add_argument("--no-verify", action="store_true",
                    help="send --on-boot commands exactly once each (no echo-verified retries). "
                         "REQUIRED for non-idempotent commands like 'switchslot', which toggles "
                         "on every call - a retry would undo it.")
    ap.add_argument("--send-gap", type=float, default=1.5,
                    help="seconds between --no-verify commands (default 1.5)")
    ap.add_argument("--marker", metavar="TEXT",
                    help="override the boot-detection marker line for --on-boot")
    ap.add_argument("--settle", type=float, default=0.3,
                    help="seconds to wait after the marker before sending (default 0.3)")
    ap.add_argument("--raw", action="store_true", help="keep ANSI colour codes")
    ap.add_argument("--eol", default="\\r\\n",
                    help="line ending to send, escaped (default CRLF; try \\r if commands double up)")
    ap.add_argument("--char-delay", type=float, default=0.03,
                    help="delay between sent characters in seconds (default 0.03). The console "
                         "UART uses a 1-char interrupt buffer re-armed by the CLI task, so "
                         "burst-written commands lose characters whenever the CPU is busy.")
    args = ap.parse_args()

    eol = args.eol.encode("ascii").decode("unicode_escape").encode("ascii")

    def send_slow(data: bytes) -> None:
        for i in range(len(data)):
            port.write(data[i:i + 1])
            port.flush()
            time.sleep(args.char_delay)

    try:
        port = serial.Serial(args.port, args.baud, timeout=0.05)
    except serial.SerialException as e:
        print(f"Cannot open {args.port}: {e}", file=sys.stderr)
        print("Is TeraTerm (or another terminal) still holding the port?", file=sys.stderr)
        return 2

    try:
        port.reset_input_buffer()

        cmd = " ".join(args.command).strip()
        if cmd:
            send_slow(cmd.encode("ascii", "replace") + eol)

        deadline = time.monotonic() + (args.listen if args.listen else args.timeout)
        quiet_cutoff = None if args.listen else args.quiet_ms / 1000.0
        last_data = time.monotonic()
        got_any = False
        buf = b""

        # --on-boot queue state (echo-verified sends)
        pending = [c.strip() for c in args.on_boot.split(";") if c.strip()] if args.on_boot else []
        markers = (args.marker,) if args.marker else BOOT_MARKERS
        boot_seen_at = None   # monotonic time the boot marker was seen
        sent_at = None        # when the current pending[0] was last sent
        attempts = 0

        while time.monotonic() < deadline:
            now = time.monotonic()

            # drive the command queue
            if pending and boot_seen_at is not None and now >= boot_seen_at + args.settle:
                # "@wait:N" pseudo-command: pause the queue N seconds (e.g. to let
                # camera init finish before a capture) without losing the keep-awake
                # effect of commands already sent.
                if pending[0].startswith("@wait:"):
                    if sent_at is None:
                        secs = float(pending[0].split(":", 1)[1])
                        print(f">>> waiting {secs:g}s", flush=True)
                        sent_at = now + secs  # reuse sent_at as the resume time
                    elif now >= sent_at:
                        pending.pop(0)
                        sent_at = None
                        attempts = 0
                elif args.no_verify:
                    # send each command exactly once, spaced by --send-gap.
                    # sent_at holds the time of the last send; gap between sends only.
                    if sent_at is None or now - sent_at >= args.send_gap:
                        print(f">>> sending (once): {pending[0]}", flush=True)
                        send_slow(pending[0].encode("ascii", "replace") + eol)
                        pending.pop(0)
                        sent_at = time.monotonic()
                elif sent_at is None or now - sent_at > ECHO_TIMEOUT:
                    if attempts >= MAX_ATTEMPTS:
                        print(f">>> giving up on: {pending[0]}", flush=True)
                        pending.pop(0)
                        sent_at = None
                        attempts = 0
                    else:
                        attempts += 1
                        tag = "" if attempts == 1 else f" (attempt {attempts})"
                        print(f">>> sending: {pending[0]}{tag}", flush=True)
                        send_slow(pending[0].encode("ascii", "replace") + eol)
                        sent_at = time.monotonic()

            data = port.read(4096)
            if data:
                got_any = True
                last_data = now
                buf += data
                # stream complete lines as they arrive
                while True:
                    nl = buf.find(b"\n")
                    if nl < 0:
                        break
                    line = buf[:nl].rstrip(b"\r").decode("utf-8", "replace")
                    buf = buf[nl + 1:]
                    emit(line, args.raw)
                    if pending and boot_seen_at is None and any(m in line for m in markers):
                        boot_seen_at = time.monotonic()
                    elif (not args.no_verify) and pending and sent_at is not None and pending[0] in line:
                        # device echoed the command - it was received
                        pending.pop(0)
                        sent_at = None
                        attempts = 0
            elif quiet_cutoff is not None and got_any and (now - last_data) > quiet_cutoff:
                break

        if buf:  # trailing partial line (e.g. the 'cmd> ' prompt)
            emit(buf.rstrip(b"\r").decode("utf-8", "replace"), args.raw)

        if not got_any:
            print("(no output - device may be in deep power down; "
                  "it wakes on motion, its RTC alarm, or the BLE WAKE pin, not on UART)",
                  file=sys.stderr)
        return 0
    finally:
        port.close()


if __name__ == "__main__":
    sys.exit(main())
