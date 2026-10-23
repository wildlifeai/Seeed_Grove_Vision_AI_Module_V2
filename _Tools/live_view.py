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
from PIL import Image, ImageTk

ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

FRAME_MARKER = '"name": "INVOKE"'
MAX_LINE = 512 * 1024          # a VGA frame line is ~35-70 KB; anything huge is junk
CHAR_DELAY = 0.03              # per-character send delay (see module docstring)
# NB: interval 0 wedges the IMX708 datapath (frame timeouts + endless sensor
# restarts). 500 ms is reliable and still gives ~1 fps once capture,
# WB-correct and UART streaming time are included.
# Free run uses short bursts: the firmware cannot cancel a burst, so Stop and
# One image wait for the current one to end (BURST_FRAMES at ~1 fps).
BURST_FRAMES = 10
CAPTURE_CMD = f"capture {BURST_FRAMES} 500"
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
ARM_SEQUENCE = [PREVIEW_STEP, (CAPTURE_CMD, CAPTURE_OK)]
SINGLE_SEQUENCE = [PREVIEW_STEP, (SINGLE_CMD, CAPTURE_OK)]

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

    def __init__(self, port, baud, frame_q, log_q, wake_s=0):
        super().__init__(daemon=True)
        self.frame_q = frame_q
        self.log_q = log_q
        self.wake_s = wake_s        # timelapse wake (op7) to set at a cold boot, 0 = off
        self.wake_set = False       # op7 was set by us, so clear it on close
        self.seq = deque()          # entries: [cmd, expect, tries_left]
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
        steps = SINGLE_SEQUENCE if single else ARM_SEQUENCE
        if not with_preview:
            steps = steps[1:]
        self.enqueue_verified(steps)

    def _burst_ended(self):
        self.capture_busy = False
        if self.single_pending:
            self.single_pending = False
            self.arm("burst finished", single=True, with_preview=False)
        elif self.free_run:
            self.arm("burst finished", with_preview=False)

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
                self.log_q.put(f"[viewer] confirmed: {cmd}")
                self.seq.popleft()
                self.seq_last_send = 0.0  # next step goes out immediately

    # ---- receive path ----------------------------------------------------

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
                    self.frame_q.put((data.get("count", 0),
                                      data.get("resolution", [0, 0]), jpeg))
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
        self.status.pack(fill=tk.X, pady=(0, 8))
        self._set_status(None)

        tk.Button(side, text="Free run", command=self.start_stream).pack(fill=tk.X)
        tk.Button(side, text="Stop", command=self.stop_stream).pack(fill=tk.X, pady=(4, 0))
        tk.Button(side, text="One image", command=self.one_image).pack(fill=tk.X, pady=(4, 0))
        tk.Button(side, text="Save frame", command=self.save_frame).pack(fill=tk.X, pady=(4, 12))

        tk.Label(side, text="Command:").pack(anchor="w")
        self.cmd_entry = tk.Entry(side, width=28, font=("Consolas", 10))
        self.cmd_entry.pack(fill=tk.X)
        self.cmd_entry.bind("<Return>", self.send_command)
        tk.Button(side, text="Send", command=self.send_command).pack(fill=tk.X, pady=(4, 0))

        tk.Label(side, text="Quick commands:").pack(anchor="w", pady=(12, 0))
        for cmd in ("getop 27", "setop 27 286", "setop 28 326",
                    "vcm 512", "status", "setop 8 1000"):
            tk.Button(side, text=cmd, font=("Consolas", 9),
                      command=lambda c=cmd: self.worker.send(c)).pack(fill=tk.X, pady=1)

        self.log = scrolledtext.ScrolledText(root, height=12, font=("Consolas", 9),
                                             state=tk.DISABLED, wrap=tk.NONE)
        self.log.pack(fill=tk.BOTH, expand=False, padx=4, pady=(0, 4))

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

    def _set_status(self, info):
        if info is not None:
            self.last_info = info
        info = getattr(self, "last_info", None)
        mode = self._mode_text()
        if info is None:
            self.status.config(text=(f"mode      {mode}\nframes    -\nsize      -\n"
                                     f"fps       -\ndropped   {self.worker.dropped}"))
            return
        count, res, kbytes, fps = info
        self.status.config(text=(f"mode      {mode}\n"
                                 f"frames    {count}\n"
                                 f"size      {kbytes:.1f} KB  {res[0]}x{res[1]}\n"
                                 f"fps       {fps:.2f}\n"
                                 f"dropped   {self.worker.dropped}"))

    def start_stream(self):
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

    def _append_log(self, line):
        print(line, flush=True)  # tee to stdout so a supervising session can watch
        self.log.config(state=tk.NORMAL)
        self.log.insert(tk.END, line + "\n")
        # keep the pane bounded
        if float(self.log.index(tk.END)) > 2000:
            self.log.delete("1.0", "500.0")
        self.log.see(tk.END)
        self.log.config(state=tk.DISABLED)

    def _show_frame(self, count, res, jpeg):
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
        if img.width > DISPLAY_MAX[0] or img.height > DISPLAY_MAX[1]:
            img.thumbnail(DISPLAY_MAX)
        self.photo = ImageTk.PhotoImage(img)
        self.image_label.config(image=self.photo, text="", width=img.width, height=img.height)
        self._set_status((count, res, len(jpeg) / 1024.0, fps))

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
            left = ", ".join(step[0] for step in w.seq)
            print(f"[viewer] WARNING: not confirmed, the device may be asleep: {left}. "
                  f"Type these at the console when it is awake.", flush=True)
        else:
            print("[viewer] device reset to lab-safe values", flush=True)
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
    args = ap.parse_args()

    frame_q = queue.Queue()
    log_q = queue.Queue()
    try:
        worker = SerialWorker(args.port, args.baud, frame_q, log_q, wake_s=args.wake)
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
