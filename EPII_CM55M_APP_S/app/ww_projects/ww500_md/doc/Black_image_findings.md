# JPEG Black Image Analysis — Findings

## Overview

Four JPEG images were examined: two that display normally and two that appear black.
All four images are `640 × 480` pixels, baseline DCT, 4:2:0 chroma subsampling,
produced by the same camera with the same encoding settings.

---

## File Sizes

| File | Size | Visible? |
|------|------|----------|
| 59200840.JPG | 12.2 KB | Yes |
| 59200860.JPG | 12.2 KB | Yes |
| 59200CD1.JPG | 6.1 KB | No — black |
| 59200CD2.JPG | 6.1 KB | No — black |

The black images are roughly **half the size** of the visible ones.

---

## Segment Structure

Every JPEG file is a sequence of typed blocks (segments). All four files share
an identical segment layout and near-identical segment sizes:

| Segment | Purpose | Size (approx) |
|---------|---------|--------------|
| SOI | Start of Image marker | 2 bytes |
| APP1 | EXIF / metadata | ~370 bytes |
| DQT × 2 | Quantization tables (luma + chroma) | 69 bytes each |
| SOF0 | Frame header — dimensions, colour components | 19 bytes |
| DHT × 4 | Huffman coding tables | ~33–183 bytes each |
| SOS | Start of Scan header | 14 bytes |
| **ECD** | **Entropy-coded image data (the actual pixels)** | **see below** |
| EOI | End of Image marker | 2 bytes |
| Trailing | Camera appended bytes (non-image) | 33–34 bytes |

The metadata, quantization tables, Huffman tables, and frame header are
**identical across all four files**. The only meaningful difference is the
entropy-coded data (ECD) block.

---

## Why the Black Images Are Smaller

The size difference is entirely due to the ECD block, which holds the
JPEG-compressed pixel data:

| File | ECD size | ECD entropy | FF00 stuffing |
|------|----------|-------------|---------------|
| 59200840.JPG | 11,510 bytes | 7.54 | 21 |
| 59200860.JPG | 11,485 bytes | 7.55 | 20 |
| 59200CD1.JPG | 5,254 bytes | 2.87 | 0 |
| 59200CD2.JPG | 5,254 bytes | 2.87 | 0 |

Key observations:

- **ECD entropy of 7.5** for visible images is characteristic of well-compressed
  natural photographs (near the theoretical maximum of 8.0).
- **ECD entropy of 2.87** for the black images indicates a highly repetitive,
  near-uniform bitstream.
- **Zero FF00 byte-stuffing** in the black images means the compressed data
  contains no `0xFF` bytes at all — strongly characteristic of near-constant
  pixel values.

The black images are not truncated or corrupt. The image data is present and
valid JPEG; it is small because a near-uniform dark scene compresses
extremely efficiently.

---

## The Two Black Images Are Identical

`59200CD1.JPG` and `59200CD2.JPG` are byte-for-byte duplicates — identical
MD5 hash, zero difference. They appear to be the same capture written twice
or duplicated by the camera firmware.

---

## Decoded Pixel Values

Full JPEG decoding (Huffman decode → dequantize → IDCT → level-unshift) was
performed in pure Python to obtain actual pixel values.

| Component | Min | Max | Mean | Interpretation |
|-----------|-----|-----|------|----------------|
| Y (luma) | 0 | 210 | 25.9 | Very dark |
| Cb (blue-diff) | 128 | 128 | 128.0 | Perfectly neutral |
| Cr (red-diff) | 128 | 128 | 128.0 | Perfectly neutral |

Converting from YCbCr to RGB gives an approximate mean of **RGB(26, 26, 26)**,
which is roughly **10% brightness** — a uniform very dark grey with no colour
at all.

The Y brightness histogram confirms this:

- **99.6%** of all 307,200 pixels fall in the Y = 21–30 range.
- The 50th, 95th, and 99th percentiles of Y are all 26.
- The image appears black to the human eye; it is technically a very dark grey.

The small number of pixels outside this range (max Y = 210) are JPEG
quantisation ringing artefacts, not separate visible content — with one
notable exception (see below).

---

## Bottom-Row Anomaly

A row-by-row analysis found that the last two pixel rows (478 and 479) are
significantly different from the rest:

| Row | Y min | Y max | Y mean | Notes |
|-----|-------|-------|--------|-------|
| 472–477 | 0–50 | 12–50 | ~26 | Mild ringing from below |
| **478** | 5 | 85 | 20.3 | Distinct bright cluster |
| **479** | 0 | **210** | 4.6 | Pronounced bright pixels |

The bright pixels in row 479 form two small spatial clusters in the
**bottom-left corner** of the frame:

- Columns 10–13: peak Y = 191–215
- Columns 20–22: peak Y = 210–255

This pattern is consistent with the **tail of a camera OSD timestamp or
status overlay** — one or two characters burned into the bottom-left corner
before JPEG encoding. The scene was so underexposed that only the bottom
pixels of the characters survived at detectable brightness. The ringing visible
in rows 472–477 is a JPEG blocking artefact caused by the DC component of
those 16 × 16 MCU blocks being slightly lifted by the bright bottom pixels.

---

## Python Tools Produced

Three scripts were written during this investigation. All accept files or folders
and provide `--help`.

### `jpeg_segments.py`

Parses the JPEG segment structure of one or more files and prints a table of
every block (type, byte offset, header size, data size, percentage of file).
Includes a cross-file comparison table showing ECD size, entropy, and
byte-stuffing count.

**Best for:** quickly understanding why files differ in size; identifying
missing or malformed segments; spotting structural anomalies.

```
python3 jpeg_segments.py img/
python3 jpeg_segments.py a.jpg b.jpg
```

### `jpeg_decode_analyse.py`

Fully decodes one or more baseline-DCT JPEG files in pure Python (no external
libraries). Reports:

- Per-component pixel statistics (min / max / mean for Y, Cb, Cr)
- Approximate mean RGB and perceived brightness percentage
- Y-channel brightness histogram with percentiles
- Row anomaly scan — flags rows whose mean Y is a statistical outlier
  (using interquartile range fencing), then zooms into the anomalous region
  with an ASCII pixel map and exact Y values

**Best for:** confirming whether an image is truly black / uniform; detecting
OSD overlays, timestamp burns, or damaged rows; comparing pixel distributions
across a batch of images.

```
python3 jpeg_decode_analyse.py img/
python3 jpeg_decode_analyse.py dark.jpg reference.jpg
```

> Note: the full IDCT is computed in pure Python and is slow for large images.
> A 640 × 480 image takes a few seconds per file.

### `jpegdump.py` (pre-existing, Didier Stevens)

A general-purpose JPEG segment dumper with many options (entropy, hex dumps,
segment extraction, SOI carving). Used here to get an initial overview of all
four files. See the script's `--man` output for full documentation.

---

## Summary

The black images are smaller because the camera captured a near-uniform
dark scene: the JPEG encoder faithfully encoded ~26/255 luma pixels, which
compress to roughly half the size of a real photograph. The image data is
complete and structurally valid. A faint OSD artefact visible as a handful
of bright pixels in the bottom-left corner suggests the camera was applying
a timestamp overlay even when the scene itself was entirely dark.
