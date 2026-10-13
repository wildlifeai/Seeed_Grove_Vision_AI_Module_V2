# Some measures to improve resilience against camera errors

#### File: `README.md`
#### Path: `_Documentation\development reports\2026-10-12_cameraErrors\README.md`
#### Author: Charles Palmer
#### Date: 12 October 2026

## Status

Open. The code for all the steps is written, and bench-tested by Charles and Victor on 12 and
13 October. A few tests are still to run (see "Tests still to run"), and the open items below
need issues.

- Plan, evidence and bench results: [`CLAUDE_camera_error_handling_proposal.md`](CLAUDE_camera_error_handling_proposal.md)
- How the firmware behaves now: [`ww500_md/doc/camera_error_handling.md`](../../../EPII_CM55M_APP_S/app/ww_projects/ww500_md/doc/camera_error_handling.md)
- What the app developers need to know: the proposal, "Implications for the app"

## Outcome

- **Event names.** Datapath events print with a name, not `'Unrecognised'`.
- **`WDTIMOUTFIX` resolved.** The `#ifdef` is gone. Timeouts and the other datapath errors share
  one "frame fault" path. The HM0360 build restarts the sensor once, which recovered a real
  missing first frame on 12 October. The RP builds do not retry: stopping an RP camera powers
  it down, so the retry could never work there.
- **A failed capture is reported and nothing more.** The app gets
  `Capture failed after N of M images: <reason>`. The 1 s re-wake that used to follow a failure
  is removed. It is for the app to retry.
- **Our own frame timeout** (the capture interval plus 7 s) catches the case where no frame and
  no datapath event arrives.
- **Quieter inactivity.** At least 1 s between inactivity events, and the image task no longer
  reports them to the app as "unhandled".
- **Captures before the first DPD are refused.** `capture` and `light` reply
  `Error: no captures until after the first sleep.` after a cold boot.
- **Faults end in a reset.** Every fault handler prints a report and resets the processor with
  the watchdog. `hardfault` on the console forces one.
- **A watchdog runs while awake** (`watchdog.c`). If the processor stops, it is reset after 20 s.
  `freeze` on the console tests it. Not yet shown: that it rescues a real freeze caused by
  pressure on a connector.
- **A failed `configASSERT()` reports and resets.** It prints the file, line and task, then
  resets like a fault. Before, it looped for ever. `assert` on the console tests it.

## Tests still to run

Updated 13 October, evening. Done since the first list: `reset`, `switchslot`, `hardfault`,
`freeze`, `assert` and a firmware update from the app, on both camera variants (Charles and
Victor).

1. A model load from the SD card to flash: completes, no reset.
2. A file transfer in each direction, and `setutc`: no reset.
3. `ww500_minimal` still builds. It shares the FreeRTOS config header that was changed.
4. Power with the PPK2: `wdt 0` against `wdt 1`.
5. A real freeze: pressure on the HM0360 connector until the processor stops, to see whether the
   watchdog resets it. In Victor's 17 presses below, no freeze happened.
6. Victor's check script again on the later build, which has the fix for the two timing faults
   his first run found (see "Review of Victor's bench" in the proposal).

## Victor's bench, 13 October

Board 1 of the connector-pressure thread (WILD-DKOB), this branch at 187302ce built locally, the
HM0360 build active, SD card in. Logs in `logs/dkob_*`, with a time on every line.

**Tests 2, 3 and 4** (`dkob_1446_checks.log`, see its `##### CHECK` lines):

| Test | HM0360 | RP3 |
|---|---|---|
| Capture straight after a cold boot | refused (line 124) | refused (544) |
| 3, `hardfault` | report with PC and LR, reset 1.1 s later, no "secure bus error" (173-206) | the same (602-635) |
| 4, `freeze` straight after a boot | reset after **9.5 s** (236-255) | reset after 19.5 s (684-703) |
| 4, `freeze` mid-capture | reset after 16.8 s (`dkob_1449_freeze_midcapture.log`) | not run |
| 2, `reset` and `switchslot` | work (405, 529, 844) | work |

The 9.5 s reset is the only one well short of 20 s. That freeze came 0.4 s after the CLI prompt,
before the first feed.

**Test 7**, pressure during captures started by the timelapse wake (op5 300, op6 1000, op7 10,
op8 5000), 30 s presses timed by `ribbon_bench.py --press 30` from the connector-pressure thread:

| Result | Tries |
|---|---|
| Frame timeouts, each recovered by its one retry | 9 |
| `Capture failed after N of 300 images: EDM WDT2 timeout`, then DPD 5 s later and a new capture at the next wake | 3 |
| No effect | 5 |
| Stuck, frozen or hard fault | 0 |

- Every failure ended cleanly: the message reached the BLE processor (`nrf | BLE out: ...
  'Capture failed ...'`), the board slept, and no RESET was needed
  (`dkob_1506_press_capture_failed.log` lines 394-471, `dkob_1508_press_capture_failed_twice.log`
  1028-1105 and 1340-1912).
- Under sustained pressure each restart gives exactly one photo and the next times out 5 s later.
  The capture fails when a restart gives none (`dkob_1508` 1340-1850).
- `dkob_1452_press_recovers.log` 283-823: four timeouts in one press, all recovered.
- No 0x012a/0x0131 and no freeze happened, against 4 stuck in 11 tries on the same board on 12
  October. So the new handling of those events, and the watchdog on a real freeze (test 7), are
  not yet shown on a real failure.

Not run: tests 1, 5 and 6.

## Open items

- [#272](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/272): the AI
  processor does not recover from a camera datapath failure. Most of this work answers it.
  To be closed or narrowed once test 7 is done.
- [ww-hardware#69](https://github.com/wildlifeai/ww-hardware/issues/69): the BLE processor does
  not notice a dead AI processor.
- [ww-hardware#70](https://github.com/wildlifeai/ww-hardware/issues/70): the holder must not
  press on the connector.

Not yet filed as issues (Charles to file):

- The app: the seven items in "Implications for the app".
- `Sleep` is sent to the app while a capture is still running (finding C in the
  `2026-09-03_capture_bench_findings` thread).
- `_Tools/ww500_ship_check.py` and `_Tools/live_view.py` capture straight after a cold boot and
  now get the error.
- `CAMERA_CONFIG_RUN` reports success when the RP3 does not start.

## Background

The `2026-10-11_camera_connector_pressure` development report describes how some of Victor's boards 
give errors. Reasons unclear. Could be related to mechanical issues with HM0360 connector.

The log files indicate some events that are arriving instead of the expected Frame Ready event.

It would be good to handle these better. 

I made a quick inspection of some of the error logs (see below) and suggested some areas for further work.

Exploring options here is the job of this development task.

## My quick review of some logs

__b1_1712_capture_30s_press_hardfault__

It looks like the WW500 has been asked to take several photos (300?) the first 2 reported in this log seem OK and return 'IMAGE Task received event 'Image Event Frame Ready''. The third in this log gives error message at line 120. 

There is code inside the #ifdef WDTIMOUTFIX block that then attempts to stop and restart the HM0360 - writing to the HM0360 I2C registers clearly works OK 

But there are then a series of 6 more APP_MSG_DPEVENT_EDM_WDT2_TIMEOUT or APP_MSG_DPEVENT_EDM_WDT3_TIMEOUT events then many 'No Register EDM Timing Callback Fun 0x20000' messages then a hard fault (AI processor will stop).

My comments: 
1. I don't think the WDTIMOUTFIX is helpful and it should be removed.
2. We should add hardfault error handling.
3. I had seen APP_MSG_DPEVENT_EDM_WDT2_TIMEOUT errors before - we need these to be gracefully handled. Reason unknown.

__b2_1735_capture_30s_press_stuck__

Captures several images fine until line 432 when events APP_MSG_DPEVENT_XDMA_WDMA2_ABNORMAL1 and APP_MSG_DPEVENT_XDMA_WDMA3_ABNORMAL1 arrive instead of the expected frame ready). This is about 45s after requesting an image. (But what is '\x00  [no newline]' at line 401?)

Then the AI processor gets an inactivity event (after 60s) and sleeps. The HM0360 responds properly at around line 520. The next cold boot seems to wake OK and the HM0360 is fine. So the problem is unlikely to be the HM0360 module.

My comments: 
1. Consider the APP_MSG_DPEVENT_XDMA_WDMA2_ABNORMAL1 events and others that might arise and figure out a way to handle them.

__b1_1716_capture_30s_press_freeze__

At line 276 we have 'IMAGE Task state changed from 'NN Processing' (3) to 'Capturing' ' which should have triggered a frame ready, or possibly one of the timeout events - but nothing.

I am not sure what happens next - the log does not continue long enough for the 60s inactivity to trigger...

__b2_1734_capture_30s_press_freeze__

At line 340 we have 'IMAGE Task state changed from 'NN Processing' (3) to 'Capturing' ' which should have triggered a frame ready, Note this at line 356: '\x00\x00\x00\x00\x00  [no newline]'

__b1_1711_capture_5s_press__

Here we got an APP_MSG_DPEVENT_EDM_WDT2_TIMEOUT at line 267 then APP_MSG_DPEVENT_EDM_WDT3_TIMEOUT at 276 then a frame ready at 278. Then normal operation resumes!

__*_pressure_at_boot__

Several files with related content: text generated by the python script concerning boot failures, text of this format: ''\x00\x00\x00\x00\x00\x00\x''

Analysis difficult as there are no timestamps in the logs.

My comments: 
1. It looks like the script attempts to take photos after cold boot and before the first DPD - this was never the expected way of operating and will only ever happen if AI is driving the boards. 
2. Always collect timestamps in logs.

__b2_1729_tape_hm0360_latched__

Line 30 shows HM0360 not seen - indicative of a connector fault. 

Then we get this: Forcing DPD by clearing inactivity period
cmd> Inactive for 1ms

and then many ' Inactive for 1ms' messages.

This is repeated 3 reset cycles and then it comes good (HM0360 is seen). The comment in the markdown is 'Not present after three RESETs, present after a power cycle' -

My comments: 
1. There should be code that inhibits inactivity messages after the first one.
2. I guess it is possible that there is actually a difference between hitting the RESET button and power cycling.

## Reconsider the `WDTIMOUTFIX` code

See [`CLAUDE_camera_error_handling_proposal.md`](CLAUDE_camera_error_handling_proposal.md):
step 2 has the recommendation, and Appendix A has the evidence (moved from here by Claude,
12 October 2026).

In short: the retry recovered 9 of 10 HM0360 frame timeouts on 11 October, always on the first
retry. Every case where all retries failed was the I2C address fault (#238), fixed on
30 September.

## Provide code for error events and strings for them

Logs show messages like this: `IMAGE Task received event 'Unrecognised' (0x011c)`

Can we identify the errors that have shown up in the `2026-10-11_camera_connector_pressure`
logs and propose strings for them (instead of 'Unrecognised').

Identify events that have been seen which don't have code handling them. e.g. `APP_MSG_DPEVENT_XDMA_WDMA2_ABNORMAL1`
Propose code changes that explicitly include these.

## Prevent attempts at image capture after Cold Boot

My intention was that the cold boot would set up certain things and then the board would enter DPD quickly.

Some Claude experiments attempt image capture before the first DPD. 

Attempts to start image capture before the first DPD should be prevented (fail gracefully with error messages).

---

## Task for Claude

1.	Read this document and refer to the `2026-10-11_camera_connector_pressure` development report.
2.	Look at code where relevant.
3.	Create a markdown document that proposes dealing with issues raised above in a series of steps that we can carry out one at a time.
Place it in the same folder as this document.
4.	Don't write any code until after we have both reviewed that proposal document.
