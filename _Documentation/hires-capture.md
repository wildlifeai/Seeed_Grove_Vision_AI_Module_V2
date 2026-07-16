# High-resolution capture — one 1280x960 JPEG (op32)

#### Claude - 11 July 2026 (branch `feat/hires-capture`)

## Why not the Himax 4-tile workaround

The WE2's hardware JPEG encoder and HW5x5 demosaic top out at 640x480, so
Himax's suggested route to higher resolution (Nov 2024 thread with Steve
Huang; bench-validated by Tobyn via `allon_jpeg_encode`) captures one
1280x960 RAW frame and splits it into **four** 640x480 JPEGs — which then
need EXIF quadrant tagging, 4-files-per-capture handling on the SD and
through upload, forced-uniform WB across tiles, and a server-side stitch
job in the website.

That workaround predates this firmware's **software JPEG encoder**
(`sw_jpeg.c`, added for the white-balance work). The software encoder has
no resolution limit, so the whole tiling problem disappears:

```
sensor 2304x1296 --INP centre-crop--> RAW Bayer 1280x960 -> WDMA2 (1.22 MB)
     then on the CPU, 16 rows at a time:
bilinear demosaic -> black level -> auto WB -> CCM -> gamma
     -> streaming JPEG encode -> ONE ~200-300 KB file, ONE EXIF block
```

No seams (one WB gain pair per frame, by construction), no stitching
anywhere, and the EXIF question from the email thread evaporates.

Answers to the open questions in that thread, from the reference code:
- `allon_jpeg_encode` takes **one** capture; the four JPEGs are quadrants
  of the same instant (no time offsets).
- Raw Bayer is 1 byte/pixel: a 1280x960 frame is 1.22 MB, not 2.4.

## Memory (all within existing regions; checked at runtime)

| Buffer | Size | Lives in |
|---|---|---|
| RAW Bayer frame | 1,228,800 B | tensor arena (512 KB, idle with NN off) + the free SRAM tail after it |
| JPEG output | ~430 KB cap | `demosbuf` (idle: the RAW path never writes WDMA3) |
| strip YUV (16 rows) | 30,720 B | `demosbuf`, after the JPEG area |

**Hi-res therefore requires the NN to be off (op14 = 0)** — the raw frame
occupies the NN arena. This matches the "trail-cam without NN" use case
the resolution question came from. If a model is loaded, capture falls
back to 640x480 with a console note.

## Usage

```
setop 14 0        # NN off (frees the arena)
setop 32 1        # 1280x960 single-JPEG mode  (0 = normal 640x480)
reset             # datapath mode is chosen at sensor init, per wake
```

Everything else behaves identically: auto-exposure (measured from the raw
green channel), auto white balance (measured from the raw Bayer means),
EXIF (correct 1280x960 dimensions), SD save, and the live preview stream
(~4 s/frame at this size). Field of view: the 1280x960 centre crop sees
**twice the width and height** of the current 640x480 pipeline (which is
also a centre crop of the same sensor stream).

Capture cost: ~2-3 s per frame (CPU demosaic + colour + JPEG on the
CM55M), so use generous capture intervals (`capture N 3000`).

## Design notes

- The RAW datapath is selected **from init, per wake** (op32 checked in
  `CAMERA_CONFIG_INIT_COLD`) — the WE2 datapath cannot be reconfigured
  mid-stream (see `RP3_white_balance_reencode_issue.md` §5).
- `SENSORDPLIB_STATUS_XDMA_FRAME_READY` fires for the RAW path exactly as
  for the integrated path (verified in `allon_jpeg_encode`), so the image
  task's event flow is unchanged.
- The streamed encoder is bit-exact with the whole-frame one (PC-verified
  at 1280x960); DC predictors carry across strips.
- The demosaic is bilinear (fused with the colour transform in one pass);
  a sharper kernel (e.g. Malvar) can be swapped in later without touching
  the API.
- Files: `hires.c/h` (orchestration + buffer overlay), `demosaic.c/h`,
  `sw_jpeg.c/h` (streaming API), `cisdp_sensor.c` (imx708: hires override),
  `image_task.c` (mode selection + frame branch).
