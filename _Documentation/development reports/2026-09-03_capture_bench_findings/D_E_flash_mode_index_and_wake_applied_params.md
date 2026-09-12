# Decide: the flash-mode op index, and whether setop should apply what is only read at wake

#### File: D_E_flash_mode_index_and_wake_applied_params.md
#### Author: Claude (Fable 5.1), reviewed by Victor Anton
#### 3 September 2026

**Repo:** Seeed_Grove_Vision_AI_Module_V2, `ae_review` at e8b7feb5. **Labels:** decision,
review-finding. Two questions that shape the app's Capture Picture flow, best settled together.

## 1. The flash-mode op parameter needs an agreed index

`flash_led_modes_proposal.md` (light sensor thread, on `ae_review`) adds `OP_PARAMETER_FLASH_MODE`
and the two time-of-day parameters "at indices 32+, exact numbers TBD" (the last index today is 31,
`fatfs_task.h:94`; `OP_PARAMETER_NUM_ENTRIES` at `:96` is 32).
The mobile app already binds two indices there from other branches: **32 = `CAM_RESOLUTION`**
(the hi-res colour mode) and **33 = `MD_BLOCK_NUM_MAX`**. The app reads the length of `getop -1`
to decide what the firmware supports (`firmware exposes 32 ops, no op32: hi-res unavailable`),
so a `FLASH_MODE` at 32 would be read as a resolution setting by the app the day it ships.

Options: start the flash parameters at 34 and reserve 32 and 33 for what the app expects; or
renumber the app once, with the cost that older app builds misread newer firmware. The first
is cheaper for everyone. Until the mode exists the app forces a flash by writing op25 = 1 before
the capture (`TODO(flash-mode-op)` in the app), which is a stand-in, not a design.

## 2. op8, op9, op13 and op10 apply at the next wake, not on setop

`setop` (`prvSetOpParam()`, `CLI-commands.c:1886`) updates the parameter in RAM and posts
`SAVE_CONFIG`, but the running configuration is built at wake: `setupLEDFlash()`
(`image_task.c:2047`, called from the wake path at `:1925` and `:1952`) reads op9 and op13 into
`ledFlashBrightness()` and `ledFlashSetFlashModeFromOpParam()`, the inactivity period comes from
op8 at boot (`fatfs_task.c:1616`, `Inactivity period set at Nms`), and op10 is read once when the
image task starts (`image_task.c:1581`). So a changed flash LED, brightness or sleep timer only takes effect after a sleep
and a wake, and the app pays 3 to 5 s per changed setting waiting for that sleep before each
capture.

Two ways out, either would do:

- **Apply on setop.** Have `prvSetOpParam()` call the same routines the wake path calls for the
  parameters that have one (`setupLEDFlash()` for 9 and 13, `inactivity_setPeriod()` for 8).
- **A sleep on demand.** `AI dpd` already exists (`prvDpd()`, `CLI-commands.c:815`, sets the
  period to 1 ms). If it is the supported way for the app to say "sleep now, then I will wake
  you", say so and the app will use it after every settings write and after `switchslot`. It
  worked on the bench at 19:35 (the device slept at once), but while #205 is open every use is a
  1 ms race between its own reply and the detector, and #207 means a `setop` sent just before it
  may not be saved, so the app will not lean on it until those are closed.

**Evidence:** the timings are in wildlifeai/ww-mobile-app,
`documentation/resources/Capture-Picture.md` (10.1 s tap-to-picture with nothing changed,
15.9 s with one setting changed). The op25 gate and its history are in `Light-Sensor.md` there.
