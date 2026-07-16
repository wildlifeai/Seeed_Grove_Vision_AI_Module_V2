#!/usr/bin/env python3
"""Validate the 1280x960 hi-res capture: enable op32, reboot into the RAW
datapath, capture over the preview stream, verify dimensions, save frames.

Sequence (from a caught boot or awake device):
  setop 8 60000, setop 32 1  ->  setop 8 3000, reset  ->  catch boot,
  expect 'Hi-res capture active' banner, keep awake, preview 1,
  capture 3 6000  ->  decode frames (expect 1280x960), save first/last,
  then setop 32 0 + restore (device back to normal VGA on next boot).
"""
import argparse
import base64
import io
import json
import re
import sys
import time

import numpy as np
import serial
from PIL import Image

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except AttributeError:
    pass

ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
CHAR_DELAY = 0.03
MAX_LINE = 1024 * 1024   # hi-res frame lines are ~300-450 KB of base64
CLI_UP_RE = re.compile(r"Starting CLI Task|Enter 'help' to view")
BOOT_DONE_RE = re.compile(r"Image sensor and data path initialised|Hi-res capture active")


def note(s):
    print(s.rstrip(), flush=True)


def send_slow(ser, text):
    data = text.encode("ascii", "replace")
    for i in range(len(data)):
        ser.write(data[i:i + 1])
        ser.flush()
        time.sleep(CHAR_DELAY)


class Link:
    def __init__(self, ser):
        self.ser = ser
        self.port = ser.port
        self.baud = ser.baudrate
        self.buf = bytearray()

    def _reopen(self, deadline_s=120.0):
        try:
            self.ser.close()
        except Exception:
            pass
        end = time.monotonic() + deadline_s
        while time.monotonic() < end:
            try:
                self.ser = serial.Serial(self.port, self.baud, timeout=0.2)
                note("    (port reopened)")
                return True
            except serial.SerialException:
                time.sleep(0.1)
        return False

    def send(self, text):
        data = text.encode("ascii", "replace")
        for i in range(len(data)):
            try:
                self.ser.write(data[i:i + 1])
                self.ser.flush()
            except serial.SerialException:
                self._reopen()
                return
            time.sleep(CHAR_DELAY)

    def next_line(self, timeout_s):
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            nl = self.buf.find(b"\n")
            if nl >= 0:
                raw = bytes(self.buf[:nl])
                del self.buf[:nl + 1]
                return ANSI_RE.sub("", raw.decode("utf-8", "replace")).strip("\r\n \t")
            try:
                chunk = self.ser.read(16384)
            except serial.SerialException:
                # the port re-enumerates when the device resets/power-cycles
                self._reopen()
                chunk = b""
            if chunk:
                self.buf.extend(chunk)
                if len(self.buf) > MAX_LINE:
                    self.buf.clear()
        return None

    def next_frame(self, timeout_s):
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            line = self.next_line(2.0)
            if line is None:
                continue
            if '"name": "INVOKE"' in line:
                start = line.find('{"type"')
                try:
                    msg = json.loads(line[start:])
                    jpeg = base64.b64decode(msg["data"]["image"], validate=True)
                    return msg["data"].get("count", 0), msg["data"].get("resolution"), jpeg
                except (ValueError, KeyError):
                    continue
            elif re.search(r"Hi-res|AE:|WB auto|timed out|About to capture|giving up|"
                           r"Entering DPD|Starting CLI|initialised|Preview mode", line):
                note(f"    {line}")
        return None

    def cmd_expect(self, cmd, expect, attempts=6, cycle_s=3.0):
        for _ in range(attempts):
            self.send(cmd + "\r\n")
            deadline = time.monotonic() + cycle_s
            while time.monotonic() < deadline:
                line = self.next_line(0.4)
                if line and '"INVOKE"' not in line:
                    note(f"    {line}")
                    if expect in line:
                        return True
        return False

    def catch_boot(self, wait_s, label):
        note(f"== waiting up to {wait_s/60:.0f} min for a boot ({label}) ==")
        deadline = time.monotonic() + wait_s
        stage = 0
        while time.monotonic() < deadline:
            line = self.next_line(1.0)
            if line is None:
                continue
            note(f"    {line}")
            if stage == 0 and CLI_UP_RE.search(line):
                if self.cmd_expect("setop 8 60000", "Set OpParam 8 = 60000",
                                   attempts=8, cycle_s=1.5):
                    stage = 1
            elif stage == 1 and BOOT_DONE_RE.search(line):
                time.sleep(3)
                return True
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--wait-min", type=float, default=60.0)
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    ser.reset_input_buffer()
    link = Link(ser)

    note("== probe: is the device awake? ==")
    if link.cmd_expect("setop 8 60000", "Set OpParam 8 = 60000", attempts=3, cycle_s=2.5):
        note("== device awake ==")
    else:
        if not link.catch_boot(args.wait_min * 60, "power-cycle the WW500"):
            note("!! never caught the device")
            return 2

    note("== enable hi-res (op32=1) ==")
    link.cmd_expect("setop 32 1", "Set OpParam 32 = 1")

    note("== POWER-CYCLE THE DEVICE NOW (cold boot into the RAW datapath - "
         "warm reboots lose the sensor bring-up lottery) ==")
    if not link.catch_boot(args.wait_min * 60, "cold boot into hi-res"):
        return 3

    note("== preview + hi-res capture burst ==")
    # Re-assert the keep-awake AFTER the boot finished: the one sent at
    # CLI-up precedes the config load, which restores the persisted op8
    # (1000 ms in the field config) and puts the device to sleep mid-burst
    link.cmd_expect("setop 8 60000", "Set OpParam 8 = 60000")
    link.cmd_expect("preview 1", "Preview mode 1")
    # 12s interval: a hi-res INVOKE line is ~440KB of base64 (~5s at this
    # baud); wide spacing keeps async console prints out of the send window
    link.send("capture 6 12000\r\n")

    frames = 0
    ok_dims = False
    deadline = time.monotonic() + 300
    while time.monotonic() < deadline and frames < 3:
        fr = link.next_frame(90)
        if fr is None:
            note("  (no frame within window)")
            break
        count, res, jpeg = fr
        try:
            img = Image.open(io.BytesIO(jpeg))
            img.load()
        except OSError as e:
            note(f"  frame #{count}: JPEG decode FAILED ({e})")
            continue
        frames += 1
        a = np.asarray(img.convert("RGB"), dtype=np.float64)
        luma = (0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2])
        note(f">> frame #{count}: {img.size[0]}x{img.size[1]}, {len(jpeg)} bytes, "
             f"luma={luma.mean():.1f} p5={np.percentile(luma,5):.1f} p95={np.percentile(luma,95):.1f}")
        if img.size in ((1280, 960), (1280, 928), (1248, 960), (1216, 960)):
            ok_dims = True
        name = f"hires_frame_{frames}.jpg"
        with open(name, "wb") as f:
            f.write(jpeg)
        note(f"   saved {name}")

    verdict = "PASS" if (frames > 0 and ok_dims) else "FAIL"
    if verdict == "PASS":
        note("== restore normal state (op32=0, op8=1000) ==")
        link.cmd_expect("setop 32 0", "Set OpParam 32 = 0")
        link.cmd_expect("setop 8 1000", "Set OpParam 8 = 1000")
    else:
        note("== leaving op32=1 for the next attempt ==")
    note(f"== HIRES CHECK: {verdict} ({frames} frames) ==")
    ser.close()
    return 0 if verdict == "PASS" else 5


if __name__ == "__main__":
    sys.exit(main())
