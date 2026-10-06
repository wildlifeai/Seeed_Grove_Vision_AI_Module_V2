"""Merge the Himax, nRF and app logs into one timeline for a time window, trimmed.

  merge3.py <start HH:MM:SS> <end HH:MM:SS> [--src himax,nrf,app] [--grep REGEX] [--all]

Stamps are this PC's read time. App lines carry the phone's own clock, about 2 s
ahead of the PC here, and are shifted back by --app-offset (default 2 s). The nRF
flushes its log in bursts, so order events by the Himax lines.
Hex dumps, NULs, colour codes and the nRF's LoRa and GATT chatter are dropped
unless --all is given. --logs is the folder bench_daemon.py wrote (himax.log, nrf.log)
and the app_logcat.log captured beside them.
"""
import argparse, re, sys

ANSI = re.compile(r"\x1b\[[0-9;]*m|\[[0-9;]*m(?=\S)")
HEX = re.compile(r"^\s*[0-9a-fA-F]{3}: ([0-9a-fA-F]{2}\s+){1,}")
NOISE = re.compile(r"MAC rxTimeOut|nrf_ble_gatt|max_(rx|tx)_(octets|time)|Data length updated|DEBUG: we waited|"
                   r"^\s*$|^cmd>\s*$|Require cycle|beginning of main|TX Hex:")


def secs(hms):
    h, m, s = hms.split(":"); return int(h) * 3600 + int(m) * 60 + float(s)


def fmt(t):
    h = int(t // 3600); m = int(t % 3600 // 60); s = t % 60
    return f"{h:02d}:{m:02d}:{s:06.3f}"


def read(path, src, offset=0.0):
    out = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n").replace("\x00", "")
            if src == "app":
                m = re.match(r"\d\d-\d\d (\d\d:\d\d:\d\d\.\d+) \w/ReactNativeJS\(\d+\): (.*)", line)
                if not m:
                    continue
                t, text = secs(m.group(1)) - offset, m.group(2)
            else:
                m = re.match(r"\[(\d\d:\d\d:\d\d\.\d+)\] (.*)", line)
                if not m:
                    continue
                t, text = secs(m.group(1)), m.group(2)
            text = ANSI.sub("", text).rstrip()
            out.append((t, src, text))
    return out


ap = argparse.ArgumentParser()
ap.add_argument("start"); ap.add_argument("end")
ap.add_argument("--src", default="himax,nrf,app")
ap.add_argument("--grep", default=None)
ap.add_argument("--all", action="store_true")
ap.add_argument("--app-offset", type=float, default=2.0)
ap.add_argument("--logs", default="bench")
a = ap.parse_args()
t0, t1 = secs(a.start), secs(a.end)
rows = []
for src in a.src.split(","):
    path = {"himax": f"{a.logs}/himax.log", "nrf": f"{a.logs}/nrf.log", "app": f"{a.logs}/app_logcat.log"}[src]
    rows += read(path, src, a.app_offset if src == "app" else 0.0)
rows = [r for r in rows if t0 <= r[0] <= t1]
if not a.all:
    rows = [r for r in rows if not HEX.match(r[2]) and not NOISE.search(r[2])]
if a.grep:
    g = re.compile(a.grep)
    rows = [r for r in rows if g.search(r[2])]
rows.sort(key=lambda r: r[0])
# a run of the same line from one source (other sources may interleave) keeps its first and last
out, skip = [], set()
for i, (t, src, text) in enumerate(rows):
    if i in skip:
        continue
    # the run ends at the first different line from the same source
    k = i + 1
    same = [i]
    while k < len(rows):
        if rows[k][1] == src:
            if rows[k][2] != text:
                break
            same.append(k)
        k += 1
    out.append(rows[i])
    if len(same) > 2:
        skip.update(same[1:-1])
        out.append((rows[same[-2]][0], src, f"... the same line {len(same) - 2} more times"))
for t, src, text in sorted(out, key=lambda r: r[0]):
    text = text.replace("�", "-").replace("—", "-")
    sys.stdout.write(f"[{fmt(t)}] {src:5s} | {text}\n")
