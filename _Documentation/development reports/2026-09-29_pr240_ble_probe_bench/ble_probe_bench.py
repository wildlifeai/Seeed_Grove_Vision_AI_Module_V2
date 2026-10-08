"""
PR #240 bench: does the 300 ms BLE probe (BLE_PROBE_TIME in if_task.c) ever fire on a board
that HAS a BLE processor? Bench board WILD-7VQI: Himax console COM6 at 921600, nRF console
COM5 at 115200 (logged alongside, with timestamps).

Phases (argument 1, then optional N):
  flash    Victor presses RESET at the beep; the PR #240 ww500_md HM0360 image is burnt over
           XMODEM (backup slot, which becomes active). Then N watchdog-reset cycles (default 20):
           at each boot 'reset' is typed, the board sleeps by inactivity 1 s later and reboots.
           Then M timelapse DPD wakes (op 7 = 20 s, default 10) with nothing typed while they run.
  app      Victor connects the phone app, presses RESET at the beep; N reset cycles (default 10).
  restore  RESET at the beep; the production pair back, both slots labelled, op 7 / op 8 as found.

Per boot the AI console is scanned for: the probe failure line, 'I2C master did not read', the
first command received from the nRF ('selftest' follows a Wake it has read), dropped messages,
the inactivity time, the sleep path the IF task took, and how the boot ended (DPD or watchdog).
Results: ble_probe_results.csv, status.txt, ai_console.log, nrf_console.log.
"""
import csv, datetime, importlib.util, json, os, re, sys, threading, time
import serial
try:
    import winsound
except ImportError:
    winsound = None

# The ship-check tool in this repo, three folders up
TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "_Tools", "ww500_ship_check.py")
spec = importlib.util.spec_from_file_location("w", TOOL); w = importlib.util.module_from_spec(spec); spec.loader.exec_module(w)
# The bench PC's images and results folder on 29 Sep; change it for another run
S = r"C:\Users\Victor\AppData\Local\Temp\claude\C--Users-ww\4734aec8-67a9-407b-8080-2dd46f8ce1d4\scratchpad"
OUT = os.path.join(S, "pr240", "bench")
PR_HM = os.path.join(S, "pr240", "build", "md_HM0360", "output.img")
PROD_HM = os.path.join(S, "ship_images", "firmware-image-HM0360", "WW500_C02_HM0360_20260923041821.img")
PROD_RP = os.path.join(S, "ship_images", "firmware-image-RP3", "WW500_C02_RP3_20260923041826.img")
AI_PORT, NRF_PORT = "COM6", "COM5"
PHASE = sys.argv[1] if len(sys.argv) > 1 else "flash"
N = int(sys.argv[2]) if len(sys.argv) > 2 else {"flash": 20, "app": 10}.get(PHASE, 10)
M = int(sys.argv[3]) if len(sys.argv) > 3 else 10          # timer wakes in the flash phase
CSV = os.path.join(OUT, f"ble_probe_results_{PHASE}.csv")
ORIG = os.path.join(OUT, "orig_ops.json")

MARKS = {
    "cli":        re.compile(rb"Enter 'help'"),
    "wake_sent":  re.compile(rb"Sending \d+ bytes: Header 4, payload \d+, checksum 2 '(Wake|Timer|MD) "),
    "wake_read":  re.compile(rb"I2C transmission complete"),
    "cold":       re.compile(rb"Cold boot"),
    "wake_evt":   re.compile(rb"Wakeup_event = (0x[0-9a-f]+), WakeupEvt1 = (0x[0-9a-f]+)"),
    "probe_fail": re.compile(rb"BLE processor did not read the first message within (\d+)ms"),
    "mm_timer":   re.compile(rb"I2C master did not read our I2C message"),
    "nrf_cmd":    re.compile(rb"MKL62BA command received: '([^']*)'"),
    "dropped":    re.compile(rb"BLE processor unresponsive - not sending message to master"),
    "contacted":  re.compile(rb"BLE processor has contacted us"),
    "inactive":   re.compile(rb"Inactive for (\d+)ms"),
    "ready":      re.compile(rb"IF task ready to sleep([^\r\n]*)\."),
    "dpd":        re.compile(rb">>> Entering DPD"),
    "wdt":        re.compile(rb">>> Reset by watchdog"),
}
rows = []


def say(msg):
    line = f"{time.strftime('%H:%M:%S')} {msg}"
    print(line, flush=True)
    with open(os.path.join(OUT, "status.txt"), "a") as f:
        f.write(line + "\n")


def beep(n):
    if winsound:
        for _ in range(n):
            winsound.Beep(1200, 250); time.sleep(0.15)


class NrfLog(threading.Thread):
    """Logs the nRF console with a host timestamp per chunk. Best effort."""
    def __init__(self, port, path):
        super().__init__(daemon=True)
        self.stop = False
        self.ser = serial.Serial()
        self.ser.port, self.ser.baudrate, self.ser.timeout = port, 115200, 0.05
        self.ser.dtr = False; self.ser.rts = False
        self.ser.open()
        self.f = open(path, "ab")

    def run(self):
        while not self.stop:
            data = self.ser.read(4096)
            if data:
                self.f.write(f"\n[{datetime.datetime.now():%H:%M:%S.%f}] ".encode() + data)
                self.f.flush()


os.makedirs(OUT, exist_ok=True)
con = w.Console(AI_PORT)
con.set_log(os.path.join(OUT, "ai_console.log"))
con.note(f"phase {PHASE} N={N} M={M}")
try:
    nrf = NrfLog(NRF_PORT, os.path.join(OUT, "nrf_console.log")); nrf.start()
    say(f"nRF console {NRF_PORT} logged")
except Exception as e:
    nrf = None
    say(f"nRF console not logged ({e})")


def cmd(c, expect, timeout=1.5, tries=5):
    return con.command(c, expect, timeout=timeout, tries=tries)


def burn(path, wait_s):
    if not con.flood_until_download(wait_s):
        raise SystemExit("bootloader never seen")
    if not con.xmodem_send(path, lambda p: None):
        raise SystemExit(f"xmodem failed for {path}")
    if con.wait_for(w.REBOOT_Q, 10):
        time.sleep(0.3); con.buf = b""; con.ser.write(b"y")
    say(f"flashed {os.path.basename(os.path.dirname(path))}/{os.path.basename(path)}")


def wait_boot(timeout):
    """Waits for the banner, then the CLI. Returns True if both came."""
    con.hist = b""
    mb = con.wait_for(w.BANNER, timeout, keep=True)
    if not mb:
        return False
    con.hist = con.buf[mb.start():]
    return bool(con.wait_for(MARKS["cli"], 15, keep=True))


def save_row(row):
    rows.append(row)
    new = not os.path.exists(CSV)
    with open(CSV, "a", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=list(row))
        if new:
            wr.writeheader()
        wr.writerow(row)


def cycle(phase, i, type_at_cli=None, expect=None, then_cmd=None, then_after_s=8, banner_timeout=120, end_timeout=45):
    """One boot: wait for the banner, optionally type a command once the CLI is up (and a second
    one, e.g. 'dpd', once the nRF's first command has been seen or then_after_s has passed: console
    typing holds the board awake for 60 s, so the sleep has to be forced), scan the console until
    the board sleeps (DPD) or the watchdog resets it. Returns the row, or None."""
    con.hist = b""
    mb = con.wait_for(w.BANNER, banner_timeout, keep=True)     # keep: the banner may already be in the buffer
    if not mb:
        say(f"{phase} {i}: no boot banner within {banner_timeout}s")
        return None
    tb = time.time()
    con.hist = con.buf[mb.start():]      # this boot only: the previous boot's last lines were still in the buffer
    seen = {}
    typed = tries = then_typed = then_tries = 0
    while time.time() - tb < end_timeout:
        con.pump()
        seg = con.hist
        for name, rx in MARKS.items():
            if name not in seen:
                m = rx.search(seg)
                if m:
                    seen[name] = (round(time.time() - tb, 2), m)
        if type_at_cli and "cli" in seen and typed == 0:
            con.type_cmd(type_at_cli); typed = 1; t_typed = time.time()
        if type_at_cli and typed == 1 and expect and expect not in con.hist and time.time() - t_typed > 2 and tries < 3:
            con.type_cmd(type_at_cli); tries += 1; t_typed = time.time()
        if then_cmd and typed and not then_typed and ("nrf_cmd" in seen or time.time() - tb > then_after_s):
            time.sleep(0.5); con.pump()
            con.type_cmd(then_cmd); then_typed = 1; t_then = time.time()
        if then_cmd and then_typed and b"Forcing DPD" not in con.hist and time.time() - t_then > 2.5 and then_tries < 3:
            con.type_cmd(then_cmd); then_tries += 1; t_then = time.time()
        if "dpd" in seen or "wdt" in seen:
            mk = seen["dpd"][1] if "dpd" in seen else seen["wdt"][1]
            con.buf = con.hist[mk.end():]     # what follows the marker belongs to the next boot (the reboot is quick)
            break
    seg = con.hist
    kind = "cold" if "cold" in seen else ("warm " + seen["wake_evt"][1].group(1).decode() if "wake_evt" in seen else "?")
    cmds = [m.group(1).decode(errors="replace") for m in MARKS["nrf_cmd"].finditer(seg)]
    row = {
        "phase": phase, "boot": i, "time": time.strftime("%H:%M:%S", time.localtime(tb)), "kind": kind,
        "first_msg": seen["wake_sent"][1].group(1).decode() if "wake_sent" in seen else "",
        "wake_sent_s": seen["wake_sent"][0] if "wake_sent" in seen else "",
        "wake_read_s": seen["wake_read"][0] if "wake_read" in seen else "",
        "probe_fail": "probe_fail" in seen,
        "mm_timer": len(MARKS["mm_timer"].findall(seg)),
        "nrf_first_cmd_s": seen["nrf_cmd"][0] if "nrf_cmd" in seen else "",
        "nrf_cmds": " ".join(cmds[:6]),
        "dropped": len(MARKS["dropped"].findall(seg)),
        "contacted": "contacted" in seen,
        "inactive_s": seen["inactive"][0] if "inactive" in seen else "",
        "if_sleep_path": seen["ready"][1].group(1).decode(errors="replace").strip() or "Sleep msg read" if "ready" in seen else "none",
        "end": "DPD" if "dpd" in seen else ("watchdog" if "wdt" in seen else "timeout"),
        "end_s": seen["dpd"][0] if "dpd" in seen else (seen["wdt"][0] if "wdt" in seen else ""),
        "typed_ok": ((expect in seg) and (not then_cmd or b"Forcing DPD" in seg)) if (type_at_cli and expect) else "",
    }
    save_row(row)
    say(f"{phase} {i}: {kind}; '{row['first_msg']}' sent {row['wake_sent_s'] or '-'}s read {row['wake_read_s'] or '-'}s; "
        f"probe {'FAILED' if row['probe_fail'] else 'ok'}; nRF first cmd "
        f"{row['nrf_first_cmd_s'] or '-'}s ({row['nrf_cmds'] or 'none'}); mm_timer {row['mm_timer']}; "
        f"IF sleep path '{row['if_sleep_path']}'; {row['end']} at {row['end_s'] or '-'}s")
    return row


def summary(phase):
    ph = [r for r in rows if r["phase"] == phase]
    if not ph:
        return
    say(f"SUMMARY {phase}: {len(ph)} boots, probe failed {sum(r['probe_fail'] for r in ph)}, "
        f"mm_timer boots {sum(1 for r in ph if r['mm_timer'])}, nRF answered {sum(1 for r in ph if r['nrf_first_cmd_s'] != '')}, "
        f"ended by DPD/watchdog {sum(1 for r in ph if r['end'] != 'timeout')}, "
        f"nRF first cmd s: {sorted(r['nrf_first_cmd_s'] for r in ph if r['nrf_first_cmd_s'] != '')}")


try:
    if PHASE in ("flash", "cycles"):
        awake = False
        if PHASE == "cycles":
            try:                                   # still awake from a previous run? then no RESET needed
                cmd("getop 8", re.compile(rb"OpParam 8 = (\d+)"), timeout=1.0, tries=2)
                awake = True
                say("board is awake, no RESET needed")
            except w.Failed:
                pass
        if not awake:
            say("Press RESET on the bench board now"); beep(1)
        if PHASE == "flash":
            burn(PR_HM, 300)
            if not wait_boot(40):
                raise SystemExit("PR image did not boot")
        elif not awake and not wait_boot(300):
            raise SystemExit("no boot after RESET")
        ver = w.BANNER.search(con.hist)
        say(f"booted: {ver.group(0).decode(errors='replace') if ver else 'no banner?'}")
        orig = {str(n): cmd(f"getop {n}", re.compile(rb"OpParam %d = (\d+)" % n)).group(1).decode() for n in (7, 8, 24)}
        with open(ORIG, "w") as f:
            json.dump(orig, f)
        cam = w.CAMERA.search(con.hist); sd = w.SD_STATE.search(con.hist)
        say(f"board: op7={orig['7']} op8={orig['8']} op24={orig['24']}; {cam.group(0).decode() if cam else 'camera ?'}; "
            f"{sd.group(0).decode() if sd else 'SD ?'}")
        if orig["8"] != "1000":
            cmd("setop 8 1000", b"Set OpParam 8 = 1000")
        cmd("setop 7 0", b"Set OpParam 7 = 0")
        cmd("reset", b"Forcing reset soon"); cmd("dpd", b"Forcing DPD")
        for i in range(1, N + 1):
            if cycle("reset", i, type_at_cli="reset", expect=b"Forcing reset soon", then_cmd="dpd", banner_timeout=90) is None:
                break
        summary("reset")
        # the boot after the last watchdog reset: set the timelapse and sleep
        if wait_boot(60):
            cmd("setop 7 20", b"Set OpParam 7 = 20")
            cmd("dpd", b"Forcing DPD")
            say(f"timelapse 20 s set; {M} timer wakes with nothing typed")
            for i in range(1, M + 1):
                if cycle("timer", i, banner_timeout=90) is None:
                    break
            summary("timer")
            if wait_boot(90):
                cmd("setop 7 0", b"Set OpParam 7 = 0")
                cmd("dpd", b"Forcing DPD")
        say(f"PHASE {PHASE} DONE. Board asleep on the PR #240 HM0360 image. Next: connect the app, then phase 'app'.")

    elif PHASE == "app":
        say("App connected? Press RESET on the bench board now"); beep(1)
        for i in range(1, N + 1):
            if cycle("app", i, type_at_cli="reset", expect=b"Forcing reset soon", then_cmd="dpd", banner_timeout=300 if i == 1 else 90) is None:
                break
        summary("app")
        say("PHASE app DONE (the board reboots once more and sleeps by itself)")

    elif PHASE == "restore":
        with open(ORIG) as f:
            orig = json.load(f)
        say("Press RESET on the bench board now"); beep(1)
        # Only the RESET boot counts ('Cold boot'): a WAKE or motion boot just before it must not be acted on
        t0 = time.time()
        while True:
            if time.time() - t0 > 300 or not wait_boot(300):
                raise SystemExit("no cold boot")
            if b"Cold boot" in con.hist:
                break
            say("a non-RESET boot (WAKE or motion): still waiting for the RESET boot")
            con.buf = b""
        cmd("getop 8", re.compile(rb"OpParam 8 = (\d+)"))       # holds the board awake 60 s
        # let the start-up exchange with the nRF finish (Wake, selftest, reply) before asking for the reset
        t_q = time.time(); n = len(con.hist)
        while time.time() - t_q < 2.0:
            con.pump()
            if len(con.hist) != n:
                n = len(con.hist); t_q = time.time()
        cmd("reset", b"Forcing reset soon"); cmd("dpd", b"Forcing DPD")
        if not con.wait_for(re.compile(rb">>> Reset by watchdog"), 20, keep=True):
            raise SystemExit("no watchdog reset after 'reset' + 'dpd'")
        say("watchdog reset seen: catching the bootloader")
        burn(PROD_HM, 40)
        burn(PROD_RP, 40)
        wait_boot(40)
        lab = con.wait_for(w.LABEL, 15, keep=True)
        say(f"production RP3 booted, {lab.group(0).decode() if lab else 'NO LABEL LINE'}")
        cmd("setop 7 " + orig["7"], f"Set OpParam 7 = {orig['7']}".encode())
        cmd("switchslot", re.compile(rb"Switched to slot \d"))
        cmd("setop 8 " + orig["8"], f"Set OpParam 8 = {orig['8']}".encode())
        cmd("dpd", b"Forcing DPD")
        wait_boot(60)
        lab = con.wait_for(w.LABEL, 15, keep=True)
        cmd("setop 8 60000", b"Set OpParam 8 = 60000")
        slots = cmd("slots", re.compile(rb"Active slot[^\r\n]*")).group(0).decode(errors="replace")
        cmd("setop 8 " + orig["8"], f"Set OpParam 8 = {orig['8']}".encode())
        cmd("dpd", b"Forcing DPD")
        ok = "unknown" not in slots and "running 'HM0360" in slots
        say(f"restored: {lab.group(0).decode() if lab else 'no label line'}; {slots}; op7={orig['7']} op8={orig['8']}; "
            f"{'LABELS OK' if ok else 'CHECK LABELS'}")
        say("PHASE restore DONE")
    elif PHASE == "watch":
        # Passive: log the console for N seconds while the operator runs a file transfer from the app,
        # then count the probe and missing-master lines and show the file-transfer lines.
        say(f"watching the console for {N} s: start the app's File Transfer Test now")
        t0 = time.time(); con.hist = b""
        while time.time() - t0 < N:
            con.pump()
        seg = con.hist
        counts = {k: len(MARKS[k].findall(seg)) for k in ("probe_fail", "mm_timer", "dropped", "contacted", "wake_sent")}
        files = [m.decode(errors="replace").strip() for m in re.findall(rb"[^\r\n]*(?:FILE|file|Deferring|packet)[^\r\n]*", seg)]
        say(f"WATCH: boots {len(w.BANNER.findall(seg))}; {counts}; {len(files)} file-related lines, first: {files[:6]}, last: {files[-4:]}")
        say("PHASE watch DONE")
    else:
        raise SystemExit(f"unknown phase {PHASE}")
except w.Failed as e:
    say(f"FAILED: {e}")
finally:
    if nrf:
        nrf.stop = True
