# Live Preview: live video from the WW500 on a PC

#### File: `live_preview.md`
#### Author: Claude, first written 10 July 2026; quick start added 23 October 2026
#### Date: 25 October 2026

`_Tools/live_view.py` is a Python script that shows live video from the WW500 camera on your
PC, over the console serial cable. It also gives you a command box, so you can change camera
settings and see the effect on the next frame. It works with both camera builds (RP3 and
HM0360).

## Quick start

Close TeraTerm or any other terminal first: only one program can use the COM port.

The first time only, install the two Python packages it needs:

```
py -m pip install pyserial pillow
```

Then, in a Windows command prompt (`py -m serial.tools.list_ports` lists the COM ports if you
don't know yours):

```
cd D:\Development\wildlife.ai\Seeed_Grove_Vision_AI_Module_V2\_Tools
py live_view.py --port COM4 --wake 10
```

Then reset or power-cycle the WW500. After about 20 seconds frames start to appear and keep
coming. The default port is COM14 if you leave out `--port`.

If no frames appear after a cold boot, send `AI ver` from the app over BLE. That wakes the
AI processor into a warm boot, and the viewer then starts streaming.

| Button | What it does |
|---|---|
| Free run | Continuous frames, in bursts of 30 with a short gap (about 0.5 s) between bursts |
| Stop | Stops after the current burst (up to 30 more frames); the mode line counts them down |
| One image | Takes a single frame |
| Save frame | Saves the last frame to `_Tools/live_frames/` (without the grid) |
| Frame interval (ms) | Time between frames. Takes effect from the next frame: during a burst the viewer sends `interval N` |
| Help | Shows where this guide is, and the `--help` text |
| Motion detection | One switch for all the MD tools: the grid over the image, the **WAKE** lamp, the MD status lines, Sensitivity, Wake threshold and grid rows. Off by default: plain live video at the camera's fastest default interval (HM0360 20 ms). On, the HM0360 frame interval follows op 11. See [Motion grid overlay](#motion-grid-overlay) |

Type any console command (`setop`, `getop`, `camreg`, `vcm` ...) in the Command box (right-hand
column). **Keep awake** (on by default) sends a bare carriage return when the link has been
quiet for 20 s, which restarts the device's 60 s console inactivity period, so the device
stays awake while the viewer is open and the close can always run. It is never sent during
the cold-boot sleep or after a `dpd`. Untick it to let the device sleep, e.g. to test a wake.
**Log to file** (on by default) writes everything in the log pane, with timestamps, to
`_Tools/live_frames/live_log_<date>_<time>.txt`; the path is shown under the box.

**When you finish, close the window with its close button.** That leaves the device in a quiet
lab state: no timelapse and short inactivity (`setop 7 0`, `setop 8 1000`). These are saved
to the SD card, so set them again before a deployment. The MD settings (ops 11, 17 and 33)
are left as they are: the MD controls save them on purpose.

**Why `--wake 10`?** After a reset the firmware takes no pictures until it has slept once.
`--wake 10` makes it sleep at once and wake itself 10 s later. Without it, wake the device
yourself: from the app, or with motion if motion detection is on.

| If you see | Do this |
|---|---|
| `Cannot open COM4` | Close TeraTerm, or check the port number |
| "waiting for frames..." for more than a minute | Reset the device, or use `--wake 10` |
| `dropped` count climbing | Old firmware: needs the 23 Oct 2026 `if_task.c` fix |

The rest of this document is the detail.

## Background

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
 "resolution": [640, 480], "boxes": [], "md": "<64 hex>", "image": "<base64 JPEG>"}}
```

* `md` (added 23 Oct 2026) is the HM0360's 32 ROI_OUT motion bytes read for this capture
  (`preview_setMdBits()`). It is missing when there is no HM0360.

* AE-light-check wakes are not streamed.
* Throughput at 921600 baud is about 92 KB/s. Measured 23 Oct 2026, one free-run
  frame takes about 1.0 s on the RP3 (32 ms capture, 270 ms colour correction and
  re-encode, 0.13 s on the UART, 500 ms interval) and about 1.8 s on the HM0360
  (about 1 s for the sensor to wake for each frame). Details in
  `development reports/2026-10-23_mdEnhancements/`. Setting the FTDI latency timer
  to 1 ms helps a little (see `FTDI_Cable_Speed.md`).

## The viewer: `_Tools/live_view.py`

```
py _Tools\live_view.py --port COM4 --wake 10
```

Options: `--port` (default COM14), `--baud` (default 921600), `--wake SECONDS` (timelapse
wake after a cold boot), `--save-dir DIR` (where Save frame writes), `--interval MS`
(free-run frame interval, 20 ms minimum). The default is 20 ms on the HM0360 and 100 ms on
the RP3. With **Motion detection** on, the HM0360 uses op 11, the MD frame interval (read
with `getop 11` at connect), or 1000 ms if op 11 is 0.
The **Frame interval (ms)** box in the window changes it from the next frame (`interval N`
during a burst, then the next `capture`).

Shows the live image plus a console log pane, a command box and quick-command
buttons, so `setop 27 ...`, `setop 28 ...`, `vcm ...`, `camreg ...` can be
sent **while watching**.

* **Free run** sends `preview 1` + `capture 30 <interval>` and re-arms a new burst the
  moment one ends (`Current captures completed` or `Capture failed`). Until 23 Oct 2026 the
  re-arm waited 3 s, so there was a pause of several seconds after every 10 frames (not an
  error: the viewer was waiting before typing the next `capture`).
* **Stop** stops re-arming. The firmware cannot cancel a burst, so up to 30 more
  frames arrive. Preview stays on, so they are not saved to SD.
* **One image** stops free run and takes a single frame (after the current
  burst, if one is running).
* **Save frame** writes the last JPEG to `_Tools/live_frames/` (git-ignored; change
  with `--save-dir`) for A/B records. The log shows the full path.
* **Closing the window leaves the device quiet**, each step confirmed: `preview 0`,
  `setop 7 0` (no timelapse), `setop 8 1000` (short inactivity). The firmware saves op
  params to CONFIG.TXT, so this replaces whatever the card had. The MD op params (11, 17
  and 33) are not touched (24 Oct 2026; earlier versions set op 11 to 0, then put ops 11
  and 17 back).
  Closing the console instead (or Ctrl+C) skips it, and if the device is asleep the
  viewer prints which commands were not confirmed.

The status panel shows the mode: free run, stopped, busy, one image or cold boot.

Close TeraTerm first - Windows COM ports are exclusive.

## Motion grid overlay

Added 23 Oct 2026. Orientation and row count checked on the bench 24 Oct 2026.

* The HM0360 reports motion as one bit per cell, in 32 `ROI_OUT` registers (0x20A1 to
  0x20C0). The viewer draws **16 columns x 16 rows** (cells of 40 x 30 pixels): on the bench
  motion shows in all 16 rows, although the datasheet gives 16 x 15 for VGA and `ROI_V =
  0xe0` selects rows 0 to 14. **15 rows** is kept to compare.
* Bit `b` is byte `b / 8`, bit `b % 8` (LSB first), row `b / 16`, column `b % 16`, the
  order of the console grid (`hm0360_md_printGrid()`). The sensor's own order is reversed
  (MD application note, figure 4.1), so the viewer rotates it 180 degrees. Read the console
  printout upside down.
* The sensor smooths motion over several frames (`MD_IIR_PARAMETER` 0x209A), so hold a hand
  still for three or four frames before judging where the cells are.
* **On an RP3 build** the bits come from the HM0360, which has a different lens and
  position, so the grid is only a rough guide on the RP3 image.

### Would this frame wake the device?

Added 24 Oct 2026. The firmware sends each frame's `"mdstat"` (`INT_INDIC` 0x2064, `MD_CTRL`
0x2080, `MD_CTRL1` 0x209E, and `MD_BLOCK_NUM_TH` for context A, 0x354C, and B, 0x35A6).

The HM0360 raises its motion flag when the number of cells with motion is **greater than**
`MD_BLOCK_NUM_TH` (MD application note 3.1). With `MD_CTRL1` bit 0 = 0, as the firmware sets
it, that flag drives the MD interrupt that wakes the device. On the bench (24 Oct 2026) the
interrupt came on at exactly the first frame where cells exceeded the threshold, and never
with the lens taped. So the viewer predicts a wake per frame from the cell count:

| Status line | Meaning |
|---|---|
| `motion` | Cells with motion in this frame |
| `threshold` | `MD_BLOCK_NUM_TH`, context A (preview frames), B in brackets (sleep) |
| `wake` | `YES` when cells > threshold; the **WAKE** lamp goes red |
| `wakes` | Frames that would have woken the device, out of all frames, since **Free run** |

The log gets an `[md]` line whenever the prediction changes, with the raw registers for
reference.

* `INT_INDIC` itself is not shown: during a burst the firmware's per-frame clear does not
  take effect, so the bit stays set after the first motion (bench, 24 Oct 2026). The field
  wake is not affected.
* If `MD_CTRL1` bit 0 is ever set (interrupt follows the latency logic), the status shows a
  `NOTE`: the prediction does not model latency.
* Preview frames are context A (VGA). The wake uses context B (QVGA, one frame per op 11
  interval), so a sleep test still decides what wakes the device.

### MD controls

The controls are the op params the device uses for its motion wake, so what you set here is
what the device does in the field. The viewer reads them (`getop`) at connect and after each
wake, writes them with `setop` (saved to CONFIG.TXT at once), and reads each back, so a value
the firmware rejects does not stay on screen.

| Control | Op | Range | Notes |
|---|---|---|---|
| Sensitivity Low / Med / High | 17 | 1 to 3 | Per-cell threshold and light coefficient (the Himax low/medium/high tables). Applied to the HM0360 at once |
| Wake threshold (cells) | 33 | 1 to 255, default 4 | `MD_BLOCK_NUM_TH`: wake when **more than** this many cells show motion. Written to the main bank and contexts A and B, at once |
| MD interval (ms, 0 = off) | 11 | 0 to 1997 | Time between MD frames while asleep. 0 is the only way to switch motion detection off. With Motion detection on, the frame interval follows it |

Op 10 (camera enabled) is read too: with 0, the status shows that there will be no motion
wake. See `config_file.md` for the op params.

### Why the HM0360 interval defaults to op 11

With `USE_HM0360_CAPTURE_TIMER` (defined in the HM0360 build) the HM0360 takes the burst's
frames by itself: one frame, then a sleep of the capture interval, then the next. MD runs on
**every** sensor frame and compares it with the previous one. The AI processor needs about
200 ms to stream a frame, then takes whichever sensor frame comes next. So:

* **At a 20 ms interval** the sensor makes a frame about every 55 ms, but only
  about one in four is shown. The `ROI_OUT` bits of the others are overwritten unseen.
* **Slow movement is missed for a second reason:** frames 55 ms apart differ too little
  for a slowly moving object to pass the per-cell threshold. In the field the frames are an
  op 11 interval apart, so the same movement makes a bigger difference between frames.
* **So the viewer uses op 11 as the interval** (24 Oct 2026). If op 11 is over about
  250 ms, every sensor frame is shown and MD compares frames as far apart as in the field.
  If op 11 is 0 (MD off), it uses 1000 ms.
* **The HM0360 sleep register stops at about 1997 ms** (`calculateSleepTime()`: interval x
  0x8030 / 1000, capped at 0xFFFF). A longer op 11 or interval runs at that, so the viewer
  caps the HM0360 interval there and says so in the log.

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
    is done. Closing the viewer sets op7 back to 0.
  * **Any character typed on the console keeps the device awake for 60 s**
    (`INACTIVITYTIMEOUTCLI`, regardless of op8). So whenever the viewer has typed
    at a cold-booted device (the `--wake` setup, or a refused capture after
    connecting), it finishes with `dpd`, otherwise the device sits awake for a
    minute before it can sleep and wake.
  * **A power cycle is a cold boot**, so it no longer starts the stream by itself.
* **Never use a 0 ms capture interval** (`capture N 0` with N > 1), on either camera. The
  firmware passes it to `xTimerChangePeriod()`, and FreeRTOS asserts on a zero period
  (`timers.c`), which now resets the AI processor and used to hang it. This is the likely
  cause of the July note that it "wedges the IMX708". The viewer never sends less than 1 ms.
  Found 23 Oct 2026; the firmware should reject 0 (`MIN_IMAGE_INTERVAL` is 0).
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
