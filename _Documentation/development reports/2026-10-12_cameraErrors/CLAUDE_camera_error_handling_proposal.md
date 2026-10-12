# Proposal: handling camera errors on the AI processor, step by step

#### File: `CLAUDE_camera_error_handling_proposal.md`
#### Path: `_Documentation\development reports\2026-10-12_cameraErrors\CLAUDE_camera_error_handling_proposal.md`
#### Author: Claude (Opus 5.5), for Charles Palmer
#### Date: 12 October 2026

## Summary

Eight steps, each small enough to build and test alone. Step 4 has since been merged into
step 2 (see "Agreed plan"). Steps 1 and 2 are coded. Sources:
[`README.md`](README.md) in this folder, the
[connector-pressure report](../2026-10-11_camera_connector_pressure/README.md) and its logs, and
the code on `261012_cameraErrors`.

| Step | What | Risk | Fixes |
|---|---|---|---|
| 1 | Names for datapath events in the console | none | `'Unrecognised' (0x011c)` |
| 2 | One "frame fault" path with one retry, replacing `WDTIMOUTFIX` (now includes step 4) | low | the open question from July, and "stuck in Capturing" (`b2_1735`) |
| 3 | One "capture failed" path that tells the app | low | silent failures |
| 4 | Merged into step 2 | | |
| 5 | A software timeout on each frame. More urgent than first rated | low | 44 s with no event at all (`b2_1735`) |
| 6 | Quieter repeated inactivity | low | the `Inactive for 1ms` flood (`b2_1729`) |
| 7 | Hard fault handler resets the processor | medium | dead after a hard fault (`b1_1712`) |
| 8 | A watchdog while awake | high, experiment first | silent freezes (3 logs) |

Added on 12 October as step 9: refuse captures before the first DPD. See "Captures after a
cold boot" below.

**Limits.** Pressure on the connector also corrupts the console and stops the boot ROM, so it
disturbs more than the camera. Firmware can't prevent that. Steps 1 to 6 make the AI processor
behave sensibly when the datapath misbehaves but the processor still runs. Steps 7 and 8 are for
when it doesn't.

## Agreed plan (12 October 2026)

Charles reviewed the proposal and the discussion that followed. Agreed so far:

| Step | State |
|---|---|
| 1 | Done. Built and run by Charles |
| 2, with 4 | Done. Built by Charles, and seen to recover a real frame timeout |
| 3, 5, 6, 7, 8 | Not yet agreed |
| Captures after a cold boot | Decided: refuse them. Not yet coded (step 9) |

What changed from the first draft, and why:

- **The retry stays, with one attempt.** Only the first retry has ever worked.
- **`0x011c` and `0x011b` are not given separate paths.** We have never seen `0x011b` alone, and
  nothing shows they mean different things. The split is between the first event of a fault and
  the events that follow it.
- **Step 4 is merged into step 2.** `0x012a` and `0x0131` are WDMA2 and WDMA3, the same two
  channels as WDT2 and WDT3, so they are the same kind of fault and belong on the same path.
- **Step 5 matters more than first rated.** In `b2_1735` the board (August firmware) only slept
  because the IF task reported "ready to sleep" twice. That bug is fixed
  (`readyToSleepReported` in `if_task.c`), and the image task ignores inactivity while
  Capturing. So on current code a board with no frame and no datapath event would probably stay
  awake in Capturing indefinitely. This is from reading the code, not from a log.

## What the logs show

| Log | What happened | Step |
|---|---|---|
| `b1_1711`, `b1_1715` | `0x011c`, restart, `0x011b`, Frame Ready. Recovered 9 times | 2 |
| `b1_1712` | `0x011c`, restart, 6 more timeout events in 12 ms, quiet for 5 s, about 70 `No Register EDM Timing Callback Fun 0x20000` lines from the Himax library, hard fault | 7 |
| `b2_1735` | No event for 44 s (the EDM watchdog never fired), then `0x012a` and `0x0131`, unhandled. Stuck in Capturing for 2.5 minutes, then slept | 4, 5 |
| `b1_1716`, `b2_1734`, `b2_1740` | Output stops mid-capture. No reply to `selftest` 1 and 2 minutes later | 8 |
| `b2_1729` | HM0360 not found after RESET. `dpd` gives `Inactive for 1ms` 24 times | 6 |

Datapath events seen in the 14 logs: `0x011c` 13 times, `0x011b` 12, `0x012a` once, `0x0131`
once. Nothing else.

## Answers to questions in the README

- **The `\x00` lines (`b2_1735` line 401, `b2_1734` line 356).** A NUL byte arrived on the AI
  processor console with no text around it. The pressure-at-boot logs are full of the same
  bytes. I think it is electrical noise on the console line while the connector is pressed, not
  firmware output. Not proven.
- **`b1_1716`: what happened next.** The log does continue. `selftest` was typed 53 s after the
  freeze and got no echo, so the CLI task was not running. This is a dead processor, not a
  missing event.
- **RESET against power cycle (`b2_1729`).** They differ for the HM0360. It runs from the
  always-on 1V8 and 2V8 and has no reset line, so RESET never resets it. It did not answer at
  0x24, so a software reset over I2C is not possible either. This is hardware
  ([ww-hardware#70](https://github.com/wildlifeai/ww-hardware/issues/70)).
- **Timestamps.** The photo app log (`ww500_ship_check.py`) stamps only its own actions.
  `ribbon_bench.py` stamps every line. Stamping every console line in the photo app is a small
  tool change, separate from the firmware steps.

## Step 1: names for datapath events

**Status, 12 October 2026: done, built and run by Charles.** `dpEventString()` is in
`image_task.c` and names all 77 `APP_MSG_DPEVENT_*` events. The app message is now
`IMAGE task unhandled event '<name>' (0x....) in '<state>'`. Not checked: whether the app parses
that text. The app repo is not on this machine. No bench script in this repo depends on it.

**Now.** The image task prints a name only for its own events (`0x0a00` up). Everything from the
datapath prints as `'Unrecognised'` (`image_task.c:1877`), and `flagUnexpectedEvent()` sends a
bare hex code to the app.

**Change.** A function `dpEventString(event)` in `image_task.c`, used by both prints. One short
name for each `APP_MSG_DPEVENT_*` value in `app_msg.h`, taken from its comment. For the four
seen so far:

| Event | Proposed string |
|---|---|
| `0x011b` | `EDM WDT3 timeout` |
| `0x011c` | `EDM WDT2 timeout` |
| `0x012a` | `WDMA2 frame end count not reached` |
| `0x0131` | `WDMA3 frame end count not reached` |

Anything not in the table prints as `Unknown datapath event`. The hex code stays in every
message.

**Check first.** The "unhandled event" text goes to the app. I'll confirm the app doesn't parse
it.

## Step 2: one "frame fault" path with one retry

**Status, 12 October 2026: done, built and tested by Charles (see "Bench result").** This replaces the
`WDTIMOUTFIX` code and includes what was step 4. The evidence for keeping the retry is in
Appendix A.

**A frame fault** is any of these datapath events arriving in Capturing, when a frame is
expected:
- the four timeouts (`0x011b` to `0x011e`)
- the CDM FIFO errors (`0x011f`, `0x0120`)
- the WDMA and RDMA errors (`0x0121` to `0x013e`)
- the parser error (`0x0151`)

These are all the error events that `os_app_dplib_cb()` can send. It has no case for the EDM
timing errors (`0x0109` to `0x011a`), and it folds the nine parser errors into `0x0151`. Only
`0x011b`, `0x011c`, `0x012a` and `0x0131` have ever appeared in a saved log.

**The change, in `image_task.c`.**
- `isFrameFaultEvent()` says whether an event is one of the above.
- `frameFault()` handles one in Capturing. It replaces the timeout cases in
  `handleEventForCapturing()`.
- **First event and followers.** The first event of a fault is acted on. Any frame fault event
  in the next 500 ms (`FRAME_FAULT_FOLLOWER_MS`) is part of the same fault: it is printed and
  ignored. This replaces the guard that dropped events less than 1000 ms after `startTime`.
- **One retry.** `MAX_CAPTURE_RETRIES` is 1, with a fixed 100 ms delay
  (`CAPTURE_RETRY_DELAY_MS`). The count is per frame: it is cleared at Frame Ready.
- **Give up** as before: stop the sensor, set `g_wdt_event` (the 1 s re-wake), save state if
  there is an SD card, sleep.
- **Other states.** A frame fault event outside Capturing is a leftover from a capture that has
  ended. `flagUnexpectedEvent()` prints it on the console and no longer sends it to the app.
- The `#define WDTIMOUTFIX`, its four `#ifdef` pairs and the old `#else` branch are removed.

**Console text.**
```
>>>> Frame fault 'EDM WDT2 timeout' (0x011c) after 5073ms <<<<
>>>> Frame fault - restarting sensor, retry 1/1
>>>> 'EDM WDT3 timeout' (0x011b) follows the frame fault - ignored
```
On giving up: `>>>> Frame fault after 1 retries - giving up`. The words "restarting sensor" and
"giving up" are kept, because `ribbon_bench.py` looks for them. "Frame timed out" is gone.

**Not in this step.** Telling the app that the capture failed, and limiting the 1 s re-wake.
Those are step 3.

**What to test.** A frame timeout is hard to cause without pressing a connector. Things that
can be checked on any board:
- normal captures on both variants are unchanged
- `capture 3 500` after a cold boot still works on the HM0360

**Bench result, 12 October 2026 (Charles, HM0360 image, this branch).** `capture 3 1000` after
a cold boot, on an undisturbed board:

```
IMAGE Task received event 'EDM WDT2 timeout' (0x011c). Rx data = 0x00000000
>>>> Frame fault 'EDM WDT2 timeout' (0x011c) after 5026ms <<<<
>>>> Frame fault - restarting sensor, retry 1/1
IMAGE Task received event 'EDM WDT3 timeout' (0x011b). Rx data = 0x00000000
>>>> 'EDM WDT3 timeout' (0x011b) follows the frame fault - ignored
IMAGE Task received event 'Image Event Frame Ready' (0x0a04). Rx data = 0x00000000
Image capture 1/3 took 52ms
```

- The first frame never arrived, one restart recovered it, and captures 2 and 3 were normal.
- Charles: this happens sometimes after a cold boot, not always. Usually the same command works
  at once. He has seen it more than once.
- This is the first recovery seen without pressure on a connector, and the first HM0360
  cold-boot frame timeout on record. The #238 fix is in this build, so that fault is not the
  cause.
- Not yet known: whether it ever happens after a DPD wake on the same board.
- The "took 52ms" is measured from the restart. The real time was about 5.2 s.

**Known limit.** With the HM0360's own capture timer, the sensor keeps running while the image
task is in NN Processing (waiting for the disk write). A datapath error arriving then is printed
and ignored. The next frame then never arrives, and the EDM timeout, or step 5, has to catch it.

**The one log against the retry** is `b1_1712`, where the restart was followed by the hard
fault. The logs can't show whether the restart contributed. Step 7 covers the fault either way.

## Step 3: one "capture failed" path that tells the app

**Now.** When a capture is abandoned, the console says so but the app only sees `Sleep`, or
nothing. The give-up code is written inline in the timeout case.

**Change.** A function `captureFailed(reason)` in `image_task.c`, holding the existing give-up
code: stop the sensor, save state if there is an SD card, sleep. It also sends one line to the
app, for example `Capture failed: EDM WDT2 timeout`. Step 5 calls it, and so does the give-up branch of `frameFault()` from step 2.

**Also.** The give-up path sets `g_wdt_event`, which makes the device re-wake after 1 s and try
again. Nothing limits that. With a persistent fault it would wake every few seconds until the
battery is flat. I suggest a limit of 3 re-wakes in a row, then the normal sleep.

**Check first.** The new string goes to the app, so it is a cross-repo contract. Agree the
wording with the app developers.

## Step 4: merged into step 2

The other datapath errors in Capturing are now frame faults. See step 2.

## Step 5: a software timeout on each frame

**Now.** Capturing relies on the EDM watchdog (5 s) to notice a missing frame. In `b2_1735` it
never fired, and nothing happened for 44 s. The inactivity event is ignored in Capturing
("expect WDT timeout soon?").

**Change.** A FreeRTOS timer started when a frame is requested and stopped at Frame Ready. Its
period is the capture interval plus 7 s, so the EDM watchdog normally gets there first. On
expiry it sends an event to the image task, which calls `captureFailed("no frame")`.

## Step 6: quieter repeated inactivity

**Now.** `dpd` sets the inactivity period to 1 ms. The detector re-arms each time a task runs, so
while the IF task is still sending, it fires again and again. Each time, the console prints
`Inactive for 1ms`, and both tasks print "unhandled event" and send it to the app (`b2_1729`:
24 times).

The repeats themselves are needed. The IF task can't act on the event while it is transmitting,
and the next repeat is what gets it to sleep. So the events should not be inhibited.

**Change.**
- Print `Inactive for N ms` once per period of activity, not on every repeat.
- In states that can't act on the event (IF task in I2C TX State, image task in Uninitialised
  and Save State), ignore it without a print or a message to the app.

## Step 7: hard fault handler resets the processor

**Now.** `HardFault_Handler()` prints and loops for ever (`hardfault_handler.c:93-104`). The
reset is inside `#if 0`. NMI and MemManage loop too. Nothing but RESET or a power cycle brings
the processor back.

In `b1_1712` the registers were `CFSR 0x00020000` and `HFSR 0x40000000`. That is a usage fault
(invalid state, usually a jump to a bad address), escalated to a hard fault. The handler only
decodes bus faults, so it printed "Another secure bus error", which is misleading.

**Change.**
- Print `CFSR`, `HFSR`, and the stacked `PC` and `LR`, so the faulting code can be found.
- Then reset. `NVIC_SystemReset()` is marked "does not work" in `image_task.c`, and the working
  method there is a 100 ms watchdog reset (`hx_drv_watchdog_start()` with `WATCHDOG_RESET`). Use
  the same.
- The same for the NMI, MemManage, BusFault, UsageFault and SecureFault handlers.

**Consequences.** A watchdog reset boots as a cold boot and the RTC restarts at the 2024 epoch
(a known, separate item). The BLE processor gets a fresh `Wake`. While the pressure stays on,
the board may reset repeatedly.

**Test.** A temporary console command that jumps to a bad address, on both variants.

**Worth an experiment.** The fault came after about 70 `No Register EDM Timing Callback` lines,
printed by the prebuilt library from inside an interrupt. `sensordplib_set_edm_timing_inlib(0)`
turns that detection off. I can't say it caused the fault, and it can only be tested by pressing
on a connector.

## Step 8: a watchdog while awake

**Now.** The watchdog runs only to force a reset before sleep. Three of the six sustained-press
runs ended in a silent freeze that only RESET cleared.

**Change, in outline.** Start the hardware watchdog at boot with a period of 10 to 20 s, feed it
from a FreeRTOS timer, and stop it on the way into DPD. That catches a dead scheduler. It does
not catch one stuck task.

**Why it is last, and an experiment first.**
- Some code keeps interrupts off or runs for a long time: `hx_drv_rtc_set_time()` (1.4 s),
  flash erase during a firmware update, NN inference. The period must clear all of them.
- Tickless idle: the feed timer must wake the processor in time.
- It must never run into DPD, and the power cost while awake needs a PPK2 check.
- I don't know that a watchdog reset recovers these freezes. RESET did, once the pressure was
  off. A short experiment on a pressed board would tell us before the full change is written.

The matching work on the BLE processor is
[ww-hardware#69](https://github.com/wildlifeai/ww-hardware/issues/69).

## Captures after a cold boot

The README asks for captures before the first DPD to fail gracefully. Before doing that:

- **The ship-check photo tool depends on it.** `_Tools/ww500_ship_check.py` (PR #274) has the
  operator press RESET, then sends `capture 3 500`. RESET is a cold boot.
- **It usually works on current code, for the HM0360.** `b1_1702` (11 October, `dev` 6c8ee1ca):
  cold boot, `capture 3 500`, `Image capture 1/3 took 73ms`. But on 12 October Charles saw the
  first frame time out after a cold boot, more than once, recovered by the retry (step 2,
  "Bench result"). The failures in August and September
  were on the RP3 and were the #238 fault, fixed on 30 September. I found no RP3 cold-boot
  capture logged since the fix.
- **`reset` and `switchslot` also produce a cold boot,** because they are watchdog resets. So a
  capture would be refused after either, until the next sleep.
- **The app can hit it.** For a few seconds after power-on the AI processor is awake, and a
  capture sent then would be refused.

**Options.**
1. Leave it allowed, and add an RP3 cold-boot capture to the bench checks. My suggestion.
2. Refuse it, with a message to the console and the app, and change the ship-check tool to go
   through one DPD first (it already does this for the RP3 with `setop 7 5` and `dpd`).

**Decided, 12 October 2026 (Charles): option 2, as step 9.** There is no need for captures
before the first DPD, and refusing them removes one variable. The ship-check tool will be
updated so that it does not capture before the first DPD.

To be settled when step 9 is coded:
- the wording of the message to the console and the app
- whether `light` (a one-frame capture) is refused too
- that `reset` and `switchslot` are followed by a cold boot, so a capture is refused after
  either until the next sleep

## Decisions for Charles

1. Step 2: decided, see "Agreed plan".
2. Step 3: limit the 1 s re-wake to 3 in a row?
3. Captures after a cold boot: decided, option 2 (step 9).
4. Which of the remaining steps to do, and in what order? I suggest 5, 3, 6, then 7, with 8 as
   its own task after an experiment.

When the steps are agreed, the open ones should be linked to GitHub issues. Steps 4, 5, 7 and 8
are already inside
[#272](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/272).

## Appendix A: the `WDTIMOUTFIX` evidence

Moved here from the README. From the development reports and their saved logs.

**What the code does.** On a frame timeout (`0x011c` then `0x011b`, 5 s after the capture
starts) it stops and restarts the sensor and waits again, up to `MAX_CAPTURE_RETRIES` (5), with
a delay of 100 ms times the retry number before each restart. Without `WDTIMOUTFIX` the first
timeout abandons the capture sequence, and the device sleeps and re-wakes after 1 s.

**History.**
- July, PR #141 review: Charles saw all 3 retries fail and put the code behind `#ifdef WDTIMOUTFIX`
  for later removal.
- August: reproduced on the bench, verdict "park". PR #140 changed it to 5 retries with the
  growing delay. The question was never closed.
- 30 September: the cold-boot RP3 failure was the I2C address left on the HM0360 (#238, fixed in
  PR #252). No sensor restart could clear that.

**Every frame timeout in the saved logs:**

| Outcome | Count | Where |
|---|---|---|
| Recovered on retry 1, frame 120 to 190 ms after the restart | 9 | HM0360 connector pressure, 11 October (`b1_1711` 2, `b1_1715` 7) |
| Retry 1, then more timeout events and a hard fault | 1 | `b1_1712`, same bench (#272) |
| Retry 1, then put to sleep by the inactivity bug | 1 | HM0360 after a DPD wake, 2 September (finding C) |
| All retries failed, image task left Uninitialised | 2 | RP3 capture after cold boot, before the first DPD: 3 of 3 in August, 5 of 5 on 4 September (#238) |

**What this shows.**
- The retry does recover captures: 9 of 10 on the connector-pressure bench. All 9 are from one
  board, one afternoon and one cause.
- Only the first retry has ever worked. Retries 2 to 5 have only run in the two cold-boot
  sequences, and all failed.
- The growing delay has no supporting evidence. Five delayed retries failed just as three
  instant ones had.
- Each failed retry costs a 5 s timeout, so 5 retries keep the device awake about 30 s.
- The "does not work" verdict came from RP3 captures after a cold boot, and from a fault the
  retry could not fix.
- Frame timeouts also occur in normal operation (after DPD wakes, and mid-sequence).

**Frame Ready after a restart.**
- In `b1_1715` the 7 timeouts were consecutive frames (19 to 25 of 300). Each restart gave exactly
  one frame, then the next frame timed out 5 s later. The restart recovered each capture but did
  not clear the fault while the pressure was on.
- RP3 after cold boot: no Frame Ready after any of 8 restarts (#238).

**The `0x011c` and `0x011b` events.**
- `0x011c` (EDM WDT2) is followed by `0x011b` (EDM WDT3) at every retry in the saved logs.
  `0x011b` never appears alone or first.
- The two `0x011c` events without a "timeout event" line for `0x011b` are the "giving up"
  timeouts. The `0x011b` still arrived (4 September log, 60 ms later), but the code had left
  the Capturing state, and the Save State handler ignores it.
- `0x011c` arrives about 5 s after the capture starts. `0x011b` is printed as "after 1ms", but
  that is measured from the restart, which resets the clock. It is logged about 120 ms after the
  `0x011c`, straight after the restart messages. The logs can't show whether WDT3 fired with
  WDT2 and waited in the queue, or was raised by the restart.
- In `b1_1712` the pair came three more times in 12 ms after the restart, then stopped.
- `0x011d` (WDT1) and `0x011e` (sensor control) never appear in any saved log.
