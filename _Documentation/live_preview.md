# Live Preview - streaming WW500 images to a PC for camera tuning
#### Claude - 10 July 2026 (branch `feat/uart-live-preview`)

Answers the problem statement in `Sensecraft.md`: a live stream of video from
the WW500 onto a laptop so image quality (RP3 white balance, auto-exposure,
focus, LED flash timing) can be adjusted dynamically and the effect seen
immediately.

The stream shows the **same JPEG the firmware would save to SD** - i.e. after
the software white-balance correction (`img_correct.c`, op27/op28) and
`sw_jpeg` re-encode - so what you tune is what the camera traps will record.
This is the key difference from flashing the Himax `tflm_fd_fm` demo, which
streams Himax's own sensor init instead.

## How it works

* New CLI command **`preview <mode>`** (`preview.c` in `ww500_md`):
  * `0` - off (normal operation)
  * `1` - stream every captured frame over the console UART, **skip SD saves**
  * `2` - stream **and** save to SD as normal
* Frames are only produced by captures, so streaming is driven with the
  existing `capture <n> <interval_ms>` command, e.g. `capture 1000 0`.
* Each frame goes out as one JSON line, the same "INVOKE" framing the
  Himax/SSCMA scenario apps use (`send_result.cpp`), so the
  [Himax AI web toolkit](https://github.com/HimaxWiseEyePlus/Seeed_Grove_Vision_AI_Module_V2/releases/download/v1.1/Himax_AI_web_toolkit.zip)
  can also display it:

```
{"type": 1, "name": "INVOKE", "code": 0, "data": {"count": 12,
 "resolution": [640, 480], "boxes": [], "image": "<base64 JPEG>"}}
```

* AE-light-check wakes are not streamed.
* Throughput at 921600 baud (~92 KB/s): a VGA JPEG of 25-50 KB costs
  ~0.4-0.7 s plus capture/NN time, so expect **~1.5-2.5 fps**. Setting the
  FTDI latency timer to 1 ms helps a little (see `FTDI_Cable_Speed.md`).

## The viewer: `_Tools/live_view.py`

```
py _Tools\live_view.py            # defaults COM13 (Himax), 921600
```

> **Port:** the preview stream is emitted by the **Himax HX6538** console, which
> is **COM13 @ 921600**. COM14 @ 115200 is the separate nRF52 BLE/LoRaWAN
> co-processor (its console shows `Secure Bootloader`, `Initialising LoRaWAN`,
> `AI processor sends stats: ...`) and carries no preview - don't point the
> viewer there.

Shows the live image plus a console log pane, a command box and quick-command
buttons, so `setop 27 ...`, `setop 28 ...`, `vcm ...`, `camreg ...` can be
sent **while watching**. "Start stream" sends `preview 1` + `capture 1000 0`
and automatically re-arms the capture burst when it runs out; "Stop stream"
sends `preview 0`. "Save frame" writes the last JPEG to disk for A/B records.

Close TeraTerm first - Windows COM ports are exclusive.

## Motion-detection overlay (MD tuning)

When the firmware is built with the MD-in-preview change (`preview.c` emits
`"md"`/`"md_blocks"` in each frame), the viewer draws the HM0360 **16x16 motion
grid** as a translucent red heatmap over the live image, and shows the
moving-block count in the status panel. Toggle it with the **"MD motion
overlay"** checkbox.

This turns preview into a live MD-tuning bench: the sidebar has
`camreg 35a9 ...` (MD_TH_STR_H_B, sensitivity) and `camreg 35a6 ...`
(MD_BLOCK_NUM_TH_B, trigger block count) buttons - press one and watch which
grid cells light up as you move a target, then read the effect straight off the
image. This works because preview arms the HM0360 MD engine to run *concurrently*
with the main-camera stream (`hm0360_md_prepare(true, interval)`); normally MD
only runs on the sleep path, so the grid reads empty during an ordinary awake
capture. See `_Tools/md_sweep.py` for the headless (no-preview) register sweep,
and `doc/Motion_detection_presets.md` for the parameter map.

Caveat: the grid comes from the HM0360 (its own FOV); on colour builds the
preview image is the RP3/IMX708 (a different FOV), so the overlay is a spatial
guide, not a pixel-aligned mask. On HM0360 (night) builds the two align.

**Hi-res exclusion:** the overlay is disabled while hi-res capture (op32) is
active. Running the HM0360's MD frames concurrently with an IMX708 RAW capture
corrupts the image through the shared camera fabric (regional colour casts +
partial-width banding from frame 2 of a burst - frame 1 escapes because the MD
arming is lazy; bench 16 Jul 2026). VGA preview ran the overlay clean on the
bench and keeps it. The two uses don't overlap in practice: the heatmap serves
MD tuning (VGA), hi-res preview serves image-quality checks.

## Bench session recipe

```
setop 8 600000        # 10 min before DPD, so the device stays awake between bursts
preview 1             # stream, don't wear out the SD card
capture 1000 0        # frames start flowing
setop 27 286          # ...adjust WB red gain while watching...
setop 28 326          # ...adjust WB blue gain...
vcm 512               # ...focus (RP3)...
camreg w 0x0202 0x03e8   # ...exposure etc. (staged writes - see camreg help)
preview 0             # stop streaming
```

Notes:

* The viewer is self-arming: on connect it probes with `ver`; if the device is
  asleep, power-cycle it and the viewer catches the boot, types the keep-awake
  immediately (the device enters DPD ~4 s after an idle cold boot), waits for
  "Image sensor and data path initialised" plus a settle delay, then arms
  `preview 1` and the capture burst - every step echo-verified with retries.
* **Do not use a 0 ms capture interval with the RP3** - `capture N 0` wedges
  the IMX708 datapath (endless frame timeouts / sensor restarts). 500 ms is
  reliable; the viewer uses `capture 1000 500` (~1 fps net).
* **The first capture sequence after a cold boot often frame-times-out**
  (`>>>> Frame timed out - restarting sensor`) even when started well after
  camera init. Later sequences work; the viewer just re-arms every 8 s until
  frames flow, which typically takes 30-90 s after a cold boot. Root cause in
  the RP3 cold-boot sensor bring-up - worth a firmware look someday.
* The console UART receive path has a 1-character buffer, so the viewer sends
  commands one character at a time in the quiet window after each frame. If a
  command echo looks garbled in the log pane, send it again.
* An occasional frame is corrupted when another firmware print (e.g. the
  fatfs task) lands inside the frame line. The viewer counts it as "dropped"
  and skips it. This is accepted for a bench tool.
* If frames stop because the capture burst (max 1000) finished, the viewer
  re-arms automatically; from a bare terminal just repeat `capture 1000 0`.
* `preview 1` skips SD saves only while preview is on. If you stop preview
  mid-burst and want no files written, also set `setop 18 8`
  (TEST_BIT_SKIP_FILE_CREATION) for the session, and `setop 18 0` after.

## Files changed (this branch)

* `EPII_CM55M_APP_S/app/ww_projects/ww500_md/preview.c` / `.h` - new: frame
  emission (chunked base64, no frame-sized buffer) and mode state. The MD
  overlay adds: arm the HM0360 MD engine on the first previewed frame
  (`hm0360_md_prepare(true, OP_PARAMETER_MD_INTERVAL)`) and append
  `"md_blocks"` + the 32-byte `"md"` grid to each frame line.
* `EPII_CM55M_APP_S/app/ww_projects/ww500_md/image_task.c` - hook in
  `APP_MSG_IMAGETASK_FRAME_READY`: runs the WB correction in the skip-save
  path when previewing, then `preview_sendFrame()`; `preview 1` joins the
  skip-file-save condition.
* `EPII_CM55M_APP_S/app/ww_projects/ww500_md/CLI-commands.c` - the `preview`
  command.
* `_Tools/live_view.py` - PC viewer + tuning console; MD motion-grid overlay +
  MD-tuning quick commands; default port COM13.

Build as usual (`_Tools/build_ww500.sh`, or CI). Works for both camera
variants; on HM0360 builds the WB correction is simply not compiled in.
