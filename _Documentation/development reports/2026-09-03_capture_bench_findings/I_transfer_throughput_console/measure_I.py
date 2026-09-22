#!/usr/bin/env python3
"""Measure finding I: how much of an image download the nRF spends on its own console.

Three modes, all reading the three-way bench log the logger is writing:

    python measure_I.py txfile 59200BE0.JPG   # ask the nRF's version, then time a download
    python measure_I.py upload                 # analyse the last app-driven upload (ftx, quiet path)
    python measure_I.py loopback               # summarise the last FILE_LOOPBACK benchmark

The download is driven over adb through the Engineer Console (input focused, keyboard
open). For each transfer the script reports bytes, packets, duration, rate, the median
gap between packets, the nRF's own measure of the BLE wait, and how many characters the
nRF wrote to its 115200 baud console per packet, which is the cost under test.
"""
import re
import subprocess
import sys
import time

LOG = (r"C:\Users\Victor\AppData\Local\Temp\claude\C--Users-ww"
       r"\2c78acd7-f4c0-4115-a1cf-a222e9aae63a\scratchpad\bench\2026-09-04_finding_I\finding_I_bench.log")
SEND = (985, 1289)
STAMP = re.compile(r"^\[(\d\d):(\d\d)\.(\d\d\d)\] (\w+) +\| (.*)$")
HEXROW = re.compile(r"^[0-9A-Fa-f]{3}: [0-9A-Fa-f]{2} ")
UART_CPS = 115200 / 10.0   # characters per second at 8N1


def adb(*args):
    subprocess.run(["adb", "shell", *args], check=False, capture_output=True)


def type_and_send(text):
    adb("input", "text", text.replace(" ", "%s"))
    adb("input", "tap", str(SEND[0]), str(SEND[1]))


def parse(path):
    recs = []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = STAMP.match(line.replace("\0", "").rstrip("\n"))
            if m:
                t = int(m.group(1)) * 60 + int(m.group(2)) + int(m.group(3)) / 1000
                recs.append((t, m.group(4), m.group(5)))
    return recs


def median(xs):
    xs = sorted(xs)
    return xs[len(xs) // 2] if xs else 0


def console_cost(recs, t0, t1, packets):
    nrf = [(t, s) for t, src, s in recs if src == "nrf" and t0 <= t <= t1]
    chars = sum(len(s) + 2 for t, s in nrf)
    hexrows = sum(1 for t, s in nrf if HEXROW.match(s))
    print(f"  nRF console in that window: {chars} chars, {hexrows} hex-dump rows"
          f" = {chars / UART_CPS:.1f} s of UART time at 115200"
          + (f", {chars / packets:.0f} chars ({chars / packets / UART_CPS * 1000:.0f} ms) per packet" if packets else ""))


def report_download(recs):
    txin = [t for t, src, s in recs if src == "nrf" and "'AI txfile" in s]
    fin = [(t, s) for t, src, s in recs if src == "nrf" and "BLE out: Sent" in s and "Finished sending" in s]
    if not txin or not fin:
        print("no complete download in the log")
        return 2
    t0 = txin[-1]
    t1, fs = next((t, s) for t, s in fin if t > t0)
    m = re.search(r"Finished sending (\d+) bytes \((\d+) packets\)", fs)
    nbytes, npk = int(m.group(1)), int(m.group(2))
    pk = [t for t, src, s in recs if src == "nrf" and "BLE binary: Sending" in s and t0 <= t <= t1]
    waits = [int(x) for t, src, s in recs if src == "nrf" and t0 <= t <= t1
             for x in re.findall(r"we waited (\d+)ms", s)]
    gaps = [b - a for a, b in zip(pk, pk[1:]) if b > a]
    dur = t1 - pk[0]
    print(f"DOWNLOAD  AI txfile: {nbytes} bytes, {npk} packets, {dur:.1f} s from first packet to Finished"
          f" = {nbytes / dur / 1024:.2f} KB/s")
    print(f"  packet gap median {median(gaps) * 1000:.0f} ms; nRF's own BLE wait median {median(waits)} ms, max {max(waits) if waits else 0} ms")
    console_cost(recs, pk[0], t1, npk)
    return 0


def report_upload(recs):
    app = [(t, s) for t, src, s in recs if src == "app"]
    starts = [(t, s) for t, s in app if "[FileTransfer]" in s and "FILE_START sent" in s]
    if not starts:
        print("no upload in the log")
        return 2
    t0, s0 = starts[-1]
    name = re.search(r"FILE_START sent: (\S+) \((\d+) bytes\)", s0)
    done = [(t, s) for t, s in app if t > t0 and "Transfer complete" in s and "[FileTransfer]" in s]
    phase = [(t, s) for t, s in app if t > t0 and "DATA phase complete" in s]
    if not done:
        print("upload started but no 'Transfer complete' yet")
        return 2
    t1 = done[0][0]
    nbytes = int(name.group(2)) if name else 0
    plan = [s for t, s in app if t <= t0 and "[FileTransfer]" in s and " packets, CRC=" in s]
    npk = int(re.search(r"(\d+) packets, CRC=", plan[-1]).group(1)) if plan else 0
    acks = [t for t, s in app if t0 <= t <= t1 and "ftx ack" in s]
    gaps = [b - a for a, b in zip(acks, acks[1:]) if b > a]
    print(f"UPLOAD    ftx {name.group(1) if name else '?'}: {nbytes} bytes, {npk} packets, {t1 - t0:.1f} s FILE_START to 'ftx done'"
          f" = {nbytes / (t1 - t0) / 1024:.2f} KB/s, {(t1 - t0) / npk * 1000:.0f} ms per packet" if npk else "UPLOAD: packet count not found")
    if acks:
        print(f"  {len(acks)} 'ftx ack' lines seen by the app, median gap {median(gaps) * 1000:.0f} ms")
    if phase:
        print("  app: " + phase[0][1].split("[FileTransfer] ")[-1])
    console_cost(recs, t0, t1, npk)
    return 0


def report_loopback(recs):
    app = [(t, s) for t, src, s in recs if src == "app"]
    size = None
    rounds = {}
    for t, s in app:
        m = re.search(r"Payload size: (\d+) bytes", s)
        if m:
            size = int(m.group(1))
            rounds.setdefault(size, [])
        m = re.search(r"R\d+: (\d+)ms", s)
        if m and size is not None:
            rounds[size].append(int(m.group(1)))
    if not rounds:
        print("LOOPBACK  no benchmark lines in logcat; read the medians off the screen")
        return 2
    for size, rs in rounds.items():
        if rs:
            print(f"LOOPBACK  {size} B payload: median {median(rs)} ms round trip, {len(rs)} rounds"
                  f" = {size / (median(rs) / 1000) / 1024:.1f} KB/s each way if streamed one at a time")
    return 0


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("txfile", "upload", "loopback"):
        print(__doc__)
        return 1
    mode = sys.argv[1]
    if mode == "txfile":
        fname = sys.argv[2] if len(sys.argv) > 2 else "59200BE0.JPG"
        type_and_send("version")
        time.sleep(3)
        type_and_send(f"AI txfile {fname}")
        # Wait for Finished sending after this command, up to 90 s.
        end = time.time() + 90
        while time.time() < end:
            recs = parse(LOG)
            txin = [t for t, src, s in recs if src == "nrf" and "'AI txfile" in s]
            if txin and any(src == "nrf" and t > txin[-1] and "BLE out: Sent" in s and "Finished sending" in s
                            for t, src, s in recs):
                break
            time.sleep(1)
        recs = parse(LOG)
        ver = [s for t, src, s in recs if src == "nrf" and "BLE out: Sent" in s and re.search(r"[Vv]ersion|\d+\.\d+\.\d+", s)]
        print("nRF version reply: " + (ver[-1].split("Sent")[-1].strip() if ver else "none seen"))
        return report_download(recs)
    recs = parse(LOG)
    return report_upload(recs) if mode == "upload" else report_loopback(recs)


if __name__ == "__main__":
    sys.exit(main())
