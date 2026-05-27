#!/usr/bin/env python
"""Program a firmware image into one 1 MB firmware slot.

Usage: python burn_slot.py <slot> <file>
  slot  0 = Slot A (0x00000000)
        1 = Slot B (0x00100000)
  file  binary image to program (e.g. output.img, dump_slot_a.bin)

The image is written starting at the slot base address.  swdflash.py pads
the data to 4 KB sector boundaries before programming.
"""

import sys
import os
import subprocess

SLOTS = {
    0: {'name': 'A', 'addr': 0x00000000},
    1: {'name': 'B', 'addr': 0x00100000},
}

SWDFLASH_SCRIPT = os.path.join(os.path.dirname(__file__), 'swdflash.py')


def burn_slot(slot_num, binfile):
    if slot_num not in SLOTS:
        print(f"ERROR: slot must be 0 or 1, got {slot_num!r}")
        return False

    if not os.path.isfile(binfile):
        print(f"ERROR: file not found: {binfile}")
        return False

    s = SLOTS[slot_num]
    size = os.path.getsize(binfile)
    print(f"Programming Slot {s['name']} (slot {slot_num})")
    print(f"  addr  : 0x{s['addr']:08X}")
    print(f"  file  : {binfile}  ({size} bytes)")

    cmd = [
        sys.executable, SWDFLASH_SCRIPT,
        f'--addr=0x{s["addr"]:08X}',
        f'--bin={binfile}',
    ]
    print()
    result = subprocess.run(cmd)
    return result.returncode == 0


if __name__ == '__main__':
    if len(sys.argv) != 3 or sys.argv[1] not in ('0', '1'):
        print("Usage: python burn_slot.py <slot> <file>")
        print("  slot  0 = Slot A,  1 = Slot B")
        sys.exit(1)
    sys.exit(0 if burn_slot(int(sys.argv[1]), sys.argv[2]) else 1)
