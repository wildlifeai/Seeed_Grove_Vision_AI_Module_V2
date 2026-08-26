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

(Further tasks may follow).

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