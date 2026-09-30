# Testing and Correcting some App Comms Bugs

#### File: `README.md`
#### Author: Charles Palmer
#### 30 September 2026

## Purpose

To test recent changes (unresponsive BLE processor, large file tx from app, and some of the github issues.

Some work has been merged into the dev branch (and checked out as `260930_appCommsBugs` branch.
These will be tested:

* Unresponsive BLE processor (referenced in `_Documentation\development reports\
2026-09-17_BLE_processor_unresponsive`)
* Large file transfer (referenced in `_Documentation\development reports\
2026-09-14_firmware_update_fails`)
* Fixed 'RP3 camera can't take a photo after cold boot' - this was issue #238 and related to PR#239.
* Transfer improvements from `ww500_minimal` - relates to issue #249
* Proposed bigger changes to reduce boot ime -  [issue #251](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/251)

## Unresponsive BLE processor

This was tested on:

1.	A working board (no problem)
2.	The depopulated board (which contains no BLE processor) where the problem is expected. The board now reports this:
```
I2C master did not read our I2C message
BLE processor did not read the first message within 300ms: treating it as unresponsive
```
and reports `selfTest 4000` - this is correct mitigation
3.	Then Victor will return a WW500 which shows a fault (the subject of the `2026-09-17_BLE_processor_unresponsive` 
development report) - and I will carry out further tests.


## Large file transfer

Use the app FILE_TRANSFER_TEST menu.

1.	I tested with no SD card - failed gracefully. Error reported on app.
2.	I tested with BIG.TXT then LARGE.BIN (500k) - no errors seen. transfer speed (my phone and SD card) 3.5kB/s

## RP3 camera can't take a photo after cold boot

This was [issue #238](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/238)
 and a fix proposed in [PR#239](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/pull/239) 
(by Victor's AI). The problem related to nested calls to `saveMainCameraConfig()` and `restoreMainCameraConfig()` 
in `hm0360_md.c`

The changes seemsed quite intensive. I asked my Claude to comment. We agreed to make a fix but only in `hm0360_md.c`
as the other chnages were unnecessary.

We made the changes to `hm0360_md.c` in this branch and I will add a comment to [PR#239](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/pull/239) :

```
 Thanks for the clear write-up in #238; the diagnosis is right. 
 
 The fault is that saveMainCameraConfig()/restoreMainCameraConfig() in hm0360_md.c are nested 
 (hm0360_md_init() and hm0360_md_prepare() call hm0360_md_setMode(), 
 which calls hm0360_md_enableInterrupt()/hm0360_md_disableInterrupt()), 
 so an inner save overwrites mainCameraID with the HM0360's own address and the bus is left on the HM0360.
 
 The necessary change is the idSaveDepth counter in hm0360_md.c. With it, every HM0360 access hands the bus back 
 to the address it found, so the IMX708 is already selected at stream on/off, sensor start/stop and the AE writes. 
 
 The extra cisdp_select_main_camera_i2c() calls and the ae.c change only re-select an address that is already set, 
 so they are not needed, and they add a second way of choosing the bus address outside the save/restore pair. 
 (If another task could touch the HM0360 during an IMX708 sequence, re-selecting would not be a reliable fix anyway; 
 that would need a lock.)

I have made just the hm0360_md.c counter change (the same code as this PR) on branch `260930_appCommsBugs`, 
and left the other three files out. Please reduce this PR to the hm0360_md.c change, or close it in favour of 
that branch, whichever suits the split series.
```

## Transfer improvements from `ww500_minimal`

Victor has already picked up on this and lodged 
[issue #249: The RP3 takes about 320 ms to power up at every wake](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/249)

Here we move some of the enhancements found in the `ww500_minimal` work into `ww500_md`

The source is the list in
[`ww500_minimal/doc/README.md`](../../../EPII_CM55M_APP_S/app/ww_projects/ww500_minimal/doc/README.md#changes-to-transfer-to-ww500_md),
section `Changes to transfer to ww500_md` with the measurements in
[`ww500_minimal/doc/power_investigation.md`](../../../EPII_CM55M_APP_S/app/ww_projects/ww500_minimal/doc/power_investigation.md)
section , "Time from `capture` to the frame".
The first three are the #249 changes; the times are from #249 and the `ww500_minimal` bench work.

| # | Change in `ww500_md` | Where | Effect |
|---|---|---|---|
| 1 | `IMX708_POWERUP_DELAY` from 100 to 10 ms | `cis_sensor/cis_imx708/cisdp_cfg.h` | About 90 ms less in the early camera check and 90 ms less in the camera start-up (RP3 builds) |
| 2 | Sensor I2C at 400 kHz by default in the RP builds, slowed to 100 kHz for each HM0360 access | `ww500_md.c` (100 kHz kept only in the HM0360 build), `hm0360_md.c` (switching) | About 73 ms less in the camera start-up. The HM0360 needs 100 kHz once in motion detection (Himax) and shares the bus |
| 3 | RP3 driver progress messages compiled out, failure messages and `Initialising IMX708` kept (`CISDP_DBG_TYPE`) | `cis_sensor/cis_imx708/cisdp_sensor.c` | About 6 ms less in the camera start-up |
| 4 | `FreeRTOS.h` first among the includes | The ten files listed in the `ww500_minimal` note | None expected; from experience |

Together 1-3 should take the RP3 power-up at each wake from about 320 ms to about 60 ms.

**Not transferred:** making SENSOR_ENABLE (PB7) an output, low, in every build, and the LED pin changes. Those pin
assignments were for the `ww500_minimal` board only. In `ww500_md` the GPIO pin assignments stay as they are (PB7 and the
blue LED on PB10 share GPIO1). Also not transferred for now: the same `dbg_printf()` filter in the other drivers.

**How the I2C speed now works (item 2).** `platform_driver_init()` (board code, shared by all apps, unchanged) already
sets the sensor I2C bus to 400 kHz. `ww500_md.c` now slows it to 100 kHz only in the HM0360 build, where the HM0360 is the
main camera; the second, redundant setting in `checkForCameras()` was removed. In the RP builds the outermost
`saveMainCameraConfig()` in `hm0360_md.c` slows the bus to 100 kHz and the outermost `restoreMainCameraConfig()` puts it
back to 400 kHz, together with the I2C address (`hx_drv_i2cm_set_speed()`). `hm0360_md_isSensorPresent()` does the same
around its probe of the HM0360. So returning to the RP3 after an HM0360 access restores 400 kHz automatically. The switch
is chosen at compile time (`SWITCH_I2C_SPEED_FOR_HM0360`), as `hm0360MainCamera` is still false early in the boot even in
the HM0360 build.

**Status (30 September 2026):** items 1 to 4 made on branch `260930_appCommsBugs`, built, and run on the bench with the
RP3 build on motion wakes, with two SD cards. The early camera check fell from about 110 ms to 16 ms and the RP3
start-up to about 60 ms, so the #249 aim is met; the measurements are in
[CLAUDE_Time_to_first_photo.md](CLAUDE_Time_to_first_photo.md). The HM0360 build and the other checks below have not
been reported yet.

**Time to the first photo** after a motion wake (how it is measured, the results, and where the time now goes): see
[CLAUDE_Time_to_first_photo.md](CLAUDE_Time_to_first_photo.md). The main remaining delay is now the SD card start-up,
which the camera waits for because the operational parameters are on the card. Reducing it means restructuring how
state is saved across DPD, which will be done as a separate piece of work: [issue #251](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/251), "Restructure the way state is
saved across DPD to decrease time to the first photo".

**Checks:** build both camera variants; with the RP3, a photo after a cold boot and after a wake, the console power-up
times against #249's figures, and a motion detection wake (the HM0360 still works after the 400 kHz traffic).

## What was changed on `260930_appCommsBugs`

Code, all in `EPII_CM55M_APP_S/app/ww_projects/ww500_md`:

| Change | Files | For |
|---|---|---|
| Nesting-safe save/restore of the camera I2C address (`idSaveDepth`) | `hm0360_md.c` | #238, PR #239 (only this part taken) |
| `IMX708_POWERUP_DELAY` 100 to 10 ms | `cis_sensor/cis_imx708/cisdp_cfg.h` | #249 |
| Sensor I2C at 400 kHz in the RP builds, 100 kHz for each HM0360 access (and throughout the HM0360 build) | `ww500_md.c`, `hm0360_md.c` | #249 |
| RP3 driver progress messages compiled out (`CISDP_DBG_TYPE`), failures and `Initialising IMX708` kept | `cis_sensor/cis_imx708/cisdp_sensor.c` | #249 |
| `FreeRTOS.h` first among the includes | `cis_file.c`, `cis_sensor/cis_hm0360/`, `cis_imx219/` and `cis_imx708/cisdp_sensor.c`, `fatfs_task.c`, `freertos_app.c`, `if_task.c`, `image_task.c`, `img_correct.c`, `timer_task.c` | From `ww500_minimal` |
| Boot timing to the first frame (`Boot timing` console line, `BOOT_TIMING_ENABLED`) | new `boot_timing.c/.h`; marks in `ww500_md.c`, `fatfs_task.c`, `image_task.c` | Measuring #249 and #251 |

Documentation:

- This README, and [CLAUDE_Time_to_first_photo.md](CLAUDE_Time_to_first_photo.md) (new).
- `ww500_minimal/doc/README.md`: the status of its "Changes to transfer to ww500_md" list.
- `.agents/skills/references/hardware-traps.md`: the cold-boot IMX708 trap now says it was fixed (#238), and two new
  entries: HM0360 accesses in the RP builds go through `hm0360_md.c`'s save/restore (I2C address and speed); GPIO0/1/2
  each appear on two pins.

Tested here but made earlier (merged to `dev` with PR #240): the unresponsive BLE processor handling and the large file
transfer fixes (sections above).

**Next:** [issue #251](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/251) (state across DPD, time to the first photo), with the analysis and ideas in
[CLAUDE_Time_to_first_photo.md](CLAUDE_Time_to_first_photo.md).
