#!/usr/bin/env python3
"""Grab one normal-mode (640x480) frame -> compare_vga.jpg, restore op8."""
import base64
import io
import json
import re
import sys
import time

import serial
from PIL import Image

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


def main():
    note("== probe: is the device awake? ==")
    if not expect("setop 8 60000", r"Set OpParam 8 = 60000", tries=3, window=2.0):
        note("== waiting up to 45 min for a boot (POWER-CYCLE the WW500) ==")
        end = time.monotonic() + 2700
        stage = 0
        ok = False
        while time.monotonic() < end and not ok:
            line = next_line(1.0)
            if line is None:
                continue
            note("    " + line)
            if stage == 0 and CLI_UP_RE.search(line):
                if expect("setop 8 60000", r"Set OpParam 8 = 60000", tries=8, window=1.5):
                    stage = 1
            elif stage == 1 and "data path initialised" in line:
                time.sleep(3)
                ok = True
        if not ok:
            note("!! never caught the device")
            return 2

    expect("preview 1", r"Preview mode 1")
    send("capture 5 4000")

    end = time.monotonic() + 90
    got = None
    while time.monotonic() < end and got is None:
        line = next_line(2.0)
        if line is None:
            continue
        if '"name": "INVOKE"' in line:
            start = line.find('{"type"')
            try:
                msg = json.loads(line[start:])
                jpeg = base64.b64decode(msg["data"]["image"], validate=True)
                img = Image.open(io.BytesIO(jpeg))
                img.load()
            except (ValueError, KeyError, OSError) as e:
                note("    (frame skipped: %s)" % e)
                continue
            with open("compare_vga.jpg", "wb") as f:
                f.write(jpeg)
            md_blocks = msg["data"].get("md_blocks", "absent")
            md_hex = msg["data"].get("md", "")
            md_state = "empty" if md_hex == "" else ("all-zero" if set(md_hex) == {"0"} else "ACTIVE")
            note(">> saved compare_vga.jpg: %dx%d, %d bytes | md_blocks=%s md=%s" %
                 (img.size[0], img.size[1], len(jpeg), md_blocks, md_state))
            got = img.size
        elif re.search(r"AE:|WB auto|timed out|About to capture", line):
            note("    " + line)

    expect("setop 8 1000", r"Set OpParam 8 = 1000")
    note("== VGA GRAB: %s ==" % ("PASS" if got else "FAIL"))
    ser.close()
    return 0 if got else 5


if __name__ == "__main__":
    sys.exit(main())
