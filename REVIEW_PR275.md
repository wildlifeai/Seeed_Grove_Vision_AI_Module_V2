# PR #275: Camera error handling: frame faults, Capture failed, fault reset, awake watchdog

#### File: `REVIEW_PR275.md`
#### Author: Claude, for Charles Palmer
#### Date: 13 October 2026

Proposed description for
[PR #275](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/pull/275), branch
`261012_cameraErrors` into `dev`. It replaces the first description, which was written before
the watchdog fix and the `configASSERT()` change.

---

Follows the connector-pressure report
([#272](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/272)).

- The record of the work: `_Documentation/development reports/2026-10-12_cameraErrors/`
- How the firmware behaves now: `EPII_CM55M_APP_S/app/ww_projects/ww500_md/doc/camera_error_handling.md`

## What changed

**When a capture goes wrong**
- Datapath events are named on the console, not shown as `'Unrecognised'`.
- `WDTIMOUTFIX` is removed. Timeouts and the other datapath errors share one "frame fault"
  path. The HM0360 build restarts the sensor once. The RP3 build does not retry.
- A failed capture sends `Capture failed after N of M images: <reason>` to the app, then sleeps.
  There is no automatic retry.
- A frame timer (the capture interval plus 7 s) catches a capture where nothing arrives at all.
- `capture` and `light` are refused between a cold boot and the first sleep.
- The inactivity detector fires at most once a second, and the image task no longer reports it
  to the app as an "unhandled event".

**When the processor stops**
- Every fault handler prints a report, with the stacked `PC` and `LR`, and resets the processor.
- A failed `configASSERT()` prints the file, line and task, and resets. Before, it looped for
  ever.
- A watchdog runs while the processor is awake (`watchdog.c`). If the processor stops, it is
  reset after 20 s. It is stopped for DPD and suspended during flash writes.

**Console commands for testing:** `hardfault`, `assert`, `freeze`, `wdt <0|1>`.

## Build note

**Do a clean build.** `FreeRTOSConfig.h` changed and `ww500_md.mk` adds `-DWW_ASSERT_RESETS`, so
the FreeRTOS kernel must be recompiled.

## Tested

Charles and Victor, 12 and 13 October, both camera variants:
- normal captures, sleep and wake
- a real frame timeout recovered by the restart (HM0360)
- 17 presses on the HM0360 connector (Victor, WILD-DKOB): 9 recovered by the restart, 3 ended in
  a clean `Capture failed`, 5 had no effect, none stuck
- `hardfault`, `assert`, `freeze`, `reset`, `switchslot`
- a firmware update from the app
- DPD current is unchanged

## Still to test

- A model load from the SD card, a file transfer in each direction, and `setutc`.
- That `ww500_minimal` still builds (it shares the FreeRTOS config header).
- A real freeze from pressure on a connector. None has happened since the watchdog went in.

## What else needs changing

- **The app.** See "Implications for the app" in
  `CLAUDE_camera_error_handling_proposal.md`: the new `Capture failed` message, no automatic
  retry, and the new error reply after a cold boot.
- **`_Tools/ww500_ship_check.py` and `_Tools/live_view.py`** capture straight after a cold boot
  and now get `Error: no captures until after the first sleep.`
- **A debugger that halts the processor** is reset by the watchdog after 20 s. Type `wdt 0`
  first.
