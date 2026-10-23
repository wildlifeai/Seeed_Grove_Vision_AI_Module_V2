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
