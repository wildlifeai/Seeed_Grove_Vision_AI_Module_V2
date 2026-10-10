"""ww-hardware #58 retest: python retest58.py <log time to start from, HH:MM:SS> [delay s].
 after the timer wake's failed sends, wait DELAY s, then connect the app over adb,
and report whether the nRF drops the link early ('BLE Timeout: disconnecting')."""
import re, sys, time
import ec

NRF = "bench/nrf.log"     # written by bench_daemon.py
SINCE = sys.argv[1] if len(sys.argv) > 1 else "15:19:50"
DELAY = float(sys.argv[2]) if len(sys.argv) > 2 else 40
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def lines():
    with open(NRF, encoding="utf-8", errors="replace") as f:
        return [ANSI.sub("", l.rstrip("\n")) for l in f if l[1:13] >= SINCE]


def say(s):
    print(f"{time.strftime('%H:%M:%S')} {s}", flush=True)


# 1. wait for the failed 'Sleep' that ends the timer wake
say("waiting for the timer wake's failed sends")
end = time.time() + 240
sleep_fail = None
while time.time() < end and not sleep_fail:
    for l in lines():
        if "Failed to send" in l and "'Sleep'" in l:
            sleep_fail = l
    time.sleep(0.5)
if not sleep_fail:
    sys.exit("no failed 'Sleep' seen")
say(f"last failed send: {sleep_fail[:80]}")
fails = [l for l in lines() if "Failed to send" in l]
say(f"{len(fails)} failed sends in the wake, first {fails[0][1:13]}")

# 2. wait DELAY s, then connect
t_fail = time.time()
while time.time() - t_fail < DELAY:
    time.sleep(0.5)
say("tapping Search for Devices")
n = ec.find(ec.dump(), lambda n: n.get("content-desc") == "Search for Devices")
if n is None:
    sys.exit("no Search button")
ec.tap(*ec.centre(n))
dev = None
t0 = time.time()
while time.time() - t0 < 20 and dev is None:
    time.sleep(1)
    dev = ec.find(ec.dump(), lambda n: "WILD-KOMB" in (n.get("text") or "") or "WILD-KOMB" in (n.get("content-desc") or ""))
if dev is None:
    sys.exit("WILD-KOMB not found in the scan (press the middle button?)")
say(f"tapping WILD-KOMB {dev.get('bounds')}")
ec.tap(*ec.centre(dev))
time.sleep(2)
root = ec.dump()
btn = ec.find(root, lambda n: (n.get("content-desc") or n.get("text") or "").strip().lower() in ("connect", "connect to device"))
if btn is not None:
    say(f"tapping {btn.get('content-desc') or btn.get('text')}")
    ec.tap(*ec.centre(btn))

# 3. watch for the connection and an early drop
t0 = time.time()
conn = None
while time.time() - t0 < 90:
    for l in lines():
        if "<info> app: Connected" in l and not conn:
            conn = l; say(f"nRF: {l[:60]}")
        if conn and l[1:13] > conn[1:13] and ("BLE Timeout" in l or "Disconnected" in l or "BLE in:" in l or "BLE out" in l):
            pass
    if conn:
        after = [l for l in lines() if l[1:13] >= conn[1:13] and ("BLE Timeout" in l or "Disconnected" in l or "BLE in:" in l or "BLE out" in l)]
        if any("Disconnected" in l for l in after):
            break
    time.sleep(1)
for l in lines():
    if l[1:13] >= sleep_fail[1:13] and ("Connected" in l or "BLE Timeout" in l or "Disconnected" in l or "BLE in:" in l or "BLE out" in l):
        print("   ", l[:120])
say("done")
