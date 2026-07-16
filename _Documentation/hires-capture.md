# High-resolution capture — one 1216x960 JPEG (op32)

#### Claude - 11 July 2026 (branch `feat/hires-capture`)

> **RETEST (16 Jul 2026): all fixes confirmed on hardware.** Three-frame
> burst on the shipped build: artifact repairs verified (no PDAF dashes,
> no right-edge strip), the MD-overlay/hi-res exclusivity fix verified
> (frame 2 - previously always corrupted - pristine), and the VGA
> preview heatmap confirmed working alongside it. One new open item from
> the burst (frame 3, load-dependent line delivery) is recorded under
> Known limitations below.

> **STATUS (12 Jul 2026): WORKING - first hi-res frames captured on the
> bench** (`Hi-res: 1280x928 JPEG 301871 bytes in 1641ms`, auto-WB
> applied; the 928 was a stale-object build artifact - the shipped
> config produces the full 1280x960). Root causes found and fixed, in
> the order they were unmasked, each hiding the next:
>
> 1. **RUN/CONTINUE clobbered the RAW path.** Every capture start
>    re-runs cisdp_dp_init() with the VGA HW5x5+JPEG arguments; the
>    hi-res override only redirected one case. Fix: dp_type is pinned
>    to SENSORDPLIB_PATH_INP_WDMA2 while hi-res is staged.
> 2. **CIS I2C slave-ID poisoning (fleet-relevant).** hm0360_md.c's
>    saveMainCameraConfig()/restore pair is not nesting-safe: an inner
>    save captured the HM0360's own address, after which every restore
>    pinned the bus to the HM0360 and the IMX708 silently missed its
>    stream-on (sensor in standby, MIPI totally silent). Likely the
>    cause of the long-standing "first capture after boot times out"
>    lottery. Fix: depth counter in hm0360_md.c + the IMX708 driver
>    pins its slave ID in stream_on/off/start/stop + the AE loop
>    selects the main camera before register writes.
> 3. **The INP cannot crop wider than 640 (DS 5.6.3 confirmed).** With
>    the bus fixed, INP-crop mode delivered frames but every WDMA2
>    ended in ERR_FE_COUNT_NOT_REACH. The sensor must do the windowing:
>    IMX708 digital-crop stage (standard Sony pipeline; accepted and
>    streaming at 32fps, verified via in-firmware register readback +
>    frame counter).
> 4. **The capture gate opens ~18.4 lines after frame start** (constant
>    to 32 bytes across boots - measured with a sentinel-fill high-water
>    diagnostic). A 960-line sensor window therefore delivered only
>    ~941.6 lines and the WDMA2 byte count never completed. Fix: the
>    sensor window is 992 lines tall (digital crop @512,152, output
>    1280x992); the DMA counts exactly 1280x960 bytes and completes
>    mid-frame with 960 contiguous rows. Bonus finding: the lib's RAW
>    WDMA2 config is cyclic (targetloopCnt=10), so count-completion
>    inside the first frame is the only reliable finish.
>
> Diagnostics added along the way (kept): cisdp_dump_diag() (sensor
> regs + CSI-RX IRQ/lane state + INP state on first capture retry),
> hires_dump_hwm_and_refill() (per-arm DMA delivery measurement), the
> 'rawdump' CLI command (base64 raw-buffer dump for PC analysis), and
> WDMA2-abnormal events now feed the capture retry path instead of
> wedging 'Capturing' forever. Known cosmetic leftover: a retry restart
> immediately after a WDMA2 abnormal can hit I2C -60 errors (suspected
> sensorctrl auto-I2C interaction); irrelevant in normal operation now
> that the first frame completes, tracked for cleanup.
>
> **Final wrinkle (12 Jul 2026 evening): delivered-line geometry.** In
> this RAW pass-through mode the INP swallows a fixed 32 wire-bytes of
> every line: a 1280-px RAW10 line (1600 B on the wire) lands in memory
> as 1254/1255 bytes (fractional in pixels -> +/-1 jitter), and the DMA
> byte stream has no line alignment. Root-caused entirely OFFLINE
> against a full raw-buffer dump ('rawdump' -> full_raw.bin, PC
> iteration - no reflash loops). The fix is demosaic_track_lines()
> (demosaic.c): per-line start offsets are chained over the two legal
> strides with a sparse SAD against the same-parity line two above plus
> an even/odd-column parity penalty; the tracker runs under all four
> per-row-parity sign hypotheses and the winner is selected by
> green-diagonal equality (the two G planes of the Bayer quad must
> match; in a wrong phase they are really R and B). Verified to
> reproduce the reference solution 960/960 lines on the golden dump,
> and clean images on-device. Encoded output is **1216x960**: valid line
> content ends at column ~1225 (each line's final DMA burst carries
> corrupted alternating bytes), so the width is the largest 16-multiple
> below that. Two sensor artifacts are also repaired in the pipeline:
> fixed-position zero runs (masked-PDAF positions, rendered as coloured
> dashes) are replaced from the same-CFA row two above
> (demosaic_repair_zeros() - raw zero never occurs naturally, the black
> level is ~16). AE and WB operate on the tracked rows, so
> exposure/colour are unaffected.

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
sensor digital-crop window 1280x992 --INP pass-through--> RAW Bayer
     -> WDMA2 counts exactly 1280x960 bytes (1.22 MB)
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
setop 32 1        # hi-res single-JPEG mode, output 1216x960  (0 = normal 640x480)
reset             # datapath mode is chosen at sensor init, per wake
```

Everything else behaves identically: auto-exposure (measured from the raw
green channel), auto white balance (measured from the raw Bayer means),
EXIF (correct 1216x960 dimensions), SD save, and the live preview stream
(~5 s/frame at this size; the MD overlay is disabled during hi-res - see
live_preview.md). Field of view: **near-identical to the 640x480
pipeline** - VGA takes a 1280x960 INP centre crop of the sensor's
2304x1296 stream and subsamples 2:1; hi-res has the sensor itself stream
a centred 1280x992 digital-crop window of the same binned readout (the
INP cannot crop wider than 640, DS 5.6.3) and keeps every pixel. The
usable encode is 1216x960 (see STATUS: delivered-line geometry), so
hi-res is a ~3.8x detail upgrade with no meaningful framing change.

Capture cost: ~2 s per frame (CPU line-tracking + demosaic + colour +
JPEG on the CM55M). Use generous capture intervals: 12 s minimum, and
**>= 20 s for guaranteed-clean back-to-back frames** - see Known
limitations.

## Known limitations / open items

- **Burst contention (open, filed on PR #144):** when a frame's MIPI
  delivery overlaps the previous frame's CPU-heavy encode/preview-send,
  the per-line delivery loss grows beyond the fixed 32 wire-bytes and
  the line tracker's {1254, 1255} stride assumption misaligns row tails
  (ghosted band at the right edge). Single-shot field captures
  (wake -> one capture -> sleep) never overlap and are unaffected.
  Mitigation: >= 20 s spacing for bursts. Fix candidate: the tracker's
  green-split score flags bad solves - gate on it and re-capture once.
- **Retry-restart I2C -60 (cosmetic):** a capture-retry restart
  immediately after a WDMA2 abnormal can fail stream-on with I2C bus
  errors (suspected sensorctrl auto-I2C interaction). Irrelevant in
  normal operation now that first frames complete.
- **Preview stream splice sensitivity:** a hi-res INVOKE line is
  ~250-450 KB of base64 (several seconds at 921600 baud); a console
  print from another task inside that window corrupts that frame only.
  Viewers drop bad frames; bench scripts use wide capture spacing.

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

## Bench tooling (`_Tools/`)

All of these tolerate the bench USB re-enumerating whenever the device
resets or power-cycles (reopen-per-loss), and re-assert the keep-awake
AFTER boot completes (an op8 sent at CLI-up is silently overwritten by
the config load).

- `flash_reopen.py --image <img>`: catch-anywhere flasher. Hammers the
  bootloader's 30 ms key window from the first banner byte (and blindly
  right after a port re-enumeration, when a boot is by definition in
  progress), then drives the X-Modem burn with erase-tolerant timeouts.
- `hires_check.py --port COM13`: end-to-end hi-res validation - arms
  op32, walks the operator through the cold boot, captures a burst over
  the preview stream, decodes and saves frames, restores field config
  on pass.
- `vga_grab.py`: one normal-mode frame + reports the preview MD-overlay
  fields (`md_blocks`/grid state) - the quick heatmap sanity check.
- `full_rawdump.py`: captures one hi-res frame and pulls the ENTIRE raw
  Bayer buffer to the PC via the `rawdump` CLI (10 chunks, ~90 s) for
  offline analysis - this is the tool that let the line-delivery
  geometry be solved without reflash loops.
