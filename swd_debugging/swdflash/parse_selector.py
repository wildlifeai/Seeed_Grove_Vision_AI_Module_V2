#!/usr/bin/env python
"""Parse and validate a slot selector binary file.

Usage: python parse_selector.py [file]
  file  path to binary dump of the selector sector (default: dump_selector.bin)

Checks the 20-byte SlotSelectorHeader at the start of the file and reports
which firmware slot the bootloader will boot from.

SlotSelectorHeader layout (little-endian, 20 bytes total):
  offset  0  char[8]   magic        "HIMAXWE2" (not NUL-terminated)
  offset  8  uint32    flash_offset 0x00000000=Slot A, 0x00100000=Slot B
  offset 12  uint32    constant_02  always 0x00000002
  offset 16  uint16    hx_dsp_flag  always 0x0001
  offset 18  uint16    checksum     0x4D04=Slot A, 0x167C=Slot B
"""

import sys
import struct

MAGIC           = b'HIMAXWE2'
SLOT_A_OFFSET   = 0x00000000
SLOT_B_OFFSET   = 0x00100000
SLOT_A_CHECKSUM = 0x4D04
SLOT_B_CHECKSUM = 0x167C
CONSTANT_02     = 0x00000002
HX_DSP_FLAG     = 0x0001

HEADER_FMT  = '<8sIIHH'
HEADER_SIZE = struct.calcsize(HEADER_FMT)   # 20 bytes


def parse_selector(path):
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except OSError as e:
        print(f"ERROR: cannot read '{path}': {e}")
        return False

    print(f"File : {path}  ({len(data)} bytes)")

    if len(data) < HEADER_SIZE:
        print(f"ERROR: file too small — need {HEADER_SIZE} bytes, got {len(data)}")
        return False

    magic, flash_offset, constant_02, hx_dsp_flag, checksum = \
        struct.unpack_from(HEADER_FMT, data, 0)

    ok = True

    # magic
    magic_ok = (magic == MAGIC)
    print(f"  magic        : {magic!r:<14}  {'OK' if magic_ok else f'ERROR (expected {MAGIC!r})'}")
    ok = ok and magic_ok

    # flash_offset → determines slot
    if flash_offset == SLOT_A_OFFSET:
        slot, slot_name, expected_checksum = 0, 'A', SLOT_A_CHECKSUM
    elif flash_offset == SLOT_B_OFFSET:
        slot, slot_name, expected_checksum = 1, 'B', SLOT_B_CHECKSUM
    else:
        slot, slot_name, expected_checksum = None, '?', None

    offset_ok = slot is not None
    print(f"  flash_offset : 0x{flash_offset:08X}      "
          f"{'→ Slot ' + slot_name if offset_ok else 'ERROR (unknown offset)'}")
    ok = ok and offset_ok

    # constant_02
    c02_ok = (constant_02 == CONSTANT_02)
    print(f"  constant_02  : 0x{constant_02:08X}      "
          f"{'OK' if c02_ok else f'ERROR (expected 0x{CONSTANT_02:08X})'}")
    ok = ok and c02_ok

    # hx_dsp_flag
    dsp_ok = (hx_dsp_flag == HX_DSP_FLAG)
    print(f"  hx_dsp_flag  : 0x{hx_dsp_flag:04X}            "
          f"{'OK' if dsp_ok else f'ERROR (expected 0x{HX_DSP_FLAG:04X})'}")
    ok = ok and dsp_ok

    # checksum
    if expected_checksum is not None:
        csum_ok = (checksum == expected_checksum)
        print(f"  checksum     : 0x{checksum:04X}            "
              f"{'OK' if csum_ok else f'ERROR (expected 0x{expected_checksum:04X})'}")
        ok = ok and csum_ok
    else:
        print(f"  checksum     : 0x{checksum:04X}            (cannot validate — unknown slot)")

    print()
    if ok:
        print(f"Selector VALID — board will boot Slot {slot_name} (slot {slot})")
    else:
        print("Selector INVALID or unrecognised")

    return ok


if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else 'dump_selector.bin'
    sys.exit(0 if parse_selector(path) else 1)
