# Proposal: live view working, MD grid overlay, live MD tuning

#### File: `CLAUDE_mdEnhancements_proposal.md`
#### Author: Claude (Opus 5.5), for review by Charles Palmer
#### Date: 23 October 2026

Answers the task in [README.md](README.md). Six steps, each small enough to do, test and
review on its own. Step 2 is done and bench-checked on both cameras (23 Oct); see
**Progress, end of 23 October 2026** at the end. Steps 3 to 6 are not started.

## What exists today

| Piece | Where | State |
|---|---|---|
| `preview <0/1/2>` command | `ww500_md/preview.c`, `CLI-commands.c` | Merged with PR #140. Streams each captured JPEG as one base64 JSON line on the console UART |
| Hook in the image task | `image_task.c` ~line 1119 | Sends the frame after the JPEG is ready, before the SD write |
| PC viewer | `_Tools/live_view.py` | Tk window: image, log pane, command box. Auto-arms `preview 1` + `capture 1000 500` |
| Doc | `_Documentation/live_preview.md` | Written July 2026, benched **on the RP3 only**. Says HM0360 "works", never tested |
| MD grid on the console | `image_task.c` ~line 1055 | Every capture prints `HM0360 motion in N blocks:`, the 32 ROI_OUT bytes in hex, and a 16x16 `#`/`.` grid. Also sent to the app over BLE |
| MD grid viewer | `_Tools/md_display.py` | Older (April 2025) Tk + matplotlib grid. Expects lines starting `>`, which the firmware no longer prints |
| MD settings | `hm0360_md.c`, op17 | op17 picks one of three presets (low/medium/high) written to the threshold, light-coefficient and IIR registers |

MD registers that op17 does **not** touch, set once by the init table: `MD_BLOCK_NUM_TH`
(0x209B/0x354C/0x35A6, currently 4 blocks, the number of changed cells needed to raise the
interrupt), ROI window (0x354D/E, 0x35A7/8), `MD_LATENCY`/`MD_LATENCY_TH` (0x209C/D),
`MD_TH_MIN` (0x2083). `MD_BLOCK_NUM_TH` is the "threshold to issue an MD interrupt" the
README asks about.

## Step 1. Find out why the image area stays black (bench, no code)

The viewer's log pane already tells us which failure it is. Run `live_view.py` for a minute
and look for:

| What the log shows | Meaning |
|---|---|
| `[frame] #1 ... bytes` lines, image area black **with no text** | Frames arrive and decode. The JPEG itself is black (see below) |
| Grey area still saying "waiting for frames...", long base64 lines in the log | Frame lines arrive but are not recognised as frames |
| `dropped corrupt frame line` climbing | Another task's print lands inside every frame line |
| `undecodable JPEG` | Line parsed, JPEG bytes bad |
| Only `>` commands, no device output | Port, baud or the device is asleep |

**Found 23 Oct (Charles): the viewer collides with a recent firmware change.** The log shows
`Error: no captures until after the first sleep.` Since 13 Oct (camera-errors step 9,
`captureRefused()` in `CLI-commands.c`) `capture` is refused between a cold boot and the
first DPD. The viewer was written before that. On a cold boot it sends the keep-awake
(`setop 8 60000`) and then `capture`, resending it up to 8 times before giving up. The
keep-awake stops the device sleeping, so it never reaches the state where captures are
allowed. Result: the image area never leaves "waiting for frames...". This is fixed in
step 2.

**Found 23 Oct, second log (HM0360 build): every frame is corrupted, which is the real
cause of the black screen.** Frames were flowing (`Image capture 108/1000`), but the viewer
reported `dropped corrupt frame line (4295 chars, total dropped 20)`, so all 20 frames had
been dropped. The sequence:

1. Before each frame, the image task sends `HM0360 AE regs` and `HM0360 motion in N blocks`
   to the BLE processor.
2. That I2C send completes while the frame is going out on the UART.
3. The IF task prints `IF Task received event 'I2C Tx'`, `I2C transmission complete.` and
   its state change inside the frame line.
4. The tail of the frame then shows up in the log as a bare base64 line.

(Corrected later on 23 Oct: the RP3 build sends the same HM0360 AE and motion messages, so the
claim first written here, that the RP3 avoids this, was wrong. Why July saw it only
"occasionally" is unknown.) Fix: step 2. The same log answers part of step 4: the bottom grid row
and the last two ROI_OUT bytes are always empty, matching the ROI window ending at row 14
(`0x354D = 0xe0`).

The earlier suspicion of a genuinely dark HM0360 JPEG is ruled out: once frames displayed
(23 Oct), they looked normal. The current makefile builds the HM0360 variant, live
view was only ever proven on the RP3, and an HM0360 frame indoors without IR illumination
can be genuinely near-black. The `Black_image_findings.md` doc records HM0360 JPEGs that
decode but display black. Quick check: press **Save frame** and open the file, and compare
with a JPEG the same build saves to SD.

Please record `ver` and `slots` with the result.

## Step 2. Fix whatever step 1 finds, then document live view

**Status 23 Oct: both changes below coded, built and benched on the HM0360 and RP3 builds;
they work.** The cold-boot handling ended up different from the plan below (it sends `dpd`);
see the Progress section at the end. The viewer also gained
free run in 10-frame bursts (so Stop takes effect within a burst), a **One image** button,
and `--wake N` (timelapse wake after a cold boot). `live_preview.md` is updated to match.
The viewer's state machine was checked off-device against a scripted console transcript.

**Firmware change for the corrupted frames** (small, `preview.c` and `if_task.c`):

- `preview_sendFrame()` sets a "sending" flag for the length of the frame.
- `consoleQuiet()` in `if_task.c` also returns true while that flag is set, so the IF task's
  per-message prints, already suppressed during BLE downloads, stay quiet during a frame.
- Check the log afterwards for any other print that still lands in a frame, e.g. from the
  fatfs task. Stopping those would need a console lock, which is a bigger change.

**Viewer change for the cold-boot rule** (Python only, no firmware change):

- Tell the two boots apart by their banners, `### Cold Boot ###` and `### Warm Boot ###`
  (`ww500_md.c`).
- On a cold boot, send nothing: no keep-awake and no capture. Let the device go to sleep,
  and show "cold boot: waiting for the device to sleep and wake" in the window.
- On a warm boot, send the keep-awake first and then arm `preview 1` and `capture`, as now.
- If `Error: no captures until after the first sleep.` arrives, stop resending and say why
  in the log, rather than burning the 8 retries.
- Connecting to a device that is already awake: `ver` doesn't say whether it cold booted,
  so try one capture. If it's refused, fall back to the cold-boot behaviour.

**Open question:** what wakes the device from DPD at the bench? A power cycle is another
cold boot. That leaves motion (waving at the lens), a timelapse interval, or the BLE
processor. The doc must give one dependable recipe. A short timelapse, set from the viewer
before the device sleeps, is probably the easiest. *(Answered: `live_view.py --wake N`, which
also sends `dpd`; motion or the app's `AI ver` work too.)*

Other fixes depend on what step 1 finds, for example an HM0360 exposure or flash setting
during preview. Then rewrite `_Documentation/live_preview.md` as the user
doc: both variants, setup, a troubleshooting table (the one above), and the known limits.
Also retire or update `md_display.md` / `md_display.py` once step 4 supersedes them.

## Step 3. Speed and robustness: what transfers from the BLE download work

Lessons from `2026-10-06_speedImageTx`:

1. **Measure before changing.** Add a one-line timing print per preview frame (capture,
   JPEG ready, UART send), as `printDownloadTiming()` did for downloads.
2. **Quiet console during the transfer.** `consoleQuiet()` in `if_task.c` suppressed
   other prints during BLE downloads. Bench evidence showed this is needed for preview
   correctness, not just speed, so it moved to step 2.
3. **Find the real bottleneck.** For BLE it was the link (one packet per connection
   interval), not the AI processor. Here the UART is the ceiling: 921600 baud is about
   92 KB/s, so a 40 KB JPEG as base64 takes about 0.6 s. Options, in order of cost:
   - a lower JPEG quality or a QVGA option for preview,
   - raw binary instead of base64 (25% less, but loses Himax web toolkit compatibility),
   - a higher baud rate (needs a check that the HX6538 UART and the FTDI cable support it).

Expect at most 2 to 3 fps from these. That is fine for tuning.

## Step 4. MD grid overlay on the live image

- **Firmware:** add the 32 ROI_OUT bytes to the frame's JSON line (e.g. `"md": "dd06f4..."`).
  The grid then belongs to that exact frame, and the viewer doesn't have to match up
  separate console lines.
- **Viewer:** draw a 16x16 grid over the image, shading the cells that changed, with a
  checkbox to hide it. Bit-to-cell order comes from `md_display.py` (column-major, flipped),
  but **must be verified** with a hand waved in one corner.

**Open question to settle first:** captures run the HM0360 in context A (VGA, streaming),
but field MD runs in context B (QVGA, sleep mode, strobed). We need to confirm ROI_OUT
updates meaningfully during a capture burst. If it doesn't, the overlay shows the wrong
thing. Then we'd need an "MD stream" mode: the sensor runs its own MD cycle, and the AI
processor polls ROI_OUT and sends grid lines, with an occasional frame. The console print
in `image_task.c` already lets us check this before writing any code.

## Step 5. Live MD controls in the viewer

Sliders or spin boxes in the viewer for:

| Control | Registers |
|---|---|
| Threshold strength | `MD_TH_STR_H/L` (context A and B) |
| Light coefficient | `MD_LIGHT_COEF` |
| IIR filter | `MD_IIR_PARAM` |
| Blocks needed for interrupt | `MD_BLOCK_NUM_TH` |
| ROI window | `ROI_START_END_V/H` |
| Latency | `MD_LATENCY`, `MD_LATENCY_TH` |

How the viewer writes them:

- `camreg` exists but is the wrong tool. It **saves every write to SD and re-applies it at
  every init**, and on an RP3 build it addresses the IMX708, not the HM0360.
- Proposed: a new `mdreg <addr> [<val>]` console command that reads/writes the HM0360 only
  (via the existing save/restore slave-ID wrappers in `hm0360_md.c`) and does not persist.
  The viewer reads back current values on connect.

The useful range and exact meaning of each register need checking before the controls are
laid out. The Himax documents are in the ww-hardware repo under `Himax_Data/HM0360/`: the
preliminary datasheet, `HM0360_Motion_Detection_Setting_20250306.pdf` and its context B
companion (20250408).

## Step 6. Promote what works to operational parameters

Once bench tuning shows which settings matter, add op parameters for them (or extend op17
from three presets to a value). Op parameter indices are a cross-repo contract with the app,
the BLE processor and the website, so this step starts as a GitHub issue agreed with Victor
and the app team. Not a unilateral change.

## Questions for you

1. Which camera variant and build were you running when the image stayed black?
2. Did the base64 appear in the viewer's log pane, or in another terminal?
3. Is the target the HM0360 as the main camera, or the HM0360 as MD sensor on an RP3
   build, or both? Step 5's `mdreg` matters most for the second case.
4. May I read the Himax HM0360 documents in ww-hardware for step 5? I'd treat them as
   confidential, citing section numbers only and not copying their tables into this repo.

## Progress, end of 23 October 2026

**Step 2 done; checked on the bench by Charles (HM0360 build).** Free run, Stop, One image and
Save frame all produce visible frames. Changes beyond the plan above:

- **`dpd`, not `setop 8 1000`.** Every character typed on the console sets the inactivity
  period to 60 s (`INACTIVITYTIMEOUTCLI`), whatever op8 says. So after a cold boot the viewer
  ends with `dpd`: straight after a refused capture, or after camera init when `--wake` is set.
- **`preview 1` is sent as soon as a wake is detected.** Preview is a RAM flag that every DPD
  clears, and a motion wake starts its own op5 burst straight away, which was being saved to SD.
- **A burst the device starts itself** (`Images to capture: N`) counts as busy and ends the
  cold-boot state.
- **Save frame** writes to `_Tools/live_frames/` (git-ignored), with `--save-dir` to change it.
- The two traps (the 60 s console rule, prints splitting frames) were added to
  `.agents/skills/references/hardware-traps.md`.

The RP3 build was also checked with these changes on 23 Oct and works. On-screen scaling:
frames are drawn at 640 x 480 image pixels, but Windows display scaling above 100% enlarges
the window. A DPI-aware or zoom option was offered and declined.

**Next:** step 4's open question. Does ROI_OUT mean anything during context A captures? The
first HM0360 log showed 122 of 256 cells changed, a later one showed 1. We need to know whether
the scene was moving.

### Frame timing per camera (bench logs, 23 Oct, input to step 3)

Both cameras use the same software `captureTimer` for the interval (`USE_HM0360_CAPTURE_TIMER`
is not defined). It starts only after the previous frame is fully handled, so the interval adds
to the cycle. What happens when it fires differs:

| Part | RP3 | HM0360 |
|---|---|---|
| Timer to frame ready (`Image capture took`) | 32 ms, the sensor is already streaming (`sensordplib_retrigger_capture()`) | 1013 ms, a sleep/wake for every frame (`hm0360_md_setMode(CONTEXT_A, MODE_SW_NFRAMES_SLEEP, 1, 0)`) |
| Colour correction + `sw_jpeg` | 90 + 180 ms | none (hardware JPEG) |
| UART stream | 8.6 KB, ~0.13 s | 17 KB, ~0.25 s |
| Interval | 500 ms | 500 ms |
| Cycle | ~1.0 s | ~1.8 s |

Levers:
- **RP3:** the interval is half the cycle. Try 100 to 200 ms (0 wedges the IMX708), then
  the colour pipeline (#173).
- **HM0360:** keep the sensor streaming while previewing, instead of a sleep/wake for every frame.

The viewer would need a per-camera interval, e.g. taken from the
`Image sensor and data path initialised for camera ...` line.

**For step 4:** on an RP3 build the motion grid comes from the HM0360, which has a different lens
and position from the RP3, so an overlay won't line up cell for cell. Each HM0360 capture also
prints `Inhibiting MD`, so MD interrupts are off during captures.


**Later on 23 Oct, close behaviour.** The bench showed op7 = 10 and op8 = 60000 left on the
card after viewer sessions (setop values are saved to CONFIG.TXT straight away). Agreed with
Charles: the viewer is a lab tool, so on close it sets lab-safe values rather than restoring
earlier ones. These are `preview 0`, `setop 7 0`, `setop 11 0` and `setop 8 1000`. Each is
confirmed; if the device is asleep, the viewer lists what wasn't confirmed.

## Step 4 coded (23 October 2026, not yet built or benched)

- **Firmware:** the image task passes the 32 ROI_OUT bytes it already reads each capture to
  `preview_setMdBits()`, but only when an HM0360 is present and not on AE-check frames.
  `preview_sendFrame()` adds them to that frame's line as `"md"`.
- **Viewer:** draws the grid over the frame, with blue cell lines and a red outline on cells
  with motion. Controls: Show, 15 or 16 rows, Mirror, Flip. The status panel counts cells
  with motion.
- **Assumption, per Charles ("make an assumption and we can test"):** 16 columns x 15 rows,
  from `ROI_H = 0xf0` and `ROI_V = 0xe0` in both contexts, and the 16th row of bits always
  empty in logs so far. Bit order is the console grid's (`hm0360_md_printGrid()`), checked
  against both bench logs (row 10 column 2, and row 13 column 1). Orientation against the
  image is not verified, hence Mirror and Flip.
- Still open: whether the bits mean anything during captures (`Inhibiting MD`), and the
  different field of view on RP3 builds.

**Step 4, first bench result (23 Oct, HM0360 build).** Charles saw the overlay working, but
the red cells were diagonally opposite the motion. So the console-grid bit order is rotated
180 degrees relative to the JPEG; `IMG_ORIENTATION` (0x0101) is not written by the init
table. Charles wanted the JPEG left alone, so the fix is in `live_view.py` only: `md_cells()`
rotates the grid, and Mirror and Flip are now corrections on top of that. Placement was "not
perfect" even when rotated. Still to compare: 15 rows (32 px) against 16 rows (30 px), and
lag from the sensor's IIR smoothing.

**Frame rate and the pause every 10 frames (23 Oct, Python only, not yet benched).**
- **Pause** (not an error; the stream resumed every time): at the end of each burst, the viewer waited for 3 s of quiet before typing the
  next `capture`; that rule exists so it never types during a frame line. A burst end is a
  quiet point, so the viewer now types the re-arm straight away.
- **Interval:** the viewer asks `camera` and defaults to 20 ms on the HM0360 and 100 ms on
  the RP3, instead of 500. These are new and untested values. `--interval MS` and an Interval
  box in the window override them.
- **Found while doing this:** `capture N 0` with N > 1 can never work. FreeRTOS asserts on a
  zero timer period (`timers.c`), so the AI processor resets (it hung before 13 Oct). That
  is probably the July "wedges the IMX708" note. The firmware should reject 0; that needs an
  issue, and a C change for a later round.
- Expected effect: the RP3 at about 0.6 s per frame (was about 1.0 s). On the HM0360, about
  1.3 s, because its 1 s sleep/wake per frame remains; fixing that needs firmware (keep the
  sensor streaming during preview).

## 24 October 2026: register banks, and motion flag against MD interrupt

**Bench (Charles):** orientation is right with Mirror and Flip off. With 16 rows, motion
shows in all rows, so 16 is now the default. That contradicts the datasheet (16 x 15 for
VGA, section 4.1) and `ROI_V = 0xe0`; still to explain.

**Sources read** (Himax confidential, cited by section only): datasheet Pre-HM0360 v06,
MD application note v01 (May 2020), MD setting notes of 6 March and 8 April 2025.

**Register banks.** Context A (0x3500 to 0x3559) and B (0x355A to 0x35B3) each have their
own MD_CTRL, MD_BLOCK_NUM_TH, ROI V/H, MD_TH_STR H/L, flicker and TH_COEF registers
(datasheet 10.26, 10.27). MD_TH_MIN, MD_LIGHT_COEF 0x2099, IIR, latency, MD_CTRL1, ROI_OUT
and INT_INDIC/INT_CLEAR exist only in the main bank and are shared. Context MD_CTRL holds the
light coefficient in [7:1] and enable in [0]; main MD_CTRL has latency select in [5:4]. That
is why the sensitivity values are 0x41/0x31/0x21. The firmware always selects a context
(0x3024), so the main copies of the duplicated registers are probably ignored (inference,
untested). Captures use context A, sleep and wake use context B. **Decision (Charles):**
`mdreg` writes A and B together.

**Flag and interrupt.** The app note's motion flag is the "original flag": cells with motion
greater than MD_BLOCK_NUM_TH (app note 3.1). Its motion interrupt is the "latency flag"
(app note 3.2). `MD_CTRL1` bit 0 picks which one drives the interrupt (datasheet 10.16).
Only the interrupt can be read by register (`INT_INDIC` bit 3); both flags are in embedded
data byte 28, which is off. The firmware writes `MD_CTRL1` = 0x06 when the interrupt is on,
so **the wake follows the original flag and the latency settings have no effect** (to
confirm). Each capture sets `MD_CTRL1` to 0.

**Coded (not built or benched).**
- Firmware: `hm0360_md_getMDStatus()` (hm0360_md.c/h) reads INT_INDIC, MD_CTRL, MD_CTRL1 and
  both MD_BLOCK_NUM_TH. In preview, the image task reads them at frame-ready, before its
  existing `hm0360_md_clearInterrupt(0xff)`, and `preview_setMdStatus()` sends them as the
  frame's `"mdstat"`. Only preview frames are affected.
- Viewer: `md_status()` predicts the flag (cells > context A threshold); the status panel
  shows threshold, flag, MD INT and its source; two lamps; an `[md]` log line on each change.

**First bench questions:** does `INT_INDIC` bit 3 latch while the interrupt is off? Does it
follow cells > threshold frame by frame? If it never latches, the next step is keeping the
interrupt on during preview captures (check the INT pin while awake first).

**First bench result (24 Oct, HM0360 build).** The motion count updates with motion. The
flag lamp follows "cells > 4", but the viewer computes that itself, so it proves nothing
about the sensor. **MD INT is set from the first frame and never clears.** Raw values:
`0x2064 = 0xC8` (MD INT set, AE converged, reserved bit 7), `0x2080 = 0x31`,
`0x209E = 0x06` (interrupt on, source = motion flag).

This is not what the code should produce. The init table writes `0x209E = 0x06` at cold
init, but every capture writes 0 (`hm0360_md_setMode(..., 0)`, `Inhibiting MD`) before the
frame, and at frame-ready the image task writes `0x2065 = 0xFF`. Nothing found in the code
re-enables the interrupt while awake (`setSensitivity`, `prepare` and the warm-boot path
were checked). So two HM0360 writes appear not to take effect: MD_CTRL1 = 0 and the
INT_CLEAR.

Explanations, in the order to test:
1. **A staged `camreg` write** on the bench card (replayed after the init table at every
   cold init by `cis_file_process(CAMERA_EXTRA_FILE)`).
2. **The sensor ignores writes to the MD/interrupt registers in the sleep phase** of
   `MODE_SW_NFRAMES_SLEEP` (MODE_SELECT writes clearly work). Guess only. If true it matters
   in the field: the clear in `hm0360_md_prepare()` before DPD would also be lost.

**Pending bench test (console, viewer stopped):** `camreg list`; `camreg 209e 0` then
`camreg 209e` (0x00 = write works); `camreg 2065 08` then `camreg 2064` (0xC0 = clear
works); then `camreg clear`, because camreg writes are saved to the SD card. If both writes
work from the console, look for what re-enables the interrupt. If not, the next step is a
small firmware test of the same writes while the sensor streams (ask first).

The viewer now also shows the raw registers (`regs` status line, and in each `[md]` log
line, logged whenever they change).

**Console test result (24 Oct, HM0360 build).**
- `camreg list`: no registers staged, so explanation 1 is out.
- `camreg 209e 0` read back 0x00, and `camreg 2065 08` took 0x2064 from 0xC8 to 0xC0. The
  sensor accepts both writes when idle, so explanation 2 is unlikely.
- Then **One image**: `2064=c8 2080=31 209e=00`.

**Cause of 0x209E = 0x06: a correction.** The 23 Oct "Frame timing per camera" section says
`USE_HM0360_CAPTURE_TIMER` is not defined. **That is wrong:** `image_task.c` defines it for
the HM0360 build (since commit 7b73436d). So at the start of a burst the image task calls
`hm0360_md_setMode(CONTEXT_A, MODE_SW_NFRAMES_SLEEP, 1, g_timer_period)`. A non-zero sleep
time enables the MD interrupt (`MD_CTRL1` = 0x06) and the sensor times the burst itself.
Only the last frame of a burst calls `setMode(..., MODE_SLEEP, 0, 0)`, which disables it,
before the status is read. So during a burst the interrupt is on, and on a single image it
reads 0x00. The firmware does what it is written to do; the earlier timing figures for the
HM0360 need re-reading with this in mind.

**Still open:** MD INT latched again during the single capture (0xC8 after a clear), and in
free run it stays set even on frames where the cell count is at or below the threshold. So
`INT_INDIC` bit 3 does not yet match "cells > MD_BLOCK_NUM_TH" frame by frame.

**Tape test (24 Oct, HM0360 build, log `_Tools/live_frames/live_log_20261024_153454.txt`,
not committed).** Lens taped, free run, tape lifted briefly once, replaced, stop.
- Frames 1 to 60, taped: 0 cells, **MD INT 0** on every frame. So the interrupt does not set
  by itself with no motion.
- Frame 61, tape lifted: 19 cells, flag 1, **MD INT 1**. Frames 66 and 67: 40 and 8 cells.
- Frames 62 to 90, tape back on (mostly 0 cells, one with 1): **MD INT stays 1**, across a
  burst end (frame 90, interrupt off).
- So the sensor latches MD INT on motion as expected, but the firmware's per-frame
  `hm0360_md_clearInterrupt(0xff)` does not clear it, whereas the console `camreg 2065 08`
  did. Candidates: the value (0xFF sets reserved bits 7:6 and 4) or the timing (the sensor
  is already taking the next frame at frame-ready, 20 ms sleep). **Field relevance:**
  `hm0360_md_prepare()` also clears with 0xFF before DPD.
- Side result: with the sensor timing the burst, the HM0360 preview runs at about 5 fps
  (30 frames in about 6 s).
- Caution for frame-by-frame comparison: ROI_OUT is read later in the frame handling than
  INT_INDIC, and with a 20 ms sleep the sensor may have taken another frame in between.

**Clear test (24 Oct, log `live_log_20261024_154440.txt`, not committed).** Tape lifted
during a burst: frames 11 to 22 had 13 to 48 cells and MD INT 1. MD INT then stayed 1
through frame 30 with 0 cells, and after the burst ended. With the sensor idle,
`camreg 2064` read 0xC8, `camreg 2065 ff` and then `camreg 2064` read 0xC0.
- **So 0xFF is a valid clear; the value is not the problem.** The firmware's identical
  0xFF writes at frame-ready do not take effect, including the one on the last frame, made
  just after `setMode(..., MODE_SLEEP, ...)`.
- The earlier wake (MD INT already 0xC0 after DPD) suggests the 0xFF clear in
  `hm0360_md_prepare()`, made with the sensor idle, does work. That is reassuring for the
  field, but unproven.
- Working hypothesis: the HM0360 ignores INT_CLEAR, or re-latches at once, while a frame is
  being read out. At frame-ready in a 20 ms-interval burst the sensor is already on its next
  frame, and on the last frame it is still finishing one after the MODE_SLEEP request.
- Proposed diagnostic (needs Charles's go-ahead): read INT_INDIC again straight after the
  frame-ready clear and send it with the frame. 0xC0 then would mean the clear works but
  something re-latches it; 0xC8 would mean the write is ignored.
- The two test writes were still staged on the card at the end of this session (`camreg
  clear` not yet run).

**Decision (Charles, 24 Oct): stop investigating INT_INDIC.** The MD interrupt works in the
field; what the viewer needs is to show when a wake would be triggered, to evaluate the
controls still to come. The proposed INT_INDIC diagnostic is dropped. Last observation: with
the lens taped, **One image** gave `2064=80`, MD INT clear.

The logs support predicting the wake from the cell count: MD INT went from 0 to 1 on exactly
the first frame with cells > MD_BLOCK_NUM_TH (frames 61 and 11 in the two logs) and never
while taped. The viewer now shows a **WAKE** lamp and `wake YES/no (cells > threshold)`, plus
a count of wake frames per free run. MD INT is kept only in the log for reference. The
prediction ignores the latency logic, which the firmware leaves unused (`MD_CTRL1` bit 0 =
0); the status warns if that changes.

**Controls and frame timing (24 Oct, coded, not built or benched).**
- Charles asked for: Mirror and Flip removed; a wake-threshold control; sensitivity limited
  to the existing low/medium/high.
- Firmware: new console command `mdth [N]` (CLI-commands.c) and
  `hm0360_md_setBlockThreshold()` (hm0360_md.c/h). It writes MD_BLOCK_NUM_TH to the main
  bank and both contexts and replies with the A and B values read back. Not saved; a cold
  boot or `inithm0360` restores the table value. Bench-only, so not added to
  `ble_commands.md`.
- Viewer: Sensitivity Low/Med/High sends `md N` (op 17, saved at the next sleep; the viewer
  reads `getop 17` at connect and puts it back on close). Wake threshold spinbox sends
  `mdth N`.
- **Charles's observation:** a quick finger movement is detected, a slow one usually not.
  His question: is the HM0360 free-running in preview, with frames (and their MD bits) not
  shown? **Answer: yes.** With `USE_HM0360_CAPTURE_TIMER` the sensor takes one frame, sleeps
  for the capture interval, and repeats by itself. The AI processor streams one frame in
  about 200 ms (log timestamps) and then takes whichever frame comes next. At the 20 ms
  interval the sensor makes a frame about every 55 ms (estimate: the first capture after the
  mode change took 55 ms), so roughly three in four frames are never shown.
- **The likelier cause of missing slow motion** is the frame spacing itself: MD compares each
  frame with the previous one, and frames 55 ms apart differ too little for slow movement.
  In the field the frames are an op 11 interval apart. Recommendation: in preview, set
  Interval to the field op 11 value (over about 250 ms, so every frame is shown).

**Open (Charles, 24 Oct):** come back to MD sensitivity, meaning slow movement not being
detected, after the controls are built and tested. First check on the bench: with Interval
set to the field op 11 value, is slow movement detected at each of Low, Med and High?
