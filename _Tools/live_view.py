#!/usr/bin/env python3
"""Live camera preview + tuning console for the WW500.

Companion to the firmware 'preview' CLI command (branch feat/uart-live-preview).
The firmware streams each captured frame on the console UART as one JSON line:

  {"type": 1, "name": "INVOKE", "code": 0, "data": {"count": N,
   "resolution": [W, H], "boxes": [], "image": "<base64 JPEG>"}}

This tool displays the frames live and keeps the CLI usable at the same time,
so camera settings can be adjusted while watching the picture change, e.g.:

    setop 27 286               WB red gain (Q8.8, 256 = 1.0x)
    setop 28 326               WB blue gain
    vcm 512                    focus position (RP3 only)
    camreg w 0x0202 0x03e8     sensor register write (staged - see camreg help)

Two capture modes:
  Free run    - short capture bursts, re-armed each time a burst ends, so Stop
                takes effect within one burst (the firmware cannot cancel one).
  One image   - stops free run and takes a single frame.

It also handles the WW500's sleep habits by itself. On connect it probes the
device with 'ver' and arms. If the device is asleep it waits for a boot banner:
 - '### Warm Boot ###' (a wake from DPD): it types the keep-awake at once, then
   turns preview on and starts capturing - every step echo-verified with retries.
 - '### Cold Boot ###' (power-up, reset, slot switch): the firmware refuses
   captures until it has slept once ("Error: no captures until after the first
   sleep."), so the viewer lets it sleep. Something then has to wake it: motion
   (op11 > 0), the app, or a timelapse. With --wake N the viewer types
   'setop 7 N' at the cold boot, then 'dpd' once camera init is done, so the
   device wakes N s later. ('dpd' is needed because any
   character typed on the console keeps the device awake for 60 s.)

Usage:
    py live_view.py                    # defaults: COM14, 921600
    py live_view.py --port COM13
    py live_view.py --wake 10          # timelapse wake after a cold boot
    py live_view.py --save-dir D:/pics # where 'Save frame' writes (default
                                       # _Tools/live_frames, git-ignored)

NOTES
 - Close TeraTerm first: Windows COM ports are exclusive.
 - A print from another firmware task inside a frame line corrupts that frame.
   The firmware keeps the IF task quiet while a frame is sent
   (preview_isSending()); other dropped frames are counted as 'dropped'.
 - The console UART receive path has a 1-character buffer re-armed by the CLI
   task, so commands are sent one character at a time, preferably in the quiet
   gap right after a frame arrives.
 - An occasional frame is lost when another firmware print lands inside the
   frame line; it is counted as 'dropped' and skipped.
 - op8 (inactivity) is 16-bit: 60000 ms is the usable maximum. Streaming
   resets the timer continuously, so the device stays awake while you tune.
 - Closing the window leaves the device quiet for the lab, each step confirmed:
   preview 0, setop 7 0 (no timelapse), setop 11 0 (no motion detection),
   setop 8 1000 (short inactivity). These are saved to CONFIG.TXT, so they
   replace whatever the card had. Killing the console instead skips this.
"""

import argparse
import base64
import io
import json
import os
import queue
import re
import sys
import threading
import time
from collections import deque
from datetime import datetime

import serial
import tkinter as tk
from tkinter import scrolledtext
from PIL import Image, ImageDraw, ImageTk

ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

FRAME_MARKER = '"name": "INVOKE"'
MAX_LINE = 512 * 1024          # a VGA frame line is ~35-70 KB; anything huge is junk
CHAR_DELAY = 0.03              # per-character send delay (see module docstring)
# Free run uses short bursts: the firmware cannot cancel a burst, so Stop and
# One image wait for the current one to end (up to BURST_FRAMES frames).
BURST_FRAMES = 30             # 30 from 23 Oct 2026 (was 10): fewer gaps, slower Stop
# Interval between frames in a burst. Never 0: the firmware passes it to
# xTimerChangePeriod(), and FreeRTOS asserts on a zero period (timers.c), which
# resets the AI processor. That is the likely cause of the old note that
# 'capture N 0' wedges the IMX708. 1 ms is the minimum (1 tick).
INTERVAL_MIN_MS = 1
INTERVAL_FALLBACK_MS = 500     # until the camera type is known
INTERVAL_DEFAULT_MS = {"HM0360": 20, "RP3": 100, "RP2": 100}
SINGLE_CMD = "capture 1 500"
CAPTURE_OK = "About to capture"  # the CLI's reply to an accepted capture
REARM_QUIET_S = 20.0           # fallback re-arm if a burst-end line was missed
DISPLAY_MAX = (800, 600)
# "Save frame" default: beside this script, git-ignored (see .gitignore)
SAVE_DIR_DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "live_frames")
SEND_RETRY_S = 3.0             # resend an unconfirmed sequence step this often
SEND_TRIES = 8

CLI_UP_RE = re.compile(r"Starting CLI Task|Enter 'help' to view")
COLD_BOOT_RE = re.compile(r"### Cold Boot ###")
WARM_BOOT_RE = re.compile(r"### Warm Boot ###")
# Camera/datapath init is complete only here - sending a capture before this
# collides with the init and every frame times out until the next power-cycle.
BOOT_DONE_RE = re.compile(r"Image sensor and data path initialised")
BOOT_SETTLE_S = 4.0            # boot-done -> arm delay (lets the boot capture finish)
WARM_ARM_FALLBACK_S = 10.0     # arm anyway if boot-done is not seen after a warm boot
BURST_END_RE = re.compile(r"Current captures completed:|Capture failed after")
# The image task starting a burst, ours or its own (a motion or timelapse wake
# captures op5 images by itself)
BURST_START_RE = re.compile(r"Images to capture: \d+")
# MD settings: replies to 'getop 17', 'md N' and 'mdth [N]'
OP17_RE = re.compile(r"OpParam 17 = (\d+)")
MD_SENS_RE = re.compile(r"MD sensitivity (?:set to|saved as) (\d+)")
MDTH_RE = re.compile(r"MD threshold A = (\d+), B = (\d+)")

# Camera type: the reply to 'camera', or the end of the boot's camera init
CAMERA_RE = re.compile(r"^(?:cmd> )?(HM0360|RP3|RP2)$|initialised for camera (HM0360|RP3|RP2)")
# Firmware refuses captures between a cold boot and the first DPD (captureRefused()
# in CLI-commands.c)
REFUSED_RE = re.compile(r"no captures until after the first sleep")

KEEPAWAKE_STEP = ("setop 8 60000", "Set OpParam 8 = 60000")
# Every character typed on the console stretches the inactivity period to 60 s
# (INACTIVITYTIMEOUTCLI), whatever op8 says. So once the viewer has typed
# anything after a cold boot, it must send 'dpd' or the device stays awake 60 s.
DPD_STEP = ("dpd", "Forcing DPD")
PREVIEW_STEP = ("preview 1", "Preview mode 1")

# Each step is resent until its confirmation string appears. The keep-awake
# goes out earlier, at CLI-up (safe before the camera init).
SINGLE_SEQUENCE = [PREVIEW_STEP, (SINGLE_CMD, CAPTURE_OK)]

# HM0360 motion detection grid. Both contexts set ROI_H = 0xf0 (columns 0-15)
# and ROI_V = 0xe0 (rows 0-14), and the datasheet gives 16 x 15 cells for VGA,
# but on the bench (24 Oct 2026) motion shows in all 16 rows, so 16 is the
# default. The viewer can switch to 15 rows to compare.
MD_COLS = 16
MD_ROWS_DEFAULT = 16
GRID_COLOUR = (0, 170, 255)      # cell lines
MOTION_COLOUR = (255, 40, 40)    # outline of a cell whose MD bit is set


def cmd_text(cmd):
    """A queued command is a string, or a function that builds it when sent."""
    return cmd() if callable(cmd) else cmd


def md_cells(md, rows):
    """Decode the 32 ROI_OUT bytes into the cells that report motion.

    Bit b (0-255) is byte b // 8, bit b % 8 (LSB first), and belongs to MD
    cell row b // 16, column b % 16: the order hm0360_md_printGrid() uses for
    the console grid. On the bench (23 Oct 2026, HM0360 build) that order is
    rotated 180 degrees relative to the JPEG: the console grid's row 0,
    column 0 is the image's bottom-right cell, so it is rotated here. The
    sensor's own order is reversed the same way (MD application note,
    figure 4.1).

    Returns (cells, outside): the set of (row, col) with motion in image
    coordinates (row 0 at the top), and how many set bits fall in rows
    beyond `rows`.
    """
    cells = set()
    outside = 0
    for b in range(len(md) * 8):
        if md[b >> 3] & (1 << (b & 7)):
            row, col = divmod(b, MD_COLS)
            if row >= rows:
                outside += 1
                continue
            # rotate 180 degrees into image coordinates
            row = rows - 1 - row
            col = MD_COLS - 1 - col
            cells.add((row, col))
    return cells, outside


def md_status(md, mdstat):
    """Summarise one frame's MD interrupt status.

    md is the 32 ROI_OUT bytes (or None); mdstat is the frame's "mdstat" dict:
    INT_INDIC ("int"), MD_CTRL ("ctrl"), MD_CTRL1 ("ctrl1") and the context A
    and B MD_BLOCK_NUM_TH ("thA", "thB"). The sensor raises its motion flag
    when the number of cells with motion is greater than MD_BLOCK_NUM_TH (MD
    application note 3.1); preview frames are context A, so thA applies.
    INT_INDIC bit 3 is the MD interrupt, which wakes the device. MD_CTRL1 bit 1
    enables it, and bit 0 chooses its source: 0 the motion flag, 1 the flag
    after the latency logic (MD_CTRL bits 5:4 = latency mode).

    Returns a dict, or None if the frame had no "mdstat".
    """
    if not isinstance(mdstat, dict):
        return None
    try:
        intr = int(mdstat["int"])
        ctrl = int(mdstat["ctrl"])
        ctrl1 = int(mdstat["ctrl1"])
        th_a = int(mdstat["thA"])
        th_b = int(mdstat["thB"])
    except (KeyError, TypeError, ValueError):
        return None
    cells = sum(bin(b).count("1") for b in md) if md else 0
    return {
        "cells": cells,
        "thA": th_a,
        "thB": th_b,
        "flag": cells > th_a,
        "int": bool(intr & 0x08),
        "int_enabled": bool(ctrl1 & 0x02),
        "source": "latency" if ctrl1 & 0x01 else "flag",
        "latency_mode": (ctrl >> 4) & 0x03,
        "raw": (intr, ctrl, ctrl1),
    }


def draw_md_grid(img, md, rows):
    """Draw the MD grid over a copy of img: thin lines between cells, and a
    red outline on each cell with motion. md may be None (lines only).

    Returns (image, cells with motion, set bits beyond `rows`).
    """
    img = img.convert("RGB")
    draw = ImageDraw.Draw(img)
    w, h = img.size
    xs = [round(c * w / MD_COLS) for c in range(MD_COLS + 1)]
    ys = [round(r * h / rows) for r in range(rows + 1)]
    for x in xs[1:-1]:
        draw.line([(x, 0), (x, h - 1)], fill=GRID_COLOUR)
    for y in ys[1:-1]:
        draw.line([(0, y), (w - 1, y)], fill=GRID_COLOUR)
    cells, outside = md_cells(md or b"", rows)
    for row, col in cells:
        draw.rectangle([xs[col], ys[row], xs[col + 1] - 1, ys[row + 1] - 1],
                       outline=MOTION_COLOUR, width=2)
    return img, len(cells), outside


# Sent on close, each confirmed. The viewer is a lab tool, so it leaves the
# device quiet rather than restoring earlier values: no timelapse (op7), no
# motion detection (op11), the default short inactivity period (op8). All three
# are saved to CONFIG.TXT by the firmware.
CLOSE_SEQUENCE = [
    ("preview 0", "Preview off"),
    ("setop 7 0", "Set OpParam 7 = 0"),
    ("setop 11 0", "Set OpParam 11 = 0"),
    ("setop 8 1000", "Set OpParam 8 = 1000"),
]
CLOSE_TIMEOUT_S = 10.0         # give up confirming after this (device asleep?)

COLD_BOOT_HINT = ("[viewer] cold boot: the firmware refuses captures until it has slept once, "
                  "so letting it sleep. It needs something to wake it: run the viewer with "
                  "--wake 10 (timelapse wake 10 s after it sleeps), or wave at the lens if MD "
                  "is on (op11 > 0).")


class SerialWorker(threading.Thread):
    """Reads the console UART, splitting frames from ordinary console lines.

    Outgoing commands go through a verified-send queue: each (cmd, expect)
    step is typed one character at a time and resent until `expect` shows up
    in the console output. Steps with expect=None are fire-once (used for
    manual commands).

    The capture state lives here because the receive path drives it:
    free_run re-arms a burst each time one ends; single_pending takes one
    frame once the current burst ends; cold_boot means the firmware will
    refuse captures until it has slept.
    """

    def __init__(self, port, baud, frame_q, log_q, wake_s=0, interval_ms=None):
        super().__init__(daemon=True)
        self.frame_q = frame_q
        self.log_q = log_q
        self.wake_s = wake_s        # timelapse wake (op7) to set at a cold boot, 0 = off
        self.wake_set = False       # op7 was set by us, so clear it on close
        self.camera = None          # "HM0360", "RP3", ... once reported
        self.interval_fixed = interval_ms is not None   # --interval given
        self.interval_ms = interval_ms if interval_ms is not None else INTERVAL_FALLBACK_MS
        self.seq = deque()          # entries: [cmd, expect, tries_left]
        self.last_md_key = None     # wake prediction of the last frame, for _log_md_change()
        self.md_sensitivity = None  # op 17 as last reported by the device
        self.md_sensitivity_start = None    # ... as first reported: put back on close
        self.md_threshold = None    # context A MD_BLOCK_NUM_TH, from 'mdth' or a frame
        self.md_frames = 0          # frames with MD data since the counts were reset
        self.wake_frames = 0        # ... of which would have woken the device
        self.seq_last_send = 0.0
        self.dropped = 0
        self.last_frame_ts = 0.0
        self.last_cli_up = 0.0
        self.arm_at = None          # deferred arm time after boot-done
        self.sleep_at = None        # deferred 'dpd' after a cold boot (--wake)
        self.autoarm_enabled = True
        self.boot_kind = None       # "cold" or "warm", from the last boot banner
        self.cold_boot = False      # captures refused until the device has slept
        self.capture_busy = False   # a capture burst is running on the device
        self.busy_ts = 0.0          # when the last capture was accepted
        self.free_run = True        # auto-armed on connect
        self.single_pending = False
        self.running = True
        self.ser = serial.Serial(port, baud, timeout=0.05)

    # ---- command sending -------------------------------------------------

    def send(self, cmd):
        """Fire-once manual command (no confirmation)."""
        self.seq.append([cmd.strip(), None, 1])

    def enqueue_verified(self, steps):
        for cmd, expect in steps:
            self.seq.append([cmd, expect, SEND_TRIES])

    def _capture_queued(self):
        return any(step[1] == CAPTURE_OK for step in self.seq)

    def arm(self, reason, single=False, with_preview=True):
        """Queue a capture: a free-run burst, or one frame if single.
        with_preview=False skips 'preview 1' (re-arming between bursts)."""
        if self.cold_boot:
            self.log_q.put(f"[viewer] {reason}: not sent, the device is in its cold-boot "
                           f"period (captures refused until it has slept and woken)")
            return
        if self._capture_queued():
            return  # a capture is already on its way
        if (self.capture_busy and time.monotonic()
                - max(self.last_frame_ts, self.busy_ts) > REARM_QUIET_S):
            self.capture_busy = False  # nothing for a while: the burst is over
        if self.capture_busy:
            if single:
                self.single_pending = True
                self.log_q.put("[viewer] one image will be taken when the current burst ends")
            return
        self.log_q.put(f"[viewer] {reason} - " + ("one image" if single else "arming preview stream"))
        # self.capture_cmd, not its result: the interval is read when the
        # command is typed, so a 'camera' reply that arrives meanwhile counts
        steps = SINGLE_SEQUENCE if single else [PREVIEW_STEP, (self.capture_cmd, CAPTURE_OK)]
        if not with_preview:
            steps = steps[1:]
        self.enqueue_verified(steps)

    def capture_cmd(self):
        return f"capture {BURST_FRAMES} {max(INTERVAL_MIN_MS, int(self.interval_ms))}"

    def _burst_ended(self):
        self.capture_busy = False
        if self.single_pending:
            self.single_pending = False
            self.arm("burst finished", single=True, with_preview=False)
        elif self.free_run:
            self.arm("burst finished", with_preview=False)
        # The device is idle between bursts, so type the re-arm now rather
        # than waiting out the 3 s quiet rule (that wait was the pause after every
        # BURST_FRAMES frames)
        self._pump_seq(force=True)

    def _write_now(self, cmd):
        self.log_q.put(f"> {cmd}")
        data = cmd.encode("ascii", "replace") + b"\r\n"
        for i in range(len(data)):
            self.ser.write(data[i:i + 1])
            self.ser.flush()
            time.sleep(CHAR_DELAY)

    def _pump_seq(self, force=False):
        """Send/resend the head of the queue. Called from the reader loop
        (when the port is quiet) and right after each frame (the safe gap)."""
        if not self.seq:
            return
        now = time.monotonic()
        if not force and (now - self.seq_last_send) < SEND_RETRY_S:
            return
        cmd, expect, tries = self.seq[0]
        cmd = cmd_text(cmd)
        if expect is None:
            self.seq.popleft()
            self._write_now(cmd)
            self.seq_last_send = time.monotonic()
            return
        if tries <= 0:
            self.log_q.put(f"[viewer] no confirmation for '{cmd}' - giving up "
                           f"(the viewer re-arms when the device next wakes)")
            self.seq.popleft()
            return
        self.seq[0][2] = tries - 1
        self._write_now(cmd)
        self.seq_last_send = time.monotonic()

    def _confirm(self, line):
        if self.seq:
            cmd, expect, _tries = self.seq[0]
            if expect and expect in line:
                self.log_q.put(f"[viewer] confirmed: {cmd_text(cmd)}")
                self.seq.popleft()
                self.seq_last_send = 0.0  # next step goes out immediately

    # ---- receive path ----------------------------------------------------

    def _log_md_change(self, count, status):
        """Count the frames that would wake the device, and log each frame
        where that changes, so no change is lost when the display skips
        frames. The raw registers are logged for reference."""
        if status is None:
            return
        self.md_frames += 1
        if status["flag"]:
            self.wake_frames += 1
        key = status["flag"]
        if key != self.last_md_key:
            self.log_q.put(f"[md] frame #{count}: {status['cells']} cells, "
                           f"threshold {status['thA']} -> {'WAKE' if key else 'no wake'} "
                           f"(for reference: interrupt {'on' if status['int_enabled'] else 'off'}, "
                           f"source {status['source']}; INT_INDIC 0x{status['raw'][0]:02x}, "
                           f"MD_CTRL 0x{status['raw'][1]:02x}, MD_CTRL1 0x{status['raw'][2]:02x})")
        self.last_md_key = key

    def _handle_line(self, raw):
        line = ANSI_RE.sub("", raw.decode("utf-8", "replace")).strip("\r\n \t")
        if not line:
            return
        if FRAME_MARKER in line:
            start = line.find('{"type"')
            if start >= 0:
                try:
                    msg = json.loads(line[start:])
                    data = msg["data"]
                    jpeg = base64.b64decode(data["image"], validate=True)
                    self.last_frame_ts = time.monotonic()
                    self.capture_busy = True
                    count = data.get("count", 0)
                    if count <= 3 or count % 20 == 0:
                        self.log_q.put(f"[frame] #{count} {len(jpeg)} bytes")
                    md = data.get("md")     # 64 hex chars, absent without an HM0360
                    try:
                        md = bytes.fromhex(md) if isinstance(md, str) and len(md) == 64 else None
                    except ValueError:
                        md = None
                    status = md_status(md, data.get("mdstat"))
                    if status is not None:
                        self.md_threshold = status["thA"]
                    self._log_md_change(count, status)
                    self.frame_q.put((data.get("count", 0),
                                      data.get("resolution", [0, 0]), jpeg, md, status))
                    # frame boundary = the quiet window: send any pending step
                    self._pump_seq(force=True)
                    return
                except (ValueError, KeyError, TypeError):
                    self.dropped += 1
                    self.log_q.put(f"[viewer] dropped corrupt frame line "
                                   f"({len(line)} chars, total dropped {self.dropped})")
                    return
        self._confirm(line)
        now = time.monotonic()
        m = OP17_RE.search(line) or MD_SENS_RE.search(line)
        if m:
            self.md_sensitivity = int(m.group(1))
            if self.md_sensitivity_start is None and OP17_RE.search(line):
                self.md_sensitivity_start = self.md_sensitivity
        m = MDTH_RE.search(line)
        if m:
            self.md_threshold = int(m.group(1))
        m = CAMERA_RE.search(line)
        if m:
            camera = m.group(1) or m.group(2)
            if camera != self.camera:
                self.camera = camera
                if not self.interval_fixed:
                    self.interval_ms = INTERVAL_DEFAULT_MS.get(camera, INTERVAL_FALLBACK_MS)
                self.log_q.put(f"[viewer] camera {camera}: frame interval {self.interval_ms} ms")
        if CAPTURE_OK in line or BURST_START_RE.search(line):
            # A capture is running, so the cold-boot period is evidently over
            self.capture_busy = True
            self.busy_ts = now
            self.cold_boot = False
        if BURST_END_RE.search(line):
            self._burst_ended()
        if REFUSED_RE.search(line):
            # Cold-boot period (e.g. connected to a device that had just been
            # power-cycled): stop resending, the device must sleep first
            self.cold_boot = True
            self.seq = deque(s for s in self.seq if s[1] != CAPTURE_OK)
            self.log_q.put(line)
            if not self.wake_s:
                self.log_q.put(COLD_BOOT_HINT)
            # We have typed at it, so it would stay awake 60 s: send it to sleep
            self._sleep_now()
            return
        if COLD_BOOT_RE.search(line) or WARM_BOOT_RE.search(line):
            self.boot_kind = "cold" if COLD_BOOT_RE.search(line) else "warm"
            self.cold_boot = (self.boot_kind == "cold")
            self.capture_busy = False
            self.single_pending = False
            self.sleep_at = None
        if (self.autoarm_enabled and CLI_UP_RE.search(line)
                and now - self.last_frame_ts > 10 and now - self.last_cli_up > 10):
            # Fresh boot: drop any stale queued steps
            self.last_cli_up = now
            self.arm_at = None
            self.seq.clear()
            if self.cold_boot:
                # No keep-awake and no capture: the device has to sleep before
                # it will capture. Without --wake, type nothing so it sleeps
                # after its own short idle. With --wake, set the timelapse now
                # (it is certainly awake) and send 'dpd' once the cold boot's
                # camera init is done, or after a fallback delay.
                if self.wake_s:
                    self.log_q.put(f"[viewer] cold boot: captures refused until the device "
                                   f"has slept, so setting a {self.wake_s} s timelapse wake "
                                   f"(op7) and sending it to sleep after camera init")
                    self._set_wake()
                    self.sleep_at = now + WARM_ARM_FALLBACK_S
                else:
                    self.log_q.put(COLD_BOOT_HINT)
            else:
                # Woken from DPD (or unknown): keep the device awake and turn
                # preview on right away (a RAM flag, cleared by every DPD), so
                # the wake's own burst is streamed instead of saved to SD.
                # The capture waits for the camera init (see BOOT_DONE_RE),
                # with a fallback if that line is missed.
                self.log_q.put("[viewer] wake detected - keeping device awake, preview on, "
                               "waiting for camera init before capturing")
                self.enqueue_verified([KEEPAWAKE_STEP, PREVIEW_STEP])
                self.arm_at = now + WARM_ARM_FALLBACK_S
        if self.autoarm_enabled and BOOT_DONE_RE.search(line):
            if not self.cold_boot:
                self.arm_at = now + BOOT_SETTLE_S
                self.log_q.put(f"[viewer] camera init done - arming in {BOOT_SETTLE_S:.0f} s")
            elif self.sleep_at is not None:
                self.sleep_at = now + BOOT_SETTLE_S
                self.log_q.put(f"[viewer] camera init done - sending to sleep in "
                               f"{BOOT_SETTLE_S:.0f} s")
        self.log_q.put(line)

    def _set_wake(self):
        """Queue the --wake timelapse (op7), once per session."""
        if self.wake_s and not self.wake_set:
            self.enqueue_verified([(f"setop 7 {self.wake_s}",
                                    f"Set OpParam 7 = {self.wake_s}")])
            self.wake_set = True

    def _sleep_now(self):
        """Cold-boot period: set the --wake timelapse if asked, then 'dpd'."""
        self.sleep_at = None
        self._set_wake()
        self.log_q.put("[viewer] sending the device to sleep (dpd)" +
                       (f", it wakes in {self.wake_s} s" if self.wake_s else ""))
        self.enqueue_verified([DPD_STEP])

    def run(self):
        # Probe: if the device is already awake this confirms and arms;
        # if it is asleep the probe goes nowhere and the boot catcher arms.
        self.enqueue_verified([("ver", "WW500"), KEEPAWAKE_STEP])
        self.send("camera")         # sets the default frame interval (CAMERA_RE)
        self.send("getop 17")       # MD sensitivity, to show and to put back on close
        self.arm("connected")

        buf = bytearray()
        while self.running:
            try:
                chunk = self.ser.read(4096)
            except serial.SerialException as e:
                self.log_q.put(f"[viewer] serial error: {e}")
                break
            if chunk:
                buf.extend(chunk)
                while True:
                    nl = buf.find(b"\n")
                    if nl < 0:
                        break
                    line = bytes(buf[:nl])
                    del buf[:nl + 1]
                    self._handle_line(line)
                if len(buf) > MAX_LINE:
                    buf.clear()
                    self.dropped += 1
            # deferred 'dpd' once a cold boot has settled
            if self.sleep_at is not None and time.monotonic() >= self.sleep_at:
                self._sleep_now()
            # deferred arm once the boot has settled
            if self.arm_at is not None and time.monotonic() >= self.arm_at:
                self.arm_at = None
                if self.free_run or self.single_pending:
                    single = self.single_pending
                    self.single_pending = False
                    self.arm("boot settled", single=single)
            # device quiet (asleep, booting, or between frames): safe to send
            if time.monotonic() - self.last_frame_ts > 3.0:
                self._pump_seq()
        # The port is closed by the owner (Viewer.close), which may still
        # need to send the closing commands

    def stop(self):
        self.running = False


class Viewer:
    def __init__(self, root, worker, save_dir=SAVE_DIR_DEFAULT):
        self.root = root
        self.worker = worker
        self.save_dir = save_dir
        self.frame_q = worker.frame_q
        self.log_q = worker.log_q
        self.closing_deadline = None   # set by close()
        self.last_jpeg = None
        self.last_img = None           # decoded frame, before the MD overlay
        self.last_md = None            # its 32 ROI_OUT bytes, or None
        self.last_md_status = None     # its md_status() summary, or None
        self.md_text = "-"
        self.md_row16_warned = False
        self.last_rearm = time.monotonic()
        self.frame_times = deque(maxlen=10)
        self.photo = None  # keep a reference or tkinter drops the image

        root.title(f"WW500 Live View - {worker.ser.port} @ {worker.ser.baudrate}")

        main = tk.Frame(root)
        main.pack(fill=tk.BOTH, expand=True)

        self.image_label = tk.Label(main,
                                    text="waiting for frames...\n\n"
                                         "if nothing appears in ~15 s the device is asleep:\n"
                                         "power-cycle it and this window will catch the boot",
                                    width=80, height=24, bg="#202020", fg="#a0a0a0")
        self.image_label.grid(row=0, column=0, padx=4, pady=4, sticky="nsew")

        side = tk.Frame(main)
        side.grid(row=0, column=1, padx=4, pady=4, sticky="ns")

        self.status = tk.Label(side, justify=tk.LEFT, anchor="w", font=("Consolas", 10))
        self.status.pack(fill=tk.X, pady=(0, 4))

        # Would this frame wake the device? Red when the cells with motion
        # exceed MD_BLOCK_NUM_TH, the sensor's motion flag, which drives the
        # MD interrupt (MD_CTRL1 bit 0 = 0)
        self.wake_lamp = tk.Label(side, text="WAKE", relief=tk.SUNKEN,
                                  font=("Consolas", 12, "bold"))
        self.wake_lamp.pack(fill=tk.X, pady=(0, 8))
        self.lamp_off_bg = self.wake_lamp.cget("bg")
        self._set_status(None)

        tk.Button(side, text="Free run", command=self.start_stream).pack(fill=tk.X)
        tk.Button(side, text="Stop", command=self.stop_stream).pack(fill=tk.X, pady=(4, 0))
        tk.Button(side, text="One image", command=self.one_image).pack(fill=tk.X, pady=(4, 0))
        tk.Button(side, text="Save frame", command=self.save_frame).pack(fill=tk.X, pady=(4, 8))

        # MD grid overlay (HM0360 motion bits sent with each frame)
        self.show_md = tk.BooleanVar(value=True)
        self.md_rows = tk.IntVar(value=MD_ROWS_DEFAULT)
        tk.Label(side, text="MD grid:").pack(anchor="w")
        md_box = tk.Frame(side)
        md_box.pack(fill=tk.X, pady=(0, 12))
        tk.Checkbutton(md_box, text="Show", variable=self.show_md,
                       command=self._render).grid(row=0, column=0, sticky="w")
        tk.Radiobutton(md_box, text="15 rows", variable=self.md_rows, value=15,
                       command=self._render).grid(row=0, column=1, sticky="w")
        tk.Radiobutton(md_box, text="16 rows", variable=self.md_rows, value=16,
                       command=self._render).grid(row=0, column=2, sticky="w")

        # MD controls. Sensitivity is the firmware's 'md' command (op 17: saved
        # to CONFIG.TXT at the next sleep, so the close puts back the value
        # found at connect). The wake threshold is 'mdth', which is not saved.
        tk.Label(md_box, text="Sensitivity:").grid(row=1, column=0, sticky="w")
        self.md_sens = tk.IntVar(value=0)       # 0 = not known yet, or off
        for col, (text, value) in enumerate((("Low", 1), ("Med", 2), ("High", 3)), start=1):
            tk.Radiobutton(md_box, text=text, variable=self.md_sens, value=value,
                           command=self._sensitivity_changed).grid(row=2, column=col - 1, sticky="w")
        tk.Label(md_box, text="Wake threshold (cells):").grid(row=3, column=0, columnspan=2, sticky="w")
        self.md_th_var = tk.StringVar(value="")
        self.md_th_shown = None
        th_spin = tk.Spinbox(md_box, from_=1, to=255, width=5, textvariable=self.md_th_var,
                             command=self._threshold_changed)
        th_spin.grid(row=3, column=2, sticky="w")
        th_spin.bind("<Return>", self._threshold_changed)
        self.md_th_spin = th_spin

        # Frame interval for free run: set from the camera type, or by hand.
        # Takes effect from the next burst.
        tk.Label(side, text="Interval (ms):").pack(anchor="w")
        self.interval_var = tk.StringVar(value=str(worker.interval_ms))
        self.interval_shown = worker.interval_ms
        spin = tk.Spinbox(side, from_=INTERVAL_MIN_MS, to=5000, increment=10, width=8,
                          textvariable=self.interval_var, command=self._interval_changed)
        spin.pack(anchor="w", pady=(0, 12))
        spin.bind("<Return>", self._interval_changed)
        spin.bind("<FocusOut>", self._interval_changed)

        # Third column: commands and the log file, so the side column stays
        # short and the log pane keeps its height
        cmds = tk.Frame(main)
        cmds.grid(row=0, column=2, padx=4, pady=4, sticky="ns")

        tk.Label(cmds, text="Command:").pack(anchor="w")
        self.cmd_entry = tk.Entry(cmds, width=28, font=("Consolas", 10))
        self.cmd_entry.pack(fill=tk.X)
        self.cmd_entry.bind("<Return>", self.send_command)
        tk.Button(cmds, text="Send", command=self.send_command).pack(fill=tk.X, pady=(4, 0))

        tk.Label(cmds, text="Quick commands:").pack(anchor="w", pady=(12, 0))
        for cmd in ("getop 27", "setop 27 286", "setop 28 326",
                    "vcm 512", "status", "setop 8 1000"):
            tk.Button(cmds, text=cmd, font=("Consolas", 9),
                      command=lambda c=cmd: self.worker.send(c)).pack(fill=tk.X, pady=1)

        # Log file: everything shown in the log pane, also written to save_dir
        self.log_file = None
        self.log_path = None
        self.log_to_file = tk.BooleanVar(value=True)
        tk.Checkbutton(cmds, text="Log to file", variable=self.log_to_file,
                       command=self._log_file_changed).pack(anchor="w", pady=(12, 0))
        self.log_path_label = tk.Label(cmds, text="", font=("Consolas", 8),
                                       justify=tk.LEFT, anchor="w", wraplength=200)
        self.log_path_label.pack(fill=tk.X)

        # Packed at the bottom before the main frame, so it keeps its height
        # however tall the image and controls are
        self.log = scrolledtext.ScrolledText(root, height=12, font=("Consolas", 9),
                                             state=tk.DISABLED, wrap=tk.NONE)
        self.log.pack(side=tk.BOTTOM, fill=tk.BOTH, expand=False, padx=4, pady=(0, 4),
                      before=main)
        self._log_file_changed()

        main.grid_columnconfigure(0, weight=1)
        main.grid_rowconfigure(0, weight=1)

        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(30, self.poll)

    def _mode_text(self):
        w = self.worker
        if w.cold_boot:
            return "cold boot"
        if w.single_pending:
            return "one image"
        if w.free_run:
            return "free run"
        return "busy" if w.capture_busy else "stopped"

    def _md_status_text(self):
        st = self.last_md_status
        w = self.worker
        wakes = f"wakes     {w.wake_frames} of {w.md_frames} frames"
        if st is None:
            return f"threshold -\nwake      -\n{wakes}"
        text = (f"threshold {st['thA']} (B {st['thB']})\n"
                f"wake      {'YES' if st['flag'] else 'no'}  ({st['cells']} > {st['thA']}?)\n"
                f"{wakes}")
        if st["source"] == "latency":
            # the prediction would need to model the latency logic
            text += f"\nNOTE      interrupt follows latency mode {st['latency_mode']}"
        return text

    def _set_lamps(self):
        st = self.last_md_status
        on = st is not None and st["flag"]
        self.wake_lamp.config(bg="#ff3030" if on else self.lamp_off_bg)

    def _set_status(self, info):
        if info is not None:
            self.last_info = info
        info = getattr(self, "last_info", None)
        mode = self._mode_text()
        if info is None:
            self.status.config(text=(f"mode      {mode}\nframes    -\nsize      -\n"
                                     f"fps       -\ndropped   {self.worker.dropped}\n"
                                     f"motion    -\n{self._md_status_text()}"))
            return
        count, res, kbytes, fps = info
        self.status.config(text=(f"mode      {mode}\n"
                                 f"frames    {count}\n"
                                 f"size      {kbytes:.1f} KB  {res[0]}x{res[1]}\n"
                                 f"fps       {fps:.2f}\n"
                                 f"dropped   {self.worker.dropped}\n"
                                 f"motion    {self.md_text}\n"
                                 f"{self._md_status_text()}"))

    def start_stream(self):
        self.worker.md_frames = 0      # wake counts are per free run
        self.worker.wake_frames = 0
        self.worker.free_run = True
        self.worker.single_pending = False
        self.last_rearm = time.monotonic()
        self.worker.arm("free run requested")

    def stop_stream(self):
        # The firmware cannot cancel a burst: stop re-arming and let the
        # current one (up to BURST_FRAMES) finish. Preview stays on so those
        # frames are streamed, not saved to SD.
        self.worker.free_run = False
        self.worker.single_pending = False
        if self.worker.capture_busy:
            self._append_log(f"[viewer] stopping after the current burst "
                             f"(up to {BURST_FRAMES} frames)")

    def _interval_changed(self, _event=None):
        try:
            ms = max(INTERVAL_MIN_MS, int(self.interval_var.get()))
        except ValueError:
            ms = self.worker.interval_ms
        self.interval_var.set(str(ms))
        if ms != self.worker.interval_ms:
            self.worker.interval_ms = ms
            self.worker.interval_fixed = True   # a camera reply no longer overrides it
            self._append_log(f"[viewer] frame interval {ms} ms (from the next burst)")
        self.interval_shown = ms

    def _sensitivity_changed(self):
        n = self.md_sens.get()
        if n:
            self.worker.send(f"md {n}")
            self.worker.md_sensitivity = n   # until the reply (or a later getop) says otherwise

    def _threshold_changed(self, _event=None):
        try:
            n = int(self.md_th_var.get())
        except ValueError:
            return
        if 1 <= n <= 255:
            self.worker.send(f"mdth {n}")
            self.md_th_shown = n
            self.worker.md_threshold = n     # until the reply or the next frame says otherwise

    def _sync_md_controls(self):
        """Show the sensitivity and threshold the device reports, unless the
        user is typing a threshold."""
        w = self.worker
        if w.md_sensitivity is not None and w.md_sensitivity != self.md_sens.get():
            self.md_sens.set(w.md_sensitivity)    # 0 (off) selects none
        if (w.md_threshold is not None and w.md_threshold != self.md_th_shown
                and self.root.focus_get() is not self.md_th_spin):
            self.md_th_shown = w.md_threshold
            self.md_th_var.set(str(w.md_threshold))

    def one_image(self):
        self.worker.free_run = False
        self.last_rearm = time.monotonic()
        self.worker.arm("one image requested", single=True)

    def send_command(self, _event=None):
        cmd = self.cmd_entry.get().strip()
        if cmd:
            self.worker.send(cmd)
            self.cmd_entry.delete(0, tk.END)

    def save_frame(self):
        if not self.last_jpeg:
            self._append_log("[viewer] no frame to save yet")
            return
        name = datetime.now().strftime("live_%Y%m%d_%H%M%S.jpg")
        try:
            os.makedirs(self.save_dir, exist_ok=True)
            path = os.path.join(self.save_dir, name)
            with open(path, "wb") as f:
                f.write(self.last_jpeg)
        except OSError as e:
            self._append_log(f"[viewer] could not save frame: {e}")
            return
        self._append_log(f"[viewer] saved {path} ({len(self.last_jpeg)} bytes)")

    def _log_file_changed(self):
        """Open a new log file in save_dir when 'Log to file' is ticked, close
        it when unticked."""
        if self.log_to_file.get() and self.log_file is None:
            name = datetime.now().strftime("live_log_%Y%m%d_%H%M%S.txt")
            try:
                os.makedirs(self.save_dir, exist_ok=True)
                self.log_path = os.path.join(self.save_dir, name)
                self.log_file = open(self.log_path, "a", encoding="utf-8")
            except OSError as e:
                self.log_file = None
                self.log_to_file.set(False)
                self._append_log(f"[viewer] could not open a log file: {e}")
                return
            self.log_path_label.config(text=self.log_path)
            self._append_log(f"[viewer] logging to {self.log_path}")
        elif not self.log_to_file.get() and self.log_file is not None:
            self._append_log("[viewer] log file closed")
            self.log_file.close()
            self.log_file = None
            self.log_path_label.config(text="")

    def _append_log(self, line):
        print(line, flush=True)  # tee to stdout so a supervising session can watch
        if self.log_file is not None:
            try:
                self.log_file.write(datetime.now().strftime("%H:%M:%S.%f")[:-3] + " " + line + "\n")
                self.log_file.flush()
            except OSError:
                self.log_file = None
        self.log.config(state=tk.NORMAL)
        self.log.insert(tk.END, line + "\n")
        # keep the pane bounded
        if float(self.log.index(tk.END)) > 2000:
            self.log.delete("1.0", "500.0")
        self.log.see(tk.END)
        self.log.config(state=tk.DISABLED)

    def _show_frame(self, count, res, jpeg, md=None, md_status=None):
        self.last_jpeg = jpeg
        now = time.monotonic()
        self.frame_times.append(now)
        fps = 0.0
        if len(self.frame_times) >= 2:
            span = self.frame_times[-1] - self.frame_times[0]
            if span > 0:
                fps = (len(self.frame_times) - 1) / span
        try:
            img = Image.open(io.BytesIO(jpeg))
            img.load()
        except OSError as e:
            self.worker.dropped += 1
            self._append_log(f"[viewer] undecodable JPEG ({e})")
            return
        self.last_img = img
        self.last_md = md
        self.last_md_status = md_status
        self._render()
        self._set_lamps()
        self._set_status((count, res, len(jpeg) / 1024.0, fps))

    def _render(self):
        """Display the last frame, with the MD grid if enabled. Also called
        when an MD grid control changes, to redraw the same frame."""
        if self.last_img is None:
            return
        img = self.last_img
        if self.show_md.get():
            rows = self.md_rows.get()
            img, n, outside = draw_md_grid(img, self.last_md, rows)
            if self.last_md is None:
                self.md_text = "no data"
            else:
                self.md_text = f"{n} cells" + (f" (+{outside} beyond row {rows})" if outside else "")
                if outside and not self.md_row16_warned:
                    self.md_row16_warned = True
                    self._append_log(f"[viewer] MD bits set beyond row {rows}: "
                                     f"the grid may have 16 rows, try '16 rows'")
        else:
            self.md_text = "hidden"
        if img.width > DISPLAY_MAX[0] or img.height > DISPLAY_MAX[1]:
            img = img.copy()
            img.thumbnail(DISPLAY_MAX)
        self.photo = ImageTk.PhotoImage(img)
        self.image_label.config(image=self.photo, text="", width=img.width, height=img.height)

    def poll(self):
        try:
            while True:
                self._append_log(self.log_q.get_nowait())
        except queue.Empty:
            pass
        frame = None
        try:
            while True:  # drain to the newest frame; show only that one
                frame = self.frame_q.get_nowait()
        except queue.Empty:
            pass
        if frame:
            self._show_frame(*frame)

        # Fallback: bursts are normally re-armed on the burst-end line
        # (SerialWorker._burst_ended). If that line was missed, re-arm once
        # the stream has been quiet for a while.
        now = time.monotonic()
        w = self.worker
        if (w.free_run and not w.cold_boot and w.last_frame_ts > 0
                and now - w.last_frame_ts > REARM_QUIET_S
                and now - self.last_rearm > REARM_QUIET_S
                and not w.seq):
            self._append_log("[viewer] stream quiet - re-arming capture")
            w.capture_busy = False
            w.arm("stream quiet", with_preview=False)
            self.last_rearm = now

        # show an interval chosen by the worker (from the camera type)
        if w.interval_ms != self.interval_shown:
            self.interval_shown = w.interval_ms
            self.interval_var.set(str(w.interval_ms))

        self._sync_md_controls()
        self._set_status(None)
        self.root.after(30, self.poll)

    def close(self):
        """Leave the device quiet: preview off, then the lab-safe op values
        (CLOSE_SEQUENCE), each confirmed by the device. Each setop is saved to
        CONFIG.TXT, so without this a session's keep-awake and --wake
        timelapse would stay on the card."""
        if self.closing_deadline is not None:
            return  # already closing
        w = self.worker
        w.free_run = False
        w.single_pending = False
        w.autoarm_enabled = False   # a boot now must not clear the close queue
        w.arm_at = None
        w.sleep_at = None
        w.seq.clear()
        if (w.md_sensitivity_start is not None and w.md_sensitivity is not None
                and w.md_sensitivity != w.md_sensitivity_start):
            # 'md' changed op 17, which is saved at the next sleep: put it back
            n = w.md_sensitivity_start
            w.enqueue_verified([(f"md {n}", f"MD sensitivity set to {n}")])
        w.enqueue_verified(CLOSE_SEQUENCE)
        w.seq_last_send = 0.0       # first step goes out at once
        self._append_log("[viewer] closing: resetting the device to lab-safe values "
                         "(preview off, op7 0, op11 0, op8 1000)")
        self.closing_deadline = time.monotonic() + CLOSE_TIMEOUT_S
        self._finish_close()

    def _finish_close(self):
        w = self.worker
        # drain the log so the confirmations show while closing
        try:
            while True:
                self._append_log(self.log_q.get_nowait())
        except queue.Empty:
            pass
        if w.seq and time.monotonic() < self.closing_deadline and w.is_alive():
            self.root.after(100, self._finish_close)
            return
        if w.seq:
            left = ", ".join(cmd_text(step[0]) for step in w.seq)
            print(f"[viewer] WARNING: not confirmed, the device may be asleep: {left}. "
                  f"Type these at the console when it is awake.", flush=True)
        else:
            print("[viewer] device reset to lab-safe values", flush=True)
        if self.log_file is not None:
            self.log_file.close()
            self.log_file = None
        w.stop()
        w.join(timeout=1.0)
        try:
            w.ser.close()
        except serial.SerialException:
            pass
        self.root.destroy()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="COM14")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--save-dir", default=SAVE_DIR_DEFAULT, metavar="DIR",
                    help="folder for 'Save frame' (default: live_frames beside this script)")
    ap.add_argument("--wake", type=int, default=0, metavar="SECONDS",
                    help="after a cold boot, set a timelapse wake (op7) of this many "
                         "seconds so the device wakes and can capture (op7 is set to 0 on close)")
    ap.add_argument("--interval", type=int, default=None, metavar="MS",
                    help="free-run frame interval in ms (default by camera: "
                         + ", ".join(f"{k} {v}" for k, v in INTERVAL_DEFAULT_MS.items())
                         + f"; minimum {INTERVAL_MIN_MS})")
    args = ap.parse_args()

    frame_q = queue.Queue()
    log_q = queue.Queue()
    try:
        worker = SerialWorker(args.port, args.baud, frame_q, log_q, wake_s=args.wake,
                              interval_ms=(None if args.interval is None
                                           else max(INTERVAL_MIN_MS, args.interval)))
    except serial.SerialException as e:
        print(f"Cannot open {args.port}: {e}", file=sys.stderr)
        print("Is TeraTerm (or another terminal) still holding the port?", file=sys.stderr)
        return 2
    worker.start()

    root = tk.Tk()
    Viewer(root, worker, save_dir=os.path.abspath(args.save_dir))
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
