# Step 8 proposal: HM0360 image task for ww500_minimal

#### File: CLAUDE_Step8_camera_proposal.md
#### Author: Claude (for Charles Palmer)
#### Date: 21 September 2026
#### Status: DONE 22 September 2026: built and working on the bench. Answers in section 7. What was done and what is outstanding: README.md, "Step 8".

## 1. Request

Add the minimum camera functionality to `ww500_minimal`:

1. Initialise the HM0360.
2. Put it in mode 2 (`MODE_SW_NFRAMES_SLEEP`) - believed to be the lowest-power state, to be checked.
3. Take one picture and save it as a JPEG, **only when a CLI command asks for it**.

Purpose: confirm the 10 uA DPD current survives adding the camera and JPEG-to-SD.

## 2. Why a proposal rather than code

The camera stack in `ww500_md` is not one file. The pieces the minimal app needs:

| Piece | Where (ww500_md) | Size / notes |
|---|---|---|
| Sensor + datapath init, start/stop, JPEG info | `cis_sensor/cis_hm0360/cisdp_sensor.c/.h`, `cisdp_cfg.h` | Reusable as is. Needs `CIS_SUPPORT_INAPP = cis_sensor` and `-DUSE_HM0360` in the .mk |
| HM0360 register table | `cis_sensor/cis_hm0360/*.i` | Long table, written on cold boot only |
| Mode control (`hm0360_md_setMode`) | `hm0360_md.c/.h` | Small. Mode 2 with N frames |
| Datapath library | `sensordp` in `LIB_SEL` (prebuilt lib) | Adds to the .mk; adds `-D` and include paths |
| Capture state machine, frame-ready callback | `image_task.c` | Multi-thousand lines (EXIF, NN, AE, timers, flash LED). **Do not copy**; write a small task that uses only the calls below |
| I2C master | `hx_drv_i2cm_init(USE_DW_IIC_1, HX_I2C_HOST_MST_1_BASE, DW_IIC_SPEED_STANDARD)` | One call |
| JPEG/raw buffers | `jpegbuf`, `demosbuf` in `.bss.NoInit` (system SRAM) | `JPEG_BUFSIZE` / `RAW_BUFSIZE` from `cisdp_cfg.h`; check the minimal `.ld` has room |

The calls that `image_task.c` actually makes for one frame, which the new task will reproduce:

```
init:     hx_drv_i2cm_init(...)                       // once
          cisdp_sensor_init(coldBoot)                 // slave ID, power-up delay, MODE_SLEEP, register table
          cisdp_dp_init(true, SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG,
                        callback, jpgRatio, APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X)
capture:  hm0360_md_setMode(CONTEXT_A, MODE_SW_NFRAMES_SLEEP, 1, 0)
          sensordplib_retrigger_capture()
          ... callback(SENSORDPLIB_STATUS_XDMA_FRAME_READY) -> message to task
          SCB_InvalidateDCache_by_Addr(jpegAddr, jpegLen)   // after cisdp_get_jpginfo(&len,&addr)
stop:     cisdp_sensor_stop()                         // capture stop, swreset, MODE_SLEEP
```

## 3. Proposed design

**Files (all new, following `c_file_format.md`):** `image_task.c/.h` in `ww500_minimal/`. The `cis_sensor/cis_hm0360` sources are compiled from `ww500_md` via the .mk if the build system allows a cross-folder path, otherwise copied unchanged into `ww500_minimal/cis_sensor/` (the `ww500_md` copy already follows the "copy, don't edit" pattern used for `FreeRTOS_CLI.c`).

**Task:** "Image" (priority 3, next to FatFS). State: UNINIT / IDLE / CAPTURING / BUSY_WRITING / NO_CAMERA. Public API in the same style as `fatfs_task`: `image_task_createTask/getState/getStateString/requestCapture/notifyInactivity/printStatus`.

**Init (at boot, before `barrier_ready(&startupBarrier)`):**
1. I2C master init, then `hm0360_md_isSensorPresent()`. If absent -> state NO_CAMERA, everything else keeps working (mirrors the no-SD behaviour).
2. Sensor init (cold-boot register table). Since the minimal app always cold-inits the sensor after DPD (the camera supply is presumably lost - see question 2), there is no warm-init path.
3. Datapath init, then `hm0360_md_setMode(MODE_SLEEP)` so the sensor idles at its minimum until asked.

**Mode 2 and "lowest power".** In `hm0360_md.c` the comment gives about 700 uA for MODE_SLEEP, 520 uA for HW_TRIGGER and 270 uA for SW_NFRAMES_SLEEP. Two caveats to check on the bench, not assume:
* Mode 2 with N frames returns to sleep *after* the frames, so it is a "capture then sleep" mode, not a resting state. The resting current between pictures is whatever mode it falls back to. I propose `capture` = mode 2 with 1 frame, and a CLI `cam <mode>` command that can set any mode (0..4, 7) so the resting current of each can be measured with your meter - which is the "we will have to check".
* Sleep current is measured on the camera's own supply, so it only shows up in the board total if the HM0360 is on the rail you measure.

**Capture command (CLI):** `capture` (and `camera` status, `cam <mode>` for the experiment above).
1. Refuse if: no camera; no card mounted; `fatfs_task_bootCountValid()` false (per your Q3 answer: skip the JPEG if the boot count increment failed).
2. Set mode 2 / 1 frame, retrigger, wait (timeout ~2 s, longer for auto-exposure convergence - see question 4) for the frame-ready callback.
3. Get JPEG address/length, invalidate D-cache, send a `fileOperation_t` to the FatFS task (`fatfs_task_sendFileOp`) - the image task never touches the card.
4. On the FatFS completion message, print length, time taken, and result, free the buffer for the next capture.

**File name (8.3):** `Bnnnnnnn.JPG`? 8.3 allows 8 chars, so boot count (up to 6 digits) plus a 2-digit sequence within the boot: `BBBBBBSS.JPG` (e.g. `00001203.JPG` = boot 1203, picture 3). Wraps at 100 pictures per boot (then refuse).

**Inactivity / DPD:** the image task joins the shutdown barrier: on INACTIVITY it aborts any capture, calls `cisdp_sensor_stop()` (MODE_SLEEP) and does `barrier_ready(&shutdownBarrier)`. `barrier_init(&shutdownBarrier, 3, blinky_task_sleepNow)` and `WW500_MINIMAL_NUMBER_OF_TASKS 4`. A capture in progress or a pending write defers the inactivity, as the FatFS write already does.

**Build changes (`ww500_minimal.mk`):** `LIB_SEL += sensordp`; `CIS_SUPPORT_INAPP = cis_sensor`; `CIS_SUPPORT_INAPP_MODEL = cis_hm0360`; `-DUSE_HM0360`. As in `ww500_md.mk`. Only the HM0360 variant is built (no RP3/IMX708 matrix for this app).

**power_diag caveat:** `clkoff image` and `clkoff hsc` switch off the camera datapath, JPEG and xDMA clocks. `capture` after those would hang. I propose the `capture` command refuses if those groups are gated (or `clkon` restores them first) and the docs say so.

## 4. Risks

* **Memory:** JPEG buffer + raw/demosaic buffer (`RAW_BUFSIZE` = 640x480x1.5 = 460 KB) in system SRAM. Should fit as in `ww500_md` (which also holds NN buffers), but I will check the map file after your first build.
* **DPD current:** the camera control pins and the I2C pads may leak if left driven into an unpowered sensor. That is exactly what this step is to measure; the fix (drive pins low/hi-Z before DPD) would come after the measurement, not before.
* **Cold-init time:** the register table write is slow (tens of ms of I2C at 100 kHz); it happens every boot since DPD is a cold boot. Consider lazy init on first `capture` (question 3).
* No EXIF: the JPEG is written as the encoder produces it, so a `.JPG` opens in viewers but has no timestamp metadata (question 5).

## 5. Questions for Charles

1. **Wiring:** which HX6538 pins connect to the HM0360 on your board - I2C (`SCL/SDA` on which pads, address 0x24 as in `cisdp_cfg.h`?), the camera data/clock lines (the dedicated sensor interface, presumably as in ww500_md), XSLEEP, INT (motion) and STROBE? Anything additional to hand-wire beyond what `initPins()` sets now?
2. **Camera supply:** is the HM0360 supply switched by PA1/VMUTE with the SD card (3V3_WE), or is there a separate enable? Is there a rail on which I can expect to measure the camera's mode currents?
3. **Init timing:** initialise at every boot (camera present check visible in the banner, costs boot time and current while running) or lazily on the first `capture`? I recommend at boot for the first version, since it simplifies state and the run time is only 3 s cold / 10 s warm.
4. **Image quality and exposure:** 640x480, `jpg_ratio` as ww500_md (which value?), and how many frames should mode 2 run? One frame of a cold sensor may be dark; ww500_md discards/throws away frames for AE convergence in some paths. Do you want a `capture [frames]` parameter, default 1?
5. **EXIF:** none for now (recommended), or the minimal `exif_builder` header?
6. **File name:** OK with `BBBBBBSS.JPG` (boot count + per-boot sequence)?
7. **Source sharing:** may `ww500_minimal.mk` compile the `cis_hm0360` sources directly from `ww500_md/cis_sensor/` (no duplication, but a dependency between apps) or should I copy them into `ww500_minimal/cis_sensor/` (self-contained, duplicated)? I recommend copying, for consistency with what step 1 did.
8. **`cam <mode>` experiment command:** yes/no? It is the only way to find out the resting current of each mode.

## 6. Plan after your answers

1. Update memory; write code (image task, CLI commands, .mk).
2. You build (I will not run `make`).
3. Bench: mode currents via `cam <mode>`, `capture` run, DPD current with camera and card fitted.
4. Document results in the thread README and `ww500_minimal/doc/`.

## 7. Answers (Charles Palmer, 22 September 2026) and what they change

| # | Answer | Effect on the design |
|---|---|---|
| 1 | HM0360 wiring is identical to ww500_md. | No new pin set-up in `initPins()`. I2C master is `USE_DW_IIC_1` as in md. |
| 2 | The camera supply is always on. 0R links can be lifted to insert a meter. | The sensor is never power-cycled, not even by DPD. Its register state (including MODE_SELECT) should survive DPD. |
| 3 | Initialise the HM0360 only at cold boot, as ww500_md does. This also confirms the sensor retains its state over DPD. | Cold boot: `cisdp_sensor_init(true)` (long register table). Warm boot: `cisdp_sensor_init(false)`, so no register table. On a warm boot the MODE_SELECT value found *before* the init is printed, as evidence of retention. The data path (`cisdp_dp_init`) is initialised at every boot because it lives in the HX6538, which loses it in DPD. |
| 4 | Capture 1 frame for now. | `capture` = mode 2, 1 frame. Adjust later. |
| 5 | No EXIF. | The encoder's JPEG is written as it is. |
| 6 | File name `Bnnnnnnn.JPG`. | 8.3 name: `B` + 7 digits. **Interpretation:** the boot count in the first 5 digits and a 2-digit picture number in the last two, e.g. boot 1203, picture 3 = `B0120303.JPG`, so more than one picture per boot does not overwrite. Boot counts above 99999 wrap. Tell me if you meant something else. |
| 7 | Copy the `cis_hm0360` folder (the pattern all apps use). | Done: `ww500_minimal/cis_sensor/cis_hm0360/` (unchanged). Also copied from ww500_md: `hm0360_md.c/.h` and `hm0360_regs.h` (the mode control and register names, which `cisdp_sensor.c` calls). Only two edits, both in `hm0360_md.c`: the unused `fatfs_task.h` include removed and the register-table include path corrected. |
| 8 | Add CLI commands. | `capture`, and `cam [mode]` (shows the sensor mode, or sets mode 0-4 or 7). |

Decisions I made without asking (say if you disagree):

* The four tasks have priorities 4 (CLI), 3 (FatFS), 2 (Image) and 1 (Blinky), allocated in that order by `app_main()`.
* JPEG quality: `jpg_ratio` 10 (the `JPEG_ENC_QTABLE_10X` table, which md uses by default and which the buffer in `cisdp_cfg.h` is sized for).
* Each capture repeats what md does in `CAMERA_CONFIG_RUN`: `cisdp_dp_init`, `hm0360_md_setMode(CONTEXT_A, MODE_SW_NFRAMES_SLEEP, 1, 0)`, `cisdp_sensor_start`. Sleep time 0 means md's "inhibit MD": the longest sleep interval (about 2 s) and the motion-detect interrupt disabled. The frame may arrive after up to that interval, so the wait timeout is 5 s and the time to the frame is printed.
* After the frame arrives the JPEG address and size are read, the data path is stopped (`cisdp_sensor_stop`, which puts the sensor in MODE_SLEEP) and the sensor is then put back in the resting mode (default mode 2, 1 frame, longest sleep). Note the sensor in mode 2 keeps producing a frame about every 2 s with nobody listening; that is what md does in DPD and is part of what the meter will show.
* The camera is initialised in its own task (`image_task.c`), which joins the startup and shutdown barriers.
* A capture is refused when there is no camera, no card or no valid boot count. If `clkoff image` / `clkoff hsc` have gated the data path clocks the capture times out after 5 s (`clkon` restores them); I did not add a check to `power_diag`, as the power investigation is closed to code changes.
