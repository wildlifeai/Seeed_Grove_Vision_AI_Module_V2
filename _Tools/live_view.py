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

It also handles the WW500's sleep habits by itself: on connect it probes the
device with 'ver'; if there is no answer it waits for a boot banner (power-
cycle the device, or wave at the lens if MD is armed), then immediately types
the keep-awake (the device DPDs ~4 s after an idle cold boot), turns preview
on and starts a capture burst - every step echo-verified with retries.

Usage:
    py live_view.py                    # defaults: COM14, 921600
    py live_view.py --port COM13

NOTES
 - Close TeraTerm first: Windows COM ports are exclusive.
 - The console UART receive path has a 1-character buffer re-armed by the CLI
   task, so commands are sent one character at a time, preferably in the quiet
   gap right after a frame arrives.
 - An occasional frame is lost when another firmware print lands inside the
   frame line; it is counted as 'dropped' and skipped.
 - op8 (inactivity) is 16-bit: 60000 ms is the usable maximum. Streaming
   resets the timer continuously, so the device stays awake while you tune.
   Restore field behaviour when done: setop 8 1000
"""

import argparse
import base64
import io
import json
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
FRAME_OK = "\x00FRAME"         # sentinel: step is confirmed by a decoded frame
MAX_LINE = 512 * 1024          # a VGA frame line is ~35-70 KB; anything huge is junk
CHAR_DELAY = 0.03              # per-character send delay (see module docstring)
# NB: interval 0 wedges the IMX708 datapath (frame timeouts + endless sensor
# restarts). 500 ms is reliable and still gives ~1 fps once capture,
# WB-correct and UART streaming time are included.
CAPTURE_CMD = "capture 1000 500"
REARM_QUIET_S = 8.0            # re-send capture when streaming and no frame this long
DISPLAY_MAX = (800, 600)
SEND_RETRY_S = 3.0             # resend an unconfirmed sequence step this often
SEND_TRIES = 8

CLI_UP_RE = re.compile(r"Starting CLI Task|Enter 'help' to view")
# Camera/datapath init is complete only here - sending a capture before this
# collides with the init and every frame times out until the next power-cycle.
BOOT_DONE_RE = re.compile(r"Image sensor and data path initialised")
BOOT_SETTLE_S = 4.0            # boot-done -> arm delay (lets the boot capture finish)

KEEPAWAKE_STEP = ("setop 8 60000", "Set OpParam 8 = 60000")

# The arm sequence: preview mode 1 (stream, skip SD saves), then a long
# capture burst. Each step is resent until its confirmation string appears.
# The keep-awake goes out earlier, at CLI-up (safe before the camera init).
ARM_SEQUENCE = [
    ("preview 1", "Preview mode 1"),
    (CAPTURE_CMD, FRAME_OK),
]


class SerialWorker(threading.Thread):
    """Reads the console UART, splitting frames from ordinary console lines.

    Outgoing commands go through a verified-send queue: each (cmd, expect)
    step is typed one character at a time and resent until `expect` shows up
    in the console output (or a frame arrives, for the FRAME_OK sentinel).
    Steps with expect=None are fire-once (used for manual commands).
    """

    def __init__(self, port, baud, frame_q, log_q):
        super().__init__(daemon=True)
        self.frame_q = frame_q
        self.log_q = log_q
        self.seq = deque()          # entries: [cmd, expect, tries_left]
        self.seq_last_send = 0.0
        self.dropped = 0
        self.last_frame_ts = 0.0
        self.last_cli_up = 0.0
        self.arm_at = None          # deferred arm time after boot-done
        self.autoarm_enabled = True
        self.running = True
        self.ser = serial.Serial(port, baud, timeout=0.05)

    # ---- command sending -------------------------------------------------

    def send(self, cmd):
        """Fire-once manual command (no confirmation)."""
        self.seq.append([cmd.strip(), None, 1])

    def enqueue_verified(self, steps):
        for cmd, expect in steps:
            self.seq.append([cmd, expect, SEND_TRIES])

    def arm(self, reason):
        if any(step[1] == FRAME_OK for step in self.seq):
            return  # an arm sequence is already pending
        self.log_q.put(f"[viewer] {reason} - arming preview stream")
        self.enqueue_verified(ARM_SEQUENCE)

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
                           f"(power-cycle the device and it will re-arm)")
            self.seq.popleft()
            return
        self.seq[0][2] = tries - 1
        self._write_now(cmd)
        self.seq_last_send = time.monotonic()

    def _confirm(self, line):
        if self.seq:
            cmd, expect, _tries = self.seq[0]
            if expect and expect != FRAME_OK and expect in line:
                self.log_q.put(f"[viewer] confirmed: {cmd}")
                self.seq.popleft()
                self.seq_last_send = 0.0  # next step goes out immediately

    def _confirm_frame(self):
        if self.seq and self.seq[0][1] == FRAME_OK:
            self.log_q.put(f"[viewer] confirmed: {self.seq[0][0]} (frames flowing)")
            self.seq.popleft()

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
                    count = data.get("count", 0)
                    if count <= 3 or count % 20 == 0:
                        self.log_q.put(f"[frame] #{count} {len(jpeg)} bytes")
                    self._confirm_frame()
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
        if (self.autoarm_enabled and CLI_UP_RE.search(line)
                and now - self.last_frame_ts > 10 and now - self.last_cli_up > 10):
            # Fresh boot: drop any stale queued steps, keep the device awake
            # right away, and defer preview/capture until the camera init is
            # done (see BOOT_DONE_RE).
            self.last_cli_up = now
            self.arm_at = None
            self.seq.clear()
            self.log_q.put("[viewer] boot detected - keeping device awake, "
                           "waiting for camera init before arming")
            self.enqueue_verified([KEEPAWAKE_STEP])
        if self.autoarm_enabled and BOOT_DONE_RE.search(line):
            self.arm_at = now + BOOT_SETTLE_S
            self.log_q.put(f"[viewer] camera init done - arming in {BOOT_SETTLE_S:.0f} s")
        self.log_q.put(line)

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
            # deferred arm once the boot has settled
            if self.arm_at is not None and time.monotonic() >= self.arm_at:
                self.arm_at = None
                self.arm("boot settled")
            # device quiet (asleep, booting, or between frames): safe to send
            if time.monotonic() - self.last_frame_ts > 3.0:
                self._pump_seq()
        try:
            self.ser.close()
        except serial.SerialException:
            pass

    def stop(self):
        self.running = False


class Viewer:
    def __init__(self, root, worker):
        self.root = root
        self.worker = worker
        self.frame_q = worker.frame_q
        self.log_q = worker.log_q
        self.streaming = True   # auto-armed on connect
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

        tk.Button(side, text="Start stream", command=self.start_stream).pack(fill=tk.X)
        tk.Button(side, text="Stop stream", command=self.stop_stream).pack(fill=tk.X, pady=(4, 0))
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

    def _set_status(self, info):
        if info is None:
            self.status.config(text="frames    -\nsize      -\nfps       -\ndropped   0")
            return
        count, res, kbytes, fps = info
        self.status.config(text=(f"frames    {count}\n"
                                 f"size      {kbytes:.1f} KB  {res[0]}x{res[1]}\n"
                                 f"fps       {fps:.2f}\n"
                                 f"dropped   {self.worker.dropped}"))

    def start_stream(self):
        self.streaming = True
        self.last_rearm = time.monotonic()
        self.worker.arm("start requested")

    def stop_stream(self):
        self.streaming = False
        self.worker.send("preview 0")

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
        with open(name, "wb") as f:
            f.write(self.last_jpeg)
        self._append_log(f"[viewer] saved {name} ({len(self.last_jpeg)} bytes)")

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

        # burst of 1000 captures ran out? start another while streaming
        now = time.monotonic()
        if (self.streaming and self.worker.last_frame_ts > 0
                and now - self.worker.last_frame_ts > REARM_QUIET_S
                and now - self.last_rearm > REARM_QUIET_S
                and not self.worker.seq):
            self._append_log("[viewer] stream quiet - re-arming capture")
            self.worker.enqueue_verified([(CAPTURE_CMD, FRAME_OK)])
            self.last_rearm = now

        self.root.after(30, self.poll)

    def close(self):
        try:
            if self.streaming:
                self.worker.send("preview 0")
                time.sleep(1.5)  # give the command a chance to go out
        finally:
            self.worker.stop()
            self.root.destroy()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="COM14")
    ap.add_argument("--baud", type=int, default=921600)
    args = ap.parse_args()

    frame_q = queue.Queue()
    log_q = queue.Queue()
    try:
        worker = SerialWorker(args.port, args.baud, frame_q, log_q)
    except serial.SerialException as e:
        print(f"Cannot open {args.port}: {e}", file=sys.stderr)
        print("Is TeraTerm (or another terminal) still holding the port?", file=sys.stderr)
        return 2
    worker.start()

    root = tk.Tk()
    Viewer(root, worker)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
