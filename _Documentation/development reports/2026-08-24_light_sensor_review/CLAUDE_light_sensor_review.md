# Task: Review of some aspects of Light Sensor Operation

#### File: CLAUDE_light_sensor_review.md
#### Author: Charles Palmer
#### Date: 24 August 2026

## Background

The WW500 board uses an HM0360 camera. It is possible that this can operate as a light sensor.

A different Claude session worked on this, producing changes to the C code and new Python tools.
The smart phone app also interacts with the light sensor messages.

Many of the changes were done by the PR #141 pull request. Documentation from this process is in the 
[_Documentation\development reports\2026-08-06_pr141-camera-features-review/README.md](../2026-08-06_pr141-camera-features-review//README.md) 
file. 

The [REVIEW_PR141.md](../2026-08-06_pr141-camera-features-review/REVIEW_PR141.md) is from Claude 
and says what is in the PR - a code review from Charles
is in [CGP_Code_Review_July26.md](../2026-08-06_pr141-camera-features-review/CGP_Code_Review_July26.md)

Some of this is about the light sensor, and other files exist to document the work (not essential to read them all).

Claude and I produced a summary of how the C code works in 
[light_sensor.md](../../../EPII_CM55M_APP_S/app/ww_projects/ww500_md/doc/light_sensor.md). 

## What we are going to do in this review

I want to review the light sensor functionality so it is clear to me what the code does.
In particular I want to see how effective the code is as a light sensor.

It is possible that I might then request some code changes and documentation. So this review will be
broken into several sub-tasks, which will be listed here:

1. Modify Light Sensor messages (done)
2. Clean code; run hm0360_md_getAEStats() just once per loop. (done)
3. Advise on moving light meter code to a separate .c & .h file. (done)
4. Implement Separate lightSensor.c & .h (done)
5. Add CLI on-demand "just check the light" command (done)
6. Create a python script to run the new 'light' command continuously. (done)
7. Change the light/dark decision algorithm. (done)
8. Add other options for enabling the flash LED (done)

(Further tasks may follow).

## Add other options for enabling the flash LED ___completed___

1. I am still unsure that either light sensor algorthm is reliable. Maybe we can refine the light
  sensor algorithm in the future.
2. I would like to consider re-instating options to manually set the flash as always on, always off and determined by time of day.
This would mean extending `FlashLedMode_t` in `ledFlash.h`.
These were once there but removed by a different Claude run by a different user. They are mentions in 
`AE_Light_Sensor_Roadmap.md` section 8.1 
3. I think the flashMode would be set to one of these values directly from a fresh
operational parameter, rather than being dynamically set by ledFlashSetFlashModeFromOpParam().
4. Please consider the implications of this. Write a markdown report for me to consider.
5. Ask questions if useful.
6. Don't make code changes until agreed.


## Change the light/dark decision algorithm ___completed___

1. The algorithm is unreliable. I want to test a different algorithm.
2. Leave existing code in place in lightSensor.c & .h and add a #define to select between exisiting and new.
3. New algorithm is: it is dark if (a) AE is not Converged or (b) AE analog gain exceeds a #define level 
(initially 2).
4. No need to run multiple tests (currently defined in AE_SAMPLE_COUNT).
5. Consider the new console output and whether this affects existing python scruipts. 

## Create a python script to run the new 'light' command continuously. ___completed___

1. Create 'ae_stream.py' to complement the existing 'ae_monitor.py'.
2. The task is simply to run the 'light' command at full speed.
3. Stop when the user types 'esc'.
4. Carefully check that the serial port is released when done. Do the same check on ae_monitor.py as I 
thought I might have had trouble running Teraterm aagin after running ae_monitor. Check on all possible exit paths.


## Add CLI on-demand "just check the light" command ___completed___

1.	This is referenced in the `light_sensor.md` document. Let's do it. 
2.	Command should be `light` and we might as well print the numeric light value as well as the current binary light/dark assessment.
2.	Review the new light sensor API and confirm that suitable functions exist. If so, just code it. If not, ask for advice.
4. Ask any useful questions.

## Implement Separate lightSensor.c & .h ___completed___

1.	Implement this based on the dicsussion and 'lightSensor.h'
2.  Leave the existing .h file in the doc directory as a reference.
3. Implement the new files using `ww500_md/doc/c_file_format.md` rules.
4.	Keep the comments modest - those in 'lightSensor.h' are OK for a reference but much too verbose
for this task.
5	Ask any questions if necessary.

## Advise on moving light meter code to a separate .c & .h file.  ___completed___

1. List functions that are used by the light sensor and flash decision-making.
2. Propose a route to moving these to their own file e.g. `light_meter.c`
3. Don't move code until instrcuted.

#### Clean code; run `hm0360_md_getAEStats()` just once per loop. ___completed___

1. The following test occurs several times:
```
((ledFlashGetFlashMode() == FLASH_MODE_AE)
        		|| (fatfs_getOperationalParameter(OP_PARAMETER_SLOT_SWITCH) == 1))
```
Do this test once, early, and use it to set `aeCheckRequired` which I have added in image_task.c
 that can be used to replace it (makes it easier for a human to follow the code).

2. If I wake the AI processor to take several images (e.g. `capture 3 1000`) then `hm0360_md_getAEStats()` 
and `ledFlashNewAEStats()` run for each image. This takes time. Ensure that it happens only 
after the final image (that might involve this test: `(g_cur_jpegenc_frame == g_captures_to_take)`)
3. Instrument the time taken to run `hm0360_md_getAEStats()` and print the time . You can use `app_getElapsedMs()`
4. I have an hunch that we don't usually need to read AE registers 16 times
in `hm0360_md_getAEStats()` Add code there that collates all 16 values of `gain.aeMean`
and prints them as a single string. That will let us check the AE settling time.

Sample output:
```
[LS] getAEStats: 16 AE_MEAN samples: 71 71 71 71 71 71 71 71 71 71 71 71 71 71 71 71
[LS] hm0360_md_getAEStats took 2363ms
[LS] AE light check: mean AE = 71 (min 71, max 71) over 16 frames, threshold = 65, gain railed = no -> DARK (flash wanted)
```
NOTES:
1. probably no need to average 16 values as they seem all the same. 
2. Threshold probably too high.
3. Hysteresis probably too large. 

#### Modify light sensor print statements ___completed___

1. Review the instructions above and confirm that you understand them.
2. Ask questions where that is helpful
3. Review the code for console output that is specific to light sensor operation and list them for me.
I have spotted some, such as "xprintf("Skipping NN processing (AE light check).\n");, 
xprintf("Timer wake for AE light check\n");, 
xprintf("Will wake to check light level in %d seconds\n", aeCheckDelay);
4.	When I agree this list, modify the console output so that the first characters start with '[LS]' 
and are coloured cyan. That will make it easier for humans to review these lines.


---
 ## Completed tasks:

* Implemented `flash_led_modes_proposal.md`'s final design in full:
  `FlashLedMode_t` (`ledFlash.h`) gains `FLASH_MODE_ALWAYS_ON`/`FLASH_MODE_TIME_OF_DAY`
  (`FLASH_MODE_OFF = 0`); three new op-parameters after Charles's own
  `OP_PARAMETER_RFU_1`/`_2` placeholders - `OP_PARAMETER_FLASH_MODE` (34),
  `OP_PARAMETER_FLASH_TOD_START`/`_TOD_DURATION` (35/36); `OP_PARAMETER_AE_CHECK_INTERVAL`
  renamed to `OP_PARAMETER_FLASH_EVALUATE_INTERVAL` (same index, 24, broadened
  meaning - also paces time-of-day re-checks now); `ledFlashSetFlashModeFromOpParam()`
  takes the new mode parameter directly and switches on it; new `evaluateTimeOfDay()`
  (simple wrap-around window, no sunrise/sunset math - deliberately, per Charles) and
  exported `ledFlash_reevaluateTimeOfDay()` (called from `prvSetUtc()` so a fresh RTC
  set takes effect immediately, not just at the next wake); `lightSensor.c` untouched
  throughout, kept exclusively about light sensing per Charles's explicit constraint -
  the periodic-wake-for-TIME_OF_DAY condition lives in `image_task.c` instead
  (`aeCheckRequired` widened there, not in `lightSensor_isRequired()`); both
  DARK/LIGHT console lines reworded to mode-agnostic `ledFlashIsActive()`-based
  "Flash is currently armed/not armed" wording. Docs updated: `MANIFEST/config_file.md`
  (now canonical - see below) and `MANIFEST/CONFIG.TXT` (new default lines),
  `AE_Light_Sensor_Roadmap.md` §8.1 annotated as superseded.
  Also consolidated `_Documentation/Operational_Parameters.md` (a near-total,
  already-drifting duplicate of `MANIFEST/config_file.md`) into a short pointer -
  `config_file.md` is now the single canonical op-parameter doc; `AGENTS.md`'s
  routing table updated to match. Created `_Tools/nz_to_utc.py` (NZST/NZDT-aware,
  stdlib `zoneinfo` with a fixed-offset fallback if `tzdata` isn't installed) to
  help compute `OP_PARAMETER_FLASH_TOD_START` values for bench testing.
  Also added the one-line note flagged in the proposal's §7 to `light_sensor.md` §6.4 -
  the `light` CLI command's DARK/BRIGHT verdict may not correspond to what's actually
  controlling the flash under `ALWAYS_ON`/`TIME_OF_DAY`.
  Not yet build-verified. (complete 4 September 2026, build verification pending)

* Skip NN initialisation entirely for a throwaway light-check-only wake
  (`aeCheckOnlyWake`, `image_task.c`) - Charles noticed, after installing a real NN
  model, that `cv_init()` ran (visible in the console: "Initialising NN with
  2412/ETHOS-U 2411 library...") on a periodic timer wake whose sole purpose was to
  re-evaluate the flash mode, between `ledFlashSetFlashModeFromOpParam()` and
  "Timer wake to re-evaluate the flash" - wasted work, since `aeCheckOnlyWake` was
  already known to skip NN *inference* later (`skip_nn` in the FRAME_READY handler).
  Root cause: `aeCheckRequired`/`aeCheckOnlyWake` were computed *after* `cv_init()`
  ran, even though every input they need (`woken`, `cameraInitialised`,
  `OP_PARAMETER_TIMELAPSE_INTERVAL`, and `lightSensor_isRequired()`/
  `ledFlashGetFlashMode()` - already set by `setupLEDFlash()`, which runs earlier)
  was available before it. Fixed by moving both computations earlier (still "once,
  early" per the existing comment, just earlier still) and gating `cv_init()` on
  `!aeCheckOnlyWake`; the later block that queues `APP_MSG_IMAGETASK_STARTCAPTURE`
  now just reads the already-computed flag instead of recomputing it. Verified every
  consumer of `nnStatus` (a local variable, defaults to -1/"disabled", used only for
  one console line - correctly still says "disabled" when skipped) and `cv_modelLoaded()`
  (the other call site is inside `prepareJpegFile()`, never reached for
  `aeCheckOnlyWake` since file save is already skipped) - nothing else depends on
  `cv_init()` having run for this wake type. A second idea from the same
  conversation - moving NN init to run *after* the picture is triggered, to reduce
  wake-to-capture latency (there's already a `TODO` comment on this) - was
  deliberately left alone: it needs restructuring how the *first* capture is
  triggered relative to task startup (the `STARTCAPTURE` message currently isn't
  processed until the task's main loop starts, after all of this init work
  completes, so simply reordering two adjacent calls would not achieve real
  overlap) - treated as a separate, bigger investigation if wanted later.
  Not yet build-verified. (complete 4 September 2026, build verification pending)

* Fixed a STROBE-flicker bug in `decideDarkBrightGainBased()` (`lightSensor.c`),
  found by Charles bench-testing `ae_stream.py --capture` with the flash enabled
  (op13): as he reduced the light, the LED flashed rapidly for 1-2s instead of once
  per capture - "every time, or not at all" (i.e. tied to whether flash was armed
  for that capture, not to any particular light level). My first two guesses were
  wrong and Charles caught both: I initially suspected `WDTIMOUTFIX`'s capture-retry
  path (added a temporary unconditional `RETRY_RE` check to `ae_stream.py` to catch
  its `>>>>`-marked console lines without needing `--verbose` - it printed nothing,
  ruling this out), then re-analysed the wrong function entirely
  (`sampleAeStats()`/`decideDarkBright()`, the *original* algorithm) before Charles
  reminded me `AE_DECISION_GAIN_BASED` is currently enabled and he changes the code
  too, not just me. Root cause, once looking at the right function:
  `decideDarkBrightGainBased()` wakes the sensor into `MODE_SW_CONTINUOUS` (with a
  500ms settle delay) whenever it finds the sensor asleep, but never touched STROBE
  at all - unlike `sampleAeStats()`, which explicitly disables it for exactly this
  reason. A real capture leaves the sensor in `MODE_SW_NFRAMES_SLEEP` (1 frame then
  auto-sleep in hardware); if that capture armed STROBE (scene judged dark), it
  stays armed straight through the sensor's own auto-sleep, so waking it back into
  continuous streaming without disabling STROBE first fired the flash on every
  frame streamed during the settle delay and read. Fixed by porting
  `sampleAeStats()`'s STROBE save/disable/restore bracketing into
  `decideDarkBrightGainBased()` (both the normal path and the early-return-on-
  register-read-failure path). Removed the now-superseded `RETRY_RE` diagnostic
  from `ae_stream.py` once the real cause was confirmed. Charles confirmed the fix
  works. (complete 3 September 2026, device-tested)

* Added a second, simpler dark/bright decision algorithm to `lightSensor.c`,
  selected by a new `AE_DECISION_GAIN_BASED` `#define` (on by default) - Charles
  judged the existing mean-AE/threshold/hysteresis algorithm unreliable and wanted
  to try an alternative without discarding the original. New algorithm: dark if AE
  hasn't converged, OR analog gain exceeds a new `DARK_ANALOG_GAIN_THRESHOLD`
  `#define` (initially 2) - a single `hm0360_md_getGainRegs()` read, no sampling
  loop, no wake-and-settle delay, and (per Charles's explicit answers) no
  hysteresis: a fresh decision every call, only persisted (`OP_PARAMETER_AE_FLASH_STATE`)
  so it is available before the next reading, not blended with the previous one.
  The original algorithm (`sampleAeStats()`/`decideDarkBright()`,
  `LightSensorStats_t`) is untouched, just now compiled out when the `#define` is
  active - undefine it to revert.
  New function `decideDarkBrightGainBased()` prints the same `[LS] AE light
  check: ... -> DARK|BRIGHT` prefix/suffix as before (no algorithm-identifying
  marker, per Charles's request) but omits the mean-AE/min/max/threshold/gain-
  railed fields entirely, since none of them feed this algorithm's decision and
  printing them would misleadingly imply they did - the line is now just
  `AE light check: analog gain = N, converged = yes|no -> DARK|BRIGHT`.
  This broke `_Tools/ae_stream.py`'s `LIGHT_RE` and `_Tools/ae_monitor.py`'s
  `AE_RE` (both required `mean AE = N`) - fixed both: `ae_stream.py`'s `LIGHT_RE`
  now treats `mean AE ...threshold` as optional (kept `analog gain`/`converged` as
  the stable anchor) and prints a leaner "Light level: -- (...)" line when mean AE
  is absent; `ae_monitor.py`'s `AE_RE` instead uses two independent alternatives
  (mean/threshold branch vs. analog-gain/converged branch) so the original,
  already-legacy-tolerant pattern is not weakened, with a matching leaner print
  branch added. `jpegAE-batch.py`/`jpegAE_annotate.py` are unaffected - they parse
  the MakerNote EXIF field, which `image_task.c` populates from its own,
  always-unconditional single AE-register read, independent of which
  `lightSensor.c` algorithm is active.
  Not yet build-verified. (complete 1 September 2026, build verification pending)

* Fixed an EXIF/MakerNote flash-state off-by-one bug, found while building a new
  bench tool (`_Tools/jpegAE_annotate.py`, burns the MakerNote AE fields plus the
  standard EXIF `TAG_FLASH` (0x9209) onto the bottom of each JPEG) - Charles noticed
  the two flash values looked shifted by one image when comparing the annotated
  frames against what the images actually showed. Root cause, confirmed by tracing
  `APP_MSG_IMAGETASK_FRAME_READY` handling in `image_task.c`: within the processing
  of a single captured frame, the post-capture light check (when it runs, i.e. the
  last frame of a burst) calls `ledFlash_setActive()` at line ~919, which updates
  `ledFlashIsActive()` to the *fresh* decision for the *next* capture - but
  `prepareJpegFile()`, called a few dozen lines later for the *same* frame, was
  reading that same live `ledFlashIsActive()` at EXIF-build time
  (`exif_input.flash_fired = ledFlashIsActive();`). So the flash value written into
  an image's EXIF (and, downstream, both the MakerNote's `flashFired` field and the
  standard `TAG_FLASH` tag, which both derive from `exif_input.flash_fired`) was the
  decision for the *next* image, not the one that was actually used to arm the flash
  for *this* image (decided earlier, before capture, in
  `configure_image_sensor(CAMERA_CONFIG_RUN)`). Confirmed `ledFlash_setActive()` has
  exactly one call site in the whole codebase (`image_task.c:919`), so no other
  write could be muddying this - a clean, consistent one-image lag.
  Fix: added a new file-scope `lastCaptureFlashState` (`image_task.c`), snapshotted
  from `ledFlashIsActive()` at the top of `FRAME_READY` handling (right after
  `ledFlashDisable()`, before the light check can touch it), and changed
  `prepareJpegFile()` to read that snapshot instead of a live `ledFlashIsActive()`
  call. Noted but NOT fixed (out of scope for this pass, flagged for a separate
  decision): `img_correct_process_mode()`'s white-balance correction (RP2/RP3 builds
  only, `image_task.c` ~lines 1038/1055) reads `ledFlashIsActive()` at the same late
  point in the same frame's handling, so it likely has the identical staleness
  problem for the flash argument it passes in.
  Not yet build-verified. (complete 1 September 2026, build verification pending)

* Found and fixed a date-rollover bug in `exif_utc.c` while investigating why the
  periodic AE-check-interval timer wake stopped working (Charles saw
  `[LS] Will wake to check light level in 60 seconds` followed by `Will wake at
  2026:09:01 00:17:54` instead of the same-day 60-seconds-later time - a genuine RTC
  alarm miscalculation, not just a bad print, since the same computed value is
  written into the hardware alarm register). Root cause: `days_in_month()`'s lookup
  table is 0-indexed but `rtc_time.tm_mon` is 1-indexed everywhere else in this file
  (confirmed via `exif_utc_utc_string_to_time()`'s direct ISO-string parse), so it
  was always reading one month ahead - silently breaking date arithmetic that
  crosses Jan 29-31, Mar 31, May 31, Aug 31, or Oct 31, and causing an out-of-bounds
  array read for any December date. Also fixed two compounding bugs found while
  tracing the fix through: the month/year wraparound used `>= 12` and reset to `0`,
  which (for 1-indexed months) incorrectly forced a false year-rollover on every
  plain November→December transition, not just a genuine December overflow, and
  wrapped to a nonexistent "month 0" instead of January; and `is_leap_year()` added
  1900 to a year value that's already a full 4-digit year (e.g. 2026), not years-
  since-1900. Confirmed unrelated to any light-sensor work this session - different
  file, and `days_in_month()`/`exif_utc_add_seconds_to_tm()` have exactly one live
  caller in the whole app (`sleep_mode.c`'s RTC wake-alarm calculation, used by both
  the AE-check timer and timelapse mode), so this had been a latent, date-dependent
  bug for both features rather than anything introduced this session. Both camera
  variants build clean. Not yet device-tested. (complete 31 August 2026, build
  verified only)

* Routed the `light` CLI command through the image task instead of calling
  `lightSensor_takeReadingForced()` directly from the CLI task - fixes both a
  consistency gap (every other light check already went via the image task's queue)
  and a real cross-task I2C race risk (`hm0360_md.c`'s `saveMainCameraConfig()` swaps
  the I2C slave ID with no locking - `light` and a real capture could otherwise race
  on it from two different tasks). First cut used a lightweight shortcut
  (`msg_data == 0` → call `lightSensor_takeReadingForced()` directly, no real
  capture); Charles then asked for the exact same state-machine path as the periodic
  `aeCheckOnlyWake` timer wake, not just the same entry point - revised so
  `msg_data == 0` now triggers a real (throwaway) single-frame capture via a new
  `aeCheckCliTriggered` flag, sharing 100% of the capture mechanics (skip-NN,
  skip-file-save, flash-suppression) with the timer path. Deliberately kept the
  *consequences* separate though: flash arming and the auto camera-switch check
  still only happen for the timer-triggered case, not a CLI-triggered one - `light`
  stays a passive, always-forced diagnostic, as already decided; only the mechanics
  became one path, not the side effects. `aeCheckOnlyWake` is now also cleared at
  the capture's true completion point (`DISK_WRITE_COMPLETE`) rather than only at
  the next DPD sleep, so a CLI-triggered check can no longer leak into a later
  genuine `capture` command run without an intervening sleep. `prvLight()` itself
  (blocking send-and-wait on `xLightCheckDoneSemaphore`) is unchanged from the first
  cut. Full design (both versions) in
  [light_command_via_image_task_proposal.md](light_command_via_image_task_proposal.md).
  Both camera variants build clean. Not yet device-tested.
  (complete 31 August 2026, build verified only)

* Fixed a third flash-hardware bug, found by Charles observing hardware directly:
  during the periodic AE-check-only wake (`aeCheckOnlyWake`, `OP_PARAMETER_AE_CHECK_INTERVAL`),
  the flash fired once even though `lightSensor.c`'s own sampling is flash-free. Root
  cause: that periodic wake still runs a real (throwaway) image capture to refresh AE
  registers - a design that predates `lightSensor.c`'s direct register-sampling and is
  now redundant for that purpose - and `configure_image_sensor(CAMERA_CONFIG_RUN)`
  (`image_task.c`) arms the flash for every capture unconditionally, using the
  *previous* light decision (the fresh one isn't known until after this capture
  finishes). Considered removing the throwaway capture entirely, but that would bypass
  the normal capture state machine's barrier/telemetry/WDT-retry handling in ways not
  fully verified safe - deferred as a separate, bigger change. Fix applied instead:
  guard the flash-arming in `CAMERA_CONFIG_RUN` with `!aeCheckOnlyWake`. For the
  HM0360/STROBE_CONTROLS_FLASH branch this needed an explicit `hm0360_md_configureStrobe(false)`,
  not just skipping the call - STROBE may already be armed from the previous
  `image_sleepNow()`'s MD-illumination setup, so omitting the call alone would not
  have turned it off (caught this refinement while implementing the originally
  proposed simpler skip). `image_sleepNow()` re-arms STROBE correctly from the fresh
  decision before the next sleep either way. Both `cis_imx708` and `cis_hm0360` build
  clean. Not yet device-verified. (complete 28 August 2026, build verified only)

* Fixed: flash LED turning on during a bare light check (Charles noticed it happening
  "sometimes" - i.e. whenever the scene was dark at the time). Root cause: my own
  `lightSensor.c` refactor (see below) put `ledFlash_setActive(dark)` inside the
  shared `decideDarkBright()` helper, which both `lightSensor_takeReading()` (the
  real per-capture path) AND `lightSensor_takeReadingForced()` (the `light` CLI
  command / `ae_stream.py`) call. That meant every on-demand `light` check now drove
  the physical flash hardware as a side effect - something the old, pre-refactor code
  could never do, since its equivalent (`ledFlashNewAEStats()`) was only ever reached
  from inside an actual capture. Fix: removed the `ledFlash_setActive()` call from
  `lightSensor.c` entirely; `image_task.c` now calls it explicitly right after
  `lightSensor_takeReading()`, only in the real capture/wake-cycle path - matching
  what the original `lightSensor.h` design draft's "Design notes" had actually
  recommended (I'd deviated from that when implementing it). `light`/`ae_stream.py`
  are now purely passive again.
  (complete 27 August 2026; hardware testing of this fix surfaced two further,
  independent flash-hardware bugs - see the 28 August entry below)

* Fixed two further flash-hardware bugs found when Charles tested the above fix on
  `capture 3 1000`: (A) a fast flicker during the ~1.9s AE-sampling window itself, and
  (B) the flash solid ON for a further ~1s afterward - both should be off throughout
  light sensing. Confirmed against Charles's hardware model (STROBE, from the HM0360
  itself, drives the LED for HM0360 captures and MD illumination; FLASHEN, from the
  PCA9574, is driven by software only for RP3 captures; colour selection is
  independent of both) by re-reading every `ledFlashActivate()`/`hm0360_md_configureStrobe()`
  call site in `image_task.c` - all correctly gated by the existing
  `STROBE_CONTROLS_FLASH` macro, confirming neither bug was in that logic.
  (A): `sampleAeStats()` (`lightSensor.c`) wakes the HM0360 into `MODE_SW_CONTINUOUS`
  for sampling but never touched STROBE_CFG, so a strobe left armed by the *previous*
  `image_sleepNow()` (for MD illumination) fired on every streamed AE-sampling frame.
  Fixed by adding a new `hm0360_md_getStrobe()` getter (`hm0360_md.c/.h`, mirrors the
  existing `hm0360_md_getMode()` pattern) and saving/restoring STROBE_CFG around the
  sampling window, the same way the streaming mode itself is already saved/restored.
  (B): `ledFlash_setActive()` (`ledFlash.c`) updated the `flashActive` flag AND
  immediately drove the PCA9574 FLASHEN hardware, even though nothing needs the LED
  lit at that moment - the capture that triggered the check is already finished, and
  the next real capture / `image_sleepNow()`'s STROBE arming both read the flag
  themselves when they actually need it (`ledFlash.c` already had precedent for a
  flag-only update with no hardware write, in `ledFlashSetFlashModeFromOpParam()`).
  Fixed by removing the `ledFlashActivate()` call from `ledFlash_setActive()` - it is
  now a pure flag setter.
  Both `cis_imx708` and `cis_hm0360` build clean. Not yet device-verified.
  (complete 28 August 2026, build verified only)

* Created `_Tools/ae_stream.py` to complement `ae_monitor.py` - sends the on-demand
  `light` CLI command back-to-back as fast as the device replies (no fixed interval),
  prints each `Light level: N (DARK|BRIGHT)` reading with a timestamp and bar graph.
  Waits for device wake the same way `ae_monitor.py` does if it's asleep at start;
  each `light` command resets the 60s CLI inactivity timer so a continuous stream
  holds the device awake indefinitely. Stops on ESC (cross-platform: `msvcrt` on
  Windows, `termios`/`select` on POSIX) or Ctrl+C. Both the serial port and (on
  POSIX) the terminal's raw-mode setting are released via `try`/`finally` /
  a context manager on every exit path - normal ESC, Ctrl+C, a device timeout, or
  any other exception.
  Audited `ae_monitor.py`'s own exit paths per Charles's request (he suspected a
  possible port-not-released issue after running it): its single `try:`/`finally:
  port.close()` wraps the entire body immediately after the port opens successfully,
  with nothing but two `def`s in between - no exit path (normal return, any
  exception, `KeyboardInterrupt`) can skip the close. No bug found; left unchanged.
  Charles tested it against real hardware and confirmed it works.
  (complete 27 August 2026, hardware-tested)

* Diagnosed and fixed a FAT task stack overflow (`save_configuration()`'s
  `comment_lines[MAXNUMCOMMENTS][80]` stack-local, ~2.6KB on a ~4.3KB task stack) hit
  while investigating an unrelated `states` CLI command crash. Found the same bug was
  already fixed on other branches (Victor, commit `13bda489`, 11 July 2026) but never
  reached `ae_review` - ported that fix (`comment_lines` now `static`, `MAXNUMCOMMENTS`
  parenthesised) directly into `fatfs_task.c`. Full writeup, including the separate
  `states`-command NULL-pointer bug found in the same session and a flagged
  branch-divergence question, in
  [fat_task_stack_overflow_missing_fix.md](fat_task_stack_overflow_missing_fix.md) -
  kept here for the PR review. (complete 27 August 2026, build/device not yet verified)

* Add CLI on-demand "just check the light" command — new `light` CLI/BLE command in
  `CLI-commands.c` (`prvLight()`/`xLight`, registered in `vRegisterCLICommands()`).
  Charles chose "always force a fresh reading" over respecting the
  `lightSensor_isRequired()` gate, so `lightSensor.h/.c` gained
  `lightSensor_takeReadingForced()` (bypasses the gate; `lightSensor_takeReading()`
  now just calls it when required). Response e.g. `Light level: 71 (DARK)`. Also
  updated `light_sensor.md` §6.4 (gap closed) and `ble_commands.md` (new `AI light`
  row). Both camera variants build clean; Charles tested the `light` command on
  device and confirmed it works. (complete 26 August 2026, device-tested)
  App now receives messages like this:
  ```
  Light level: 85 (BRIGHT)
  ```

* Implement separate lightSensor.c & .h — new public API (`lightSensor_isRequired()`,
  `lightSensor_takeReading()`, `lightSensor_getReading()`, `lightSensor_isDark()`) in
  `EPII_CM55M_APP_S/app/ww_projects/ww500_md/`, following the `doc/lightSensor.h`
  design draft (kept in the review folder as reference; comments in the real files
  are much shorter). `hm0360_md_getAEStats()`/`HM0360_AE_STATS_T` removed from
  `hm0360_md.c/.h`, replaced by 3 new primitives (`hm0360_md_getGainCeilings()`,
  `hm0360_md_getMode()`, `hm0360_md_setModeSelectOnly()`) that lightSensor.c uses to
  rebuild the sampling loop itself. `ledFlashNewAEValues()`/`ledFlashNewAEStats()`
  removed from `ledFlash.c/.h`, replaced by a one-line `ledFlash_setActive(bool)`
  setter; `image_task.c` now orchestrates
  (`lightSensor_takeReading()` → `ledFlash_setActive(lightSensor_isDark())` →
  `cameraSwitch_autoSwitchCheck()`) instead of the old function calling into
  ledFlash directly. `camera_switch.c` left unchanged (still reads
  `OP_PARAMETER_AE_FLASH_STATE` directly) - optional follow-up noted in the header
  draft, not done. The `[LS] AE light check: ...` console line `_Tools/ae_monitor.py`
  depends on is preserved verbatim in `lightSensor.c`.
  Both `cis_imx708` and `cis_hm0360` builds verified clean (zero errors) after Charles
  installed `make`. Also fixed an unrelated bug found while verifying: `mk/image_gen.mk`'s
  `device_image` target deleted both variants' device-named `.IMG` files on every build
  instead of just its own - now only cleans up its own variant letter.
  (complete 26 August 2026, build verified)

* Modify light sensor print statements — the 10 AE-light-check console lines in
  `image_task.c`, `ledFlash.c`, `hm0360_md.c` and `camera_switch.c` now prefix with
  `[LS] ` and print cyan. Builds OK. (complete 24 August 2026)
* Clean code; run hm0360_md_getAEStats() just once per loop — added `aeCheckRequired`
  (computed once in `vImageTask()`, replacing 3 repeated tests); AE sampling now gated
  to the last image of a burst; `hm0360_md_getAEStats()` call is timed and printed;
  the function now logs all sampled `AE_MEAN` values as one line. Builds and runs OK —
  Charles confirmed on-console: a flat, already-converged scene (16/16 samples at
  AE_MEAN 71) took 2353ms; settling-time check on a light change is a follow-up.
  (complete 24 August 2026)