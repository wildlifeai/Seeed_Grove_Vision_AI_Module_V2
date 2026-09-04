#!/usr/bin/env python3
"""Merge the three sides of a WW500 bench session into one time-ordered log.

The app talks to the nRF52 over BLE, and the nRF52 relays to the Himax over
I2C. Each leg is only visible from its own console, so a single-source log can
never show a command arriving and being acted on. This interleaves all three:

    [06:14.266] app   | Written AI light to the device WILD-CNKW
    [06:14.469] nrf   | Sending 'light' to AI processor (4 ctrl bytes, ...)
    [06:14.578] himax | AE light check: mean AE = 71 ... -> DARK (flash wanted)

Sources
    app    adb logcat, ReactNativeJS tag only
    nrf    MKL62BA console, J5, 115200 baud
    himax  HX6538 console, J2, 921600 baud

Timestamps are MM:SS.mmm relative to the start of the capture, matching the
format used by the earlier bench threads in the firmware repo.

Which FTDI port is which is detected rather than assumed: each is probed at
both baud rates and identified by what it says. Override with --himax/--nrf if
the detection guesses wrong.

Usage
    python bench_log.py                          # capture until Ctrl+C
    python bench_log.py --duration 120           # stop after two minutes
    python bench_log.py -o light_sensor.log      # choose the output file
    python bench_log.py --himax COM6 --nrf COM5  # skip detection

NOTE: Windows COM ports are exclusive. Close TeraTerm before running this.
"""

import argparse
import queue
import re
import subprocess
import sys
import threading
import time

import serial
from serial.tools import list_ports

HIMAX_BAUD = 921600   # HX6538 console on J2
NRF_BAUD = 115200     # MKL62BA console on J5

# FTDI adapters on this bench, by their burned-in serial number. COM numbers get
# reassigned by Windows; these do not, so they identify the cable rather than the
# port it happens to land on. Verified 1 Sep 2026 by reading each port at 921600:
# FTFAERNNA was 99% legible (Himax), FTH0BZPJA 0% (nRF, which needs 115200).
KNOWN_ADAPTERS = {
    "FTFAERNNA": "himax",
    "FTH0BZPJA": "nrf",
}

ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
# Leading "MM-DD HH:MM:SS.mmm  pid  tid I ReactNativeJS: " from adb logcat.
LOGCAT_RE = re.compile(r"^\d\d-\d\d \d\d:\d\d:\d\d\.\d+\s+\d+\s+\d+\s+\w\s+\w+\s*:\s?(.*)$")

# The Supabase anon key is public by design (RLS is the security boundary and the
# key ships inside the app bundle), but a bench log is shared and pasted into
# issues, where a JWT in plain sight invites someone to treat it as a leak.
JWT_RE = re.compile(r"eyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+")

# Lines only one of the two processors ever prints. Used to label the ports.
HIMAX_MARKERS = ("OpParam", "IMAGE Task", "[LS]", "AE light check", "Inactivity period",
                 "Starting Image Task", "HM0360", "Captured")
NRF_MARKERS = ("<info>", "<debug>", "BLE out", "AI processor", "app:")

stop = threading.Event()
lines = queue.Queue()


def printable_ratio(data: bytes) -> float:
    """Share of bytes that look like console text. Near 1.0 at the right baud."""
    if not data:
        return 0.0
    ok = sum(1 for b in data if 32 <= b < 127 or b in (9, 10, 13))
    return ok / len(data)


def sniff(port: str, baud: float, seconds: float = 1.2) -> bytes:
    """Read whatever a port emits at one baud rate, without disturbing it."""
    try:
        with serial.Serial(port, baud, timeout=0.2) as ser:
            deadline = time.time() + seconds
            data = b""
            while time.time() < deadline:
                data += ser.read(4096)
            return data
    except serial.SerialException:
        return b""


def identify(ports: list) -> dict:
    """Work out which port is the Himax and which is the nRF.

    One probe per port, at the Himax baud only. That single test separates them:
    the Himax console is legible at 921600, while the nRF at 115200 read at
    921600 is pure line noise (measured 0% printable against 99%). Reading each
    port once also halves the number of opens, which matters because opening an
    FTDI adapter toggles DTR and can reset the board.

    A silent port is inferred from whatever the other port turned out to be.
    """
    found = {}
    for port in ports:
        data = sniff(port, HIMAX_BAUD)
        ratio = printable_ratio(data)
        text = data.decode("ascii", "replace")

        if any(m in text for m in HIMAX_MARKERS) or (data and ratio > 0.8):
            found[port] = ("himax", HIMAX_BAUD, f"{ratio:.0%} readable at {HIMAX_BAUD}")
        elif data:
            found[port] = ("nrf", NRF_BAUD, f"only {ratio:.0%} readable at {HIMAX_BAUD}")
        else:
            found[port] = (None, None, "silent")

    # Infer a silent port from the role its neighbour claimed. Only safe when
    # exactly one port was silent: with both silent there is nothing to infer
    # from, and guessing would quietly produce a log of line noise read at the
    # wrong baud, which looks like a capture but contains nothing.
    silent = [p for p, v in found.items() if v[0] is None]
    if len(silent) == len(found):
        return {}
    for port in silent:
        claimed = {v[0] for p, v in found.items() if p != port and v[0]}
        role = "nrf" if "himax" in claimed else "himax"
        found[port] = (role, HIMAX_BAUD if role == "himax" else NRF_BAUD,
                       "inferred, port was silent")
    return found


def read_serial(port: str, baud: int, label: str) -> None:
    try:
        ser = serial.Serial(port, baud, timeout=0.3)
    except serial.SerialException as e:
        lines.put((time.time(), label, f"!! could not open {port}: {e}"))
        return
    buf = b""
    with ser:
        while not stop.is_set():
            try:
                buf += ser.read(4096)
            except serial.SerialException:
                break
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = ANSI_RE.sub("", raw.decode("ascii", "replace")).rstrip("\r")
                if text.strip():
                    lines.put((time.time(), label, text))


def read_logcat() -> None:
    """Stream the app's own logs. Only ReactNativeJS, so native noise stays out."""
    subprocess.run(["adb", "logcat", "-c"], capture_output=True)
    proc = subprocess.Popen(
        ["adb", "logcat", "-v", "threadtime", "-s", "ReactNativeJS:*"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1,
        # adb emits UTF-8; without saying so Python decodes as cp1252 on Windows
        # and the app's em dashes arrive as "â€”".
        encoding="utf-8", errors="replace",
    )
    try:
        for raw in proc.stdout:
            if stop.is_set():
                break
            m = LOGCAT_RE.match(raw.rstrip("\n"))
            text = m.group(1) if m else raw.strip()
            if not text.strip() or text.startswith("---------"):
                continue
            # A console.log of a large object spills across many logcat lines,
            # every continuation indented. The Supabase client dump alone is over
            # 150 lines per app start, which buries the BLE traffic this log
            # exists to show. Real log statements begin at column zero.
            if text.startswith("  "):
                continue
            lines.put((time.time(), "app", JWT_RE.sub("<jwt redacted>", text)))
    finally:
        proc.terminate()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--output", default="bench.log", help="file to write (default: bench.log)")
    ap.add_argument("--duration", type=float, help="stop after N seconds (default: until Ctrl+C)")
    ap.add_argument("--himax", help="Himax console port, skips detection")
    ap.add_argument("--nrf", help="nRF console port, skips detection")
    ap.add_argument("--no-app", action="store_true", help="serial only, skip adb logcat")
    ap.add_argument("--note", default="", help="one-line description written as the file header")
    args = ap.parse_args()

    # The app logs emoji, and the Windows console is cp1252, so an unguarded
    # print kills the capture mid-session. The file is UTF-8 either way; only
    # the mirrored console output degrades.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    if args.himax and args.nrf:
        assignment = {args.himax: ("himax", HIMAX_BAUD, "given"), args.nrf: ("nrf", NRF_BAUD, "given")}
    else:
        ftdi = [p for p in list_ports.comports() if "VID:PID=0403" in (p.hwid or "")]
        if len(ftdi) < 2:
            print(f"Found {len(ftdi)} FTDI port(s): {[p.device for p in ftdi] or 'none'}. "
                  "Expected two. Pass --himax and --nrf explicitly.", file=sys.stderr)
            return 1
        ftdi = ftdi[:2]

        # Recognised cables first: no probing, so nothing is opened until the
        # capture starts and the board is never reset just to find out its name.
        assignment = {
            p.device: (KNOWN_ADAPTERS[p.serial_number],
                       HIMAX_BAUD if KNOWN_ADAPTERS[p.serial_number] == "himax" else NRF_BAUD,
                       f"known adapter {p.serial_number}")
            for p in ftdi if p.serial_number in KNOWN_ADAPTERS
        }
        if len(assignment) != 2:
            print(f"Probing {', '.join(p.device for p in ftdi)} ...", file=sys.stderr)
            assignment = identify([p.device for p in ftdi])
        if not assignment:
            print("Both consoles were silent, so which is which cannot be told apart.\n"
                  "Wake the device and retry, or name them: "
                  "--himax COMx --nrf COMy", file=sys.stderr)
            return 1

    for port, (role, baud, how) in assignment.items():
        print(f"  {port} -> {role:<5} @ {baud} ({how})", file=sys.stderr)

    threads = [threading.Thread(target=read_serial, args=(p, b, r), daemon=True)
               for p, (r, b, _) in assignment.items()]
    if not args.no_app:
        threads.append(threading.Thread(target=read_logcat, daemon=True))
    for t in threads:
        t.start()

    started = time.time()
    deadline = started + args.duration if args.duration else None
    written = 0

    with open(args.output, "w", encoding="utf-8") as fh:
        header = args.note or "WW500 three-way bench capture"
        fh.write(f"# {header}\n")
        fh.write(f"# Started {time.strftime('%Y-%m-%d %H:%M:%S')}. "
                 f"Times are MM:SS.mmm from the start of the capture.\n")
        for port, (role, baud, how) in sorted(assignment.items(), key=lambda kv: kv[1][0]):
            fh.write(f"# {role:<5} = {port} @ {baud} ({how})\n")
        fh.write("#\n")
        fh.flush()

        print(f"\nLogging to {args.output}. Ctrl+C to stop.\n", file=sys.stderr)
        try:
            while not stop.is_set():
                if deadline and time.time() > deadline:
                    break
                try:
                    ts, label, text = lines.get(timeout=0.3)
                except queue.Empty:
                    continue
                delta = ts - started
                stamp = f"[{int(delta // 60):02d}:{delta % 60:06.3f}]"
                out = f"{stamp} {label:<5} | {text}"
                fh.write(out + "\n")
                fh.flush()
                written += 1
                # The file is the deliverable; the console is a convenience. An
                # encoding fault on the mirror must never cost us the capture.
                try:
                    print(out, flush=True)
                except (UnicodeEncodeError, OSError):
                    pass
        except KeyboardInterrupt:
            pass
        finally:
            stop.set()

    print(f"\nWrote {written} lines to {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
