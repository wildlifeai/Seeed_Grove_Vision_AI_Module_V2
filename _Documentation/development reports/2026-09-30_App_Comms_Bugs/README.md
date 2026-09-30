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

Checked the fixes 'Unresponsive BLE processor' and 'Large file transfer' are present and working. 
Fixed 'RP3 camera can't take a photo after cold boot'

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

We made the chnages to `hm0360_md.c` in thsi branch and I will add a comment to [PR#239](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/pull/239) :

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


