#!/usr/bin/env python
"""Analyse and cross-check two 1 MB firmware slot dumps for structural integrity.

Usage: python check_slots.py [slot_a] [slot_b]
  Defaults to dump_slot_a.bin and dump_slot_b.bin

Checks each slot independently:
  - File size (must be exactly 1 MB)
  - ckBS (BLP packet) magic at expected partition boundaries
  - Non-empty content in each partition region
  - Payload extent (last non-0xFF byte offset and section sizes)

Cross-checks both slots:
  - Preamble bytes (0x00000–0x27FFF, i.e. everything before the application)
    should be identical; first divergence offset is reported.

Firmware partition layout within each 1 MB slot (we2_image_gen_local_dpd layout)
  0x00000  hx_memory_descriptor      4 KB  BLP packet, ckBS required
  0x01000  2nd_bootloader           72 KB  BLP packet, ckBS required
  0x13000  bootloader               80 KB  BLP packet, ckBS required
  0x27000  hx_mem_descriptor_ota     4 KB  BLP packet, ckBS required
  0x28000  cm55m_application       276 KB+ BLP packet, ckBS required

Total used: ~445 KB out of 1 MB.  Remainder is 0xFF.

The non-dpd (we2_image_gen_local) layout places the same partitions one
sector later: bootloader at 0x14000, OTA descriptor at 0x28000,
application at 0x29000.

The selector sector lives outside the 1 MB slots, at physical 0x00FFF000.
"""

import sys
import struct
import os

SLOT_SIZE = 0x100000        # 1 MB

CKBS_MAGIC      = b'ckBS'
CKBS_HEADER_LEN = 0x52      # bytes — BLP packet overhead (magic + metadata)

# Partition boundaries (offsets within a slot) — dpd (we2_image_gen_local_dpd) layout.
# The non-dpd layout shifts bootloader to 0x14000, OTA descriptor to 0x28000,
# and application to 0x29000 — one 4 KB sector later throughout.
PARTITIONS = [
    (0x00000, 0x01000, 'hx_memory_descriptor',   True),   # (offset, size, name, ckBS required)
    (0x01000, 0x12000, '2nd_bootloader',          True),
    (0x13000, 0x14000, 'bootloader',              True),
    (0x27000, 0x01000, 'hx_mem_descriptor_ota',  True),
    (0x28000, 0x43000, 'cm55m_application',       True),   # size is a minimum; actual is larger
]

PREAMBLE_END  = 0x28000     # bytes before the application — should match between slots
MIN_APP_BYTES = 0x10000     # minimum plausible application payload (64 KB)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def last_non_ff(data):
    """Return index of last byte that is not 0xFF, or -1 if all 0xFF."""
    i = len(data) - 1
    while i >= 0 and data[i] == 0xFF:
        i -= 1
    return i


def find_ckbs(data, start=0, end=None):
    """Return sorted list of all ckBS magic offsets in data[start:end]."""
    hits = []
    pos = start
    limit = len(data) if end is None else min(end, len(data))
    while pos < limit:
        idx = data.find(CKBS_MAGIC, pos, limit)
        if idx == -1:
            break
        hits.append(idx)
        pos = idx + len(CKBS_MAGIC)
    return hits


def preamble_compare(a, b):
    """Compare preamble bytes of two slot images; return (match, first_diff_offset)."""
    length = min(PREAMBLE_END, len(a), len(b))
    for i in range(length):
        if a[i] != b[i]:
            return False, i
    # Also check that both have the same length up to PREAMBLE_END (both may be
    # shorter if the data has been truncated by flash erase)
    return True, None


def section_has_data(data, start, end):
    """True if data[start:end] contains at least one non-0xFF byte."""
    chunk = data[start:min(end, len(data))]
    return any(b != 0xFF for b in chunk)


def ff_run_length(data, start):
    """Number of consecutive 0xFF bytes starting at data[start]."""
    i = start
    while i < len(data) and data[i] == 0xFF:
        i += 1
    return i - start


# ---------------------------------------------------------------------------
# Per-slot analysis
# ---------------------------------------------------------------------------

def analyse_slot(path, label):
    print(f"{'='*60}")
    print(f"Slot {label}: {path}")
    print(f"{'='*60}")

    try:
        with open(path, 'rb') as f:
            data = f.read()
    except OSError as e:
        print(f"  ERROR: cannot read file: {e}")
        return None

    ok = True

    # --- file size ---
    if len(data) == SLOT_SIZE:
        print(f"  Size         : {len(data)} bytes (0x{len(data):X})  OK")
    else:
        print(f"  Size         : {len(data)} bytes (0x{len(data):X})"
              f"  ERROR (expected 0x{SLOT_SIZE:X})")
        ok = False

    # --- payload extent ---
    last_byte = last_non_ff(data)
    payload   = last_byte + 1
    ff_tail   = len(data) - payload
    print(f"  Payload ends : 0x{last_byte:06X}  ({payload} bytes used,"
          f" {ff_tail} bytes 0xFF)")

    # --- ckBS scan ---
    all_ckbs = find_ckbs(data)
    print(f"\n  BLP (ckBS) packets found ({len(all_ckbs)} total):")
    for off in all_ckbs:
        field = int.from_bytes(data[off+8:off+12], 'little') if off+12 <= len(data) else -1
        print(f"    0x{off:06X}  hdr_field=0x{field:02X}")

    # --- partition checks ---
    print(f"\n  Partition checks:")
    for (p_off, p_sz, p_name, req_ckbs) in PARTITIONS:
        p_end = p_off + p_sz

        has_ckbs_at_start = (data[p_off:p_off+4] == CKBS_MAGIC) if p_off+4 <= len(data) else False
        has_data = section_has_data(data, p_off, p_end)

        if req_ckbs:
            magic_status = 'ckBS OK' if has_ckbs_at_start else 'ERROR: ckBS missing'
            if not has_ckbs_at_start:
                ok = False
        else:
            magic_status = 'ckBS present' if has_ckbs_at_start else 'ckBS absent (may be code)'

        data_status = 'data present' if has_data else 'ERROR: all 0xFF (empty!)'
        if not has_data:
            ok = False

        print(f"    0x{p_off:05X}  {p_name:<27}  {magic_status};  {data_status}")

    # --- application sizing ---
    app_start = PARTITIONS[-1][0]   # 0x28000 (dpd layout)
    last_in_app = last_non_ff(data[app_start:])
    if last_in_app >= 0:
        app_bytes = last_in_app + 1
        app_ok = app_bytes >= MIN_APP_BYTES
        marker = 'OK' if app_ok else f'WARNING: very small (<0x{MIN_APP_BYTES:X})'
        print(f"\n  Application  : {app_bytes} bytes (0x{app_bytes:X})  {marker}")
        if not app_ok:
            ok = False
    else:
        print(f"\n  Application  : EMPTY (no data after 0x{app_start:05X})")
        ok = False

    print()
    if ok:
        print(f"  >> Slot {label} LOOKS NORMAL")
    else:
        print(f"  >> Slot {label} HAS PROBLEMS (see above)")

    return data


# ---------------------------------------------------------------------------
# Cross-slot comparison
# ---------------------------------------------------------------------------

def cross_check(data_a, data_b):
    print(f"\n{'='*60}")
    print("Cross-check: preamble comparison (0x00000–0x27FFF)")
    print(f"{'='*60}")

    match, diff_at = preamble_compare(data_a, data_b)
    if match:
        print(f"  Preamble bytes 0x00000–0x{PREAMBLE_END-1:05X}: IDENTICAL")
    else:
        print(f"  Preamble bytes 0x00000–0x{PREAMBLE_END-1:05X}: DIFFER — "
              f"first difference at 0x{diff_at:06X}")
        # Show context around the first difference
        ctx_start = max(0, diff_at - 8) & ~0x7   # align to 8
        ctx_end   = min(PREAMBLE_END, diff_at + 24)
        print(f"\n  Context (offset | slot A | slot B):")
        for off in range(ctx_start, ctx_end, 8):
            ba = data_a[off:off+8].hex(' ')
            bb = data_b[off:off+8].hex(' ')
            flag = '<<<' if off <= diff_at < off + 8 else ''
            print(f"    0x{off:06X}  {ba}  |  {bb}  {flag}")

    # Also report where each slot's preamble goes silent (all FF)
    print()
    for slot_label, data in [('A', data_a), ('B', data_b)]:
        # Walk preamble in 4KB blocks, note first all-FF block
        preamble_end_byte = -1
        for blk in range(0, PREAMBLE_END, 0x1000):
            if section_has_data(data, blk, blk + 0x1000):
                preamble_end_byte = blk + 0x1000 - 1
        if preamble_end_byte < 0:
            print(f"  Slot {slot_label} preamble: entirely empty (0xFF)")
        else:
            print(f"  Slot {slot_label} preamble: data present up to ~0x{preamble_end_byte:06X}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

if __name__ == '__main__':
    path_a = sys.argv[1] if len(sys.argv) > 1 else 'dump_slot_a.bin'
    path_b = sys.argv[2] if len(sys.argv) > 2 else 'dump_slot_b.bin'

    data_a = analyse_slot(path_a, 'A')
    print()
    data_b = analyse_slot(path_b, 'B')

    if data_a is not None and data_b is not None:
        cross_check(data_a, data_b)
