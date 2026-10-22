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
"""
import argparse, os, re, threading, time
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
        self.ser = self.open()
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

    def line(self, src, text, event=False):
        s = f"[{stamp()}] {src:<5}| {text}"
        with self.lock:
            self.log.write(s + "\n")
            if event or EVENTS.search(text) or "\\x" in text:
                self.ev.write(s + "\n")


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--himax", default="COM6")
    ap.add_argument("--nrf", default="COM5")
    a = ap.parse_args()
    os.makedirs(a.dir, exist_ok=True)
    out = Out(a.dir)
    out.line("note", f"capture started: himax={a.himax} nrf={a.nrf}", event=True)
    for name, port, baud, delay in (("himax", a.himax, 921600, 0.03), ("nrf", a.nrf, 115200, 0.005)):
        p = None if port.lower() == "none" else Port(name, port, baud, delay, out)
        if p:
            threading.Thread(target=p.reader, daemon=True).start()
        threading.Thread(target=inbox, args=(os.path.join(a.dir, f"inbox_{name}.txt"), p, out),
                         daemon=True).start()
    while True:
        time.sleep(3600)


if __name__ == "__main__":
    main()
