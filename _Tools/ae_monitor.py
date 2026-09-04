#!/usr/bin/env python3
"""Live AE (light sensor) monitor for the WW500 bench.

Holds the device awake and repeatedly triggers an on-demand capture, printing
the firmware's own light-sensor decision each time:

    AE light check: mean AE=64 (min 64, max 64, 16 frames) thr=65,
    AGain=1, conv=Y, gain railed = N -> DARK

Every 'capture' command is a console keystroke sequence, which resets the CLI
inactivity timer to 60 s (INACTIVITYTIMEOUTCLI), so sending one every <60 s
keeps the device out of deep sleep for the whole session. This makes the
cover/uncover test interactive instead of waiting for the 2-minute timer wake.

Because the device only listens on its UART while awake, the monitor first
waits for a wake (RTC timer or motion), grabs it, then holds it.

REQUIRED SETUP - the firmware only prints the 'AE light check' line (and thus
only feeds this monitor) when the AE decision is actually consumed by
something. Before running this script, enable one of, over the console:

    setop 13 1     # OP_PARAMETER_FLASH_LED = visible LED -> FLASH_MODE_AE
    setop 13 2     # same, but IR LED
    setop 26 1     # OP_PARAMETER_SLOT_SWITCH -> automatic day/night switching (op26)

With neither set, captures still run but the device only logs the raw
'HM0360 AE regs:' register dump, which this monitor cannot parse - you'll see
'Device is awake' and then nothing (use --verbose to confirm this is why).

lightSensor.c has two dark/bright algorithms selected by its AE_DECISION_GAIN_BASED
#define. The gain-based algorithm has no mean-AE/threshold concept at all - its
line is just "AE light check: AGain = N, conv=Y|N -> DARK|BRIGHT". When AE_RE
matches that shorter line, 'AE=.../thr.../RAILED/bar' below are not available
and the script prints a leaner line instead - see READING THE OUTPUT.

READING THE OUTPUT - each line is one capture. With the default
AE_DECISION_GAIN_BASED algorithm:

    [19:25:35] #2   flash ON  aGain=4 conv=N integ= 376 aGain= 2 dGain= 65

    flash     the firmware's decision (lightSensor.c's
              decideDarkBrightGainBased()): dark if AE hasn't converged, or
              analog gain exceeds DARK_ANALOG_GAIN_THRESHOLD. No hysteresis.
    aGain/conv  the analog gain and AE_CONVERGED values the decision above was
              actually based on (from the light check line itself).
    integ/aGain/dGain  same telemetry as below, from the separate 'HM0360 AE
              regs' dump - see that entry for the one-reading-behind caveat.

With AE_DECISION_GAIN_BASED undefined (the original algorithm):

    [19:25:35] #2   AE= 65(thr65) BRIGHT flash ON  up   RAILED integ= 376 aGain= 2 dGain= 65  |####...|

    AE=/thr   raw AE Mean reading and the configured dark threshold
              (OP_PARAMETER_AE_DARK_THRESHOLD, 'setop 23 <value>').
    state     this script's own naive ae < thr check - for quick reference only.
    flash     the firmware's REAL decision (lightSensor.c's decideDarkBright()).
              It applies hysteresis (stays ON until well above threshold) and a
              gain-railed override, so it can legitimately disagree with
              'state' near the boundary - that is not a bug.
    arrow     up/down/= vs the previous reading's AE value.
    RAILED    shown when the sensor's gain has maxed out on most frames, i.e.
              it cannot expose any darker - AE Mean becomes meaningless and
              the firmware forces DARK regardless of its value.
    integ/aGain/dGain  Integration time / Analog gain / Digital gain from the
              'HM0360 AE regs' dump, which the firmware prints AFTER the AE
              light check line each capture. So reading #1 always shows '?'
              here (nothing seen yet) and each later reading shows the
              previous capture's register values, not this one's - expected,
              not a bug.
    bar       AE Mean rendered as a simple 0-255 bar for a quick visual read.

Usage:
    python ae_monitor.py --port COM4                 # ~4 min session
    python ae_monitor.py --port COM4 --duration 360  # longer session
    python ae_monitor.py --port COM4 --verbose        # also show raw console lines
"""

import argparse
import re
import sys
import time

import serial

ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
# Two alternatives, matched independently so neither weakens the other. Field
# names/values here must track lightSensor.c's actual snprintf() exactly -
# Charles shortened the message on 2026-09-02 ('AGain'/'conv=Y/N'/'thr', not
# 'analog gain'/'converged=yes/no'/'threshold'):
#   1) the legacy single-frame line ("AE Mean = 46, threshold = 65 -> flash ON")
#      and the aggregated line ("mean AE=34 (min 3, max 66, 8 frames) thr=65,
#      AGain=1, conv=Y, gain railed = N -> DARK") - groups 1-3.
#   2) lightSensor.c's AE_DECISION_GAIN_BASED algorithm's line, which has no
#      mean/threshold concept at all ("AE light check: AGain = 4, conv=N
#      -> DARK ...") - groups 4-6.
# A match has either group(1) or group(4) set (never both) - see which branch
# fired in the code below.
AE_RE = re.compile(
    r"(?:AE Mean|mean AE)\s*=\s*(\d+).*?thr(?:eshold)?\s*=\s*(\d+).*?->\s*(DARK|BRIGHT|flash ON|flash OFF)"
    r"|"
    r"AGain\s*=\s*(\d+).*?conv\s*=\s*(Y|N).*?->\s*(DARK|BRIGHT|flash ON|flash OFF)"
)
RAILED_RE = re.compile(r"gain railed\s*=\s*(Y|N)")
INTEG_RE = re.compile(r"Integration time\s*=\s*(\d+)")
AGAIN_RE = re.compile(r"Analog gain\s*=\s*(\d+)")
DGAIN_RE = re.compile(r"Digital gain\s*=\s*(\d+)")
BOOT_MARKERS = ("Image sensor and data path initialised", "Inactivity period set",
                "available commands")


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except AttributeError:
        pass

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="COM14")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--duration", type=float, default=240.0, help="total session seconds")
    ap.add_argument("--interval", type=float, default=20.0, help="seconds between captures (<60)")
    ap.add_argument("--char-delay", type=float, default=0.03, help="inter-character send delay")
    ap.add_argument("--verbose", action="store_true", help="also stream raw console lines")
    args = ap.parse_args()

    eol = b"\r\n"

    try:
        port = serial.Serial(args.port, args.baud, timeout=0.05)
    except serial.SerialException as e:
        print(f"Cannot open {args.port}: {e}", file=sys.stderr)
        return 2

    def send(cmd: str) -> None:
        data = cmd.encode("ascii", "replace") + eol
        for i in range(len(data)):
            port.write(data[i:i + 1])
            port.flush()
            time.sleep(args.char_delay)

    def wallclock() -> str:
        return time.strftime("%H:%M:%S")

    try:
        port.reset_input_buffer()
        print(f"[{wallclock()}] Waiting for the device to wake (RTC timer ~2 min, or wave to trigger motion)...")

        deadline = time.monotonic() + args.duration
        buf = b""
        awake = False
        next_capture = 0.0
        last_ae = None
        reading_num = 0
        integ = again = dgain = None  # most recent gain regs seen

        while time.monotonic() < deadline:
            now = time.monotonic()

            # Once awake, keep the device awake and sample on a cadence
            if awake and now >= next_capture:
                send("capture 1 500")
                next_capture = now + args.interval

            data = port.read(4096)
            if data:
                buf += data
                while True:
                    nl = buf.find(b"\n")
                    if nl < 0:
                        break
                    line = ANSI_RE.sub("", buf[:nl].rstrip(b"\r").decode("utf-8", "replace"))
                    buf = buf[nl + 1:]

                    if args.verbose:
                        print(f"    | {line}")

                    if not awake and any(m in line for m in BOOT_MARKERS):
                        awake = True
                        print(f"[{wallclock()}] Device is awake - holding it up and starting captures.")
                        # first capture almost immediately; small settle so the
                        # CLI task is ready to accept the command
                        next_capture = time.monotonic() + 1.0

                    mi = INTEG_RE.search(line)
                    if mi:
                        integ = int(mi.group(1))
                    ma = AGAIN_RE.search(line)
                    if ma:
                        again = int(ma.group(1))
                    md = DGAIN_RE.search(line)
                    if md:
                        dgain = int(md.group(1))

                    m = AE_RE.search(line)
                    if m:
                        reading_num += 1
                        # gain/integration rise in the dark - a more robust dark signal
                        # than AE Mean, which the sensor's own AE re-converges upward
                        gains = f"integ={integ if integ is not None else '?':>4} " \
                                f"aGain={again if again is not None else '?':>2} " \
                                f"dGain={dgain if dgain is not None else '?':>3}"

                        if m.group(1) is not None:
                            # mean-AE/threshold branch (legacy or the original
                            # aggregated algorithm) - full display as before.
                            ae = int(m.group(1))
                            thr = int(m.group(2))
                            decision = m.group(3)
                            flash = "ON" if decision in ("DARK", "flash ON") else "OFF"
                            mr = RAILED_RE.search(line)
                            railed = (mr.group(1) == "Y") if mr else False
                            state = "DARK " if ae < thr else "BRIGHT"
                            arrow = ""
                            if last_ae is not None:
                                arrow = "up  " if ae > last_ae else ("down" if ae < last_ae else "=   ")
                            last_ae = ae
                            bar = "#" * min(40, ae * 40 // 255)
                            railtag = "RAILED " if railed else "       "
                            print(f"[{wallclock()}] #{reading_num:<3} AE={ae:3d}(thr{thr}) "
                                  f"{state} flash {flash:<3} {arrow} {railtag}{gains}  |{bar:<40}|", flush=True)
                        else:
                            # AE_DECISION_GAIN_BASED branch - no mean AE/threshold
                            # at all, just the register-based decision itself.
                            re_again = int(m.group(4))
                            converged = m.group(5) == "Y"
                            decision = m.group(6)
                            flash = "ON" if decision in ("DARK", "flash ON") else "OFF"
                            print(f"[{wallclock()}] #{reading_num:<3} flash {flash:<3} "
                                  f"aGain={re_again} conv={'Y' if converged else 'N'} {gains}", flush=True)

        print(f"[{wallclock()}] Session finished ({reading_num} readings).")
        return 0
    finally:
        port.close()


if __name__ == "__main__":
    sys.exit(main())
