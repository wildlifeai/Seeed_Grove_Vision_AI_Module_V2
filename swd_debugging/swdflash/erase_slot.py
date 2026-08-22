#!/usr/bin/env python
"""Erase one 1 MB firmware slot (fill with 0xFF).

Usage: python erase_slot.py <slot>
  slot  0 = Slot A (0x00000000)
        1 = Slot B (0x00100000)

Writes a 1 MB all-0xFF image via swdflash.py --operation erase_only.
The flash algorithm erases and writes 0xFF, leaving the slot in the blank
erased state identical to a freshly manufactured chip.
"""

import sys
import os
import tempfile
import subprocess

SLOTS = {
    0: {'name': 'A', 'addr': 0x00000000},
    1: {'name': 'B', 'addr': 0x00100000},
}

SLOT_SIZE       = 0x100000          # 1 MB
SWDFLASH_SCRIPT = os.path.join(os.path.dirname(__file__), 'swdflash.py')


def erase_slot(slot_num):
    if slot_num not in SLOTS:
        print(f"ERROR: slot must be 0 or 1, got {slot_num!r}")
        return False

    s = SLOTS[slot_num]
    print(f"Erasing Slot {s['name']} (slot {slot_num})")
    print(f"  addr  : 0x{s['addr']:08X}")
    print(f"  size  : 0x{SLOT_SIZE:X} ({SLOT_SIZE // 1024} KB)")

    with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as tf:
        tmpfile = tf.name
        tf.write(b'\xFF' * SLOT_SIZE)

    try:
        cmd = [
            sys.executable, SWDFLASH_SCRIPT,
            '--operation=erase_only',
            f'--addr=0x{s["addr"]:08X}',
            f'--bin={tmpfile}',
        ]
        print()
        result = subprocess.run(cmd)
        return result.returncode == 0
    finally:
        os.unlink(tmpfile)


if __name__ == '__main__':
    if len(sys.argv) != 2 or sys.argv[1] not in ('0', '1'):
        print("Usage: python erase_slot.py <slot>")
        print("  slot  0 = Slot A,  1 = Slot B")
        sys.exit(1)
    sys.exit(0 if erase_slot(int(sys.argv[1])) else 1)
