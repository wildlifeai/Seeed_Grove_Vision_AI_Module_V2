# Proposal: handling camera errors on the AI processor, step by step

#### File: `CLAUDE_camera_error_handling_proposal.md`
#### Path: `_Documentation\development reports\2026-10-12_cameraErrors\CLAUDE_camera_error_handling_proposal.md`
#### Author: Claude (Opus 5.5), for Charles Palmer
#### Date: 12 October 2026

## Summary

Eight steps, each small enough to build and test alone. Step 4 has since been merged into
step 2 (see "Agreed plan"). Steps 1, 2, 3, 5 and 6 are done. Sources:
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
| 3 | Done. Built and tested by Charles, 13 October |
| 5 | Done. Built and tested by Charles, 13 October (forced timeout on the HM0360, a real fault on the RP3) |
| 6 | Done. Built and tested by Charles, 13 October, on both variants |
| 7, 8 | Not yet agreed |
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
- **Give up** as before: stop the sensor, save state if there is an SD card, sleep. (As first
  coded this also set `g_wdt_event` for the 1 s re-wake. That was removed in step 3.)
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

**Status, 13 October 2026: done, built and tested by Charles.**

**Before.** When a capture was abandoned, the console said so but the app only saw `Sleep`, or
nothing. The give-up path set `g_wdt_event`, which makes the device re-wake after 1 s and try
again, with no limit. With a fault that persists it would wake every few seconds until the
battery is flat.

**The change, in `image_task.c`.**
- `captureFailed(reason)` holds the give-up code: tell the console and the app, and stop the
  sensor. The give-up branch of `frameFault()` calls it. Step 5 will
  too.
- State changes stay in the state handlers (Charles's convention). `frameFault()` returns true
  when the capture is abandoned, and `handleEventForCapturing()` then saves state or sleeps and
  sets `image_task_state`.
- The message to the app:
  ```
  Capture failed after 1 of 3 images: EDM WDT2 timeout
  ```
  The count is the frames that arrived. `Captured` is not sent.
- **No automatic retry (decided 13 October, Charles).** On a failure the AI processor reports
  the error and stops. It is for the app to ask again if it wants to. The 1 s re-wake
  (`g_wdt_event`, `>>> RETRY due to WDT event`) is removed.

**History of the 1 s re-wake.** Charles added it on 28 February 2026 (commit 7b73436d) as a
workaround for the missing first frame. Before July it ran at the first frame timeout, and from
July only after the in-place retries had failed. At the time, going through DPD was the only
way to restart the sensor. The in-place restart now does that job. Step 3 first limited the
re-wake to one in a row. The 13 October bench test showed what it does in practice: the retry
uses the op-parameter picture count and interval, not the original command, and the app gets a
`Captured` it did not ask for. So it was removed.

**Consequence.** A motion-triggered capture that fails in the field has no app to retry it. That
picture is lost, and the camera waits for the next trigger.

**Cross-repo.** The new string is documented in `_Documentation/ble_commands.md`. The app
ignores it until it is taught to show it. The wording should be agreed with the app developers
before it is relied on.

**What to test.** As with step 2, a failure is hard to cause on a healthy board. Normal captures
on both variants should be unchanged.

## Step 4: merged into step 2

The other datapath errors in Capturing are now frame faults. See step 2.

## Step 5: a software timeout on each frame

**Status, 13 October 2026: done, built and tested by Charles.**

**Before.** Capturing relied on the EDM watchdog (5 s) to notice a missing frame. In `b2_1735`
it never fired, and nothing happened for 44 s. The image task ignores inactivity in Capturing,
so on current code the board would probably stay awake indefinitely.

**The change.**
- A new event, `APP_MSG_IMAGETASK_FRAME_TIMEOUT` (`0x0a0f`), in `app_msg.h`.
- A one-shot FreeRTOS timer, `frameTimer`, in `image_task.c`. `frameTimerStart()` is called each
  time a frame is requested, and `frameTimerStop()` at Frame Ready and in `captureFailed()`.
- The period is the capture interval plus 7 s (`FRAME_TIMEOUT_MARGIN_MS`), so the EDM watchdog
  normally gets there first.
- On expiry, the event goes to the image task. In Capturing it is handled as a frame fault
  named "No frame": one sensor restart, then give up. This is one more than the first draft,
  which gave up at once. It keeps one path for every kind of frame fault.
- Each event carries an id that changes whenever the timer is started or stopped, so an event
  left over from an earlier frame is ignored.
- `handleEventForCapturing()` now has one "capture abandoned" block after its `switch`, used by
  both the datapath faults and this timeout. State changes stay in the handler.

**Console text,** when nothing arrives at all:
```
IMAGE Task received event 'Image Event Frame Timeout' (0x0a0f). Rx data = 0x00000003
>>>> Frame fault 'No frame' (0x0a0f) after 8003ms <<<<
>>>> Frame fault - restarting sensor, retry 1/1
```
If the restart doesn't help either, 8 s later: `giving up`, then
`Capture failed after 0 of 3 images: No frame`.

**How to test, including the give-up path of steps 2 and 3.** This can't be caused on a healthy
board as built. A temporary change does it, on the HM0360 image: set `FRAME_TIMEOUT_MARGIN_MS` to
10 and send `capture 1 0`. (Don't use a 0 ms interval on the RP3, it wedges the datapath.) The timer then expires before the first frame can arrive. Expect the restart,
a second expiry, "giving up", the `Capture failed` message on the console and in the app, then
a normal sleep with no re-wake. Put the margin back to 7000 afterwards.

**Bench, 13 October 2026 (Charles, HM0360).** `capture 3 3000` worked normally. The forced test
(margin 10 ms, `capture 1 0`) gave the first expiry and the restart, but the second expiry was
treated as a follower of the first and ignored, and then the frame arrived. Cause: the 500 ms
follower window is longer than the 10 ms test period. It could not happen with the real 7 s
margin, but the rule was wrong in principle, so our own timeout is now never a follower.
Re-tested the same day: the restart, the second expiry, "giving up", `Capture failed` sent to the
BLE processor, Save State, sleep. The 1 s re-wake then took a picture with the op parameters,
which led to its removal (see step 3).

Final test with the re-wake removed: the same sequence, then `Entering DPD` and no wake. The
only noise left is `IMAGE task unhandled event 'Image Event Inactivity' in 'Uninitialised'`,
sent to the app 3 s after giving up. That is step 6.

**Also check** that normal captures are unchanged on both variants, including a long interval
(for example `capture 3 3000`), where the timer must not expire early.

## Finding, 13 October 2026: the in-place restart cannot work on the RP3

**Bench (Charles, RP3 image, this branch, margin back at 7000).** `capture 1 0`, which is known
to wedge the RP3 datapath:

| Time | What happened |
|---|---|
| 0 | Capture starts. No frame |
| 3 s | Inactivity. The image task ignores it. The IF task sends `Sleep` to the app |
| 5.0 s | `0x011c`. Sensor restart, `retry 1/1` |
| | `Stopping sensor`, `Disabling RP SENSOR_ENABLE`, then the restart: three failed I2C writes to `0x0100` (`Fail (-60)`), `IMX708 on by app fail` |
| | `0x011b` follows, ignored |
| 8 s, 11 s | Inactivity twice more. `Sleep` sent to the app each time |
| 12.0 s | Our frame timer (step 5): `No frame`, `giving up`, `Capture failed after 0 of 1 images: No frame` |
| | Save State, then DPD at once |

**What it shows.**
- **Step 5 did its job on a real fault.** After the failed restart no datapath event came at
  all. Without the frame timer the board would have stayed awake in Capturing.
- **The restart cannot work on the RP cameras.** `CAMERA_CONFIG_STOP` takes `SENSOR_ENABLE` low
  (`rp_sensor_enable(false)`, in the code since November 2025, commit d50ac4cc). That powers
  the IMX708 down and loses its registers. `CAMERA_CONFIG_RUN` does not enable it or reload
  them, so the stream-on write gets no answer.
- **`CAMERA_CONFIG_RUN` reports success anyway.** The result of `cisdp_sensor_start()` is not
  checked, so `frameFault()` believes the restart worked and waits for a frame.
- **This is a second reason the old RP3 retries all failed,** besides #238. In the 4 September
  log the same sequence shows `IMX708 on by app done`: with the #238 fault the write went to
  the HM0360's address, which answered. So the retry has never been able to work on the RP3,
  and the evidence for it in Appendix A is HM0360 only.
- **The app is told `Sleep` three times** while the AI processor is still trying to capture.
  This is the existing IF task behaviour on inactivity (finding C in the 3 September thread).

**Options for the RP3 restart.**
1. Re-initialise fully: stop, then `CAMERA_CONFIG_INIT_COLD` (enables the sensor and reloads its
   registers), then run. Needs a check that repeating the init does not allocate its buffers
   again.
2. No in-place retry on the RP cameras: give up at the first fault. Simple, and honest about
   what we know.

In either case `CAMERA_CONFIG_RUN` should return false when the sensor does not start.

**Decided, 13 October 2026 (Charles): option 2 for now.** Built and tested:
`MAX_CAPTURE_RETRIES` is 0 in the RP builds and 1 in the HM0360 build. On the RP3 a frame fault
now prints `>>>> Frame fault - giving up` and goes straight to `captureFailed()`.

Not done: making `CAMERA_CONFIG_RUN` return false when the sensor does not start.
`cisdp_sensor_start()` returns nothing in both sensor drivers, so that needs a driver change.
With no retry on the RP cameras it matters less.

**Later the same day: the board or camera was faulty.** After the change above, `capture 3 1000`
and `capture 1 1000` on the RP3 also gave no frame, every time, after a DPD wake, with every I2C
write acknowledged. Raising `IMX708_POWERUP_DELAY` from 10 to 100 ms made no difference. A
second board with the same build and the 10 ms delay captures normally. So:
- the RP3 failures in this section came from a hardware fault on the first board or its camera,
  not from the 0 ms interval and not from this branch
- the 10 ms power-up delay is not the cause
- the finding about the restart still stands: it comes from the code (`SENSOR_ENABLE` is taken
  low and not raised again), and the failed I2C writes after the restart show it
- on the faulty board the new code did what it should: `Frame fault - giving up` at 5 s,
  `Capture failed after 0 of 1 images: EDM WDT2 timeout`, then sleep

**RP3 on the good board, 13 October (Charles), all passed:**
- `capture 3 1000` and `capture 3 3000` after a DPD wake
- `capture 3 1000` straight after a cold boot. This is the first RP3 cold-boot capture on record
  since the #238 fix, and it works
- `capture 2 10000`: the frame timer did not fire early

## Step 6: quieter repeated inactivity

**Status, 13 October 2026: done, built and tested by Charles.** The forced-failure test passed
on the HM0360: after `Capture failed` there is no "unhandled event" message, and the board
sleeps. The faulty RP3 board gave the expected sequence too: fault at 5 s, `giving up`,
`Capture failed`, the trailing `0x011b` ignored in Save State, sleep.

**Before.** The inactivity detector re-arms each time a task runs, and its own event makes the
tasks run. So while a task is busy it fires again after each period. Two effects:
- `dpd` sets the period to 1 ms, giving a flood (`b2_1729`: `Inactive for 1ms` 24 times).
- The image task, in a state that can't act on the event, reported
  `IMAGE task unhandled event 'Image Event Inactivity' in 'Uninitialised'` to the console and
  the app. On 13 October this reached the app straight after `Capture failed`.

**What was already fixed.** The first draft listed the IF task in I2C TX State. That was fixed
for issue #205: the IF task now defers the event and replays it when it is back in Idle.

**The change.**
- `inactivity.c`: at least 1 s between callbacks (`INACTIVITY_MIN_REPEAT_MS`). No normal period
  is shorter than that, so normal operation is unchanged. After `dpd`, a repeat comes once a
  second, not every few ms.
- `image_task.c`: `flagUnexpectedEvent()` ignores the inactivity event silently. It covers
  every state with no handler for it: Uninitialised, Save State, NN Processing, Updating NN.

The repeats themselves are kept. The image task does not defer the event, so a later repeat is
what lets it sleep once it is back in Init.

**Considered and not done: sending `Sleep` only once.** On 13 October the IF task sent `Sleep` to
the app three times while a capture was still running. Stopping the repeats looked easy, but
the BLE processor uses `Sleep` to know the AI processor is asleep. If a command arrives after
the first `Sleep`, a later `Sleep` is what tells it the AI processor has gone back to sleep.
Without it the BLE processor would send its next command with no wake pulse. The real fault is
that `Sleep` is sent while a capture is still running (finding C in the 3 September thread).
That needs a capture to count as activity, and is left for a separate change.

**What to test.**
- The forced-failure test from step 5, or the faulty board: after `Capture failed` the app
  should no longer get the "unhandled event" message.
- `dpd` with no SD card, or while the IF task is busy: `Inactive for 1ms` should appear about
  once a second, not in a burst, and the board should still sleep.
- Normal sleep after a capture, on both variants, at the usual op 8.

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
2. Step 3: decided, no automatic retry. The 1 s re-wake is removed.
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
