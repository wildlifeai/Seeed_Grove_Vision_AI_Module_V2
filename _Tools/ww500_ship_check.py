"""
WW500 ship check: flash both Himax images onto a batch of boards over the console
UART and prove both cameras take a photo, one board at a time, with one button.
Two more buttons take a black and white (HM0360) or colour (RP3) photo with the
images already on the board, after restarting it and checking the camera's self-test
bits, so each click also tests the camera's connector. They work without image files.

Runbook: _Documentation/pcb_batch_flashing.md

Operator, per board: plug in the USB-serial adapter, press the board's RESET button,
click the button. The window then shows each step, both photos and PASS or FAIL.

While idle the app streams '1' at the console, so whenever a board resets (RESET
button or power-up) the ROM bootloader's 30 ms "press any key" window is caught and
the board waits in X-Modem download mode. The click then runs:

  1. XMODEM burn of the HM0360 image (the bootloader writes the backup slot)
  2. answer the bootloader's reboot question, catch the reboot, XMODEM burn of the
     RP3 image into the other slot (no second RESET press)
  3. RP3 image boot: banner, camera, slot label, both cameras answering on I2C
  4. RP3 photo, after a 5 s sleep and RTC wake: on firmware without the I2C slave-ID
     fix (#252, 30 Sep 2026) the IMX708 cannot stream after a cold
     boot. Photos stream over the console ('preview 1'), nothing is saved to SD
  5. shipping op 7 / op 8, 'switchslot', 'dpd' so the switch happens at once
  6. HM0360 image boot and photo, SD card state, self-test bits, 'slots'

Per board, in --logdir (default ~/ww500_ship_check/batch_<date>): <board>.log (every
byte from the console), <board>_RP3.jpg, <board>_HM0360.jpg and a row in summary.csv.
The photo buttons add <board>_<camera>_<time>.jpg and no summary row.

Needs Python 3.10+, pyserial, xmodem and Pillow (pip install pyserial xmodem Pillow).
--release also needs the GitHub CLI (gh), signed in.
"""
import argparse, base64, csv, datetime, glob, io, os, queue, re, subprocess, sys, threading, time
import serial, xmodem
import tkinter as tk
from PIL import Image, ImageStat, ImageTk
try:
    import winsound
except ImportError:
    winsound = None

REPO = "wildlifeai/Seeed_Grove_Vision_AI_Module_V2"

BL_PROMPT = b"waiting input key"
MENU_READY = b"Send data using the xmodem protocol from your terminal"
REBOOT_Q = b"Do you want to end file transmission and reboot system"
BANNER = re.compile(rb"\*\*\*\* WW500 MD\. \(([^)]*)\) Built: ([0-9:]+ [A-Za-z]{3} +\d+ \d{4})")
CAMERA = re.compile(rb"Camera: (RP v3 \(IMX708\)|HM0360)")
LABEL = re.compile(rb"Slot ([AB]) labelled variant (\d)")
NOT_PRESENT = re.compile(rb"(HM0360|Main camera) not present at 0x[0-9a-f]+|no camera!")
PRESENT = re.compile(rb"(HM0360|Main camera) present at 0x[0-9a-f]+")
BOOT_DONE = re.compile(rb"Image sensor and data path initialised")
SD_STATE = re.compile(rb"SD card initialised|SD card initialisation failed[^\r\n]*")
SLOTS = re.compile(rb"Active slot \d running '([^']*)'")
B64_RUN = re.compile(rb"[A-Za-z0-9+/=]{100,}")

# Photo acceptance (a working camera in room light easily clears these)
MIN_JPEG_BYTES = 3000
MIN_STDDEV = 6.0            # a flat frame (lens cap, dead sensor) is ~0-2
MEAN_RANGE = (8, 248)       # all black / all white

SUMMARY_HEADER = ["time", "board", "result", "reason", "built", "rp3_photo", "hm0360_photo",
                  "cameras_on_i2c", "sd_card", "selftest", "slots"]

# The photo buttons: image (as 'slots' names it) -> (photo panel, what the button says, the
# self-test bits that mean, on that image, its camera did not answer on I2C at boot). The
# HM0360 image tests the HM0360 twice, as motion sensor (0200) and main camera (0100)
PHOTO = {"HM0360": ("HM0360 (night)", "black and white", 0x0300), "RP3": ("RP3 (colour)", "colour", 0x0100)}
SELFTEST = re.compile(rb"selfTest ([0-9a-f]{4})")


class Failed(Exception):
    pass


class Console:
    """The board's console UART. Every byte received goes to the board's log file."""

    def __init__(self, port):
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 921600
        self.ser.timeout = 0.002
        self.ser.dtr = False        # DTR/RTS do not reset these boards; keep them quiet
        self.ser.rts = False
        self.ser.open()
        self.log = None
        self.buf = b""      # what the current wait searches
        self.frames = b""   # unprocessed bytes, for frame extraction
        self.hist = b""     # everything since the run started (boot lines, for the record)

    def set_log(self, path):
        if self.log:
            self.log.close()
        self.log = open(path, "ab") if path else None

    def note(self, text):
        if self.log:
            self.log.write(f"\n##### {datetime.datetime.now():%H:%M:%S} {text}\n".encode())
            self.log.flush()

    def pump(self):
        data = self.ser.read(8192)
        if data:
            if self.log:
                self.log.write(data)
                self.log.flush()
            self.buf = (self.buf + data)[-200000:]
            self.hist = (self.hist + data)[-2000000:]
            self.frames += data
        return data

    def wait_for(self, pattern, timeout, keep=False):
        """Read until pattern (bytes or compiled regex) appears. keep=True also searches
        what was already read (e.g. a reply that arrived while typing)."""
        end = time.time() + timeout
        if not keep:
            self.buf = b""
        while time.time() < end:
            self.pump()
            m = pattern.search(self.buf) if hasattr(pattern, "search") else (pattern in self.buf)
            if m:
                return m
        return None

    def flood_until_download(self, timeout, stop=None):
        """Stream '1' until the bootloader says it is ready for XMODEM.
        Returns False on timeout, or as soon as `stop` is set."""
        end = time.time() + timeout if timeout else None
        self.buf = b""
        seen = False
        while end is None or time.time() < end:
            if stop and stop.is_set():
                return False
            self.ser.write(b"1" * 8)
            self.pump()
            self.frames = b""
            if not seen and BL_PROMPT in self.buf:
                seen = True
                self.note("bootloader reset seen")
            if MENU_READY in self.buf:
                self.note("bootloader in XMODEM download mode")
                return True
        return False

    def type_cmd(self, cmd):
        """The console has a 1-character receive buffer: one character every 30 ms.
        Ctrl-C first clears a half-typed line (e.g. the '1's sent while catching the
        bootloader); the CLI runs a command on '\\n' only, '\\r' is ignored."""
        self.note(f"typing {cmd!r}")
        for ch in "\x03" + cmd + "\r\n":
            self.ser.write(ch.encode())
            time.sleep(0.03)
            self.pump()

    def command(self, cmd, expect, timeout=3.0, tries=3):
        """Type a command until its confirmation shows up. Returns the match."""
        for _ in range(tries):
            self.buf = b""
            self.type_cmd(cmd)
            m = self.wait_for(expect, timeout, keep=True)
            if m:
                return m
        raise Failed(f"no reply to '{cmd}'")

    def xmodem_send(self, path, progress):
        time.sleep(1)                      # same handshake as xmodem/xmodem_send.py
        self.ser.reset_input_buffer()
        self.ser.write(b"1")
        total = (os.path.getsize(path) + 127) // 128
        self.ser.timeout = 1

        def getc(n, timeout=1):
            return self.ser.read(n) or None

        def putc(data, timeout=1):
            return self.ser.write(data) or None

        def cb(total_packets, success_count, error_count):
            if success_count % 32 == 0 or success_count >= total:
                progress(min(100, 100 * success_count // total))

        try:
            with open(path, "rb") as f:
                ok = xmodem.XMODEM(getc, putc, mode="xmodem").send(f, retry=16, callback=cb)
        finally:
            self.ser.timeout = 0.002
        self.note(f"xmodem send {os.path.basename(path)}: {'OK' if ok else 'FAIL'}")
        return ok

    def take_frames(self, want, deadline_s, rearm_every_s, capture_cmd):
        """Collect preview frames: the base64 JPEG between '"image": "' and '"}}'.

        Other tasks' console prints land inside a frame (on the HM0360 image the report
        to the BLE processor does, in every frame), so the frame is rebuilt from its
        long base64 runs. It counts only if the JPEG fully decodes."""
        frames = []
        end = time.time() + deadline_s
        last_arm = 0
        while time.time() < end and len(frames) < want:
            if time.time() - last_arm > rearm_every_s and not frames:
                self.frames = b""
                self.type_cmd(capture_cmd)
                last_arm = time.time()
            self.pump()
            while True:
                s = self.frames.find(b'"image": "')
                if s < 0:
                    self.frames = self.frames[-16:]      # keep a possible partial marker
                    break
                e = self.frames.find(b'"}}', s)
                if e < 0:
                    self.frames = self.frames[s:]        # frame still arriving
                    break
                body, self.frames = self.frames[s + 10:e], self.frames[e + 3:]
                jpeg = reassemble_jpeg(body)
                if jpeg:
                    frames.append(jpeg)
                else:
                    self.note("dropped a frame that did not decode")
        return frames


def reassemble_jpeg(body):
    """Join the long base64 runs of a frame body; return the JPEG only if it fully decodes."""
    b64 = b"".join(B64_RUN.findall(body))
    if not b64:
        return None
    try:
        jpeg = base64.b64decode(b64 + b"=" * (-len(b64) % 4))
        Image.open(io.BytesIO(jpeg)).load()
        return jpeg
    except Exception:
        return None


def judge(jpeg):
    """Returns (ok, reason, PIL image)."""
    if len(jpeg) < MIN_JPEG_BYTES:
        return False, f"JPEG only {len(jpeg)} bytes", None
    try:
        img = Image.open(io.BytesIO(jpeg))
        img.load()
    except Exception as e:
        return False, f"JPEG does not decode ({e})", None
    st = ImageStat.Stat(img.convert("L"))
    mean, sd = st.mean[0], st.stddev[0]
    info = f"{img.width}x{img.height}, {len(jpeg) // 1024} KB, mean {mean:.0f}, contrast {sd:.1f}"
    if sd < MIN_STDDEV:
        return False, f"flat image ({info})", img
    if not (MEAN_RANGE[0] <= mean <= MEAN_RANGE[1]):
        return False, f"all dark or all bright ({info})", img
    return True, info, img


def fetch_release(run, dest):
    """Download both images from a build_and_upload_firmware.yml run ('latest' = the
    newest successful run on main, i.e. what production serves). Returns (hm0360, rp3)."""
    if run == "latest":
        out = subprocess.run(["gh", "run", "list", "-R", REPO, "--workflow", "build_and_upload_firmware.yml",
                              "--branch", "main", "--status", "success", "-L", "1",
                              "--json", "databaseId", "--jq", ".[0].databaseId"],
                             capture_output=True, text=True, check=True)
        run = out.stdout.strip()
        if not run:
            sys.exit("no successful release run found on main")
    folder = os.path.join(dest, str(run))
    if not glob.glob(os.path.join(folder, "*", "*.img")):
        subprocess.run(["gh", "run", "download", str(run), "-R", REPO, "-D", folder], check=True)
    hm = glob.glob(os.path.join(folder, "firmware-image-HM0360", "*.img"))
    rp = glob.glob(os.path.join(folder, "firmware-image-RP3", "*.img"))
    if len(hm) != 1 or len(rp) != 1:
        sys.exit(f"run {run} does not hold exactly one HM0360 and one RP3 image")
    print(f"release run {run}:\n  {hm[0]}\n  {rp[0]}")
    return hm[0], rp[0]


class App:
    def __init__(self, args):
        self.a = args
        os.makedirs(args.logdir, exist_ok=True)
        self.summary = os.path.join(args.logdir, "summary.csv")
        if not os.path.exists(self.summary):
            with open(self.summary, "w", newline="") as f:
                csv.writer(f).writerow(SUMMARY_HEADER)
        self.q = queue.Queue()
        self.clicked = threading.Event()
        self.action = None
        self.can_flash = bool(args.hm0360 and args.rp3)
        self.done = 0
        self.passed = 0
        self.board = ""

        r = self.root = tk.Tk()
        r.title("WW500 ship check")
        r.attributes("-topmost", True)
        r.geometry("760x630")
        top = tk.Frame(r); top.pack(pady=(12, 4))
        tk.Label(top, text="Board:", font=("Segoe UI", 12)).pack(side="left")
        self.label = tk.StringVar(value=f"{args.prefix}{args.first:02d}")
        entry = tk.Entry(top, textvariable=self.label, width=10, font=("Segoe UI", 12))
        entry.pack(side="left", padx=6)
        # Tk's Ctrl+A moves to the line start; make it select all, as operators expect
        entry.bind("<Control-a>", lambda e: (e.widget.select_range(0, "end"), e.widget.icursor("end"), "break")[-1])
        self.count = tk.Label(top, text="0 passed, 0 runs", font=("Segoe UI", 12))
        self.count.pack(side="left", padx=12)
        self.btn = tk.Button(r, text="I have reset the board - go", font=("Segoe UI", 18, "bold"),
                             bg="#1f6feb", fg="white", activebackground="#1a5fd0", padx=20, pady=10,
                             command=lambda: self.on_click("ship"),
                             state="normal" if self.can_flash else "disabled")
        self.btn.pack(pady=(10, 6))
        row = tk.Frame(r); row.pack(pady=(0, 6))
        self.buttons = [self.btn]
        for image, (_, kind, _) in PHOTO.items():
            b = tk.Button(row, text=f"Take a {kind} photo", font=("Segoe UI", 12), padx=12, pady=4,
                          command=lambda i=image: self.on_click(i))
            b.pack(side="left", padx=8)
            self.buttons.append(b)
        self.msg = tk.Label(r, text="", font=("Segoe UI", 14), wraplength=720, justify="center")
        self.msg.pack(pady=4)
        self.detail = tk.Label(r, text="", font=("Segoe UI", 10), fg="#555555", wraplength=720)
        self.detail.pack()
        pics = tk.Frame(r); pics.pack(pady=10)
        self.blank = ImageTk.PhotoImage(Image.new("RGB", (320, 240), "#dddddd"))
        self.pic = {}
        for name in ("RP3 (colour)", "HM0360 (night)"):
            f = tk.Frame(pics); f.pack(side="left", padx=10)
            lab = tk.Label(f, image=self.blank); lab.pack()
            cap = tk.Label(f, text=name, font=("Segoe UI", 10), wraplength=330); cap.pack()
            self.pic[name] = (lab, cap)
        threading.Thread(target=self.worker, daemon=True).start()
        r.after(100, self.poll)

    # ---------- UI (the worker talks to Tk only through the queue)
    def say(self, text, colour="black"):
        self.q.put(("msg", text, colour))

    def step(self, text):
        self.q.put(("detail", text, None))

    def show(self, name, img, caption):
        self.q.put(("pic", name, (img, caption)))

    def beep(self, tones):
        self.q.put(("beep", tones, None))

    def poll(self):
        try:
            while True:
                kind, a, b = self.q.get_nowait()
                if kind == "msg":
                    self.msg.config(text=a, fg=b)
                elif kind == "detail":
                    self.detail.config(text=a)
                elif kind == "pic":
                    lab, cap = self.pic[a]
                    img, caption = b
                    if img is None:
                        lab.config(image=self.blank); lab.image = self.blank
                    else:
                        t = img.convert("RGB"); t.thumbnail((320, 240))
                        ph = ImageTk.PhotoImage(t); lab.config(image=ph); lab.image = ph
                    cap.config(text=caption)
                elif kind == "button":
                    for btn in self.buttons:
                        btn.config(state=a if self.can_flash or btn is not self.btn else "disabled")
                elif kind == "next":
                    # the label moves on only after a pass, so a retry keeps its label
                    self.count.config(text=f"{self.passed} passed, {self.done} runs")
                    m = re.match(r"(.*?)(\d+)$", self.label.get())
                    if a and m:
                        self.label.set(f"{m.group(1)}{int(m.group(2)) + 1:0{len(m.group(2))}d}")
                elif kind == "beep" and winsound:
                    for f_, d in a:
                        winsound.Beep(f_, d)
        except queue.Empty:
            pass
        self.root.after(100, self.poll)

    def on_click(self, action):
        self.action = action
        self.board = self.label.get().strip() or f"board{self.done + 1:02d}"
        self.q.put(("button", "disabled", None))
        self.clicked.set()

    # ---------- the work
    def open_port(self):
        """Open the console port, waiting for the adapter if it is unplugged."""
        warned = False
        while True:
            try:
                return Console(self.a.port)
            except Exception:
                if not warned:
                    self.say(f"Waiting for the USB adapter on {self.a.port}... plug it in.", "#c00000")
                    warned = True
                time.sleep(1)

    def worker(self):
        while True:
            self.con = self.open_port()
            self.say("Plug in a board, press its RESET button, then click the button." if self.can_flash else
                     "Click a photo button. Flashing needs --release, or --hm0360 and --rp3.")
            try:
                self.boards()
            except serial.SerialException:
                # adapter unplugged: close and wait for it to come back
                try:
                    self.con.ser.close()
                except Exception:
                    pass
                self.clicked.clear()
                self.q.put(("button", "normal", None))

    def boards(self):
        con = self.con
        while True:
            # idle: catch any reset into download mode (only when there is something to
            # flash: a photo needs the board to boot), then wait for the click
            con.set_log(None)
            caught = self.can_flash and con.flood_until_download(None, stop=self.clicked)
            if caught:
                self.step("Board is waiting in download mode.")
            while not self.clicked.is_set():
                con.pump()
                con.frames = b""
            self.clicked.clear()
            con.set_log(os.path.join(self.a.logdir, f"{self.board}.log"))
            if self.action in PHOTO:
                self.photo(self.action, caught)
                self.q.put(("button", "normal", None))
                continue
            con.note(f"board {self.board}")
            con.hist = b""
            self.say("Running code...", "black")
            for n in ("RP3 (colour)", "HM0360 (night)"):
                self.show(n, None, n)
            row = self.run_board(caught)
            with open(self.summary, "a", newline="") as f:
                csv.writer(f).writerow([row[k] for k in SUMMARY_HEADER])
            self.done += 1
            self.passed += row["result"] == "PASS"
            if row["result"] == "PASS":
                warn = f"\n(note: {row['sd_card']})" if row["sd_card"] != "present" else ""
                self.say("All checks passed. Plug in a new board, press RESET, and click the button "
                         "when ready." + warn, "#007000")
                self.beep([(880, 120), (1320, 160)])
            else:
                self.say(f"FAILED: {row['reason']}\nFirst failure: unplug the board, reseat the camera "
                         "ribbon(s), plug it back in, press RESET and click again (same label). Second "
                         "failure: put it aside, type the next label, carry on.", "#c00000")
                self.beep([(400, 300), (300, 400)])
            self.q.put(("next", row["result"] == "PASS", None))
            self.q.put(("button", "normal", None))

    def run_board(self, caught):
        con = self.con
        row = {k: "" for k in SUMMARY_HEADER}
        row["time"] = f"{datetime.datetime.now():%Y-%m-%d %H:%M:%S}"
        row["board"] = self.board
        try:
            # 1. HM0360 image (one retry: a board left waiting too long gives up on XMODEM)
            for attempt in (1, 2):
                if not caught:
                    self.ask_reset()
                    if not con.flood_until_download(120):
                        raise Failed("board never entered download mode (no RESET seen)")
                    self.say("Running code...", "black")
                try:
                    self.burn(self.a.hm0360, "HM0360 image (1 of 2)")
                    break
                except Failed:
                    if attempt == 2:
                        raise
                    caught = False
            # 2. catch the reboot, RP3 image
            self.step("Rebooting into download mode for image 2...")
            if not con.flood_until_download(25):
                self.ask_reset()
                if not con.flood_until_download(120):
                    raise Failed("could not get back to download mode for image 2")
                self.say("Running code...", "black")
            self.burn(self.a.rp3, "RP3 image (2 of 2)")

            # 3. RP3 image boot checks
            self.step("Booting the RP3 image...")
            if not con.wait_for(BANNER, 25, keep=True):
                raise Failed("RP3 image did not boot")
            row["built"] = BANNER.search(con.buf).group(2).decode()
            con.wait_for(LABEL, 8, keep=True)
            cam = CAMERA.search(con.buf)
            if not cam or b"RP v3" not in cam.group(1):
                raise Failed("RP3 image did not report its camera")
            if not LABEL.search(con.buf):
                raise Failed("RP3 slot did not label itself")
            row["cameras_on_i2c"] = " / ".join(m.group(0).decode() for m in PRESENT.finditer(con.buf)) or "?"
            bad = NOT_PRESENT.search(con.buf)
            if bad:
                raise Failed(f"camera missing on I2C: {bad.group(0).decode()}")

            # 4. RP3 photo after a sleep/wake; 5. ship settings, switch
            rp3_ok, row["rp3_photo"] = self.rp3_photo()

            # 6. the scheduled reset boots the HM0360 image
            self.step("Waiting for the HM0360 image to boot...")
            if not con.wait_for(BANNER, 150):
                raise Failed(("" if rp3_ok else f"RP3: {row['rp3_photo']}; ")
                             + "HM0360 image did not boot after switchslot")
            con.hist = con.buf          # the record below is about this boot
            con.wait_for(LABEL, 8, keep=True)
            cam = CAMERA.search(con.buf)
            if not cam or cam.group(1) != b"HM0360":
                raise Failed("HM0360 image did not report its camera")
            bad = NOT_PRESENT.search(con.buf)
            if bad:
                raise Failed(f"camera missing on I2C: {bad.group(0).decode()}")
            # As main camera the HM0360 skips the slave-ID save/restore: no wake needed
            self.step("HM0360 image running, taking a photo...")
            self.cli_up()
            self.setop(8, 60000)
            try:
                frames = self.grab()
                row["slots"] = self.con.command("slots", re.compile(rb"Active slot[^\r\n]*")) \
                    .group(0).decode(errors="replace")
                row["selftest"] = self.con.command("selftest", re.compile(rb"selfTest [0-9a-f]{4}")) \
                    .group(0).decode().split()[1]
            finally:
                self.setop(7, self.a.ship_op7)
                self.setop(8, self.a.ship_op8)
            sd = SD_STATE.search(con.hist)
            row["sd_card"] = ("present" if sd.group(0) == b"SD card initialised" else "missing") if sd else "unknown"
            hm_ok, row["hm0360_photo"] = self.verdict("HM0360 (night)", "HM0360", frames)
            fails = ([] if rp3_ok else [f"RP3 camera: {row['rp3_photo']}"]) + \
                    ([] if hm_ok else [f"HM0360 camera: {row['hm0360_photo']}"])
            if self.a.require_sd and row["sd_card"] != "present":
                fails.append(f"SD card {row['sd_card']}")
            if fails:
                raise Failed("; ".join(fails))
            row["result"], row["reason"] = "PASS", "all checks passed"
        except Failed as e:
            row["result"], row["reason"] = "FAIL", str(e)
        except serial.SerialException:
            raise
        except Exception as e:
            row["result"], row["reason"] = "FAIL", f"error {e!r}"
        con.note(f"RESULT {row}")
        return row

    def burn(self, path, what):
        con = self.con
        self.step(f"Sending {what}: 0%")
        if not con.xmodem_send(path, lambda p: self.step(f"Sending {what}: {p}%")):
            raise Failed(f"transfer of {what} failed")
        self.step(f"{what} written, rebooting...")
        if con.wait_for(REBOOT_Q, 10):
            time.sleep(0.3)
            con.buf = b""
            con.ser.write(b"y")
            con.note("answered reboot question with y")

    def ask_reset(self):
        self.say("Press the RESET button on the board now.", "#c00000")
        self.beep([(1200, 200)])

    def switch(self, to="HM0360"):
        self.step(f"Switching to the {to} image...")
        self.con.command("switchslot", re.compile(rb"Switched to slot \d|Slot switch failed[^\r\n]*"))
        if b"Slot switch failed" in self.con.buf:
            raise Failed("switchslot refused")

    def cli_up(self):
        # the console task prints this once it is running (cold and warm boot)
        self.con.wait_for(b"Enter 'help'", 10, keep=True)

    def getop(self, n):
        m = self.con.command(f"getop {n}", re.compile(rb"OpParam %d = (\d+)" % n), timeout=1.5, tries=5)
        return m.group(1).decode()

    def setop(self, n, v):
        self.con.command(f"setop {n} {v}", f"Set OpParam {n} = {v}".encode(), timeout=1.5, tries=5)

    def grab(self, wait_boot=True, deadline_s=60):
        """Stream a few preview frames (no SD writes while preview is on). wait_boot: the
        board has just booted, wait for its sensor first."""
        con = self.con
        if wait_boot:
            con.wait_for(BOOT_DONE, 10, keep=True)
            time.sleep(2)
        con.command("preview 1", b"Preview mode 1")
        # never a 0 ms interval on the RP3 (wedges the IMX708 datapath)
        frames = con.take_frames(want=1, deadline_s=deadline_s, rearm_every_s=12, capture_cmd="capture 3 500")
        time.sleep(1.5)
        con.command("preview 0", b"Preview off")
        return frames

    def rp3_photo(self):
        con = self.con
        self.step("RP3 image running: a 5 s sleep so the colour camera starts cleanly...")
        self.cli_up()
        orig8, orig7 = self.getop(8), self.getop(7)
        con.note(f"found op8={orig8} op7={orig7}")
        self.setop(7, 5)
        con.command("dpd", b"Forcing DPD")
        if not con.wait_for(BANNER, 45):
            raise Failed(f"RP3 image did not wake from its 5 s sleep (op 7 is still 5, was {orig7})")
        self.cli_up()
        self.setop(8, 60000)
        try:
            self.step("RP3 image awake again, taking a photo...")
            frames = self.grab()
        finally:
            # Ship settings, then the switch while still awake. The 60 s inactivity
            # countdown started under 'setop 8 60000' keeps running, so force the sleep:
            # the scheduled reset then boots the HM0360 image at once
            self.setop(7, self.a.ship_op7)
            self.switch()
            self.setop(8, self.a.ship_op8)
            con.command("dpd", b"Forcing DPD")
        return self.verdict("RP3 (colour)", "RP3", frames)

    def verdict(self, panel, tag, frames, name=None):
        if not frames:
            self.show(panel, None, f"{panel}: no photo")
            return False, "no photo"
        jpeg = frames[-1]
        with open(os.path.join(self.a.logdir, name or f"{self.board}_{tag}.jpg"), "wb") as f:
            f.write(jpeg)
        ok, reason, img = judge(jpeg)
        self.show(panel, img, f"{panel}: {reason}")
        return ok, reason

    # ---------- the photo buttons
    def photo(self, image, caught):
        """One photo from the camera of `image` ('HM0360' or 'RP3'), with the images already
        on the board. The firmware checks the cameras on I2C at boot only, so the board always
        boots first (RESET if it sleeps, 'switchslot' if the other image runs, 'reset'
        otherwise), and a camera the self-test then reports missing gets no photo. That makes
        each click a fresh test of the camera's connector."""
        con = self.con
        panel, kind, bits = PHOTO[image]
        con.note(f"{kind} photo")
        busy = f"Taking a {kind} photo..."
        self.say(busy, "black")
        self.step("")
        self.show(panel, None, panel)
        con.hist = b""          # this click's boots only (a RESET pressed early counts)
        try:
            running = None if caught else self.running()
            if running is None:
                self.wake(busy)
                running = self.running()
                if running is None:
                    raise Failed("the board booted but its console does not answer")
            elif running == image:
                self.reboot("reset", f"Restarting the {image} image so it checks its cameras again...")
            if running != image:
                self.switch(image)
                self.reboot(None, f"Booting the {image} image...")
                running = self.running()
                if running != image:
                    raise Failed(f"after switchslot the board runs the {running} image")
            errors = int(con.command("selftest", SELFTEST).group(1), 16)
            selftest = f"self-test {errors:04x}"
            if errors & bits:
                lost = "; ".join(m.group(0).decode() for m in NOT_PRESENT.finditer(con.hist))
                result, info = "missing", selftest + (f", {lost}" if lost else "")
            else:
                self.step(f"{selftest}: no issues with the {kind} camera. Taking a photo...")
                if not BOOT_DONE.search(con.hist):
                    con.wait_for(BOOT_DONE, 10)
                time.sleep(2)
                frames = self.grab(wait_boot=False, deadline_s=25)
                if not frames and image == "RP3":
                    frames = self.rp3_rewake()
                ok, info = self.verdict(panel, image, frames,
                                        f"{self.board}_{image}_{datetime.datetime.now():%H%M%S}.jpg")
                result = "ok" if ok else "fail"
        except Failed as e:
            result, info = "fail", str(e)
        except serial.SerialException:
            raise
        except Exception as e:
            result, info = "fail", f"error {e!r}"
        con.note(f"{kind} photo: {result.upper()} {info}")
        if result == "ok":
            self.say(f"No issues ({selftest}). Took a {kind} photo, saved in {self.a.logdir}", "#007000")
            self.step("If the board is asleep by the next click, it asks for RESET.")
            self.beep([(880, 120), (1320, 160)])
        elif result == "missing":
            self.show(panel, None, f"{panel}: disconnected")
            self.say(f"The {kind} camera is disconnected: it did not answer at boot ({info}). "
                     "No photo taken. Unplug the board before reseating the ribbon.", "#c00000")
            self.step("")
            self.beep([(400, 300), (300, 400)])
        else:
            self.say(f"No good {kind} photo: {info}", "#c00000")
            self.beep([(400, 300)])

    def reboot(self, cmd, what):
        """Restart the AI processor now: `cmd` ('reset', or None after 'switchslot' which
        already did this) schedules a reset for the next sleep, and 'dpd' sleeps at once."""
        con = self.con
        self.step(what)
        if cmd:
            con.command(cmd, b"Forcing reset soon")
        con.hist = b""
        con.command("dpd", b"Forcing DPD")
        if not con.wait_for(BANNER, 30):
            raise Failed("the board did not restart")
        self.cli_up()

    def running(self):
        """The image the board runs ('HM0360' or 'RP3'), or None if the console does not
        answer (asleep, or waiting in download mode)."""
        try:
            name = self.con.command("slots", SLOTS, timeout=1.5, tries=2).group(1).decode(errors="replace")
        except Failed:
            return None
        return next((i for i in PHOTO if name.startswith(i)), name)

    def wake(self, busy):
        """RESET the board and wait for its boot. Nothing is sent meanwhile, so the ROM
        bootloader's 30 ms window passes and the active image starts. Only a cold boot
        counts: the hand reaching for RESET can set off a motion wake just before it.
        A reset that never reaches it is reported at once: on 11 Oct 2026, moving a camera
        ribbon with the board powered garbled its output and the second-stage bootloader
        hung, then bus-faulted, and RESET did not bring it back."""
        con = self.con
        self.ask_reset()
        end, reset_at, zero_at, menu_left = time.time() + 120, None, None, False
        while b"Cold boot" not in con.hist:
            con.pump()
            if not menu_left and b"Xmodem download and burn FW image" in con.hist:
                # a keystroke (the 'slots' probe) landed in the 30 ms window: '0' reboots
                con.note("bootloader menu, sending 0 to reboot")
                con.ser.write(b"0")
                menu_left, reset_at = True, time.time()
            if b"SCB->CFSR" in con.hist:
                raise Failed("the board crashed while booting (hard fault). Unplug its power, "
                             "check the camera ribbon, plug it back in and click again")
            # Pressure on a camera connector (11 Oct 2026) left the console sending only
            # noise (00 ff fc f8 ...), with no bootloader text, until it was released.
            # A healthy reset gives at most a few stray bytes
            if sum(c >= 0x80 for c in con.hist) > 20:
                raise Failed("the board's console output is garbled, so a supply is probably "
                             "being pulled down: a camera connector may be shorting. Remove any "
                             "pressure on it, press RESET and click again; if that fails, unplug, "
                             "reseat the ribbon and plug back in")
            if reset_at is None and b"1st BL" in con.hist:
                reset_at = time.time()
            if reset_at and time.time() - reset_at > 10:
                raise Failed("the board reset but did not finish booting: a camera connector may "
                             "be shorting. Remove any pressure on it, press RESET and click again; "
                             "if that fails, unplug, reseat the ribbon and plug back in")
            # A RESET press shows on the console as a 0x00 byte, and the ROM bootloader's
            # banner follows at once. Pressure on a camera connector (11 Oct 2026) left
            # only the 0x00: the AI processor did not start at all
            if zero_at is None and b"\x00" in con.hist:
                zero_at = time.time()
            if zero_at and reset_at is None and time.time() - zero_at > 3:
                raise Failed("RESET was pressed but the AI processor did not start (its console "
                             "stayed silent): a camera connector may be shorting. Remove any "
                             "pressure on it, press RESET and click again")
            if time.time() > end:
                raise Failed("the board did not boot (no RESET in 2 minutes, or the wrong port)")
        self.say(busy, "black")
        self.cli_up()

    def rp3_rewake(self):
        """No frame from the RP3 image: firmware without #252 cannot stream from the IMX708
        after a cold boot. Sleep 5 s and take the photo after the RTC wake, as the ship
        check does."""
        con = self.con
        self.step("No photo yet: a 5 s sleep so the colour camera starts cleanly...")
        orig7 = self.getop(7)
        self.setop(7, 5)
        con.command("dpd", b"Forcing DPD")
        if not con.wait_for(BANNER, 45):
            raise Failed(f"the RP3 image did not wake from its 5 s sleep (op 7 is still 5, was {orig7})")
        self.cli_up()
        self.setop(7, orig7)
        self.step("RP3 image awake again, taking a photo...")
        return self.grab(deadline_s=25)


def main():
    ap = argparse.ArgumentParser(description="Flash both WW500 Himax images and photo-test both cameras, "
                                             "one board at a time, or take a photo with either camera. "
                                             "See _Documentation/pcb_batch_flashing.md")
    ap.add_argument("--port", default="COM6", help="Himax console port (921600 baud)")
    ap.add_argument("--hm0360", help="HM0360 image (.img)")
    ap.add_argument("--rp3", help="RP3 image (.img)")
    ap.add_argument("--release", metavar="RUN_ID|latest",
                    help="download both images from a build_and_upload_firmware.yml run instead "
                         "('latest' = newest successful run on main, what production serves)")
    home = os.path.join(os.path.expanduser("~"), "ww500_ship_check")
    ap.add_argument("--image-dir", default=os.path.join(home, "release_images"),
                    help="where --release downloads to (default: ~/ww500_ship_check/release_images)")
    ap.add_argument("--ship-op7", type=int, default=0, help="op 7 (timelapse, s) written to every board")
    ap.add_argument("--ship-op8", type=int, default=1000, help="op 8 (ms awake after idle) written to every board")
    ap.add_argument("--require-sd", action="store_true", help="fail boards whose SD card does not mount")
    ap.add_argument("--prefix", default="PCB-", help="board label prefix")
    ap.add_argument("--first", type=int, default=1, help="first board number")
    ap.add_argument("--logdir", default=os.path.join(home, f"batch_{datetime.date.today():%Y-%m-%d}"),
                    help="logs, photos and summary.csv (default: ~/ww500_ship_check/batch_<date>)")
    a = ap.parse_args()
    if a.release:
        a.hm0360, a.rp3 = fetch_release(a.release, a.image_dir)
    if bool(a.hm0360) != bool(a.rp3):
        ap.error("give both --hm0360 and --rp3, or --release (or neither, for the photo buttons only)")
    for p in (a.hm0360, a.rp3):
        if p and not os.path.isfile(p):
            sys.exit(f"missing image {p}")
    App(a).root.mainloop()


if __name__ == "__main__":
    main()
