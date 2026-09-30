# Time to the first photo after a motion wake

#### File: CLAUDE_Time_to_first_photo.md
#### Author: Claude (Opus 5.5), reviewed by Charles Palmer
#### 30 September 2026

How long the WW500 takes, after the HM0360 detects motion, to take its first photo with the RP3, how it is measured,
and where the time goes. This document records the measurements; the changes made on the branch are listed in
[README.md](README.md), "What was changed on `260930_appCommsBugs`". Reducing the time further is deferred to
[issue #251](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/251), "Restructure the way state is saved across DPD to decrease time to the first photo": the ideas
below are the starting point for it. It follows the #249 changes (see [README.md](README.md), "Transfer improvements from
`ww500_minimal`", and
[issue #249](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/249)). Branch `260930_appCommsBugs`,
RP3 build of `ww500_md`.

## What happens on a motion wake

1. The HM0360, in motion detection while the processor is in DPD, sees motion and raises **SEN_INT (active high)**,
   which wakes the processor through PA0.
2. Boot ROM and bootloader load the application (not visible to the application).
3. `app_main()`: wake reason, early camera check (`checkForCameras()`: SENSOR_ENABLE on, `IMX708_POWERUP_DELAY`, I2C
   probe), task creation, then the FreeRTOS scheduler starts.
4. FatFS task: SD card start-up (mount, `/MANIFEST`, `CONFIG.TXT`, image directory, boot counters, camera register file).
   **The image task waits for all of this** (`xSDInitDoneSemaphore`, `image_task.c`) before it touches the camera, because
   it needs the operational parameters.
5. Image task: RP3 power-up and register writes, data path, capture start.
6. First frame: `APP_MSG_IMAGETASK_FRAME_READY`. Only here is the HM0360 interrupt cleared (`image_task.c`), so SEN_INT
   stays high from step 1 to here.

## How it is measured

Two measurements, used together:

- **Scope on SEN_INT.** Because the interrupt is cleared only at the first frame, the time SEN_INT is high is the whole
  time from motion detected to the first frame, including the boot ROM and bootloader. One thing to confirm on the scope:
  the warm boot also disables the motion interrupt early (`ww500_md.c`, `hm0360_md_disableInterrupt()`); if that
  released the pin, the pulse would end at the boot instead, far too short.
- **`Boot timing` console line** (`boot_timing.c/.h`, `BOOT_TIMING_ENABLED` in `boot_timing.h`), printed once per boot
  at the first frame, in ms since `app_main()` started, with the wake reason. Points: `cameras checked`,
  `scheduler started`, `SD mounted`, `CONFIG.TXT loaded`, `image dir ready`, `SD ready`, `camera ready`,
  `capture started`, `frame ready`. Before the scheduler starts the core's cycle counter (DWT CYCCNT) is used, as nothing
  sleeps then; after it the FreeRTOS tick count is added (1 ms resolution), because the cycle counter stops while the
  core sleeps in tickless idle. The console also prints `FatFs setup took N ms` (the FatFS task's own measure).

The scope time minus the `frame ready` figure is the time before `app_main()`: boot ROM and bootloader.

## Results

| | Before the #249 changes | With the #249 changes |
|---|---|---|
| SEN_INT high (scope), motion to first frame | 410 ms | not yet measured |
| `Boot timing`: first frame, ms after `app_main()` (motion wake) | not measured | 346 ms (first card), 315 ms (second card) |

With the #249 changes (Charles, 30 September 2026, motion wake):
`cameras checked 16, scheduler started 20, camera ready 270, capture started 296, frame ready 346` ms since `app_main()`,
and `FatFs setup took 179ms`. (The SD points had not been added yet.)

| Stage | Time | Notes |
|---|---|---|
| `app_main()` to cameras checked | 16 ms | Early camera check, was about 110 ms (#249: 100 to 117 ms) |
| To scheduler started | 4 ms | |
| **Scheduler to camera ready** | **250 ms** | **SD card start-up 179 ms**, RP3 start-up about 50 ms (was 208 to 219 ms in #249), about 20 ms other |
| Camera ready to capture started | 26 ms | |
| Capture started to frame ready | 50 ms | The frame itself |

So the #249 changes did what was expected for the camera, and **the SD card start-up is now the largest delay before
the first photo**.

**SD times depend on the card:** its speed, and what is on it, since directories are searched one entry at a time.
Compare runs with the same card and similar contents; a fuller card, or one with many image directories, may be slower.

### The SD card start-up, split (second run, same card)

`cameras checked 16, scheduler started 20, SD mounted 189, CONFIG.TXT loaded 196, image dir ready 201, SD ready 207,
camera ready 270, capture started 296, frame ready 346`.

| Stage | Time | What it is |
|---|---|---|
| Scheduler to SD mounted | 169 ms | See below |
| To CONFIG.TXT loaded | 7 ms | `load_configuration()` |
| To image dir ready | 5 ms | `dir_mgr_init_image_dir()` |
| To SD ready | 6 ms | Boot counters, inactivity set-up, camera register file |
| To camera ready | 63 ms | RP3 power-up and register writes, data path |

The mount (`fatFsInit()`, `f_mount()`, the driver in `middleware/fatfs/port/mmc_spi/mmc_we2_spi.c`):

- `vTaskDelay(10)` in `fatfs_task.c` before it ("TODO - experiment - do I need settling time for 3V3_WE?"): 10 ms.
- `SD_powerUpSeq()`: a fixed 10 ms wait, then 80 dummy clocks: 10 ms. (A second 10 ms wait nearby is inside `#if 0`.)
- `CMD0`, then `ACMD41` polled every 1 ms until the card has finished its own initialisation: about 140 ms. This is the
  card's internal start-up, different from card to card, and paid at every wake as the card is powered from 3V3_WE,
  which is off in DPD.
- Then the boot sector and FAT: a few ms.

So our own SD handling is about 25 to 45 ms of the roughly 190 ms; the rest is the card.

### A second SD card (Charles, 30 September 2026)

`cameras checked 16, scheduler started 20, SD mounted 170, CONFIG.TXT loaded 175, image dir ready 179, SD ready 181,
camera ready 239, capture started 266, frame ready 315`.

| Stage | First card | Second card | Difference |
|---|---|---|---|
| Scheduler to SD mounted | 169 ms | 150 ms | -19 |
| To CONFIG.TXT loaded | 7 | 5 | -2 |
| To image dir ready | 5 | 4 | -1 |
| To SD ready | 6 | 2 | -4 |
| To camera ready | 63 | 58 | -5 |
| To capture started | 26 | 27 | +1 |
| To frame ready | 50 | 49 | -1 |
| **Frame ready, from `app_main()`** | **346** | **315** | **-31** |

The second card is about 30 ms quicker, almost all in the mount (the card's own start-up, about 130 ms against about
150 ms once the two fixed 10 ms waits are taken off). The camera and capture stages barely change, as expected. So on
these two cards the SD start-up is 150 to 190 ms of the time to the first photo, and overlapping the camera with it would
help both by about the same amount.

**Busy-waits:** the driver's `DELAY()` is `hx_drv_timer_cm55x_delay_ms(..., TIMER_STATE_DC)`, a busy-wait, and the FatFS
task has a higher priority than the image task. So during the card's start-up the image task cannot run at all, which
matters for overlapping the two.

## Other findings

### Which operational parameters affect the first photo

Checked against where `image_task.c` reads them on a motion wake (RP3 build).

| Group | Ops | Effect on the first photo |
|---|---|---|
| Whether there is a motion wake and a photo | 11 `MD_INTERVAL`, 17 `MD_SENSITIVITY`, 10 `CAMERA_ENABLED` | 11 = 0 or 17 = 0: no motion wakes (17 = 0 sets `MD_LIGHT_COEF` to 0). 10 = 0: no camera start-up, no photo. Op 11 also adds up to one interval before SEN_INT goes high |
| Time before the first frame | 14 `MODEL_PROJECT`, 15 `MODEL_VERSION` | The NN is initialised (`cv_init()`) between `camera ready` and `capture started`: most of that 26 ms stage (console: `NN Initialisation took N ms`). The code has a TODO to do it after the picture. 14 = 0 disables the NN |
| | 13 `FLASH_LED`, 34 `FLASH_MODE`, 9 `LED_BRIGHTNESS_PERCENT`, 12 `FLASH_DURATION`, 35/36 time-of-day window | Whether and how the flash is armed for the capture (PCA9574 writes before it) |
| | 23 `AE_DARK_THRESHOLD`, 25 `AE_FLASH_STATE` | With flash mode 1 (AE), the stored light decision (op 25) sets the flash for the first capture |
| How the photo looks | 29 `CAM_AE_ENABLE`, 30 `CAM_AE_TARGET` | RP3 auto-exposure. See below: the first frame is always at the table exposure |
| | 31 `CAM_WB_MODE`, 27/28 WB gains | Software white balance after the frame, before the JPEG is saved: affects the time to the file, not to the frame |
| After the first frame | 5 `NUM_PICTURES`, 6 `PICTURE_INTERVAL`, 16 `MODEL_THRESHOLD`, 18 `TEST_MODE_BITS`, 21/22 MD illumination | Later photos, the NN result, test options, lighting for the next DPD |

Ops 7, 24 and 26 change what a timer wake does, not a motion wake. Ops 0 to 4, 19 and 20 are counters, and op 25 is
state: they change during use (ops 3 or 4 at every boot), which matters for where they could be stored (below).

### The RP3's first frame is always at the table exposure

`ae_notifySensorInit()` (`ae.c`) resets auto-exposure to the register table's values (`AE_EXPOSURE_DEFAULT` 0x0940
lines, gain 1x) after every full sensor initialisation, which the RP3 gets at every wake. So the first photo after each
wake is taken at a fixed exposure whatever the light, and only the later frames are corrected. A long exposure also
lengthens the frame (`AE_EXPOSURE_MAX` 5000 lines is noted as where "frame time grows"). Remembering the last settled
exposure and gain across DPD would improve the first photo as well as its timing.

### The NN start-up is in the path to the first frame

`cv_init()` runs after the camera is ready and before the capture starts (the 26 ms `camera ready` to
`capture started` stage). Moving it after the capture start, as its TODO suggests, would take about 25 ms off the time
to the first frame, independently of the SD card.

## Idea: a fast-start record in the XIP flash

(Charles's idea, 30 September 2026; to be taken up under [issue #251](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/251).)

**The problem it solves.** The image task cannot start the camera, or know how to take the first photo, until
`CONFIG.TXT` has been read, and that waits for the SD card's own start-up (150 to 190 ms on the two cards tried, mostly
the card). The XIP flash is on the board, powered, and readable within a few ms of `app_main()`. A small copy in the flash
of what the first photo needs would let the camera start, and the first photo be taken, without waiting for the SD card.
The frame stays in RAM until the card is ready to save it.

**What it is not.** Not a replacement for `CONFIG.TXT`, which stays the master copy of the operational parameters (and
what the app, the CLI and people editing the card use). The flash record is a cache of it.

### What to store

Only what the first photo needs, and nothing that changes at every wake:

- **Settings:** op 10 (camera enabled), the flash ops (9, 12, 13, 34, 35, 36), the AE ops (23, 29, 30), the NN ops (14,
  15), the capture ops (5, 6). Perhaps also 11 and 17, to check them early. These change only when someone changes them
  (`setop`, the app, editing `CONFIG.TXT`).
- **State that helps the first photo:** op 25 (the last AE flash decision), and the RP3's last settled exposure and gain
  (not in `CONFIG.TXT` today). These change with the light, not at every wake.
- **Not:** the counters (ops 0 to 4, 19, 20, 1, 2). They change at every wake and are not needed before the first photo.
  They stay on the SD card only.

As a fixed-size record, for example: a magic number, a format version, the firmware variant (HM0360 or RP3, as the two
images could share the flash), a sequence number, the fields above, and a CRC. Well under 128 bytes.

### Where

The reserved, unused area **0x00F00000 to 0x00FEFFFF** (`xip_manager.h`), for example two 4 KB sectors at 0x00F00000
and 0x00F01000. That is clear of the firmware slots (0x00000000 to 0x001FFFFF), the NN model area (0x00200000 to
0x00EFFFFF) and the slot selector (0x00FFF000), and far from the selector, where a mistake would stop the device booting.
The layout table in `xip_manager.h` and `.c` would gain the new region.

### Writing it without wearing the flash out

- The flash erases in 4 KB sectors (`FLASH_SECTOR` in `spi_eeprom_comm.h`); a NOR flash sector typically lasts about
  100,000 erases (to be checked in the chip's datasheet).
- **Append, do not rewrite:** program each new record into the next blank slot of the sector (programming only turns
  1s to 0s, so no erase is needed), and read the last valid one at boot. With 128-byte records a sector holds 32 of them,
  so it is erased once per 32 writes; two sectors used in turn protect against a power failure during an erase.
- **Write only when the record has changed**, at the start of DPD (the image task's `image_sleepNow()` or the FatFS
  task's save of state). Settings change rarely and the exposure only as the light changes. Even writing at every wake,
  with motion wakes every minute all day, two sectors would last many years.
- **Safely:** through `xip_manager.c`'s `disable_xip()` / `enable_xip()`, which hold `xSPIMutex` for the whole erase or
  program. The application runs from TCM and SRAM, not the flash; only NN model reads use XIP, and the NN is idle at the
  start of DPD. Programming a record takes well under 1 ms; an erase (once in 32 writes) tens of ms, both off the path to
  the first photo.

### Reading it and keeping it consistent with CONFIG.TXT

1. At a warm boot, before waiting for the SD card, read the newest valid record (CRC, magic, version and firmware
   variant all correct). The flash must be opened first (`init_flash()`, done lazily today): measure what that costs this
   early.
2. If there is a valid record, use it: start the camera, arm the flash as recorded, set the recorded exposure and gain,
   take the first photo. If not (first boot, new firmware, corrupt record), do what is done today and wait for the SD card.
3. When `CONFIG.TXT` has been read, compare it with the record. `CONFIG.TXT` wins: if it differs (the card was edited on
   a PC or changed, or a setting was changed over BLE after the record was written), use its values from then on and
   write a new record at the next DPD. If the first photo was taken with settings that turn out to be stale, the only
   effect is on that one photo (for example flash or no flash), except op 10: if the card says the camera is disabled,
   discard the photo.
4. Every change to a recorded setting (`fatfs_setOperationalParameter()`) marks the record as needing a new copy.
5. Cold boots can keep today's behaviour: they are rare, and a new card or new firmware is most likely then.

### What else it needs, and what it might save

- **The SD card start-up must let the image task run.** The SD driver's waits are busy-waits and the FatFS task has the
  higher priority (see "The SD card start-up, split"), so the waits during the card start-up must become `vTaskDelay()`
  once the scheduler runs. The camera (I2C) and the SD card (SPI) are separate peripherals, so they can work at the same
  time.
- **The NN start-up after the capture start** (above), or it sits in the path again.
- **The JPEG save waits for the card** as now; only the first frame is buffered, which is enough for the first photo.
- **Estimate:** with the capture no longer waiting for the SD card, the first frame could arrive at roughly the scheduler
  start (20 ms) plus the RP3 start-up (about 60 ms) plus the frame (about 50 ms): about 130 ms after `app_main()`,
  instead of 315 to 346 ms. That is an estimate, not a measurement. With the stored exposure and flash decision, the
  first photo should also be better exposed.
- **Checks before building it:** the flash chip's endurance and sector-erase time; that nothing writes into
  0x00F00000 to 0x00FEFFFF (the NN model loader, firmware updates); the cost of opening the flash early; and whether the
  two firmware variants should share one record or keep one each.

## Next: issue #251

This investigation stops here; [issue #251](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/251) will restructure how state is saved across DPD. What it starts from:

- **Measured:** the SD card start-up is 150 to 190 ms of the 315 to 346 ms from `app_main()` to the first frame, mostly
  the card's own start-up, and the camera waits for all of it because the operational parameters are on the card.
- **Ideas, most promising first:**
  - the fast-start record in the XIP flash (above), including the RP3's last exposure and gain, so the camera and the
    first photo need not wait for the SD card (estimate: first frame about 130 ms after `app_main()`);
  - letting the image task run during the card start-up (the SD driver's busy-waits to `vTaskDelay()`);
  - moving the NN start-up after the capture start (about 25 ms);
  - the two fixed 10 ms waits before and in the card power-up (up to about 15 ms).
- **Still to measure:** SEN_INT on the scope with the #249 build, for the motion-to-first-frame figure including the boot
  ROM and bootloader.
