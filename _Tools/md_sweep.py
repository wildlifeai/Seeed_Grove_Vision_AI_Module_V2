#!/usr/bin/env python3
"""Motion-detection register-sweep harness for the WW500 (HM0360 MD engine).

Characterises MD parameters on the bench over the console UART: for each register
value it writes the register, cues the physical stimulus, reads the motion-block
count, and logs a CSV row. This is Phase 1 of the motion-preset roadmap
(doc/Motion_detection_presets.md).

Two register transports (--via):
  camreg (DEFAULT) - uses the stock `camreg <addr> [<val>]` command that already
      ships in current firmware, so it runs on the device AS FLASHED, no reflash.
      Reads/writes go straight to the HM0360; the motion-block count is derived
      host-side by popcounting the 32 MD_ROI_OUT registers (0x20A1..0x20C0).
  md - uses the `md read/write/grid/dump` instrumentation from
      feat/md-instrumentation (single-round-trip grid read, tidier) once flashed.

Prerequisite (a human/hardware step this tool can't do for you):
  The device must be AWAKE - it sleeps in DPD and wakes on motion/RTC/BLE, not
  UART. Power-cycle it (or wave at the lens if MD is armed), then this tool's
  --keep-awake pushes OP 8 (interval-before-DPD) out for the run. Running
  live_view.py first, or any recent command, also holds it up.

Examples:
  python md_sweep.py --selftest                       # validate parsers, no device
  python md_sweep.py --dump --keep-awake              # snapshot MD registers (camreg)
  python md_sweep.py --sweep 35a6 1 16 1 --csv bn.csv # sweep MD_BLOCK_NUM_TH_B 1..16
  python md_sweep.py --preset 4                        # apply the P4 (Micro) bundle
  python md_sweep.py --sweep 209d 0x11 0x55 0x11 --auto 3   # unattended, 3s/step
  python md_sweep.py --dump --via md                  # once feat/md-instrumentation flashed

CSV columns: ts, addr, value, blocks, note
"""

import argparse
import csv
import re
import sys
import time

# Context-B MD registers (the WW500 MD path) + the shared ones. See hm0360_regs.h.
REG = {
    "MD_TH_STR_L_B": 0x35AA, "MD_TH_STR_H_B": 0x35A9,
    "MD_BLOCK_NUM_TH_B": 0x35A6, "MD_LATENCY_TH": 0x209D,
    "MD_IIR_PARAM": 0x209A, "ROI_V_B": 0x35A7, "ROI_H_B": 0x35A8,
}

# Registers printed by --dump, in a readable order (name, addr). Mirrors the
# `md dump` instrumentation but works over camreg on stock firmware.
DUMP_REGS = [
    ("MD_CTRL_B (ctx-B enable)", 0x35A5), ("MD_CTRL (main)", 0x2080),
    ("MD_TH_STR_H_B", 0x35A9), ("MD_TH_STR_L_B", 0x35AA),
    ("MD_BLOCK_NUM_TH_B", 0x35A6), ("MD_BLOCK_NUM_TH (main)", 0x209B),
    ("MD_LATENCY (main)", 0x209C), ("MD_LATENCY_TH", 0x209D),
    ("MD_IIR_PARAM", 0x209A), ("MD_LIGHT_COEF", 0x2099),
    ("ROI_V_B", 0x35A7), ("ROI_H_B", 0x35A8),
    ("INT_INDIC (MD_INT=0x08)", 0x2064),
]

# Starting-hypothesis preset bundles (doc/Motion_detection_presets.md §3). These are
# to be REPLACED by the values this sweep measures - they are a bench start point.
# (addr, value) pairs written in order; Delta-t is set separately via OP 11.
PRESETS = {
    1: ("High-Noise/Medium", [(0x35AA, 0x28), (0x35A9, 0x28), (0x35A6, 0x0C),
                              (0x209D, 0x55), (0x209A, 0xF0)]),
    2: ("Macro/Ultra-slow",  [(0x35AA, 0x18), (0x35A9, 0x18), (0x35A6, 0x01),
                              (0x209D, 0x11), (0x209A, 0x00)]),
    3: ("High-Velocity",     [(0x35AA, 0x20), (0x35A9, 0x20), (0x35A6, 0x03),
                              (0x209D, 0x00), (0x209A, 0x00)]),
    4: ("Micro/Small",       [(0x35AA, 0x10), (0x35A9, 0x10), (0x35A6, 0x01),
                              (0x209D, 0x22), (0x209A, 0x80)]),
}

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
# feat/md-instrumentation `md read/write/grid` output (needs that firmware flashed)
RE_READ = re.compile(r"reg 0x([0-9a-fA-F]{4}) = 0x([0-9a-fA-F]{2})")
RE_WRITE = re.compile(r"reg 0x([0-9a-fA-F]{4}) <- 0x([0-9a-fA-F]{2}) \(now 0x([0-9a-fA-F]{2})\)")
RE_GRID = re.compile(r"MD motion in (\d+) blocks")
# Stock `camreg` output (already in shipping firmware - the default transport).
# read:  "Camera reg 0x209b = 0x04"     write echoes the same "= 0x.." form.
RE_CAMREG = re.compile(r"Camera reg 0x([0-9a-fA-F]{4}) = 0x([0-9a-fA-F]{2})")

# HM0360 motion-detection readout region: 32 registers (0x20A1..0x20C0) hold the
# 16x16 = 256-bit motion bitmap (see hm0360_regs.h, MD_ROI_OUT_0 + ROIOUTENTRIES).
# Popcount across all 32 bytes == number of blocks currently flagged as motion,
# i.e. the same figure the `md grid` instrumentation prints - computed host-side
# so it works on stock firmware via camreg.
MD_ROI_OUT_BASE = 0x20A1
MD_ROI_OUT_COUNT = 32
INT_INDIC = 0x2064   # bit MD_INT (0x08) set when the MD engine has fired
INT_CLEAR = 0x2065   # write MD_INT (0x08) to clear a stale MD-fired latch
MD_INT_BIT = 0x08


def parse_read(text):
    m = RE_READ.search(text)
    return int(m.group(2), 16) if m else None


def parse_write_readback(text):
    m = RE_WRITE.search(text)
    return int(m.group(3), 16) if m else None


def parse_grid_count(text):
    m = RE_GRID.search(text)
    return int(m.group(1)) if m else None


def parse_camreg(text):
    m = RE_CAMREG.search(text)
    return int(m.group(2), 16) if m else None


class Device:
    """Thin console-UART client (mirrors _Tools/ww_serial.py send/read idioms).

    Two transports for the same MD register operations:
      via="camreg" (default) - uses the stock `camreg` command that ships in
          current firmware. Register reads/writes go straight to the HM0360;
          the motion-block count is derived host-side by popcounting the 32
          MD_ROI_OUT registers. Works TODAY, no reflash.
      via="md" - uses the `md read/write/grid` instrumentation from
          feat/md-instrumentation (tidier, single-round-trip grid read) once
          that firmware is flashed.
    """

    def __init__(self, port=None, baud=921600, char_delay=0.03, quiet=0.8, timeout=6.0,
                 via="camreg", serial_obj=None):
        import serial
        # serial_obj lets open_pinned() hand us an already-open, awake port.
        self.port = serial_obj if serial_obj is not None else serial.Serial(port, baud, timeout=0.05)
        self.char_delay, self.quiet, self.timeout = char_delay, quiet, timeout
        self.via = via

    def _send(self, cmd):
        data = (cmd + "\r\n").encode("ascii", "replace")
        for i in range(len(data)):        # 1-char UART buffer: send slowly
            self.port.write(data[i:i + 1]); self.port.flush(); time.sleep(self.char_delay)

    def cmd(self, cmd):
        """Send one command, return the de-ANSI'd response text (until quiet)."""
        self.port.reset_input_buffer()
        self._send(cmd)
        buf, last, deadline = b"", time.monotonic(), time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            d = self.port.read(4096)
            if d:
                buf += d; last = time.monotonic()
            elif buf and (time.monotonic() - last) > self.quiet:
                break
        return ANSI.sub("", buf.decode("utf-8", "replace"))

    # ---- transport-neutral MD ops (dispatch on self.via) -----------------

    def md_write(self, addr, val):
        """Write a register, return the read-back value (None if it didn't take)."""
        if self.via == "camreg":
            self.cmd(f"camreg {addr:x} {val:x}")   # immediate write + stages it
            return self.md_read(addr)              # verify by reading back
        return parse_write_readback(self.cmd(f"md write {addr:x} {val:x}"))

    def md_read(self, addr):
        if self.via == "camreg":
            return parse_camreg(self.cmd(f"camreg {addr:x}"))
        return parse_read(self.cmd(f"md read {addr:x}"))

    def md_grid(self, count=MD_ROI_OUT_COUNT):
        """Blocks flagged as motion across the first `count` MD_ROI_OUT regs
        (32 = whole 16x16 grid; a smaller count is a fast partial sample)."""
        if self.via == "camreg":
            total = 0
            for i in range(count):
                v = self.md_read(MD_ROI_OUT_BASE + i)
                if v is None:
                    return None            # sensor unpowered / lost sync
                total += bin(v).count("1")
            return total
        return parse_grid_count(self.cmd("md grid"))

    def md_fired(self):
        """True if the MD interrupt-indicator shows the engine has triggered."""
        v = self.md_read(INT_INDIC)
        return None if v is None else bool(v & 0x08)

    def keep_awake(self, ms=60000):
        # OP 8 = interval-before-DPD; hold the device up. 16-bit reg, so 60000 ms
        # is the usable maximum (see live_view.py). Streaming/commands re-arm it.
        self.cmd(f"setop 8 {ms}")

    def close(self):
        self.port.close()


def _int(s):
    return int(s, 0)  # accepts 0x.. or decimal


def run_selftest():
    """Validate the parsers against sample device output - no hardware needed."""
    # feat/md-instrumentation transport
    assert parse_read("cmd> reg 0x209b = 0x04\ncmd> ") == 0x04
    assert parse_write_readback("reg 0x35a6 <- 0x08 (now 0x08)") == 0x08
    assert parse_grid_count("MD motion in 12 blocks (16x16 grid on console)") == 12
    assert parse_grid_count("no motion here") is None
    assert parse_read("garbage") is None
    # stock camreg transport
    assert parse_camreg("Camera reg 0x209b = 0x04") == 0x04
    assert parse_camreg("Camera reg 0x20a1 = 0xff") == 0xFF
    assert parse_camreg("Camera reg 0x0202 = 0x03 (immediate write OK). 1 staged") == 0x03
    assert parse_camreg("read failed (-1). Is the sensor powered?") is None
    # host-side grid popcount: 0xff has 8 bits; 32 x 0xff would be 256 blocks (full grid)
    assert bin(0xFF).count("1") == 8
    assert sum(bin(0xFF).count("1") for _ in range(MD_ROI_OUT_COUNT)) == 256
    print("selftest OK: md + camreg read/write/grid parsers verified")
    return 0


ALIVE_MARKERS = (b"OpParam 8", b"Enter 'help'", b"Camera reg", b"cmd>", b"WW500")


def open_pinned(port, baud=921600, via="camreg", budget=180.0):
    """Reopen `port` fresh until the device answers, pin it awake, return a Device.

    The WW500 re-enumerates its USB serial on power-cycle (severing any handle
    opened beforehand) and DPDs a few seconds after a cold boot. So we reopen a
    fresh handle each pass and blind-send the keep-awake; whichever pass lands in
    the post-boot window pins the device up. Returns None if the budget expires.
    """
    import serial
    deadline = time.monotonic() + budget
    print(f"connecting: reopening {port}@{baud} until awake "
          f"(power-cycle the device / wave at the lens). budget {budget:.0f}s", file=sys.stderr)
    passes = 0
    while time.monotonic() < deadline:
        passes += 1
        try:
            sp = serial.Serial(port, baud, timeout=0.1)
        except serial.SerialException:
            time.sleep(0.3)
            continue
        try:
            sp.reset_input_buffer()
            for ch in b"setop 8 60000\r\n":       # blind keep-awake
                sp.write(bytes([ch])); sp.flush(); time.sleep(0.03)
            buf, t = b"", time.monotonic() + 3.0
            while time.monotonic() < t:
                d = sp.read(4096)
                if d:
                    buf += d; t = time.monotonic() + 1.2
            if any(m in buf for m in ALIVE_MARKERS):
                print(f"connected (pass {passes}) - device pinned awake", file=sys.stderr)
                return Device(serial_obj=sp, via=via)
        except serial.SerialException:
            pass
        try:
            sp.close()
        except serial.SerialException:
            pass
        time.sleep(0.15)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    # NB: the Himax HX6538 console is COM13 @ 921600. COM14 @ 115200 is the nRF52
    # BLE/LoRaWAN co-processor - camreg/md/setop do NOT exist there.
    ap.add_argument("--port", default="COM13")
    ap.add_argument("--via", choices=("camreg", "md"), default="camreg",
                    help="register transport: 'camreg' (stock firmware, default) or "
                         "'md' (needs feat/md-instrumentation flashed)")
    ap.add_argument("--selftest", action="store_true", help="validate parsers offline; no device")
    ap.add_argument("--dump", action="store_true", help="print MD register snapshot (md dump)")
    ap.add_argument("--preset", type=int, choices=PRESETS.keys(), help="apply a preset bundle")
    ap.add_argument("--sweep", nargs=4, metavar=("ADDR", "START", "END", "STEP"),
                    help="sweep a register: hex ADDR, START..END step STEP (0x or dec)")
    ap.add_argument("--csv", help="append sweep results to this CSV file")
    ap.add_argument("--auto", type=float, metavar="SECS",
                    help="unattended: wait SECS between write and grid read instead of prompting")
    ap.add_argument("--connect-budget", type=float, default=180.0,
                    help="seconds to spend reopening the port waiting for a cold boot (default 180)")
    ap.add_argument("--no-pin", action="store_true",
                    help="skip the reconnect-pin; open the port once (device must already be awake)")
    ap.add_argument("--full-grid", action="store_true",
                    help="read all 32 MD_ROI_OUT registers for the block count (slow ~20s); "
                         "otherwise the sweep records the MD-fired bit (INT_INDIC), 1 read")
    ap.add_argument("--keep-awake", action="store_true",
                    help="(kept for compat) the reconnect-pin already holds OP 8 out")
    args = ap.parse_args()

    if args.selftest:
        return run_selftest()

    if args.no_pin:
        try:
            dev = Device(args.port, via=args.via)
        except Exception as e:
            print(f"Cannot open {args.port}: {e}\n"
                  "Is a terminal holding the port, or the device asleep/unplugged?", file=sys.stderr)
            return 2
    else:
        dev = open_pinned(args.port, via=args.via, budget=args.connect_budget)
        if dev is None:
            print("Device never woke within the connect budget. Power-cycle it during the window "
                  "(and wave at the lens to keep it awake).", file=sys.stderr)
            return 2

    try:
        if args.keep_awake or not args.no_pin:
            dev.keep_awake()

        if args.dump:
            if args.via == "md":
                print(dev.cmd("md dump"))
                return 0
            print(f"MD register snapshot via camreg ({args.port}):")
            for name, addr in DUMP_REGS:
                v = dev.md_read(addr)
                print(f"  0x{addr:04x}  {name:<26} = "
                      f"{('0x%02x' % v) if v is not None else '??  (read failed - sensor powered?)'}")
            blocks = dev.md_grid()
            print(f"  MD_ROI_OUT popcount (0x20a1..0x20c0) = "
                  f"{blocks if blocks is not None else '??'} blocks flagged")
            return 0

        if args.preset:
            name, bundle = PRESETS[args.preset]
            print(f"Applying preset {args.preset} ({name}) - HYPOTHESIS values, characterise before trusting:")
            for addr, val in bundle:
                got = dev.md_write(addr, val)
                print(f"  0x{addr:04x} <- 0x{val:02x}  readback {('0x%02x' % got) if got is not None else '??'}")
            return 0

        if args.sweep:
            addr = int(args.sweep[0], 16)  # ADDR is always hex (with or without 0x)
            start, end, step = map(_int, args.sweep[1:])
            writer = None
            if args.csv:
                fh = open(args.csv, "a", newline=""); writer = csv.writer(fh)
                if fh.tell() == 0:
                    writer.writerow(["ts", "addr", "value", "fired", "blocks", "note"])
            print(f"Sweeping 0x{addr:04x} from 0x{start:02x} to 0x{end:02x} step 0x{step:02x}")
            print("(per step: re-pin awake -> write reg -> clear MD latch -> cue stimulus -> read MD-fired)")
            v = start
            while (v <= end) if step > 0 else (v >= end):
                dev.keep_awake()                     # re-pin so we don't DPD mid-sweep
                dev.md_write(addr, v)
                dev.md_write(INT_CLEAR, MD_INT_BIT)  # clear any stale MD-fired latch
                if args.auto is not None:
                    time.sleep(args.auto); note = "auto"
                else:
                    note = input(f"  0x{addr:04x}=0x{v:02x}: present stimulus, press Enter (or type a note)> ").strip() or "-"
                fired = dev.md_fired()
                blocks = dev.md_grid() if args.full_grid else dev.md_grid(8)  # 8 regs = 64 blocks sample
                gtag = "" if args.full_grid else " (of 64)"
                ts = time.strftime("%Y-%m-%dT%H:%M:%S")
                state = "FIRED" if fired else ("quiet" if fired is not None else "??")
                print(f"    -> MD {state}" + (f", {blocks} blocks{gtag}" if blocks is not None else ""))
                if writer:
                    writer.writerow([ts, f"0x{addr:04x}", f"0x{v:02x}",
                                     "" if fired is None else int(fired),
                                     "" if blocks is None else blocks, note])
                v += step
            if args.csv:
                fh.close(); print(f"appended to {args.csv}")
            return 0

        ap.print_help()
        return 0
    finally:
        dev.close()


if __name__ == "__main__":
    sys.exit(main())
