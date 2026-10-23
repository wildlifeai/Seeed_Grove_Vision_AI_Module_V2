# Live Preview - streaming WW500 images to a PC for camera tuning
#### Claude - 10 July 2026 (branch `feat/uart-live-preview`), updated 23 October 2026 (HM0360 use, viewer modes)

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
  existing `capture <n> <interval_ms>` command, e.g. `capture 10 500`.
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
py _Tools\live_view.py            # defaults COM14, 921600
```

Shows the live image plus a console log pane, a command box and quick-command
buttons, so `setop 27 ...`, `setop 28 ...`, `vcm ...`, `camreg ...` can be
sent **while watching**.

* **Free run** sends `preview 1` + `capture 10 500` and re-arms a new burst each
  time one ends (`Current captures completed` or `Capture failed`).
* **Stop** stops re-arming. The firmware cannot cancel a burst, so up to 10 more
  frames arrive. Preview stays on, so they are not saved to SD.
* **One image** stops free run and takes a single frame (after the current
  burst, if one is running).
* **Save frame** writes the last JPEG to `_Tools/live_frames/` (git-ignored; change
  with `--save-dir`) for A/B records. The log shows the full path.
* Closing the window sends `preview 0`.

The status panel shows the mode: free run, stopped, busy, one image or cold boot.

Close TeraTerm first - Windows COM ports are exclusive.

## Bench session recipe

```
setop 8 600000        # 10 min before DPD, so the device stays awake between bursts
preview 1             # stream, don't wear out the SD card
capture 10 500        # frames start flowing (repeat when the burst ends)
setop 27 286          # ...adjust WB red gain while watching...
setop 28 326          # ...adjust WB blue gain...
vcm 512               # ...focus (RP3)...
camreg w 0x0202 0x03e8   # ...exposure etc. (staged writes - see camreg help)
preview 0             # stop streaming
```

Notes:

* The viewer is self-arming: on connect it probes with `ver` and arms. If the
  device is asleep it waits for a boot banner:
  * `### Warm Boot ###` (a wake from DPD): it types the keep-awake and `preview 1`
    at once. Preview is a RAM flag that every DPD clears, and a motion or
    timelapse wake starts its own op5-image burst straight away, which would
    otherwise be saved to SD. It then waits for "Image sensor and data path
    initialised" plus a settle delay (or for the wake's own burst to end) and
    starts capturing, every step echo-verified with retries.
  * `### Cold Boot ###` (power-up, `reset`, slot switch, firmware update): since
    13 Oct 2026 the firmware refuses captures until it has slept once
    (`Error: no captures until after the first sleep.`, `captureRefused()` in
    `CLI-commands.c`). The viewer sends no keep-awake and no capture, and lets it
    sleep. Something must then wake it: motion (op11 > 0), the app (e.g.
    `AI ver`), or start the viewer with `--wake 10`. That types `setop 7 10`
    (timelapse wake 10 s after DPD) at the cold boot, then `dpd` once camera init
    is done, and `setop 7 0` on close.
  * **Any character typed on the console keeps the device awake for 60 s**
    (`INACTIVITYTIMEOUTCLI`, regardless of op8). So whenever the viewer has typed
    at a cold-booted device (the `--wake` setup, or a refused capture after
    connecting), it finishes with `dpd`, otherwise the device sits awake for a
    minute before it can sleep and wake.
  * **A power cycle is a cold boot**, so it no longer starts the stream by itself.
* **Do not use a 0 ms capture interval with the RP3** - `capture N 0` wedges
  the IMX708 datapath (endless frame timeouts / sensor restarts). 500 ms is
  reliable; the viewer uses `capture 1000 500` (~1 fps net).
* Captures after a cold boot used to frame-time-out often. That is why the
  firmware now refuses them until the first sleep (see above).
* The console UART receive path has a 1-character buffer, so the viewer sends
  commands one character at a time in the quiet window after each frame. If a
  command echo looks garbled in the log pane, send it again.
* A print from another firmware task inside a frame line corrupts that frame;
  the viewer counts it as "dropped" and the tail shows in the log as a bare
  base64 line. On the HM0360 this hit **every** frame (23 Oct 2026): the IF task
  logs the I2C send of the motion/AE message to the BLE processor, which
  completes mid-frame. Since then `consoleQuiet()` in `if_task.c` also holds the
  IF task's prints while `preview_isSending()`. Other tasks are not held, so an
  occasional dropped frame is still possible.
* From a bare terminal, repeat `capture 10 500` when a burst ends.
* `preview 1` skips SD saves only while preview is on. If you stop preview
  mid-burst and want no files written, also set `setop 18 8`
  (TEST_BIT_SKIP_FILE_CREATION) for the session, and `setop 18 0` after.

## Files

* `EPII_CM55M_APP_S/app/ww_projects/ww500_md/preview.c` / `.h` - new: frame
  emission (chunked base64, no frame-sized buffer) and mode state.
* `EPII_CM55M_APP_S/app/ww_projects/ww500_md/image_task.c` - hook in
  `APP_MSG_IMAGETASK_FRAME_READY`: runs the WB correction in the skip-save
  path when previewing, then `preview_sendFrame()`; `preview 1` joins the
  skip-file-save condition.
* `EPII_CM55M_APP_S/app/ww_projects/ww500_md/CLI-commands.c` - the `preview`
  command.
* `EPII_CM55M_APP_S/app/ww_projects/ww500_md/if_task.c` - `consoleQuiet()` holds
  the IF task's console prints while `preview_isSending()` (23 Oct 2026).
* `_Tools/live_view.py` - PC viewer + tuning console. Frames saved with **Save
  frame** go to `_Tools/live_frames/`, which is git-ignored.

Build as usual (`_Tools/build_ww500.sh`, or CI). Built for both camera
variants; on HM0360 builds the WB correction is simply not compiled in. The
July 2026 bench work was on the RP3; HM0360 use started 23 Oct 2026, see
`development reports/2026-10-23_mdEnhancements/`.
