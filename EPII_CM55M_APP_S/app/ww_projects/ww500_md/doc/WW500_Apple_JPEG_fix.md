# WW500 Apple JPEG Compatibility Fix
#### CGP — 9 June 2026

## Overview

JPEG files produced by recent WW500 firmware cannot be opened on Apple devices (macOS,
iOS, or the custom iOS app), while files from older firmware open correctly.

The root cause is a 2-byte error in the EXIF APP1 segment length field inside
`build_exif_segment()` in `image_task.c`. The bug has always been present but was masked
by the file structure produced by older firmware. A later change that added sector-alignment
COM padding exposed the bug in a way that Apple's strict JPEG parser rejects.

---

## Sample Files

| File | Opens on Apple? | Firmware era |
|------|----------------|--------------|
| `MD011869.JPG` | Yes ✓ | Older (no COM padding, no GPS IFD) |
| `A27AE710.JPG` | No ✗  | Newer (COM padding + GPS IFD added) |

---

## JPEG Structure Analysis

### MD011869.JPG — working

```
SOI      offset 0x00000   2 bytes
APP1     offset 0x00002   418 bytes  (EXIF, length field = 416)
APP0     offset 0x001A4   18 bytes   (JFIF)
DQT×2, SOF0, DHT×4, SOS, ECD, EOI
TRAILING offset 0x021AF   35 bytes   (hardware encoder padding, all zeros)
```

After APP1 (at byte 0x01A4): **0xFF** → valid marker, parser continues ✓

### A27AE710.JPG — failing

```
SOI      offset 0x00000   2 bytes
APP1     offset 0x00002   421 bytes  (EXIF, length field = 419)
GAP      offset 0x001A7   89 bytes   ← parser stalls here
APP0     offset 0x00200   18 bytes   (JFIF)
DQT×2, SOF0, DHT×4, SOS, ECD, EOI
TRAILING offset 0x03D3F   35 bytes
```

After APP1 (at byte 0x01A7): **0x00** → not a valid marker → Apple parser fails ✗

The 89 bytes that the tool reports as a GAP are in fact a valid JPEG Comment (`0xFF 0xFE`)
sector-alignment pad — but because the APP1 length field is 2 bytes too large, the COM
marker's first two bytes (`0xFF 0xFE`) are consumed inside the APP1 payload.  What remains
after APP1 is the COM *length field* (`0x00 0x59`), not a marker, so a strict parser fails.

---

## Root Cause

### JPEG APP1 segment structure

```
[FF E1]        ← APP1 marker    (2 bytes — NOT counted in the length field)
[len_hi lo]    ← length field   (2 bytes — the value includes these 2 bytes
                                            plus all payload bytes)
[payload ...]  ← EXIF data
```

Per the JPEG / EXIF specification, the **length field value** must equal:

```
2  (for the length bytes)  +  N  (payload bytes)
```

Neither the SOI (0xFFD8) nor the APP1 marker (0xFFE1) are counted.

### The bug in `build_exif_segment()`

`image_task.c` around line 2779:

```c
uint16_t len = (next_data_ptr - exif_buffer) - 2; // exclude 0xFFE1 marker
write16_be(len_ptr, len);
```

`exif_buffer` starts with **SOI (2 bytes)** followed by the APP1 marker (2 bytes).
`next_data_ptr - exif_buffer` equals the total number of bytes written, which includes
both the SOI and the APP1 marker.

| Quantity | Value | Bytes subtracted |
|----------|-------|-----------------|
| `next_data_ptr - exif_buffer` | total EXIF buffer length | — |
| Subtract SOI | −2 | |
| Subtract APP1 marker | −2 | |
| **Correct length field value** | **`total − 4`** | **4** |
| **Code actually subtracts** | | **2** |
| **Error** | **+2 bytes** | |

The comment in the code says "exclude 0xFFE1 marker" but only the APP1 marker's 2 bytes
are excluded — the SOI's 2 bytes are not.  The length field is therefore always 2 bytes
too large.

### Why older files were not affected

The bug exists in both old and new firmware.  In older firmware there was no sector-alignment
COM padding.  The EXIF buffer was written directly before the JPEG body (without SOI), and
the first byte of the JPEG body is always `0xFF` (the start of the APP0/JFIF marker `0xFFE0`).

With the over-sized APP1 length, the parser over-reads by 2 bytes but then finds `0xFF` at
the (slightly wrong) position and successfully parses APP0.  Windows and most other parsers
are lenient enough to continue.

Commit `e3e6499a` ("Pad EXIF data to 512 byte increments so fatfs writes are optimised")
added the COM padding.  Now the 2 bytes that APP1 incorrectly consumes are the COM marker
`0xFF 0xFE`, and the next byte after the over-sized APP1 is `0x00` (the COM length MSB).
Apple's parser treats any non-`0xFF` byte between segments as a fatal error.

---

## Verification

Running `python3 _Tools/jpeg_segments.py` on both files confirms the structural difference.
A targeted byte-inspection of `A27AE710.JPG` at the APP1 boundary:

```
With current (buggy) code:
  APP1 length field : 419
  APP1 ends at byte : 423 = 0x01A7
  Byte at 0x01A7    : 0x00  ← NOT a valid marker — Apple fails here

With the fix applied (length field = 417):
  APP1 ends at byte : 421 = 0x01A5
  Byte at 0x01A5    : 0xFF  ← valid marker (COM segment 0xFFFE)
  Next 4 bytes      : FF FE 00 59  (COM marker + length 89)
  COM segment ends  : 0x01A5 + 2 + 89 = 0x0200 = APP0 ✓
```

---

## Differences Between Old and New Firmware (EXIF Content)

| Feature | MD011869.JPG | A27AE710.JPG |
|---------|-------------|-------------|
| IFD entries | 10 | 11 |
| GPS IFD | None | Yes (zeros — no fix) |
| MakerNote (AE regs) | No | Yes |
| UserComment | Yes (NN scores) | No (model not loaded) |
| DeploymentID | Yes | Yes |
| Sector-alignment COM pad | No | Yes (89 bytes) |

The GPS IFD and MakerNote tags were added in recent firmware and make the EXIF slightly
larger, but they are not themselves problematic.

---

## Secondary Observations

### 1 — Trailing 35 bytes after EOI

Both files contain 35 zero bytes following the JPEG EOI marker.  These are produced by the
hardware JPEG encoder as padding.  All conforming parsers stop at EOI; these bytes are
ignored and cause no compatibility issue.

### 2 — JFIF APP0 alongside EXIF APP1

The JPEG body produced by the Himax hardware encoder is in JFIF format (starts with an
APP0 segment identifying `JFIF`).  The firmware prepends a custom EXIF APP1 block,
resulting in a file that contains both APP0 and APP1.  Strictly, JFIF and EXIF are
mutually exclusive (EXIF 2.3 §4.5.4), but virtually all real-world parsers handle this
combination without issue.

### 3 — UserComment tag type (minor EXIF violation)

`TAG_USER_COMMENT` (0x9286) is stored with `TYPE_ASCII` (2), but the EXIF 2.3 specification
requires this tag to use `TYPE_UNDEFINED` (7) with the first 8 bytes as a charset identifier
(e.g. `ASCII\x00\x00\x00`).  Most parsers accept either form, and this is not the cause of
the Apple rejection.  It is worth correcting as a follow-up for full standard compliance.

---

## Proposed Fix

A single change in `build_exif_segment()` in `image_task.c`:

### Current code (buggy) — around line 2779

```c
uint16_t len = (next_data_ptr - exif_buffer) - 2; // exclude 0xFFE1 marker
write16_be(len_ptr, len);
```

### Proposed fix

```c
/* APP1 length = bytes from the length field itself to end of EXIF payload.
 * Exclude the SOI (2 bytes at exif_buffer[0]) and the APP1 marker (2 bytes
 * at exif_buffer[2]).  len_ptr = exif_buffer + 4, so this is equivalent to
 * (next_data_ptr - len_ptr). */
uint16_t len = (uint16_t)(next_data_ptr - len_ptr);
write16_be(len_ptr, len);
```

`len_ptr` is already declared two lines earlier and points to `exif_buffer + 4` (the first
byte of the APP1 length field), so `next_data_ptr - len_ptr` is both correct and
self-documenting.

Alternatively, if a numeric offset is preferred:

```c
uint16_t len = (uint16_t)((next_data_ptr - exif_buffer) - 4); // exclude SOI + APP1 marker
```

### Effect of the fix

| Scenario | Before fix | After fix |
|----------|-----------|-----------|
| No COM padding (small EXIF) | Works (first byte of JPEG body happens to be 0xFF) | Works (same behaviour, now correct) |
| With COM padding | Fails on Apple (0x00 after APP1) | Works (0xFF COM marker after APP1) |

The fix produces JPEG files that are correct per the EXIF 2.3 / JPEG ISO 10918-1 specification.

---

## Suggested Follow-up Change (not required for fix)

Correct the `TAG_USER_COMMENT` tag type from `TYPE_ASCII` to `UNDEFINED` with an 8-byte
ASCII charset prefix, per EXIF 2.3 §4.6.5.  This will make the UserComment tag readable
by strict EXIF parsers including some Apple frameworks.

In `addIFD()` for the `TAG_USER_COMMENT` case:

```c
case TAG_USER_COMMENT:
{
    const char *text = (const char *)tagData;
    uint32_t text_len = strlen(text);
    uint32_t total_len = 8 + text_len;            // charset prefix + text
    write16_le(entry_ptr, TAG_USER_COMMENT);
    write16_le(entry_ptr + 2, UNDEFINED);          // type 7 per EXIF spec
    write32_le(entry_ptr + 4, total_len);
    write32_le(entry_ptr + 8, (uint32_t)(next_data_ptr - tiff_start));
    memcpy(next_data_ptr, "ASCII\0\0\0", 8);       // charset identifier
    memcpy(next_data_ptr + 8, text, text_len);
    next_data_ptr += total_len;
    break;
}
```

---

## Files Referenced

| File | Purpose |
|------|---------|
| `EPII_CM55M_APP_S/app/ww_projects/ww500_md/image_task.c` | EXIF construction (`build_exif_segment()`) |
| `EPII_CM55M_APP_S/app/ww_projects/ww500_md/claude/MD011869.JPG` | Working sample (old firmware) |
| `EPII_CM55M_APP_S/app/ww_projects/ww500_md/claude/A27AE710.JPG` | Failing sample (new firmware) |
| `_Tools/jpeg_segments.py` | JPEG structure analyser |
| `_Tools/jpegdump_exif.py` | EXIF tag dumper |
