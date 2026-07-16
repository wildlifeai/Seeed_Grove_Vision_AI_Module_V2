#!/usr/bin/env python3
"""Capture ONE hi-res frame and pull the ENTIRE raw buffer to the PC
(full_raw.bin, 1,228,800 bytes) for local algorithm iteration.
Restores op32=0 / op8=1000 afterwards."""
import base64
import re
import sys
import time

import serial

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except AttributeError:
    pass

ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
CLI_UP_RE = re.compile(r"Starting CLI Task|Enter 'help' to view")
CHAR_DELAY = 0.03
MAX_LINE = 1024 * 1024

ser = serial.Serial("COM13", 921600, timeout=0.2)
buf = bytearray()


def note(s):
    print(s, flush=True)


def _reopen():
    # The bench USB can re-enumerate whenever the device resets/power-cycles
    global ser
    try:
        ser.close()
    except Exception:
        pass
    while True:
        try:
            ser = serial.Serial("COM13", 921600, timeout=0.2)
            print("    (port reopened)", flush=True)
            return
        except serial.SerialException:
            time.sleep(0.1)


def next_line(t):
    end = time.monotonic() + t
    while time.monotonic() < end:
        nl = buf.find(b"\n")
        if nl >= 0:
            raw = bytes(buf[:nl]); del buf[:nl + 1]
            return ANSI_RE.sub("", raw.decode("utf-8", "replace")).strip()
        try:
            c = ser.read(16384)
        except serial.SerialException:
            _reopen()
            c = b""
        if c:
            buf.extend(c)
            if len(buf) > MAX_LINE:
                buf.clear()
    return None


def send(cmd):
    for ch in cmd + "\r\n":
        try:
            ser.write(ch.encode()); ser.flush()
        except serial.SerialException:
            _reopen()
            return
        time.sleep(CHAR_DELAY)


def expect(cmd, pat, tries=6, window=2.5):
    rx = re.compile(pat)
    for _ in range(tries):
        send(cmd)
        end = time.monotonic() + window
        while time.monotonic() < end:
            line = next_line(0.3)
            if line and '"INVOKE"' not in line:
                note("    " + line)
                if rx.search(line):
                    return True
    return False


def catch_boot(label, want_hires):
    note("== waiting up to 45 min for a boot (%s) ==" % label)
    end = time.monotonic() + 2700
    stage = 0
    while time.monotonic() < end:
        line = next_line(1.0)
        if line is None:
            continue
        note("    " + line)
        if stage == 0 and CLI_UP_RE.search(line):
            if expect("setop 8 60000", r"Set OpParam 8 = 60000", tries=8, window=1.5):
                stage = 1
        elif stage == 1:
            if want_hires and "Hi-res capture active" in line:
                time.sleep(3)
                return True
            if not want_hires and "data path initialised" in line:
                time.sleep(2)
                return True
    return False


def rawdump(off_kb, len_kb):
    for attempt in range(3):
        send("rawdump %d %d" % (off_kb, len_kb))
        data = []
        started = False
        hdr = None
        end = time.monotonic() + 40
        while time.monotonic() < end:
            line = next_line(2.0)
            if line is None:
                continue
            if line.startswith("RAWDUMP END"):
                break
            m = re.match(r"RAWDUMP (\d+) (\d+)$", line)
            if m:
                started = True
                hdr = (int(m.group(1)), int(m.group(2)))
                continue
            if started and re.fullmatch(r"[A-Za-z0-9+/=]+", line):
                data.append(line)
        if data and hdr:
            try:
                blob = base64.b64decode("".join(data))
                if len(blob) == hdr[1]:
                    return blob
                note("    (chunk %d: got %d of %d - retry)" % (off_kb, len(blob), hdr[1]))
            except ValueError:
                note("    (chunk %d: b64 error - retry)" % off_kb)
    return None


def main():
    note("== probe: is the device awake? ==")
    awake = expect("setop 8 60000", r"Set OpParam 8 = 60000", tries=3, window=2.0)
    if not awake:
        if not catch_boot("POWER-CYCLE the WW500", want_hires=False):
            note("!! never caught the device")
            return 2

    expect("setop 32 1", r"Set OpParam 32 = 1")
    note("== POWER-CYCLE THE DEVICE NOW (cold boot into hi-res) ==")
    if not catch_boot("cold boot into hi-res", want_hires=True):
        note("!! never caught the hi-res boot")
        return 3

    # Re-assert the keep-awake: the one sent at CLI-up precedes the config
    # load, which restores the persisted op8 and can sleep the device
    # mid-dump (the 10-chunk pull takes ~90 s)
    expect("setop 8 60000", r"Set OpParam 8 = 60000")

    note("== capture one frame ==")
    send("capture 1 2000")
    end = time.monotonic() + 90
    got = False
    while time.monotonic() < end and not got:
        line = next_line(2.0)
        if line is None:
            continue
        note("    " + line)
        if re.search(r"Hi-res: \d+x\d+ JPEG", line):
            got = True
    if not got:
        note("!! no frame")
    time.sleep(2)

    note("== pulling the full raw buffer (1200 KB in 10 chunks) ==")
    blob = bytearray()
    ok = True
    for off in range(0, 1200, 120):
        c = rawdump(off, 120)
        if c is None:
            note("!! chunk at %dKB failed" % off)
            ok = False
            break
        blob.extend(c)
        note("    %d KB pulled" % (len(blob) // 1024))

    expect("setop 32 0", r"Set OpParam 32 = 0")
    expect("setop 8 1000", r"Set OpParam 8 = 1000")

    if ok and len(blob) == 1228800:
        with open("full_raw.bin", "wb") as f:
            f.write(blob)
        note("== FULL RAW SAVED: full_raw.bin (%d bytes) ==" % len(blob))
    else:
        note("== RAW DUMP INCOMPLETE (%d bytes) ==" % len(blob))
    ser.close()
    return 0 if ok else 5


if __name__ == "__main__":
    sys.exit(main())
