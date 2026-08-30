"""Log the mobile app, the nRF BLE processor and the Himax onto one timeline.

A WW500 exchange crosses three processors:

    phone (app)  --BLE-->  nRF52  --I2C-->  Himax HX6538

Each has its own log, and a question like "did the app actually send setop 11?"
can only be answered by seeing all three in time order. This merges them.

Sources (confirmed on this bench, 30 Aug 2026):
    app    adb logcat, filtered to the Wildlife Watcher PID
    nrf    COM5 @ 115200   ("<info> app: ..." and "BLE out: ...")
    himax  COM6 @ 921600   (FreeRTOS CLI, "cmd>" prompt)

Note the app's package name matches a Google system package on 'wildlife', so
the PID filter matters - a name grep picks up com.google.android.apps.privacy.wildlife.

    python tri_log.py --secs 600
    python tri_log.py --secs 600 --himax-cmd "getop -1"

Writes merged.log plus app.log / nrf.log / himax.log. Ctrl+C stops early.
"""
import argparse
import os
import subprocess
import sys
import threading
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
PKG = "com.wildlife.wildlifewatcher"

SERIAL_PORTS = [
    ("himax", "COM6", 921600),
    ("nrf", "COM5", 115200),
]

start = time.monotonic()
lock = threading.Lock()
stop = threading.Event()
HIMAX = {"port": None}

# Lines worth pulling out of the noise into the summary at the end.
INTEREST = ("setop", "getop", "OpParam", "Sleep ", "motion in", "AE Mean",
            "Captured", "BLE out", "capture", "md ", "Wakeup", "Camera:")


def stamp() -> str:
    t = time.monotonic() - start
    return f"{int(t) // 60:02d}:{t % 60:06.3f}"


def write(tag: str, line: str, merged, own) -> None:
    out = f"[{stamp()}] {tag:5s} | {line}\n"
    with lock:
        merged.write(out)
        merged.flush()
        own.write(f"[{stamp()}] {line}\n")
        own.flush()
        sys.stdout.write(out)
        sys.stdout.flush()


def serial_reader(tag, port_name, baud, merged, own, on_line):
    try:
        port = serial.Serial(port_name, baud, timeout=0.05)
    except serial.SerialException as e:
        write(tag, f"! cannot open {port_name}: {e}", merged, own)
        return
    if tag == "himax":
        HIMAX["port"] = port
    buf = b""
    try:
        while not stop.is_set():
            data = port.read(4096)
            if not data:
                continue
            buf += data
            while True:
                nl = buf.find(b"\n")
                if nl < 0:
                    break
                line = buf[:nl].rstrip(b"\r").decode("utf-8", "replace")
                buf = buf[nl + 1:]
                # The Himax colours its output; strip NULs and escapes so the
                # merged log stays greppable.
                line = line.replace("\x00", "")
                if not line.strip():
                    continue
                write(tag, line, merged, own)
                if on_line:
                    on_line(line)
    finally:
        port.close()


def wait_for_app(merged, own):
    """Block until the phone is on adb AND the app has a pid.

    The phone drops off adb whenever the cable is disturbed or USB debugging
    re-authorises, and the app restarts on every Metro reload - both give a new
    pid. Returning None on the first miss meant the whole app leg went dead for
    the rest of a session, so poll instead.
    """
    announced = False
    while not stop.is_set():
        pid = subprocess.run(["adb", "shell", "pidof", PKG],
                             capture_output=True, text=True).stdout.strip()
        if pid:
            return pid.split()[0]
        if not announced:
            write("app", f"! waiting for the phone on adb with {PKG} running "
                         f"(plug it in / unlock it; this leg will attach itself)",
                  merged, own)
            announced = True
        time.sleep(3)
    return None


def logcat_reader(merged, own):
    # Outer loop so a phone unplug or an app restart costs a re-attach, not the
    # rest of the session.
    while not stop.is_set():
        pid = wait_for_app(merged, own)
        if pid is None:
            return
        write("app", f"attached to pid {pid}", merged, own)
        # Restrict to the JS layer. Everything the app logs itself goes through
        # ReactNativeJS; without this filter the native linker/SoLoader chatter
        # is ~50 lines per second and buries it.
        proc = subprocess.Popen(["adb", "logcat", "-v", "time", f"--pid={pid}",
                                 "ReactNativeJS:V", "ReactNative:W", "*:S"],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, errors="replace", bufsize=1)

        # `adb logcat --pid=N` does NOT exit when process N dies - it just goes
        # quiet forever, so the read loop below blocks and the re-attach never
        # runs. That silently cost a whole app-side capture. Poll for the pid
        # changing and kill the reader ourselves.
        def watch_pid():
            while not stop.is_set() and proc.poll() is None:
                time.sleep(2)
                now = subprocess.run(["adb", "shell", "pidof", PKG],
                                     capture_output=True, text=True).stdout.strip()
                if now and now.split()[0] != pid:
                    proc.terminate()
                    return
        threading.Thread(target=watch_pid, daemon=True).start()

        try:
            for line in proc.stdout:
                if stop.is_set():
                    break
                line = line.rstrip()
                if not line.strip():
                    continue
                # Drop the logcat date/level preamble; keep the message.
                if "): " in line:
                    line = line.split("): ", 1)[1]
                write("app", line, merged, own)
        finally:
            proc.terminate()
        if not stop.is_set():
            write("app", "! logcat ended (phone unplugged or app restarted) - re-attaching",
                  merged, own)


def send_himax(text: str) -> None:
    """Slow-type into the Himax CLI (1-char IRQ buffer drops burst writes)."""
    port = HIMAX.get("port")
    if port is None:
        return
    for ch in (text.encode("ascii", "replace") + b"\r\n"):
        port.write(bytes([ch]))
        port.flush()
        time.sleep(0.006)
    with lock:
        sys.stdout.write(f"[{stamp()}] >>>>> | {text}\n")
        sys.stdout.flush()


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except AttributeError:
        pass

    ap = argparse.ArgumentParser()
    ap.add_argument("--secs", type=float, default=600)
    ap.add_argument("--himax-cmd", default="",
                    help="';'-separated commands to send when the Himax wakes")
    ap.add_argument("--outdir", default=HERE)
    ap.add_argument("--no-app", action="store_true", help="skip the logcat source")
    args = ap.parse_args()

    merged = open(os.path.join(args.outdir, "merged.log"), "a", encoding="utf-8")
    merged.write(f"\n\n===== session {time.strftime('%Y-%m-%d %H:%M:%S')} =====\n")

    pending = [c.strip() for c in args.himax_cmd.split(";") if c.strip()]
    fired = threading.Event()

    def on_himax_line(line: str):
        # An untouched boot sleeps after ~1s and does NOT wake on UART, so a
        # byte has to go in the moment the CLI is alive.
        if pending and not fired.is_set():
            if any(m in line for m in ("Starting CLI Task", "cmd>", "available commands")):
                fired.set()
                threading.Thread(target=run_cmds, args=(pending,), daemon=True).start()

    def run_cmds(cmds):
        send_himax("")      # keepalive first, buys ~60s
        time.sleep(1.5)
        for c in cmds:
            send_himax(c)
            time.sleep(3.0)

    files = {}
    for tag in ("himax", "nrf", "app"):
        f = open(os.path.join(args.outdir, f"{tag}.log"), "a", encoding="utf-8")
        f.write(f"\n===== session {time.strftime('%Y-%m-%d %H:%M:%S')} =====\n")
        files[tag] = f

    for tag, port_name, baud in SERIAL_PORTS:
        cb = on_himax_line if tag == "himax" else None
        threading.Thread(target=serial_reader,
                         args=(tag, port_name, baud, merged, files[tag], cb),
                         daemon=True).start()

    if not args.no_app:
        threading.Thread(target=logcat_reader,
                         args=(merged, files["app"]), daemon=True).start()

    try:
        deadline = time.monotonic() + args.secs
        while time.monotonic() < deadline:
            time.sleep(0.2)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        time.sleep(0.3)
        merged.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
