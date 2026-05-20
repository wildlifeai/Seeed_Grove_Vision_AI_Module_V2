# WW500 Motion JPEG
#### CGP / Claude — 17 May 2026

## Executive Summary

The WW500 already captures and saves individual JPEG files.  Motion JPEG (MJPEG) extends
this to multi-frame video clips by writing several JPEG frames into a single file.  The
simplest usable format — raw MJPEG (`.mjpg`) — requires almost no new code: stop closing
the file between frames and change the extension.  VLC plays raw MJPEG on Windows and Mac
with no additional installation.  If a fully self-describing container is needed, AVI MJPEG
(`.avi`) is the next step up; it is more compatible but requires writing a structured header
and seeking back to fill in frame counts when the file is closed.

---

## 1. What is Motion JPEG?

Motion JPEG (MJPEG) is a video format in which every frame is stored as a complete,
independently decodable JPEG image.  There is no inter-frame compression (unlike H.264 or
MPEG-4), so:

- Any single frame can be extracted and used as a still image.
- A corrupt frame does not affect neighbouring frames.
- Files are larger than MPEG for the same quality and frame rate.
- Encoding is trivial — the WW500 hardware JPEG encoder already produces individual frames.

MJPEG was widely used in digital cameras, webcams, and IP cameras in the 2000s and is still
common in scientific and industrial imaging because of its simplicity and frame independence.

---

## 2. File formats

### 2.1 Raw MJPEG (`.mjpg` / `.mjpeg`) — recommended starting point

The simplest possible format: JPEG frames concatenated one after another with no wrapper:

```
[JPEG frame 1]  FF D8 ... <EXIF + image data> ... FF D9
[JPEG frame 2]  FF D8 ... <EXIF + image data> ... FF D9
...
[JPEG frame N]  FF D8 ... <EXIF + image data> ... FF D9
```

Each frame is a complete, valid JPEG file.  The file has no header and stores no frame rate;
the player is told (or assumes) the rate.

**Compatibility:**  VLC Media Player opens `.mjpg` files on Windows and macOS and lets the
user set the frame rate at open time (or it uses a default).  Windows Media Player does not
play raw MJPEG without a third-party codec pack.  This matches the user's description of the
format used on the previous project.

**Advantages:**
- Trivial to implement on top of the existing single-frame JPEG code.
- The file can be split back into individual JPEGs with a simple parser (scan for `FF D8`
  ... `FF D9` boundaries).
- If the write is interrupted mid-clip, all frames written so far are valid.
- Each frame retains its full EXIF metadata (timestamp, GPS, NN results — see Section 4).

**Disadvantages:**
- Frame rate not stored in file; must be communicated out-of-band or set by the user at
  playback time.
- Not recognised as a video file by file browsers on Windows or macOS without VLC
  association.

### 2.2 AVI MJPEG (`.avi`)

AVI (Audio Video Interleave) is a Microsoft RIFF-based container.  Most video players on
Windows and macOS play AVI MJPEG natively or with the standard MJPEG codec.  Frame rate,
resolution, and frame count are all stored in the file header.

#### Structure

```
RIFF .... 'AVI '
  LIST .... 'hdrl'          ← header list
    'avih' [56 bytes]       ← main AVI header (frame rate, total frames, dimensions)
    LIST .... 'strl'        ← stream list
      'strh' [56 bytes]     ← stream header (type='vids', handler='MJPG', fps)
      'strf' [40 bytes]     ← BITMAPINFOHEADER (width, height, compression='MJPG')
  LIST .... 'movi'          ← movie data
    '00dc' [size] [JPEG]    ← frame 1 (compressed video chunk)
    '00dc' [size] [JPEG]    ← frame 2
    ...
  'idx1' [16×N bytes]       ← optional index (offset + size of each frame)
```

#### The end-of-file problem

The AVI header must contain the total frame count and total data size, which are not known
until the last frame is written.  Two practical approaches for an embedded system:

**Option A — seek back at close (`f_lseek`):**  Write placeholder zeros in the header,
record the byte offset of each frame's size field, and seek back to update them when
`f_close` is called.  FatFS supports `f_lseek`; this is the standard embedded approach.
Requires tracking ≈ 5 byte-offsets in RAM.

**Option B — pre-allocate for a fixed maximum frame count:**  Write the header with the
maximum expected frame count, then update only the actual-count fields at the end.  Players
tolerate trailing zero-size chunks.  Simpler than option A but wastes a small amount of file
space.

#### The index (`idx1`)

The `idx1` chunk lists the file offset and size of every frame.  It is optional in AVI v1
but some players use it for fast seeking and progress display.  Writing it requires either:
- Buffering frame byte offsets in RAM as frames are written (16 bytes per frame — 80 bytes
  for 5 frames, 960 bytes for 60 frames), or
- A second pass over the file after writing all frames.

For short clips at a few fps, buffering offsets in RAM is practical.

**Advantages over raw MJPEG:**
- Frame rate is stored in the file; playback is correct without user input.
- Recognised as a video file by Windows Explorer and macOS Finder.
- Seeks correctly in VLC and Windows Media Player.

**Disadvantages:**
- Requires implementing a RIFF/AVI writer (~150 lines of new C).
- Must seek back or pre-allocate to write the header at close time.
- Each JPEG frame must be preceded by a 8-byte chunk header (`'00dc'` + size).

### 2.3 Summary

| Format | Extension | VLC | Windows Media Player | macOS QuickTime | Frame rate in file | Implementation effort |
|--------|-----------|-----|---------------------|-----------------|-------------------|----------------------|
| Raw MJPEG | `.mjpg` | Yes | No (needs codec) | No | No | Minimal |
| AVI MJPEG | `.avi` | Yes | Yes | Needs VLC or codec | Yes | Moderate |

---

## 3. EXIF data in MJPEG

Video containers have no EXIF concept.  However, because every frame in an MJPEG file is a
valid JPEG, each frame can carry its own APP1/EXIF segment exactly as the single-frame code
does today.  Most video players ignore EXIF data but it remains in the file and can be
recovered by splitting the file back into individual frames.

### Options

**Per-frame full EXIF (recommended for wildlife monitoring)**

Call `build_exif_segment()` for every frame.  Each frame then carries:
- Timestamp — changes each frame; essential for knowing when each frame was taken.
- GPS location — same for all frames if the device does not move; still correct.
- NN classification result — may change frame-to-frame; useful for post-processing.
- Deployment ID — same for all frames.
- Maker notes (AE gain values) — changes each frame.

The per-frame EXIF overhead is one 512-byte sector write per frame (after the sector-
alignment padding added in the FatFS work).  This is already paid for the first frame; the
cost for subsequent frames is identical.

**Single EXIF on first frame only**

Set `build_exif_segment()` for frame 1 only; subsequent frames start with just the bare
JPEG SOI marker (`FF D8`) and proceed directly to the image data.  This saves one 512-byte
sector write per subsequent frame but loses per-frame timestamp and NN data.  Not
recommended for wildlife monitoring where per-frame metadata has scientific value.

**Hybrid: full EXIF frame 1, timestamp-only frames 2..N**

A middle ground: frame 1 carries full EXIF; subsequent frames carry a minimal EXIF segment
containing only the timestamp tag.  Reduces overhead while preserving timing accuracy.
Requires a separate minimal `build_exif_segment` variant.  Not necessary unless write speed
becomes a bottleneck.

### EXIF and AVI containers

AVI has a `LIST 'INFO'` chunk for file-level text metadata (title, creation date, copyright,
etc.).  This could hold the deployment ID and GPS coordinates once at the start of the file.
It is not EXIF and is not parsed by the existing `exif_utc` / `exif_gps` code, so it would
require new formatting code.  Per-frame EXIF within each `00dc` JPEG chunk is simpler and
more compatible with the existing codebase.

---

## 4. Performance analysis

### Current write time budget

| Quantity | Value |
|----------|-------|
| Image resolution | 640 × 480 |
| Typical JPEG size (x4 quality) | ~20 KB |
| SD card write time per frame | ~40 ms |
| Maximum theoretical frame rate | ~25 fps |

The ~40 ms figure is dominated by SD card NAND programming latency, not SPI transfer time.
It is essentially constant regardless of JPEG size (within the ~10–50 KB range expected).

### Achievable frame rates

The frame write and the camera capture cannot fully overlap: the hardware JPEG encoder writes
into a fixed buffer (`jpegbuf`) which must be consumed (written to SD) before the next frame
can be placed there.  The effective inter-frame interval is therefore:

```
frame_interval ≥ capture_time + write_time
```

| Frame rate | Frame interval | Write budget | Feasibility |
|------------|---------------|--------------|-------------|
| 1 fps | 1000 ms | 960 ms remaining | Comfortable |
| 2 fps | 500 ms | 460 ms remaining | Comfortable |
| 5 fps | 200 ms | 160 ms remaining | Feasible if capture < 160 ms |
| 10 fps | 100 ms | 60 ms remaining | Tight — depends on capture time |
| 25 fps | 40 ms | 0 ms remaining | Not feasible with current write time |

For wildlife trigger clips at a few fps, the 40 ms write time is not a constraint.  The
capture time (sensor exposure + frame readout + JPEG encode) will be the practical limit
and is sensor-dependent.

### File size estimate

At ~20 KB per frame:

| Duration | Frame rate | Frames | File size |
|----------|-----------|--------|-----------|
| 5 s | 2 fps | 10 | ~200 KB |
| 10 s | 5 fps | 50 | ~1 MB |
| 30 s | 5 fps | 150 | ~3 MB |
| 60 s | 5 fps | 300 | ~6 MB |

These are well within FAT32 file size limits and typical SD card capacity.

---

## 5. Recommended implementation

### Phase 1 — Raw MJPEG (`.mjpg`)

This is the minimum viable change and should be implemented first to validate the concept
before adding the complexity of an AVI container.

#### What changes

**`image_task.c`**

1. Add a compile-time or runtime `MJPEG_MODE` flag (or extend the existing capture count
   logic to recognise multi-frame mode).

2. In `prepareJpegFile`: generate the filename only on the first frame (reuse the same
   filename for subsequent frames).  Set `fileOp.closeWhenDone = false` for all frames
   except the last; set it `true` on the last frame.

3. On the first frame, generate a filename with `.mjpg` extension instead of `.JPG`.

4. Call `build_exif_segment()` for every frame (no change from the single-frame path).

**`fatfs_task.c` — `fileWriteImage`**

The existing guard that closes a stale open file must be made MJPEG-aware.  Currently:

```c
// Guard: if a previous write leaked an open file, close it before opening a new one.
if (dirManager->imagesOpen) {
    f_close(&dirManager->imagesFile);
    dirManager->imagesOpen = false;
}
```

For MJPEG, `imagesOpen == true` on frames 2..N is intentional, not a bug.  Add a flag to
`directoryManager_t` (e.g. `imagesAppendMode`) to distinguish the two cases:

```c
if (dirManager->imagesOpen) {
    if (dirManager->imagesAppendMode) {
        // Intentional: skip f_open, append to existing file
        goto write_data;
    }
    // Bug: stale open file — close it before opening a new one
    f_close(&dirManager->imagesFile);
    dirManager->imagesOpen = false;
}
```

When appending, call `f_write` directly without `f_open`.  When `closeWhenDone` is true
(last frame), call `f_close` and clear `imagesAppendMode`.

#### Sequence (3-frame example)

```
Frame 1:  f_open("CLIP001.mjpg")
          f_write(exif_buffer, 512)      ← SOI + EXIF
          f_write(jpeg_body, ~19 KB)     ← JPEG body (no SOI)
          [file stays open]

Frame 2:  [file already open — skip f_open]
          f_write(exif_buffer, 512)      ← SOI + EXIF (new timestamp, new NN result)
          f_write(jpeg_body, ~19 KB)

Frame 3:  [file already open — skip f_open]
          f_write(exif_buffer, 512)
          f_write(jpeg_body, ~19 KB)
          f_close()                      ← closeWhenDone = true on last frame
```

#### Disk write count per frame in MJPEG mode

Compared with the per-file anatomy for single-frame JPEG (9 writes including FAT, directory,
and FSINFO), MJPEG reduces the metadata writes to once per clip:

| Write | Single-frame mode | MJPEG (per frame) |
|-------|------------------|-------------------|
| Directory reservation | Once per file | Once per clip |
| EXIF sector | Once per file | Once per frame |
| JPEG body CMD25 | Once per file | Once per frame |
| FAT table 1 & 2 | Once per file | Once per clip |
| Directory update | Once per file | Once per clip |
| FSINFO | Once per file | Once per clip |

For a 10-frame clip the 5 per-clip writes are amortised across 10 frames — a meaningful
reduction in metadata overhead compared with 10 separate JPEG files.

### Phase 2 — AVI MJPEG (`.avi`)

Implement if raw MJPEG proves inadequate (e.g. Windows Media Player support is required
without a codec, or correct automatic frame rate playback is needed).

#### New code required

- `avi_writer.c` / `avi_writer.h`: functions to write the AVI RIFF header, per-frame chunk
  headers, and the idx1 index.  Approximately 150–200 lines.
- An array of frame offsets in RAM (16 bytes per frame) for the idx1 index.
- `f_lseek` calls at file close to back-fill frame count and total size into the header.

#### AVI chunk wrapping

Each JPEG frame in AVI is prefixed with an 8-byte chunk header:

```c
// Before each JPEG frame:
uint8_t chunk_hdr[8];
memcpy(chunk_hdr, "00dc", 4);        // compressed video chunk type
write32_le(chunk_hdr + 4, jpeg_size); // size of the JPEG data
f_write(&file, chunk_hdr, 8, &bw);
f_write(&file, jpeg_data, jpeg_size, &bw);
if (jpeg_size & 1) {
    uint8_t pad = 0;                 // RIFF chunks must be word-aligned
    f_write(&file, &pad, 1, &bw);
}
```

#### EXIF in AVI

Each `00dc` chunk contains a complete JPEG, so per-frame EXIF works identically to the
raw MJPEG case.  No change to `build_exif_segment()` is needed.

---

## 6. EXIF sector alignment in multi-frame context

The sector-alignment padding added to `build_exif_segment()` (padding the EXIF to the next
512-byte boundary with a JPEG Comment segment) remains correct for MJPEG.  Each frame write
begins with a 512-byte EXIF sector, so the JPEG body of every frame starts on a sector
boundary, and FatFS issues a single CMD25 multi-block write for each body regardless of
frame number.

---

## 7. Summary of recommended steps

1. **Implement raw MJPEG** by modifying `prepareJpegFile` and `fileWriteImage` as described
   in Section 5.  Use full per-frame EXIF.  Validate with VLC on Windows and macOS.

2. **Test at 2 fps and 5 fps** with 5–10 frame clips.  Verify the file plays correctly in
   VLC, that timestamps in the per-frame EXIF are correct, and that there are no FatFS
   errors on multi-frame close.

3. **Evaluate AVI** only if raw MJPEG does not meet playback requirements.  The additional
   implementation cost (AVI writer + idx1 + f_lseek) is justified only if Windows Media
   Player native support or correct automatic frame rate is required.
