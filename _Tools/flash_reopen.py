#!/usr/bin/env python3
"""WW500 flasher for the reopen-per-reset USB topology: the COM port
re-enumerates whenever the device resets or power-cycles, so every serial
operation tolerates the port vanishing and reopens it.

Flow: catch the device (power-cycle or awake) -> if awake: keep-awake,
'reset', close+fast-reopen to catch the bootloader -> '1' -> xmodem burn ->
'y' reboot -> reopen again -> verify boot -> keep-awake + ver."""
import argparse
import re
import sys
import time

import serial
import xmodem

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except AttributeError:
    pass

ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
CHAR_DELAY = 0.03
APP_BOOT_MARKERS = ("Inactivity period set", "Starting Image Task",
                    "available commands", "WW500 MD", "Starting CLI Task")
BL_MARKERS_RE = re.compile(
    r"reset by watchdog|bootloader|modem build|xmodem|BL_", re.IGNORECASE)


def note(s):
    s = s.rstrip()
    if s:
        print(s, flush=True)


class Robust:
    """Serial wrapper that reopens the port whenever it vanishes."""

    def __init__(self, port, baud):
        self.port = port
        self.baud = baud
        self.ser = None
        self.reopened = False   # set when the port died and came back

    def ensure(self, deadline_s=120.0, quiet=False):
        if self.ser is not None:
            return True
        end = time.monotonic() + deadline_s
        while time.monotonic() < end:
            try:
                self.ser = serial.Serial(self.port, self.baud, timeout=0.2)
                if not quiet:
                    note("    (port %s opened)" % self.port)
                return True
            except serial.SerialException:
                time.sleep(0.05)
        return False

    def drop(self):
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None

    def read(self, n):
        if not self.ensure():
            return b""
        try:
            return self.ser.read(n)
        except serial.SerialException:
            note("    (port lost - reopening)")
            self.drop()
            if self.ensure(30):
                self.reopened = True   # a device boot is in progress NOW
            return b""

    def write(self, data):
        if not self.ensure():
            return
        try:
            self.ser.write(data)
            self.ser.flush()
        except serial.SerialException:
            note("    (port lost during write - reopening)")
            self.drop()


def read_line(rs, timeout_s):
    deadline = time.monotonic() + timeout_s
    buf = bytearray()
    while time.monotonic() < deadline:
        b = rs.read(1)
        if not b:
            continue
        if b == b"\n":
            break
        buf.extend(b)
        if len(buf) > 4096:
            break
    return ANSI_RE.sub("", buf.decode("utf-8", "replace")).strip("\r\n \t")


def send_slow(rs, text):
    for ch in text:
        rs.write(ch.encode("ascii", "replace"))
        time.sleep(CHAR_DELAY)


def send_cmd_verified(rs, cmd, attempts=6, echo_timeout=3.0):
    for _ in range(attempts):
        send_slow(rs, cmd + "\r\n")
        deadline = time.monotonic() + echo_timeout
        seen = ""
        while time.monotonic() < deadline:
            line = read_line(rs, 0.3)
            if line:
                note("    " + line)
                seen += line + "\n"
                if cmd in seen:
                    return True
    return False


def spam_key(rs, duration_s):
    """Hammer '1\\r' while draining input; return accumulated text."""
    end = time.monotonic() + duration_s
    acc = bytearray()
    while time.monotonic() < end:
        rs.write(b"1\r")
        data = rs.read(256)
        if data:
            acc.extend(data)
            if b"xmodem protocol" in acc.lower():
                break
        time.sleep(0.01)
    text = ANSI_RE.sub("", acc.decode("utf-8", "replace"))
    for ln in text.splitlines():
        if ln.strip():
            note("    " + ln.strip())
    return text.lower()


def catch_bootloader(rs, window_s):
    """After a reset/cycle: reopen fast and drive the bootloader to xmodem.
    Returns 'xmodem' | 'app' (missed the window) | None (nothing seen).
    The '1' keypress window is only 30 ms, so on ANY bootloader marker the
    key is hammered every 10 ms rather than written once per parsed line."""
    end = time.monotonic() + window_s
    saw_app = False
    while time.monotonic() < end:
        line = read_line(rs, 0.4)
        if not line:
            continue
        note("    " + line)
        low = line.lower()
        if BL_MARKERS_RE.search(line):
            text = spam_key(rs, 3.0)
            if "xmodem protocol" in text:
                return "xmodem"
        if "send data using the xmodem protocol" in low:
            return "xmodem"
        if any(m in line for m in APP_BOOT_MARKERS):
            saw_app = True
        if saw_app and "cmd>" in low:
            return "app"
    return "app" if saw_app else None


def xmodem_burn(rs, image):
    note("== xmodem transfer ==")
    time.sleep(1)
    try:
        rs.ser.reset_input_buffer()
    except Exception:
        pass
    rs.write(b"1\r")
    # the bootloader stalls to erase flash between blocks: give reads a
    # full second (the wrapper port default of 0.2 s starves the protocol)
    if rs.ensure():
        rs.ser.timeout = 1

    def getc(size, timeout=1):
        deadline = time.monotonic() + max(float(timeout), 1.0)
        data = b""
        while len(data) < size and time.monotonic() < deadline:
            chunk = rs.read(size - len(data))
            if chunk:
                data += chunk
        return data if data else None

    def putc(data, timeout=1):
        rs.write(data)
        return len(data)

    modem = xmodem.XMODEM(getc=getc, putc=putc, mode="xmodem")
    with open(image, "rb") as f:
        return modem.send(f)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM13")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--image", required=True)
    ap.add_argument("--wait-min", type=float, default=45.0)
    args = ap.parse_args()

    rs = Robust(args.port, args.baud)
    rs.ensure(10, quiet=True)

    note("== waiting up to %.0f min for the device (power-cycle it) ==" % args.wait_min)
    wait_deadline = time.monotonic() + args.wait_min * 60
    mode = None
    while time.monotonic() < wait_deadline:
        line = read_line(rs, 1.0)
        if rs.reopened:
            # the port just died and returned: a boot is happening RIGHT NOW.
            # The bootloader banner may have scrolled during enumeration, so
            # hammer the key blindly - if the 30 ms window is still open (or
            # opens in the next moments) this catches it.
            rs.reopened = False
            note("== port re-enumerated (device booting) - blind key hammer ==")
            text = spam_key(rs, 4.0)
            if "xmodem protocol" in text:
                mode = "xmodem"
                break
            continue
        if not line:
            continue
        note("    " + line)
        if BL_MARKERS_RE.search(line):
            note("== bootloader detected - hammering the key ==")
            text = spam_key(rs, 3.0)
            mode = "xmodem" if "xmodem protocol" in text else "bootloader"
            break
        note("== app is awake ==")
        mode = "app"
        break
    if mode is None:
        note("!! device never appeared")
        return 2

    if mode == "bootloader":
        state = catch_bootloader(rs, 60)
        mode = "xmodem" if state == "xmodem" else "app"
        if mode == "app":
            note("!! bootloader xmodem prompt not reached from cold boot")

    if mode == "xmodem":
        pass  # already at the xmodem prompt
    elif mode == "app":
        time.sleep(3)
        note("== keep-awake, then CLI reset (up to 4 attempts) ==")
        if not send_cmd_verified(rs, "setop 8 60000"):
            note("!! CLI not responding")
            return 3
        got = False
        for attempt in range(1, 5):
            note("== reset attempt %d: sending 'reset', reopening fast ==" % attempt)
            send_slow(rs, "reset\r\n")
            time.sleep(0.2)
            rs.drop()          # the reset re-enumerates the port
            if not rs.ensure(30):
                note("!! port never came back")
                return 4
            state = catch_bootloader(rs, 25)
            if state == "xmodem":
                got = True
                break
            if state == "app":
                note("    (missed the bootloader window - app booted; retrying)")
                time.sleep(2)
                send_cmd_verified(rs, "setop 8 60000", attempts=3)
                continue
            note("    (nothing seen after reset - retrying)")
            rs.drop()
            rs.ensure(15)
        if not got:
            note("!! could not reach the bootloader xmodem prompt")
            return 4

    if not xmodem_burn(rs, args.image):
        note("!! xmodem send FAILED")
        return 5
    note("xmodem send done")

    note("== reboot into new firmware ==")
    deadline = time.monotonic() + 120
    answered = False
    booted = False
    while time.monotonic() < deadline:
        line = read_line(rs, 0.5)
        if line:
            note("    " + line)
        low = line.lower()
        if not answered and "reboot system" in low and "(y)" in low:
            rs.write(b"y\r")
            answered = True
            # the reboot may re-enumerate the port as well
        if any(m in line for m in APP_BOOT_MARKERS):
            booted = True
            break
    if not booted:
        note("!! new firmware boot banner not seen")
        return 6

    note("== keep awake + version ==")
    time.sleep(2)
    send_cmd_verified(rs, "setop 8 60000")
    send_slow(rs, "ver\r\n")
    t_end = time.monotonic() + 4
    while time.monotonic() < t_end:
        line = read_line(rs, 0.3)
        if line:
            note("    " + line)

    rs.drop()
    note("== FLASH SEQUENCE COMPLETE ==")
    return 0


if __name__ == "__main__":
    sys.exit(main())
