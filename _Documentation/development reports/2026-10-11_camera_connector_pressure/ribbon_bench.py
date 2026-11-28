"""
Ribbon-failure bench logger for the WW500 (11 Oct 2026).

Logs the Himax console (921600) and/or the BLE processor console (115200) into one
timestamped file, and types commands dropped into inbox files, so a test can be driven
from another shell while the capture runs:

  echo "selftest" >> <dir>/inbox_himax.txt     typed to the Himax (Ctrl-C first, 30 ms/char)
  echo "ver"      >> <dir>/inbox_nrf.txt       typed to the BLE processor
  echo "# T4 start: wiggle now" >> <dir>/inbox_himax.txt   a marker line in the log only

Files in <dir>:
  bench.log    every line: [HH:MM:SS.mmm] himax| ... / nrf  | ...  Bytes outside printable
               ASCII are shown as \\xNN, so garbled output stays visible.
  events.log   only the lines that matter for ribbon tests (boots, resets, faults, frame
               timeouts, retries, camera presence, self-test) plus silence notes.

Usage: py -I ribbon_bench.py --dir <dir> [--himax COM6] [--nrf COM5]   (either may be 'none')

--press 30 opens a window that tells the operator when to press the camera connector: at
frame 10 of a capture that started on a wake from sleep (never one after a cold boot), PRESS
for 30 s, then RELEASE. Both go in the log as notes. If frames still come 20 s after the release,
the next PRESS follows in the same capture. If no frame arrives in the minute after the release
it types 'selftest' once to tell a frozen board from a stuck one, and RESET starts the next try.
--cold-capture "capture 300 1000" also types that after every cold boot and runs tries in it, to
compare captures started straight after a RESET with captures started by a wake.
"""
import argparse, os, queue, re, threading, time
import serial

ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")
EVENTS = re.compile(
    r"1st BL Modem|WW500 MD\.|Cold boot|WAKE signal|Reset by watchdog|SCB->|Fault|HardFault|"
    r"present at 0x|no camera|Camera system not enabled|Frame timed out|restarting sensor|"
    r"Sensor restart failed|giving up|RETRY due to WDT|selfTest [0-9a-f]{4}|Inactive for|"
    r"DATAPATH Init fail|EDM|WDT|not responding|NACK|Git branch|jump_addr|HX_DSP_FLAG|"
    r"Active slot|Switched to slot|Forcing", re.I)


def stamp():
    t = time.time()
    return time.strftime("%H:%M:%S", time.localtime(t)) + f".{int(t % 1 * 1000):03d}"


def render(raw):
    raw = ANSI.sub(b"", raw).rstrip(b"\r")
    return "".join(chr(c) if 32 <= c < 127 or c == 9 else f"\\x{c:02x}" for c in raw)


class Port:
    def __init__(self, name, port, baud, char_delay, out):
        self.name, self.out, self.char_delay = name, out, char_delay
        self.port, self.baud = port, baud
        self.lock = threading.Lock()
        self.ser = None             # opened by reader(), which waits for a busy port
        self.last_rx = time.time()
        self.silence_noted = True

    def open(self):
        ser = serial.Serial()
        ser.port, ser.baudrate, ser.timeout = self.port, self.baud, 0.05
        ser.dtr = ser.rts = False     # DTR/RTS low: never reset a board by opening
        ser.open()
        return ser

    def reopen(self):
        """The adapters power the board, so unplugging it drops the port: wait for it."""
        self.out.line(self.name, f"--- {self.port} lost (board unplugged?) ---", event=True)
        try:
            self.ser.close()
        except Exception:
            pass
        while True:
            time.sleep(0.5)
            try:
                self.ser = self.open()
                self.out.line(self.name, f"--- {self.port} back ---", event=True)
                return
            except Exception:
                pass

    def type(self, cmd):
        self.out.line(self.name, f"<<< typing {cmd!r}", event=True)
        seq = ("\x03" if self.name == "himax" else "") + cmd + "\r\n"
        with self.lock:
            try:
                for ch in seq:
                    self.ser.write(ch.encode())
                    time.sleep(self.char_delay)
            except Exception as e:
                self.out.line(self.name, f"<<< typing failed: {e}", event=True)

    def reader(self):
        while self.ser is None:
            try:
                self.ser = self.open()
                self.out.line(self.name, f"--- {self.port} open ---", event=True)
            except serial.SerialException:
                if not getattr(self, "said", False):
                    self.out.line(self.name, f"--- cannot open {self.port} (another program has "
                                  "it?), waiting ---", event=True)
                    self.said = True
                time.sleep(1)
        buf = b""
        while True:
            try:
                data = self.ser.read(4096)
            except Exception:
                self.reopen()
                continue
            now = time.time()
            if data:
                self.last_rx, self.silence_noted = now, False
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    self.out.line(self.name, render(line))
            elif buf and now - self.last_rx > 0.5:      # flush a partial line (prompts, hangs)
                self.out.line(self.name, render(buf) + "  [no newline]")
                buf = b""
            if not self.silence_noted and now - self.last_rx > 30:
                self.out.line(self.name, "--- silent for 30 s ---", event=True)
                self.silence_noted = True


class Out:
    def __init__(self, d):
        self.log = open(os.path.join(d, "bench.log"), "a", encoding="utf-8", buffering=1)
        self.ev = open(os.path.join(d, "events.log"), "a", encoding="utf-8", buffering=1)
        self.lock = threading.Lock()
        self.coach = None
        self.path = os.path.join(d, "bench.log")

    def last_boot(self):
        """The last boot already in the log, so a restarted window knows a capture's start."""
        boot = None
        with open(self.path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if "himax| Wakeup_event =" in line:
                    boot = "cold" if "Cold boot" in line else "wake"
        return boot

    def line(self, src, text, event=False):
        s = f"[{stamp()}] {src:<5}| {text}"
        with self.lock:
            self.log.write(s + "\n")
            if event or EVENTS.search(text) or "\\x" in text:
                self.ev.write(s + "\n")
        if self.coach:
            self.coach.shown.put((src, s))
            if src == "himax":
                self.coach.lines.put((time.time(), text))


FRAME = re.compile(r"Image capture (\d+)/(\d+)")
GIVE_UP = re.compile(r"TODO - re-initialise camera|giving up|Sensor restart failed")
TROUBLE = re.compile(r"Frame timed out|Fault|unhandled event|\\x00")    # \x00 as render() shows it


class Coach:
    """The --press window: when to press and release, and what the board did afterwards."""

    def __init__(self, out, himax, seconds, cold_capture=None, at_frame=10, probe_after=60,
                 settle=20):
        import tkinter as tk
        self.out, self.himax, self.seconds, self.cold_capture = out, himax, seconds, cold_capture
        self.typed_after_cold = True
        self.at_frame, self.probe_after, self.settle = at_frame, probe_after, settle
        self.lines, self.shown = queue.Queue(), queue.Queue()
        self.boot, self.state, self.tries = out.last_boot(), "idle", 0
        self.next_frame, self.n = at_frame, 0
        self.t0 = self.released = self.last_frame = self.gap = 0.0
        self.recovered = False
        self.root = tk.Tk()
        self.root.title("Ribbon bench")
        self.root.attributes("-topmost", True)
        self.big = tk.Label(self.root, font=("Segoe UI", 34, "bold"), width=16, pady=16)
        self.small = tk.Label(self.root, font=("Segoe UI", 13), wraplength=900, justify="left")
        self.big.pack(fill="x")
        self.small.pack(fill="x", padx=12, pady=(0, 6))
        frame = tk.Frame(self.root)
        frame.pack(fill="both", expand=True)
        bar = tk.Scrollbar(frame)
        bar.pack(side="right", fill="y")
        self.text = tk.Text(frame, font=("Consolas", 9), width=120, height=28, wrap="none",
                            yscrollcommand=bar.set)
        self.text.pack(side="left", fill="both", expand=True)
        bar.config(command=self.text.yview)
        self.text.tag_config("nrf", foreground="#46a")
        self.text.tag_config("note", foreground="#c50", font=("Consolas", 9, "bold"))
        self.show("WAIT", "#888", "Waiting for the board to wake from sleep and start a capture.")
        self.root.after(100, self.tick)

    def show(self, big, colour, small=None):
        self.big.config(text=big, bg=colour, fg="white")
        if small is not None:
            self.small.config(text=small)

    def note(self, text):
        self.out.line("note", text, event=True)

    def beep(self, *tones):
        def run():
            import winsound
            for f, ms in tones:
                winsound.Beep(f, ms)
        threading.Thread(target=run, daemon=True).start()

    def feed(self, t, text):
        if self.cold_capture and not self.typed_after_cold and "Enter 'help'" in text:
            self.typed_after_cold = True
            self.note(f"cold boot: typing {self.cold_capture!r} (as on 11 Oct)")
            threading.Thread(target=self.himax.type, args=(self.cold_capture,), daemon=True).start()
        if "Wakeup_event =" in text:
            self.boot = "cold" if "Cold boot" in text else "wake"
            self.typed_after_cold = self.boot != "cold"
            if self.state in ("watch", "probed", "done", "sleeping"):
                self.note(f"try {self.tries}: the board restarted ({self.boot} boot)")
            self.state, self.next_frame = "idle", self.at_frame
            self.show("WAIT", "#888", f"{'Cold boot' if self.boot == 'cold' else 'Woke from sleep'}"
                      f": waiting for frame {self.at_frame} of its capture." if self.boot == "wake"
                      or self.cold_capture else "Cold boot: wait for it to sleep and wake by "
                      "itself.")
            return
        m = FRAME.search(text)
        if m:
            n, total = int(m.group(1)), int(m.group(2))
            if self.state == "press":
                self.gap = max(self.gap, t - self.last_frame)
            self.last_frame, self.n = t, n
            if self.state == "idle" and n >= self.next_frame:
                if self.boot != "wake" and not (self.boot == "cold" and self.cold_capture):
                    if n == self.at_frame:
                        self.note(f"frame {n} of a capture after a {self.boot} boot: not pressing")
                    return
                if total - n < self.seconds + self.settle + 5:
                    self.show("WAIT", "#888", "This capture ends too soon for another try. The "
                              "next one starts after it sleeps and wakes.")
                    return
                self.tries += 1
                self.state, self.t0, self.gap = "press", t, 0.0
                self.note(f"try {self.tries}: PRESS the HM0360 connector for {self.seconds} s "
                          f"(capture started after a {self.boot} boot)")
                self.beep((1000, 700))
            elif self.state in ("watch", "probed"):
                self.note(f"try {self.tries}: frames resumed {t - self.released:.1f} s after the "
                          f"release (frame {n})")
                self.state, self.t0, self.recovered = "after", t, True
            return
        if self.state in ("press", "after", "watch") and GIVE_UP.search(text):
            self.note(f"try {self.tries}: the firmware gave up on the capture: it should sleep and "
                      "wake by itself")
            self.state, self.t0 = "sleeping", t
            self.show("GIVING UP", "#c60", f"Try {self.tries}: the firmware stopped the camera to "
                      "sleep and wake again. Hands off; RESET only if nothing happens in 3 minutes.")
            return
        if self.state == "sleeping" and "Entering DPD" in text:
            self.note(f"try {self.tries}: asleep {t - self.t0:.0f} s after giving up")
            return
        if self.state == "after" and TROUBLE.search(text):
            self.note(f"try {self.tries}: {text.strip()[:80]!r} {t - self.released:.0f} s after "
                      "the release")
            self.state, self.t0 = "watch", t
            return
        if self.state == "probed" and "selfTest" in text:
            self.state = "done"
            self.note(f"try {self.tries}: answers ({text.strip()}) but no frames: stuck")
            self.show("STUCK", "#c60", f"Try {self.tries}: the console answers but no frames "
                      "came back. Press RESET for the next try.")

    def tick(self):
        while not self.lines.empty():
            self.feed(*self.lines.get())
        if not self.shown.empty():
            at_end = self.text.yview()[1] > 0.999
            for _ in range(3000):
                if self.shown.empty():
                    break
                src, line = self.shown.get()
                self.text.insert("end", line + "\n", src if src in ("nrf", "note") else ())
            extra = int(self.text.index("end-1c").split(".")[0]) - 5000
            if extra > 0:
                self.text.delete("1.0", f"{extra + 1}.0")
            if at_end:
                self.text.see("end")
        now = time.time()
        if self.state == "press":
            left = self.seconds - (now - self.t0)
            if left > 0:
                self.show(f"PRESS  {left:.0f}", "#c22", f"Try {self.tries}: press firmly on "
                          "the HM0360 connector and hold.")
            else:
                self.state, self.t0 = "watch", now
                self.released = now
                self.note(f"try {self.tries}: RELEASE")
                self.beep((1500, 150), (1500, 150))
                if now - self.last_frame < 2.5 and self.gap < 2.5:
                    self.state, self.recovered = "after", False
                    self.note(f"try {self.tries}: frames kept coming through the press")
        if self.state == "after" and now - self.last_frame > 5:
            self.note(f"try {self.tries}: frames stopped {self.last_frame - self.released:.1f} s "
                      "after the release")
            self.state, self.t0 = "watch", now
            self.beep((600, 400))
        if self.state == "after":
            left = self.settle - (now - self.t0)
            word, colour = ("RECOVERED", "#2a7") if self.recovered else ("NO EFFECT", "#46a")
            if now - self.released < 5:
                self.show("RELEASE", "#2a7", f"Try {self.tries}: let go now.")
            else:
                self.show(word, colour, f"Try {self.tries}: frames are coming. Watching "
                          f"{left:.0f} s more for a late failure.")
            if left <= 0:
                self.state, self.next_frame = "idle", self.n + 3
                self.note(f"try {self.tries}: {word.lower()}, nothing more in {self.settle} s")
                self.show(word, colour, f"Try {self.tries}: {word.lower()}. The next PRESS comes "
                          "in a few seconds.")
        if self.state == "watch":
            left = self.probe_after - (now - self.t0)
            self.show("RELEASE", "#2a7", f"Try {self.tries}: hands off. Waiting for frames "
                      f"({left:.0f} s before a selftest probe).")
            if left <= 0:
                self.state, self.t0 = "probed", now
                self.note(f"try {self.tries}: no frames {self.probe_after} s after the "
                          "release, probing")
                threading.Thread(target=self.himax.type, args=("selftest",), daemon=True).start()
        if self.state == "sleeping" and now - self.t0 > 180:
            self.state = "done"
            self.note(f"try {self.tries}: no wake 3 minutes after giving up")
            self.show("NO WAKE", "#555", f"Try {self.tries}: it did not sleep and wake by itself. "
                      "Press RESET for the next try.")
            self.beep((600, 400))
        if self.state == "probed" and now - self.t0 > 15:
            self.state = "done"
            self.note(f"try {self.tries}: no answer to selftest: frozen")
            self.show("FROZEN", "#555", f"Try {self.tries}: no frames and no answer. Press "
                      "RESET for the next try.")
            self.beep((600, 400))
        self.root.after(100, self.tick)


def inbox(path, port, out):
    open(path, "a").close()
    pos = os.path.getsize(path)
    while True:
        time.sleep(0.2)
        size = os.path.getsize(path)
        if size > pos:
            with open(path, encoding="utf-8") as f:
                f.seek(pos)
                new = f.read()
            pos = size
            for cmd in new.splitlines():
                cmd = cmd.strip()
                if cmd.startswith("#"):
                    out.line("note", cmd[1:].strip(), event=True)
                elif cmd and port:
                    port.type(cmd)
                    time.sleep(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--himax", default="COM6")
    ap.add_argument("--nrf", default="COM5")
    ap.add_argument("--press", type=int, default=0, metavar="SECONDS")
    ap.add_argument("--cold-capture", metavar="COMMAND",
                    help="with --press: type this after every cold boot and press in it too")
    a = ap.parse_args()
    os.makedirs(a.dir, exist_ok=True)
    out = Out(a.dir)
    out.line("note", f"capture started: himax={a.himax} nrf={a.nrf} press={a.press}", event=True)
    ports = {}
    for name, port, baud, delay in (("himax", a.himax, 921600, 0.03), ("nrf", a.nrf, 115200, 0.005)):
        p = ports[name] = None if port.lower() == "none" else Port(name, port, baud, delay, out)
        if p:
            threading.Thread(target=p.reader, daemon=True).start()
        threading.Thread(target=inbox, args=(os.path.join(a.dir, f"inbox_{name}.txt"), p, out),
                         daemon=True).start()
    if a.press and ports["himax"]:
        out.coach = Coach(out, ports["himax"], a.press, a.cold_capture)
        out.coach.root.mainloop()
        return
    while True:
        time.sleep(3600)


if __name__ == "__main__":
    main()
