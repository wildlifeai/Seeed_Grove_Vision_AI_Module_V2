# RP3 (IMX708) White Balance — software correction + hardware JPEG re-encode hang

**Status: RESOLVED via a software JPEG encoder.** The hardware JPEG re-encode
could not be made to work (§5), so the corrected YUV is now encoded to JPEG on
the CPU (`sw_jpeg.c`). This is verified end-to-end on hardware: a strong green
cast (raw G/R ≈ 1.59) corrects to neutral (G/R ≈ 1.0) on a properly-exposed
scene, the device saves valid JPEGs and cycles with no hang, and the gains are
app-tunable Operational Parameters (op27 red, op28 blue) loadable from
CONFIG.TXT. The hardware-re-encode investigation below is kept for the record
(and in case someone with a datapath debugger wants the faster hardware path).

The remaining sections document *why* the hardware re-encode failed.

Target: WW500_C02, Himax HX6538 (WE2), RP v3 camera = Sony **IMX708**
(Raspberry Pi Camera Module 3), captured at 640×480 YUV420.
Firmware branch: `feat/camera-features-combined`. Module: `ww500_md`.

---

## 1. The problem and its root cause (settled)

RP3/IMX708 JPEGs have a fixed green cast: measured **G/R ≈ 1.12, G/B ≈ 1.27**
on neutral grey (vs a phone's 0.90 / 1.14). Cause, confirmed by testing:

- **IMX708 is a raw Bayer sensor** — it applies no white balance. Its V4L2
  "colour balance" regs (0x0B90/0x0B92) only *notify* the sensor of gains an
  external ISP applies; bench-writing them changed the channel balance by 0%.
  (Earlier attempt with 0x0210/0x0212 also: 0% change.)
- **The WE2 HW5x5 block does demosaic + FIR + crop only** — `HW5x5_CFG_T` has
  no WB/CCM/gain fields. There is no hardware WB anywhere in the pipeline.
- A neutral-grey subject shows the cast, which by definition means *unequal
  per-channel gain* (a colour-space/matrix/subsampling bug would leave grey
  neutral). Chroma-plane analysis (Cb=122.7, Cr=125.7, both < 128) confirms a
  genuine WB deficit, not a Cb/Cr swap (swapping made it worse) or a matrix
  error.

**Conclusion:** WB must be applied downstream in firmware. Correction is a fixed
per-channel gain: **R ×1.12, B ×1.27** neutralises grey. Chosen to be
app-tunable via Operational Parameters (op27 = red gain, op28 = blue gain,
Q8.8, 256 = 1.0×).

## 2. Chosen approach

After each RP3 capture, in software:
1. Take the demosaiced **YUV420 frame** the HW5x5 already wrote to memory
   (`demosbuf`, a.k.a. WDMA3).
2. Apply the per-channel WB gains on the CPU (YUV→RGB→gain→YUV, fixed point).
3. **Re-encode** the corrected YUV to JPEG with the hardware JPEG encoder via
   the TPG datapath (`RDMA → TPG → JPEG Enc → WDMA2`), then save that JPEG.

Code: `EPII_CM55M_APP_S/app/ww_projects/ww500_md/img_correct.c` (+ `.h`), called
from `image_task.c` after `APP_MSG_IMAGETASK_FRAME_READY`, before
`prepareJpegFile()`.

## 3. What works (proven on hardware via trace prints)

```
WB: enter (R=286 B=326)                       <- correction called, gains correct
WB: yuv=0x340363c0 jpg=0x340a6bc0 640x480      <- demosbuf/jpegbuf addrs, dims OK
WB: applying gains
WB: re-encoding                                <- YUV pixel maths COMPLETED, no crash
WB: stopping sensor + datapath
WB: tpg setup w=640 h=480 wdma2max=115856
WB: tpg setup returned, waiting                <- HANGS HERE, forever
```

- The CPU YUV correction loop runs to completion (no fault, correct dims).
- `setup_tpgdp_jpegenc_wdma2()` is called and **returns**.
- Then the wait for the encode-complete callback never ends — **and even a
  bounded `vTaskDelay(1ms)` loop (300 iterations) never advances**. That means
  the CPU/bus is stalled, i.e. a **bus lock**, not merely a missing callback
  (a missing callback would let the 300 ms timeout expire and print
  `WB: re-encode returned 0`; it never prints).

## 4. The datapath context

Main capture path (RP3), set in `cisdp_dp_init()`:
`SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG` → implemented by
`sensordplib_set_int_hw5x5_jpeg_wdma23(...)`. The **`wdma23`** suffix means the
integrated path writes **both** WDMA2 (hardware JPEG) **and** WDMA3 (YUV420) in
parallel every capture. So after a capture, memory holds both a JPEG and the
YUV that produced it. That YUV is what we correct.

DMA buffer map (`cis_imx708/cisdp_sensor.c`):
| DMA   | var / addr             | contents                    |
|-------|------------------------|-----------------------------|
| WDMA1 | `jpegbuf`  (0x340a6bc0) | unused ("no use")           |
| WDMA2 | `jpegbuf`  (0x340a6bc0) | hardware JPEG output        |
| WDMA3 | `demosbuf` (0x340363c0) | YUV420 640×480 (460800 B)   |
| autofill | `jpegfilesizebuf`   | JPEG size fill              |

Note WDMA1 and WDMA2 share `jpegbuf`.

The TPG re-encode we set up:
`RDMA (ch1/2/3 = Y/U/V from demosbuf) → TPG → JPEG Enc → WDMA2 (jpegbuf)`,
via `setup_tpgdp_jpegenc_wdma2()` from `library/tpgdp` (`libtpgdp.a`, prebuilt,
no source). RDMA config: ch1=Y (w·h), ch2=U (w·h/4), ch3=V (w·h/4),
`finmode = XDMA_RDMA_FINSRC_THR`. WDMA2 config:
`dma_mode = SNAPSHOT`, `fin_mode = FE`, `startaddr = jpegbuf`.

## 5. Things tried, all hang identically at "tpg setup returned, waiting"

| # | Attempt | Result |
|---|---------|--------|
| A | TPG re-encode with no datapath quiesce (mid-stream) | bus lock |
| B | + `sensordplib_stop_capture` / `start_swreset` / `stop_swreset_WoSensorCtrl` before setup | bus lock (identical) |
| C | + full `cisdp_sensor_stop()` (adds stream-off **and MIPI receiver disable**) | bus lock (identical) |
| D | **RDMA source = private scratch buffer** (memcpy corrected YUV out of demosbuf first; RDMA reads scratch that WDMA3 never touched) + clean D-cache on scratch | **bus lock (identical)** — RULES OUT WDMA3/demosbuf contention and cache coherency as the cause |
| E | **Tiny 64×64 frame** through the identical path (RDMA sizes 4096/1024/1024, wdma2max 2192) | **hang (this time inside `setup_tpgdp_jpegenc_wdma2()` — it doesn't even return)** — RULES OUT frame-size / RDMA-threshold as the cause. The conflict is **structural**, independent of frame size. |

**Deductions from D + E together:** the hang is not buffer ownership, not cache,
not DMA source, not frame size / thresholds. What remains: (i) the datapath
hardware cannot be reconfigured for `RDMA→TPG→JPEG→WDMA2` after the integrated
`HW5x5→JPEG` path has owned it (the project marks the TPG path "Not supported"
in `cisdp_dp_init` — it may never have been wired for dynamic switching), and/or
(ii) a datapath IRQ storm under FreeRTOS (the only known-good user of this path,
`allon_jpeg_encode`, is a bare-metal super-loop). Both need either an SWD
debugger or a from-boot split-datapath capture design to resolve; neither is
tractable by further blind bench iteration.

**Key deduction from D:** the hang is **not** about buffer ownership, DMA
source, or cache — the RDMA reads a pristine, cache-clean, sensor-untouched
buffer and still wedges. The problem is intrinsic to configuring/running the
`RDMA→TPG→JPEG→WDMA2` datapath *after* the integrated `HW5x5→JPEG` datapath, or
to running it under FreeRTOS at all. Note the working reference
(`allon_jpeg_encode`) is a **bare-metal super-loop** (no RTOS): if the datapath
raises a repeating error IRQ, it would spin harmlessly there but **starve the
FreeRTOS scheduler here** (which is exactly what "even `vTaskDelay` never
resumes" looks like — an interrupt storm, not necessarily a bus stall).

The `img_correct` CPU maths and the `setup_tpgdp_jpegenc_wdma2()` call itself
never fault — only the subsequent DMA operation stalls the bus. Confirmed the
next capture's `CAMERA_CONFIG_RUN` (→ `cisdp_dp_init` → `set_mipi_csirx_enable`)
would re-enable MIPI, so a full stop before re-encode is safe.

## 6. The reference that DOES work — and how it differs

`app/scenario_app/allon_jpeg_encode` performs exactly this memory→JPEG encode
(`app_mem_to_jpg()` uses the same `setup_tpgdp_jpegenc_wdma2()` and callback
pattern) and it works. **Critical difference:** that app initialises its
datapath as **`SENSORDPLIB_PATH_INP_WDMA2` (RAW capture only)** and then does
RAW→YUV (`app_mem_to_hw5x5`) and YUV→JPEG (`app_mem_to_jpg`) as **discrete TPG
memory operations** in a loop. It never uses the integrated HW5x5→JPEG path.

So the TPG JPEG encoder works from a datapath configured for RAW/TPG use, but
hangs when invoked after the **integrated HW5x5→JPEG** datapath has been
configured (even after stopping/sw-resetting it). `cisdp_dp_init()` in this
project explicitly lists `SENSORDPLIB_PATH_TPG_JPEGENC` under the *"Not support
case"* default — the project has never driven the TPG path from this app.

## 7. Open questions / candidate solutions to brainstorm

1. **Residual WDMA3 arming.** The integrated path arms WDMA3 (YUV out) at the
   same `demosbuf` the re-encode's **RDMA now reads**. Does `stop_capture` +
   `swreset` fully disarm WDMA3, or is there a read/write contention on
   `demosbuf` (WDMA3 still trying to write while RDMA reads)? → Try explicitly
   disabling WDMA3, or **RDMA from a separate scratch buffer** (copy corrected
   YUV out of `demosbuf` first).
2. **JPEG encoder block state.** The encoder was configured to be fed by HW5x5
   (integrated). Does driving it from RDMA/TPG require an explicit
   `hx_drv_jpeg` re-init / reset that the RAW-path app happens to have from a
   clean init? Is there a JPEG-encoder input-source mux that's still set to the
   HW5x5 path?
3. **Datapath clock gating.** After `sensordplib_start_swreset`, are the TPG /
   JPEG / RDMA datapath clocks gated? Is a
   `sensordplib_ungated_dp_clk_bycase(SENSORDPLIB_PATH_TPG_JPEGENC)` (or the
   `dp_var_init()` the reference app calls) required before `setup_tpgdp_*`?
4. **RDMA finmode / size.** `XDMA_RDMA_FINSRC_THR` with ch sizes w·h, w·h/4,
   w·h/4 — if the threshold/finish source is wrong for this path the RDMA never
   signals completion and holds the bus. Does the reference use the same
   finmode, or `FINSRC_...` something else for YUV420?
5. **Interrupt routing.** Are the WDMA2/RDMA datapath interrupts still routed to
   the integrated-path handler (registered by `cisdp_dp_init`) rather than our
   `setup_tpgdp_*` callbacks? Could explain a missing completion — though it
   would not by itself explain a *bus stall*.
6. **Do it the reference way (bigger change).** Switch the RP3 capture to the
   split model: `SENSORDPLIB_PATH_INP_WDMA2` (RAW) → correct RAW/Bayer or
   software-demosaic → `app_mem_to_hw5x5` → `app_mem_to_jpg`. Proven to work,
   but a significant rewrite of the capture state machine and it changes MD/AE
   timing assumptions.
7. **Re-init the JPEG encoder block** before `setup_tpgdp_*`. The encoder is
   configured once at boot (`platform_driver_init.c`:
   `hx_drv_jpeg_init(HW_JPEG_BASEADDR, HW_JPEG_HSC_BASEADDR)`) and left bound to
   the HW5x5 datapath by the integrated path. Calling `hx_drv_jpeg_init(...)`
   again (or a dedicated encoder reset) before the TPG setup might clear a stale
   input-source binding. Cheap to try. NB `dp_var_init()` in the reference app
   is only software-flag resets — not the missing step.
8. **Software JPEG encoder (no datapath).** Encode corrected YUV420 → JPEG
   entirely on the CM55 (YUV420 is JPEG-native, so no colour conversion;
   standard DCT/quant/Huffman). ~400 lines, ~30–80 ms/frame. Deterministic,
   sidesteps all datapath issues. Lowest risk; loses hardware-encode speed.

**Strongly recommended for whoever picks this up:** attach an SWD/JTAG debugger
and break in during the hang. It would immediately distinguish the three
possible causes — (a) a **BusFault** looping in the fault handler (→ read
BFAR/CFSR for the faulting address), (b) an **interrupt storm** from a datapath
error IRQ starving the scheduler, or (c) a genuinely **stalled DMA** master
holding the bus — which the blind bench iteration cannot tell apart.

## 8. Key code locations

- `app/ww_projects/ww500_md/img_correct.c` / `.h` — WB maths + TPG re-encode
- `app/ww_projects/ww500_md/image_task.c` — call site (post-FRAME_READY),
  `prepareJpegFile()` (uses corrected JPEG via `img_correct_get_jpeg`)
- `app/ww_projects/ww500_md/cis_sensor/cis_imx708/cisdp_sensor.c` — datapath
  init (`cisdp_dp_init`), buffers, `cisdp_sensor_stop()`, `app_get_raw_addr`
- `app/scenario_app/allon_jpeg_encode/` — working RAW→YUV→JPEG reference
- `library/tpgdp/tpgdp_lib.h` — `setup_tpgdp_jpegenc_wdma2()` (impl prebuilt)
- `library/sensordp/inc/sensor_dp_lib.h` — datapath paths & stop/reset APIs

## 9. Bench notes

- Trace prints are compiled in `img_correct.c` (guarded by nothing — remove for
  production). Reproduce with a fast timelapse (`setop 7 8`) so saving captures
  (not AE-check wakes) run; the correction only runs on the file-save path.
- The current firmware **hangs on every saving capture**. To use the device
  normally without reflashing, set unity gains: `setop 27 256; setop 28 256`
  (the correction self-skips when both gains are unity), done on a cold boot
  before a timelapse wake.
