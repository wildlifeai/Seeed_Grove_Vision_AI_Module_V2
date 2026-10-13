# Proposal: handling camera errors on the AI processor, step by step

#### File: `CLAUDE_camera_error_handling_proposal.md`
#### Path: `_Documentation\development reports\2026-10-12_cameraErrors\CLAUDE_camera_error_handling_proposal.md`
#### Author: Claude (Opus 5.5), for Charles Palmer
#### Date: 12 October 2026

## Summary

Eight steps, each small enough to build and test alone. Step 4 has since been merged into
step 2 (see "Agreed plan"). All the steps are coded and bench-tested (13 October 2026). The tests still to run are
listed in the README. Sources:
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
| 9 | Refuse captures before the first DPD (added 12 October) | low | the occasional missing first frame after a cold boot |

Step 9 was added on 12 October. What the app developers need to know is in "Implications for
the app", near the end.

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
| 7 | Done. Built and tested by Charles, 13 October (HM0360 or RP3 not recorded) |
| 8 | Experiment done. Real code built 13 October. Passed so far: normal use, `freeze`, awake for minutes, firmware update. Still to test: model load, file transfers, power, connector pressure |
| 9 | Done. Built and tested by Charles, 13 October |

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

**Status, 13 October 2026: built and tested by Charles.** `hardfault` gave the report, then
`>>> Reset by watchdog after fault`, the bootloader banner and a cold boot. So the watchdog can
be started from inside a fault handler. The stacked `PC` (`0x10003cb8`) is two bytes after the
stacked `LR`, which fits the forced instruction straight after the `xprintf()` call. One fix
followed the test: the old line "Another secure bus error" was printed for any fault, and is
now printed only for bus faults. `BFAR` and `MMFAR` are marked "(not valid)" when they hold no
address. That fix was built later the same day.

**Before.** `HardFault_Handler()` printed and looped for ever. So did the NMI, MemManage,
BusFault, SecureFault and UsageFault handlers. Nothing but RESET or a power cycle brought the
processor back (`b1_1712`).

In `b1_1712` the registers were `CFSR 0x00020000` and `HFSR 0x40000000`. That is a usage fault
(invalid state, usually a jump to a bad address), escalated to a hard fault. The handler only
decoded bus faults, so it printed "Another secure bus error", which was misleading.

**The change, in `hardfault_handler.c`.** Every handler now:
1. starts the watchdog, set to reset the processor 1 s later (`FAULT_RESET_DELAY_MS`)
2. prints its report: `CFSR`, `HFSR`, `BFAR`, `MMFAR`, the usage fault bits by name, and the
   stacked registers, including the `PC` and `LR` of the code that faulted
3. prints `>>> Reset by watchdog after fault` and waits

The watchdog is started first, so the reset still happens if printing the report faults again.
If the watchdog has not reset the processor after a few seconds, the handler tries a system
reset request as a last resort.

The watchdog is used because `NVIC_SystemReset()` is marked "does not work" in `image_task.c`.
The `reset` command uses the watchdog in the same way.

**A test command.** `hardfault` on the console forces a hard fault: an undefined instruction
with interrupts masked, so the usage fault escalates as it did in `b1_1712`. There is no reply,
as the processor resets.

**Consequences.**
- The reset is followed by a cold boot, and the RTC restarts at the 2024 epoch (a known,
  separate item). The BLE processor gets a fresh `Wake`.
- After the cold boot, captures are refused until the first sleep (step 9).
- While a fault persists, for example pressure on a connector, the board may reset repeatedly.
- The fault report is only on the console. Nothing records it for the app or the next boot.

**What to test,** on both variants:
- Type `hardfault`. Expect `Entering HardFault interrupt!`, `SCB->CFSR:0x00010000`
  (`UNDEFINSTR`), `SCB->HFSR:0x40000000`, the stacked registers, then
  `>>> Reset by watchdog after fault`, and a cold boot about 1 s later.
- The stacked `PC` should be an address inside `prvHardFault()` (check against the `.map` file).
- After the cold boot the board should behave normally and sleep.

**Worth an experiment, not done.** In `b1_1712` the fault came after about 70
`No Register EDM Timing Callback` lines, printed by the prebuilt library from inside an
interrupt. `sensordplib_set_edm_timing_inlib(0)` turns that detection off. I can't say it
caused the fault, and it can only be tested by pressing on a connector.

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

**The experiment (13 October 2026).** Three temporary console commands, since replaced (see
"The real code" below):
- `wdt <seconds>`: start the watchdog, to reset after that time unless fed. 0 stops it.
- `wdtfeed <seconds>`: feed it from a FreeRTOS timer at that interval. 0 stops feeding.
- `freeze`: mask interrupts and loop for ever, like a dead processor.

| Question | Test | Good result | Result |
|---|---|---|---|
| 1. Does a long period work? | `wdt 10`, then nothing | reset at 10 s | 13 Oct: yes, at twice the period. See the timings below |
| 2. Does it fire when the processor is stuck? | `wdt 10`, `wdtfeed 2`, wait a minute, `freeze` | no reset while fed, reset about 10 s after `freeze` | 13 Oct: passed. Fed every 2 s there was no reset for 67 s. The reset came 19.4 s after `freeze` ([`logs/test2.log`](logs/test2.log)) |
| 3. What happens across DPD? | `wdt 30`, `wdtfeed 2`, `dpd`, watch for 2 minutes, then wake | stays asleep, runs normally after the wake | 13 Oct: passed. Asleep 2 min 49 s with no reset (one was due at 60 s), then a normal BLE wake and a normal sleep ([`logs/test3.log`](logs/test3.log)) |
| 4. Do long operations trip it? | fed: captures on both cameras, a firmware update, a model load, `setutc`, a file transfer | no reset | |
| 5. Power | PPK2: awake with and without the watchdog, and DPD after it has been used | no measurable cost | |
| 6. The real freeze | fed, then pressure on the HM0360 connector during a capture | reset, then a normal boot once the pressure is off | |

Known already: the watchdog resets the processor from a fault handler with a 1 s period
(step 7).

**Timings, 13 October 2026 (Charles, from the terminal's timestamps).**

| Log | Command | Reply to bootloader banner | Ratio |
|---|---|---|---|
| [`logs/wdt4.log`](logs/wdt4.log) | `wdt 4`, board idle | 8.0 s | 2.0 |
| [`logs/wdt20.log`](logs/wdt20.log) | `wdt 20`, board idle | 39.8 s | 2.0 |
| [`logs/wdt10.log`](logs/wdt10.log) | `wdt 10`, then `freeze` 3.4 s later | 19.9 s | 2.0 |

- **The reset comes at twice the period,** every time. This is how the usual Arm watchdog works:
  the first timeout raises an interrupt, and the reset comes at the second if nothing clears it.
  (The first run, timed by hand, was thought to be 25 s. It will have been 20 s.)
- **It counts at the same rate whether the processor is idle or running flat out.** So it does
  not stop while the processor sleeps in idle.
- **It resets a processor stuck with interrupts masked.**
- **Feeding works.** `hx_drv_watchdog_update()` with the full period restarts the count
  (`logs/test2.log`). The board stayed awake for the whole 67 s while it was being fed.
- **DPD stops the watchdog.** A watchdog left running does not reset the board out of sleep
  (`logs/test3.log`). Not yet shown: that it stays stopped if the board is then awake for longer
  than the old period. In the test the board slept again 4 s after the wake.
- **`dpd` still works while the watchdog is being fed.**

**Design for the real code (13 October 2026, agreed by Charles and then coded, see below).**
- A small module, `watchdog.c`: start, feed, stop.
- Started when the tasks start, at every boot. A period of 10 s, so the reset comes 20 s after
  the last feed.
- Fed every 4 s from a FreeRTOS timer. The inactivity detector is changed to ignore the timer
  task, so feeding does not keep the board awake.
- Stopped just before DPD as well, although the test shows DPD stops it. The cost is one line.
- **Suspended while the flash is being written.** A firmware update or model load turns off
  execute-in-place for the flash, and it is not yet clear that the timer can run then. A reset
  in the middle of a firmware write could leave the board unable to boot. So the watchdog is
  stopped before those operations and restarted after. This is the main risk in step 8 and
  needs the `xip_manager.c` code to be read with care first.
- Behind a build flag at first, so it can be left out while it is being proved.

**The real code, 13 October 2026: built and bench-tested by Charles (see "Bench" below).** Written as designed above,
after reading `inactivity.c`, the FreeRTOS settings and `xip_manager.c`.

- **`watchdog.c`, `watchdog.h`** (new): `watchdog_start()`, `watchdog_stop()`,
  `watchdog_suspend()`, `watchdog_resume()`, `watchdog_isRunning()`. Period 10 s, so the reset
  comes 20 s after the last feed. Fed every 4 s from a FreeRTOS timer. `#define AWAKE_WATCHDOG`
  in `watchdog.h` is the build flag: comment it out and the functions do nothing.
- **`ww500_md.c`**: `watchdog_start()` just before the scheduler starts, at every boot.
- **`image_task.c`**: `watchdog_stop()` at the top of `image_sleepNow()`. That is before DPD, and
  before the `reset` command's own use of the hardware watchdog.
- **`inactivity.c`**: the detector no longer counts the FreeRTOS timer task as activity, so
  feeding does not keep the board awake. A timer callback that starts real work sends a message
  to one of our tasks, and that task still counts.
- **`xip_manager.c`**: every flash erase and write goes through `disable_xip()` and
  `enable_xip()`. The watchdog is suspended in the first and resumed in the second, on every
  exit path.
- **`CLI-commands.c`**: the three experiment commands are replaced by `wdt <0|1>` (stop or start
  the watchdog until the next boot) and `freeze` (kept, as the test).

**What reading the code showed.**
- The FreeRTOS timer task runs at priority 2. The CLI task is 4 and the IF task 3. So code in
  those two tasks that runs for more than 20 s without blocking will stop the feeds and cause a
  reset. That is a hang the watchdog should catch, but any legitimate long operation there must
  block now and then, or suspend the watchdog.
- The image task is priority 1, below the timer task. A loop in the image task does not stop the
  feeds and is not caught.
- The application runs from RAM, not from the flash, so the feed timer could probably run during
  a flash write. The watchdog is suspended anyway.
- **A debugger that halts the processor will be reset after 20 s.** Use `wdt 0` first, or build
  without `AWAKE_WATCHDOG`.

**Bench, 13 October 2026 (Charles, first build).**
- Test 1, normal use: passed. The board still sleeps by itself.
- Test 2, `freeze`: reset about 20 s later.
- Test 3, awake for several minutes: no reset.
- Test 4, `wdt 0` then `freeze`: no reset, RESET needed.
- **Test 5, firmware update from the app: the update succeeded, then the processor hung and the
  watchdog reset it.** The image was erased, written, verified and the slot selector written.
  The console stopped part way through sending the reply `Firmware update OK`, and after a
  delay the board cold-booted, into the new image.

**Cause of the hang: this change.** `watchdog_suspend()` and `watchdog_resume()` stopped and
restarted the feed timer, and each of those puts a command in the FreeRTOS timer queue, which
holds only 5 (`configTIMER_QUEUE_LENGTH`). A firmware update opens several flash windows in a
row from the CLI task (priority 4). When it finished, the IF task (priority 3) ran before the
timer task (priority 2) could empty the queue. The IF task's `xTimerChangePeriod()` for its
"missing master" timer then failed, and the `configASSERT(0)` after it (in
`i2ccomm_write_enable()`, `if_task.c`) hung the processor. That is exactly where the console
stopped. The watchdog then did its job and reset the board 20 s later.

**Fix, built and re-tested 13 October (Charles).** The feed timer is now started once and left
running. Stopping, suspending and resuming the watchdog only set a flag and call the hardware
driver, so they send no timer commands. The firmware update was repeated from the app: erase,
write, verify, the reply sent in full, a normal sleep 3 s later, and no reset.

**Still fragile, not changed.** The timer queue holds 5 commands, and that `configASSERT(0)` in
the IF task turns one full queue into a hang. Step 5's frame timer also sends a command each
time a frame is requested. Worth raising `configTIMER_QUEUE_LENGTH`, or replacing the assert
with a retry.

**What to test.**
1. Normal use on both variants: boot, captures, sleep by inactivity at the usual op 8, wake.
   The board must still go to sleep by itself. This checks the `inactivity.c` change.
2. `freeze`: a reset about 20 s later, at most 20 s after the last feed.
3. Awake for several minutes with no reset (raise op 8, or keep typing).
4. `wdt 0`, then `freeze`: no reset (press RESET). Then `wdt 1` on a fresh boot restores it.
5. A firmware update and a model load, with the flag on. The most important test.
6. `reset`, `switchslot` and `hardfault` still work.
7. A file transfer in each direction, and `setutc`.
8. Later: power (PPK2, `wdt 0` against `wdt 1`), and pressure on a connector.

**What it will and will not catch.** It catches a processor that has stopped: interrupts dead,
the scheduler dead, or stuck in an interrupt handler. It does not catch one task stuck while the
others run, because the timer task still feeds it. That needs each task to check in, and is a
later change if wanted.

**A design point found while writing the commands.** Each feed from a FreeRTOS timer runs the
timer task, and the inactivity detector counts any task as activity. So while the feed interval
is shorter than the inactivity period (op 8), the board will not go to sleep by itself. `dpd`
still works. The real code must deal with this: feed from the idle hook, or have the detector
ignore the timer task.

The matching work on the BLE processor is
[ww-hardware#69](https://github.com/wildlifeai/ww-hardware/issues/69).

## Step 9: refuse captures before the first DPD

**Status, 13 October 2026: done, built and tested by Charles.**

**Before.** A capture could be requested between a cold boot and the first DPD. Charles: that
was never intended. A cold boot is meant to set things up and then sleep quickly. The first
frame after a cold boot has sometimes not arrived (HM0360, 12 October), so refusing these
captures removes one variable.

**The change, in `CLI-commands.c`.**
- The CLI task keeps the wake reason it is given at start-up.
- `captureRefused()` is called by the `capture` and `light` commands. After a cold boot it
  writes the reply and nothing is sent to the image task.
- The reply, to the console and the app:
  ```
  Error: no captures until after the first sleep.
  ```
- Refusing in the command gives one clear reply, with no "About to capture" line first.
- `light` is refused too. It takes one frame on the same path.

**What counts as a cold boot.** Power-on, RESET, and also `reset`, `switchslot` and a firmware
update, which all end in a watchdog reset. A capture is refused after any of them until the
next sleep.

**Not affected.** Captures the firmware starts itself. They only happen on motion and timer
wakes, which are never cold boots.

**Tools that capture straight after a cold boot, and will now get the error.**
- `_Tools/ww500_ship_check.py`: the black and white photo (Charles: to be updated). Its colour
  photo already sleeps for 5 s first.
- `_Tools/live_view.py`: it can power-cycle the board and then capture.
- `_Tools/ae_stream.py` and `_Tools/ae_monitor.py`: only if run straight after a reset.

**To capture from a console with no app.** `setop 7 5`, `dpd`, wait for the timer wake, then
`setop 7 0` and the capture command.

**Documented** in `_Documentation/ble_commands.md` (note 7) and
`.agents/skills/references/hardware-traps.md`.

**What to test.**
- After a cold boot: `capture 1 1000` and `light` give the error and nothing else happens.
- After a wake: both work as before.
- After `switchslot` or `reset`: refused until the next sleep.

**How this was decided.** The first draft of this proposal argued against the step:
- the ship-check photo tool captures straight after RESET
- a cold-boot capture usually works on current code (`b1_1702`, 11 October, HM0360; and the RP3
  on 13 October, the first check since the #238 fix)
- the app can hit it in the few seconds after power-on

Charles decided on 12 October to refuse them anyway: there is no need for captures before the
first DPD, and the ship-check tool will be changed.

## Implications for the app

Things in this work that the app sees. Items 1 to 3 probably need app changes. The rest should
be checked.

**1. A new message when a capture fails.**
```
Capture failed after 1 of 3 images: EDM WDT2 timeout
```
- Sent in place of `Captured ...` when a capture sequence is abandoned. `Captured` is not sent.
- The first number is how many images were taken and saved before the failure.
- The text after the colon is the reason: a datapath event name, or `No frame`.
- Today the app waits for `Captured` and times out (2 September log: `Capture failed: TIMEOUT`
  after 30 s). It should treat this message as the end of the capture, and show the failure.

**2. The camera no longer retries a failed capture.**
- Before, the AI processor slept for 1 s, woke and captured again by itself, using the
  op-parameter picture count and interval. The app could then get a `Wake` and a `Captured` it
  had not asked for. That no longer happens.
- If a retry is wanted, the app must send the capture command again.

**3. A new error reply to `AI capture` and `AI light`.**
```
Error: no captures until after the first sleep.
```
- Returned between a cold boot and the first sleep: after power-on, RESET, `AI reset`,
  `AI switchslot` and a firmware update.
- The app should wait for the next `Sleep` and then send the command again (the command wakes
  the AI processor).
- The capture preview flow already waits for the device to sleep before it sends `AI capture`.
  The case to check is a capture sent soon after `AI switchslot`, which the camera selector
  uses.
- `AI light` sent soon after connecting to a newly powered camera gets the same reply.

**4. How long to wait for a capture.** A failure is reported:
- usually about 5 s after the frame was requested
- at worst, on the HM0360, after about twice (the capture interval plus 7 s), when no datapath
  event arrives at all and the one sensor restart does not help
- on the RP3, after at most the capture interval plus 7 s (there is no restart)

The app's timeout should be longer than this, so that it sees the failure message.

**5. `Sleep` can arrive before the capture has finished.** Not new, but more visible now. If a
capture takes longer than the inactivity period (op 8), the AI processor sends `Sleep` while
it is still capturing, and `Captured` or `Capture failed` afterwards. The app should not treat
`Sleep` as the end of a capture. This is finding C in the 3 September thread and is still open.

**6. "Unhandled event" messages.**
- For datapath events the text changed from `IMAGE task unhandled event 0x012a in 'Capturing'`
  to `IMAGE task unhandled event '<name>' (0x012a) in '<state>'`.
- Fewer are sent. Inactivity in a state that can't act on it, and datapath events left over
  from a capture that has ended, are no longer reported.
- These are diagnostics. This only matters if the app parses or displays them.

**7. Captures the app did not ask for.** A motion or timelapse capture that fails also sends
`Capture failed ...`. The app may receive it without having sent a command.

**For the BLE processor developers.** The BLE processor passes these strings through, so no
change is expected. Worth a check that nothing there looks for `Captured` to end a state.

## Decisions made

1. Step 2: keep the retry, with one attempt, on the HM0360 only.
2. Step 3: no automatic retry after a failed capture. The 1 s re-wake is removed.
3. Step 9: captures before the first DPD are refused.
4. Step 8: the awake watchdog, behind the `AWAKE_WATCHDOG` build flag, suspended during flash
   writes.
5. `image_task_state` is only set inside the `handleEventFor...()` functions.

The open items and their GitHub issues are in the [README](README.md).

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
