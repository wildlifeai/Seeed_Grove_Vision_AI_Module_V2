"""Bench driver for 2 Oct 2026: holds the Himax console (COM6) and the nRF console (COM5)
open and logged with timestamps, and runs actions appended to cmd.txt, one per line:

  type <console command>     type it on the Himax console (30 ms per char, Ctrl-C first, \r\n)
  flash <hm0360.img> <rp3.img>   stream '1' until a RESET is caught, burn both images
  beep                       sound on the PC
  note <text>                marker in the logs
  catch [timeout]            wait for the next boot's CLI and send a keystroke at once
  keepalive <seconds>        send a keystroke every so often (0 = off; off before any reset)
  wait <seconds>             keep reading for that long
  waitfor <seconds> <regex>  keep reading until the regex appears
  quit

Usage: python bench_daemon.py [output folder, default ./bench]. Ports are COM6 (Himax,
921600) and COM5 (nRF, 115200) on this bench; probe them every session.
"""
import datetime, importlib.util, os, re, sys, threading, time
import serial

# The ship-check tool in this repo, three folders up
TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "_Tools", "ww500_ship_check.py")
spec = importlib.util.spec_from_file_location("w", TOOL); w = importlib.util.module_from_spec(spec); spec.loader.exec_module(w)
try:
    import winsound
except ImportError:
    winsound = None

HIMAX_PORT, NRF_PORT = "COM6", "COM5"
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.getcwd(), "bench")
os.makedirs(OUT, exist_ok=True)
CMD = os.path.join(OUT, "cmd.txt")
STATUS = os.path.join(OUT, "status.txt")
open(CMD, "a").close()


def stamp():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]


class Stamped:
    """Writes bytes as timestamped lines (the time a line's first byte arrived)."""
    def __init__(self, path):
        self.f = open(path, "ab"); self.part = b""; self.t = None

    def feed(self, data):
        data = data.replace(b"\x00", b"")
        while data:
            if self.t is None:
                self.t = stamp()
            i = data.find(b"\n")
            if i < 0:
                self.part += data; break
            line = (self.part + data[:i]).rstrip(b"\r")
            self.f.write(b"[" + self.t.encode() + b"] " + line + b"\n")
            self.part = b""; self.t = None; data = data[i + 1:]
        self.f.flush()

    def mark(self, text):
        self.f.write(f"[{stamp()}] ##### {text}\n".encode()); self.f.flush()


class Con(w.Console):
    def __init__(self, port, ts):
        super().__init__(port); self.ts = ts

    def pump(self):
        data = super().pump()
        if data:
            self.ts.feed(data)
        return data

    def note(self, text):
        super().note(text); self.ts.mark(text)


def status(text):
    with open(STATUS, "w") as f:
        f.write(f"{stamp()} {text}\n")


hts = Stamped(os.path.join(OUT, "himax.log"))
nts = Stamped(os.path.join(OUT, "nrf.log"))
con = Con(HIMAX_PORT, hts)
con.set_log(os.path.join(OUT, "himax_raw.log"))

stop = threading.Event()


def nrf_reader():
    s = serial.Serial(); s.port = NRF_PORT; s.baudrate = 115200; s.timeout = 0.05
    s.dtr = False; s.rts = False; s.open()
    while not stop.is_set():
        d = s.read(4096)
        if d:
            nts.feed(d)


threading.Thread(target=nrf_reader, daemon=True).start()


def burn(path, what):
    con.note(f"burn {what}: {path}")
    status(f"sending {what}")
    if not con.xmodem_send(path, lambda p: status(f"sending {what} {p}%")):
        raise RuntimeError(f"transfer of {what} failed")
    if con.wait_for(w.REBOOT_Q, 10):
        time.sleep(0.3); con.buf = b""; con.ser.write(b"y"); con.note("answered reboot question with y")


def flash(hm, rp):
    status("flash: streaming '1', waiting for a RESET press")
    if not con.flood_until_download(900):
        raise RuntimeError("no RESET seen in 15 min")
    burn(hm, "HM0360")
    status("flash: catching the reboot for image 2")
    if not con.flood_until_download(25):
        status("flash: reboot not caught, press RESET again")
        if not con.flood_until_download(600):
            raise RuntimeError("could not get back to download mode")
    burn(rp, "RP3")
    status("flash: done, booting")


CLI_START = re.compile(rb"Starting CLI Task")


def catch(timeout):
    """Wait for the next boot's CLI and send a keystroke at once (keeps the console awake ~60 s)."""
    status("catch: waiting for a boot")
    if not con.wait_for(CLI_START, timeout):
        raise RuntimeError("no boot seen")
    con.ser.write(b"\r\n")
    con.note("caught the CLI start, sent a keystroke")


with open(CMD) as f:
    done = len(f.read().splitlines())      # a restart does not replay old actions
keepalive = 0
last_key = time.time()
status("running")
while True:
    con.pump()
    if keepalive and time.time() - last_key > keepalive:
        con.ser.write(b"\r\n"); last_key = time.time()
    with open(CMD) as f:
        lines = f.read().splitlines()
    for line in lines[done:]:
        done += 1
        line = line.strip()
        if not line:
            continue
        verb, _, arg = line.partition(" ")
        try:
            if verb == "type":
                con.type_cmd(arg); last_key = time.time()
            elif verb == "catch":
                catch(float(arg or 900)); last_key = time.time()
            elif verb == "keepalive":
                keepalive = float(arg)
            elif verb == "wait":
                t0 = time.time()
                while time.time() - t0 < float(arg):
                    con.pump()
            elif verb == "waitfor":
                secs, _, pat = arg.partition(" ")
                if not con.wait_for(re.compile(pat.encode()), float(secs)):
                    raise RuntimeError(f"timed out waiting for {pat}")
            elif verb == "flash":
                hm, rp = arg.split()
                flash(hm, rp)
            elif verb == "beep" and winsound:
                winsound.Beep(1200, 250)
            elif verb == "note":
                con.note(arg); nts.mark(arg)
            elif verb == "quit":
                stop.set(); status("stopped"); sys.exit(0)
            status(f"ok: {line}")
        except Exception as e:
            con.note(f"ERROR {line}: {e!r}"); status(f"ERROR {line}: {e!r}")
    t = time.time()
    while time.time() - t < 0.2:
        con.pump()
