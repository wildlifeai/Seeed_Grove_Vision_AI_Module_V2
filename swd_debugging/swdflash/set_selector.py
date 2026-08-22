#!/usr/bin/env python
"""Write the slot selector sector to select Slot A (0) or Slot B (1).

Usage: python set_selector.py <slot>
  slot  0 = Slot A (flash_offset 0x00000000, checksum 0x4D04)
        1 = Slot B (flash_offset 0x00100000, checksum 0x167C)

Builds a 4096-byte sector image with the correct SlotSelectorHeader at offset 0
and 0xFF padding, then programs it into flash at 0x00FFF000 via swdflash.py.
"""

import sys
import os
import struct
import tempfile
import subprocess

MAGIC       = b'HIMAXWE2'
CONSTANT_02 = 0x00000002
HX_DSP_FLAG = 0x0001
SECTOR_SIZE = 4096

SLOTS = {
    0: {'name': 'A', 'flash_offset': 0x00000000, 'checksum': 0x4D04},
    1: {'name': 'B', 'flash_offset': 0x00100000, 'checksum': 0x167C},
}

HEADER_FMT      = '<8sIIHH'
SELECTOR_ADDR   = 0x00FFF000
SWDFLASH_SCRIPT = os.path.join(os.path.dirname(__file__), 'swdflash.py')


def build_sector(slot_num):
    s = SLOTS[slot_num]
    header = struct.pack(HEADER_FMT,
                         MAGIC,
                         s['flash_offset'],
                         CONSTANT_02,
                         HX_DSP_FLAG,
                         s['checksum'])
    assert len(header) == 20
    return header + b'\xFF' * (SECTOR_SIZE - len(header))


def set_selector(slot_num):
    if slot_num not in SLOTS:
        print(f"ERROR: slot must be 0 or 1, got {slot_num!r}")
        return False

    s = SLOTS[slot_num]
    print(f"Programming selector sector to Slot {s['name']} (slot {slot_num})")
    print(f"  flash_offset : 0x{s['flash_offset']:08X}")
    print(f"  checksum     : 0x{s['checksum']:04X}")
    print(f"  target addr  : 0x{SELECTOR_ADDR:08X}")

    sector = build_sector(slot_num)
    assert len(sector) == SECTOR_SIZE

    with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as tf:
        tmpfile = tf.name
        tf.write(sector)

    try:
        cmd = [
            sys.executable, SWDFLASH_SCRIPT,
            f'--bin={tmpfile}',
            f'--addr=0x{SELECTOR_ADDR:08X}',
        ]
        print()
        result = subprocess.run(cmd)
        return result.returncode == 0
    finally:
        os.unlink(tmpfile)


if __name__ == '__main__':
    if len(sys.argv) != 2 or sys.argv[1] not in ('0', '1'):
        print("Usage: python set_selector.py <slot>")
        print("  slot  0 = Slot A,  1 = Slot B")
        sys.exit(1)
    sys.exit(0 if set_selector(int(sys.argv[1])) else 1)
