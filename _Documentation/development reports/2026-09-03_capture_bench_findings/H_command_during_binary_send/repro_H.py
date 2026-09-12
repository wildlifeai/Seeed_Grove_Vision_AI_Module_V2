#!/usr/bin/env python3
"""Reproduce finding H on demand: a BLE command arriving while `txfile` is streaming.

The nRF forwards any `AI ...` command the moment it arrives and zeroes its binary
packet counter on the way (aiProcessor.c:647), so the next chunk of the file goes out
as packet 1 again. The Himax queues the command behind the file and answers when the
file is done, which trips the nRF's retry timer first ("AI processor not responding").

The phone is driven over adb: the Engineer Console input is focused (keyboard open),
so `input text` types into it and a tap on the send button sends. The console writes
straight to the characteristic, so the app's own stream gate does not hold the probe.
Observation is the three-way bench log the logger is already writing.

    python repro_H.py                 # txfile, probe at packet 20, report
    python repro_H.py --at 40         # probe later in the stream
    python repro_H.py --file 59200EE0.JPG
"""
import argparse
import re
import subprocess
import sys
import time

LOG = (r"C:\Users\Victor\AppData\Local\Temp\claude\C--Users-ww"
       r"\2c78acd7-f4c0-4115-a1cf-a222e9aae63a\scratchpad\bench\2026-09-03_finding_H\repro_H_bench.log")
SEND = (985, 1289)          # send button, keyboard open (it stays open after a send)

TXFILE_IN = re.compile(r"nrf .*BLE in: Received .*'AI txfile")
PKT = re.compile(r"nrf .*BLE binary: Sending +(\d+) byte binary payload \(packet type \d+, packet num (\d+)\)")
PROBE_IN = re.compile(r"nrf .*BLE in: Received .*'AI slots'")
PROBE_FWD = re.compile(r"nrf .*Sending 'slots' to AI processor")
NOT_RESP = re.compile(r"AI processor not responding")
FINISHED = re.compile(r"nrf .*BLE out: Sent .*'Finished sending (\d+) bytes \((\d+) packets\)")
REPLY = re.compile(r"nrf .*BLE out: Sent .*'Active slot")
APP = re.compile(r"app .*(ImageReassembler|gap|corrupt|not responding|Finished sending|Active slot)", re.I)
STAMP = re.compile(r"^\[(\d\d):(\d\d)\.(\d\d\d)\]")


def adb(*args):
    subprocess.run(["adb", "shell", *args], check=False, capture_output=True)


def type_text(text):
    adb("input", "text", text.replace(" ", "%s"))


def tap(xy):
    adb("input", "tap", str(xy[0]), str(xy[1]))


def stamp(line):
    m = STAMP.match(line)
    return int(m.group(1)) * 60 + int(m.group(2)) + int(m.group(3)) / 1000 if m else None


class Tail:
    def __init__(self, path):
        self.f = open(path, "r", encoding="utf-8", errors="replace")
        self.f.seek(0, 2)

    def lines(self):
        while True:
            line = self.f.readline()
            if not line:
                return
            yield line.rstrip("\n")

    def wait(self, pattern, timeout):
        end = time.time() + timeout
        while time.time() < end:
            for line in self.lines():
                if pattern.search(line):
                    return line
            time.sleep(0.01)
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", default="59200BE0.JPG", help="a JPG on the card (26 KB, 110 packets)")
    ap.add_argument("--at", type=int, default=20, help="send the probe once this packet number has gone out")
    ap.add_argument("--log", default=LOG)
    args = ap.parse_args()

    tail = Tail(args.log)

    type_text(f"AI txfile {args.file}")
    tap(SEND)
    first = tail.wait(TXFILE_IN, 10)
    if not first:
        print("the nRF never saw the txfile command; is the console open with the input focused?")
        return 2
    t0 = stamp(first)
    print(f"[+0.000] {first[:100]}")

    # Let the stream run to the trigger packet.
    before = []
    end = time.time() + 30
    while time.time() < end and (not before or before[-1] < args.at):
        for line in tail.lines():
            m = PKT.search(line)
            if m:
                before.append(int(m.group(2)))
        time.sleep(0.01)
    if not before or before[-1] < args.at:
        print(f"stream never reached packet {args.at}; saw {before[-3:] if before else 'nothing'}")
        return 2
    print(f"[+{stamp(line) - t0:.3f}] packet {before[-1]} out, sending the probe")

    type_text("AI slots")
    tap(SEND)

    # Watch what happens to the stream and to the probe.
    after = []
    events = []
    finished = reply = None
    end = time.time() + 45
    while time.time() < end:
        for line in tail.lines():
            m = PKT.search(line)
            if m:
                after.append(int(m.group(2)))
                continue
            for p in (PROBE_IN, PROBE_FWD, NOT_RESP, FINISHED, REPLY, APP):
                if p.search(line):
                    events.append(line)
                    if p is FINISHED:
                        finished = line
                    if p is REPLY:
                        reply = line
                    break
        if reply:
            break
        time.sleep(0.01)

    print()
    for e in events:
        print(f"[+{stamp(e) - t0:7.3f}] {e[12:120]}")

    probe_in = next((e for e in events if PROBE_IN.search(e)), None)
    print()
    print(f"packets before the probe: 1..{before[-1]} ({len(before)} sent)")
    # The counter is zeroed when the command arrives, but a chunk or two already
    # queued still go out with their old numbers, so look for the first drop.
    restart_at = next((i for i in range(1, len(after)) if after[i] < after[i - 1]), None)
    if after:
        print(f"packets after the probe:  {len(after)} sent, numbered "
              + (f"{after[0]}..{after[restart_at - 1]} then {after[restart_at]}..{after[-1]}"
                 if restart_at is not None else f"{after[0]}..{after[-1]}"))
        print(f"total packets sent: {len(before) + len(after)}, last one numbered {after[-1]}")
    if finished:
        m = FINISHED.search(finished)
        print(f"Finished sending says {m.group(2)} packets, {m.group(1)} bytes")
    if probe_in and reply:
        print(f"probe reply arrived {stamp(reply) - stamp(probe_in):.1f} s after the probe went in"
              + (f", {stamp(reply) - stamp(finished):.1f} s after Finished sending" if finished else ""))
    nr = [e for e in events if NOT_RESP.search(e)]
    print(f"'AI processor not responding': {'yes, ' + str(len(nr)) + 'x' if nr else 'not seen'}")
    hit = restart_at is not None
    print()
    print(f"HIT: the packet counter restarted at {after[restart_at]} after packet {after[restart_at - 1]}" if hit
          else "no restart seen; check the stream was still running when the probe went in")
    return 0 if hit else 1


if __name__ == "__main__":
    sys.exit(main())
