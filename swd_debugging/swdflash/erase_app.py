#!/usr/bin/env python
"""Erase only the application portion of a firmware slot.

Erases from the cm55m_application offset (0x28000) to the end of the slot,
leaving the boot chain components at 0x00000–0x27FFF intact:
  hx_memory_descriptor, 2nd_bootloader, bootloader, hx_mem_descriptor_ota.

Usage: python erase_app.py <slot>
  slot  0 = Slot A  (erases 0x00028000 – 0x000FFFFF)
        1 = Slot B  (erases 0x00128000 – 0x001FFFFF)

If this leaves the board bootable, the firmware update command is safe to
target only the application area — a partial write cannot corrupt the boot chain.
"""

import sys
import os
import tempfile
import subprocess

SLOT_BASE = {0: 0x00000000, 1: 0x00100000}
SLOT_SIZE  = 0x100000   # 1 MB
APP_OFFSET = 0x28000    # cm55m_application start (dpd layout)
APP_SIZE   = SLOT_SIZE - APP_OFFSET   # 0xD8000 = 864 KB

SWDFLASH_SCRIPT = os.path.join(os.path.dirname(__file__), 'swdflash.py')


def erase_app(slot_num):
    if slot_num not in SLOT_BASE:
        print(f"ERROR: slot must be 0 or 1, got {slot_num!r}")
        return False

    slot_name = 'A' if slot_num == 0 else 'B'
    base = SLOT_BASE[slot_num]
    app_addr = base + APP_OFFSET
    chain_end = base + APP_OFFSET - 1

    print(f"Erasing application in Slot {slot_name} (slot {slot_num})")
    print(f"  erase  : 0x{app_addr:08X} – 0x{base + SLOT_SIZE - 1:08X}"
          f"  (0x{APP_SIZE:X} = {APP_SIZE // 1024} KB)")
    print(f"  kept   : 0x{base:08X} – 0x{chain_end:08X}"
          f"  (boot chain, 0x{APP_OFFSET:X} = {APP_OFFSET // 1024} KB)")

    with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as tf:
        tmpfile = tf.name
        tf.write(b'\xFF' * APP_SIZE)

    try:
        cmd = [
            sys.executable, SWDFLASH_SCRIPT,
            '--operation=erase_only',
            f'--addr=0x{app_addr:08X}',
            f'--bin={tmpfile}',
        ]
        print()
        result = subprocess.run(cmd)
        return result.returncode == 0
    finally:
        os.unlink(tmpfile)


if __name__ == '__main__':
    if len(sys.argv) != 2 or sys.argv[1] not in ('0', '1'):
        print("Usage: python erase_app.py <slot>")
        print("  slot  0 = Slot A,  1 = Slot B")
        sys.exit(1)
    sys.exit(0 if erase_app(int(sys.argv[1])) else 1)
