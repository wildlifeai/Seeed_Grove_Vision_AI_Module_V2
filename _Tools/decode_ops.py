"""Decode a WW500 'Sleep ...' or 'OpParams ...' dump into named parameters.

Both are a bare space-separated list of every operational parameter from index 0
(if_task.c loops over OP_PARAMETER_NUM_ENTRIES), so position IS the index and a
miscount silently shifts every reading. Hence this rather than counting by eye.

    python decode_ops.py                 # reads the newest dump from merged.log
    python decode_ops.py --diff          # ... and diffs the last two
    echo "Sleep 4 0 0 ..." | python decode_ops.py -
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

NAMES = [
    "SEQUENCE_NUMBER", "NUM_NN_ANALYSES", "NUM_POSITIVE_NN_ANALYSES",
    "NUM_COLD_BOOTS", "NUM_WARM_BOOTS", "NUM_PICTURES", "PICTURE_INTERVAL",
    "TIMELAPSE_INTERVAL", "INTERVAL_BEFORE_DPD", "LED_BRIGHTNESS_PERCENT",
    "CAMERA_ENABLED", "MD_INTERVAL", "FLASH_DURATION", "FLASH_LED",
    "MODEL_PROJECT", "MODEL_VERSION", "MODEL_THRESHOLD", "MD_SENSITIVITY",
    "TEST_MODE_BITS", "IMAGES_COUNT", "IMAGES_FILE_INDEX", "MD_FLASH_LED",
    "MD_FLASH_BRIGHTNESS_PERCENT", "AE_DARK_THRESHOLD", "AE_CHECK_INTERVAL",
    "AE_FLASH_STATE", "SLOT_SWITCH", "WB_RED_GAIN", "WB_BLUE_GAIN",
    "CAM_AE_ENABLE", "CAM_AE_TARGET", "CAM_WB_MODE",
]

DEFAULTS = {
    5: 1, 6: 500, 7: 0, 8: 1000, 9: 5, 10: 1, 11: 0, 12: 100, 13: 0,
    14: 0, 15: 0, 16: 18, 17: 1, 18: 0, 21: 2, 22: 50, 23: 65, 24: 15,
    25: 0, 26: 0, 27: 286, 28: 326, 29: 1, 30: 110, 31: 1,
}

# The ones that decide whether the camera actually does anything.
KEY = {7, 10, 11, 14, 17, 24}

DUMP_RE = re.compile(r"(?:Sleep|OpParams)\s+((?:-?\d+\s+)+-?\d+)")


def parse(text):
    out = []
    for m in DUMP_RE.finditer(text):
        vals = [int(v) for v in m.group(1).split()]
        if len(vals) >= 20:          # a real dump, not a stray "Sleep 4"
            out.append(vals)
    return out


def show(vals, prev=None):
    print(f"{'idx':>3}  {'name':<28} {'value':>7}  {'default':>7}  notes")
    print("-" * 74)
    for i, v in enumerate(vals):
        if i >= len(NAMES):
            print(f"{i:>3}  {'(beyond known table)':<28} {v:>7}")
            continue
        d = DEFAULTS.get(i)
        notes = []
        if d is not None and v != d:
            notes.append("NON-DEFAULT")
        if prev is not None and i < len(prev) and prev[i] != v:
            notes.append(f"CHANGED {prev[i]} -> {v}")
        if i == 11 and v == 0:
            notes.append("<< motion detection INHIBITED")
        if i == 7 and v == 0 and vals[11] == 0:
            notes.append("<< and no timelapse either: device only wakes on the AE timer")
        if i == 14 and v == 0:
            notes.append("<< NN model disabled")
        mark = "*" if i in KEY else " "
        print(f"{i:>3}{mark} {NAMES[i]:<28} {v:>7}  "
              f"{'' if d is None else d:>7}  {' '.join(notes)}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source", nargs="?", default=os.path.join(HERE, "merged.log"))
    ap.add_argument("--diff", action="store_true", help="diff the last two dumps")
    args = ap.parse_args()

    text = sys.stdin.read() if args.source == "-" else \
        open(args.source, encoding="utf-8", errors="replace").read()

    dumps = parse(text)
    if not dumps:
        print("no Sleep/OpParams dump found", file=sys.stderr)
        return 1
    print(f"({len(dumps)} dump(s) found; showing the last)\n")
    show(dumps[-1], dumps[-2] if args.diff and len(dumps) > 1 else None)
    return 0


if __name__ == "__main__":
    sys.exit(main())
