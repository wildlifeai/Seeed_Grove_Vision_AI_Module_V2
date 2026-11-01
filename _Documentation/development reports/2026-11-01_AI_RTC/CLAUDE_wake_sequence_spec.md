# Spec: the wake sequence and the BLE processor's time

#### File: `CLAUDE_wake_sequence_spec.md`
#### Path: `_Documentation\development reports\2026-11-01_AI_RTC\CLAUDE_wake_sequence_spec.md`
#### Author: Claude (Opus 5.5), for review by Charles Palmer
#### Date: 1 November 2026

The shared spec for both processors: the AI processor side is coded in this repo, the BLE processor
side in ww-hardware (`MokoTech/Workspace/WildlifeWatcher_1`, `aiStateMachine.c`, `ble_commands.c`,
`aiProcessor.h`). Background and bench results: [CLAUDE_BLE_time_proposal.md](CLAUDE_BLE_time_proposal.md).

## Why

1. **Photo times.** The AI processor's clock is wrong after some wakes (stale first read) and drifts
   2 to 4.6 %. The BLE processor's clock is right once the app or LoRaWAN has set it.
2. **Messages lost since PR #283.** The camera no longer waits for the SD card, so `NN-`, the HM0360 AE
   registers and the motion grid reach the BLE processor before the wake message. It is still in
   SLEEP and drops them (`UNHANDLED event NN- in SLEEP`): no LED flash, nothing to the app.
3. **`setutc` at wake** blocks the AI processor's interrupts for about 1.4 s during the capture, so the
   BLE processor only sends it when the clocks differ by more than 300 s.

## Today

AI processor: the wake message (`Wake|Timer|Motion <AI time>`) goes when every task has reached
`startupBarrier`, which the FatFS task reaches only after CONFIG.TXT (`if_task.c`,
`APP_MSG_IFTASK_FREERTOS_INIT`). Bench, 1 Nov: about 600 to 700 ms after boot, after `NN-` (200 to
360 ms) and after the photo is stamped.

BLE processor (`aiStateMachine.c`):

```
SLEEP --wake msg--> WOKEN --"selftest"--> SELFTEST --"selfTest <bits>"--> IDLE
                                              \--(clocks differ > 300 s) "setutc"--> SETUTC --"RTC set"--> IDLE
IDLE: queued app command --> PROCESSING --reply--> IDLE
```

- `NN+`/`NN-` and other messages from the AI processor: dropped in SLEEP (flagged) and WOKEN
  (ignored); handled in SELFTEST, SETUTC and IDLE.
- App commands that arrive while the AI processor sleeps are queued and sent on reaching IDLE.

## The new sequence

The BLE processor's order stays as it is: wake message, `selftest`, then `setutc`. The existing
`setutc` carries the time; no new command. Decided with Charles, 1 November 2026.

```
AI processor                                   BLE processor
boot
wake msg "Timer 2026-11-01T02:42:44Z"  ---->   SLEEP -> WOKEN, logs "differ by"
                                       <----   "selftest"
reply "selfTest <bits>"                ---->   SELFTEST
                                       <----   "setutc 2026-11-01T02:42:47Z"  (every wake, if it has the time)
reply "RTC set to ... (was out by N s)" --->   SETUTC -> IDLE, then queued app commands
NN-, AE regs, grid, "Captured ..."     ---->   handled in every awake state
```

From the 1 Nov logs `setutc` would reach the AI processor about 100 ms after the wake message, so
about 300 to 350 ms after boot once the wake message goes first. The first photo is ready to save at
about 650 ms.

### AI processor

1. **Send the wake message first**, as soon as the IF task starts, not at `startupBarrier`. It needs
   only the clock and the wake reason. On a cold boot keep the 1 s wait for the BLE processor. It
   stays the BLE probe (`BLE_PROBE_TIME`).
2. **`setutc` no longer writes the RTC.** Keep `setutc time - AI time` in RAM, with the tick count when
   it arrived, and reply at once. Keep the reply's `RTC set ` prefix (the BLE processor matches it,
   `Ai_Event_AiUtcReceived`), e.g. `RTC set to <time> (was out by N s)`. `getutc`, photo stamps and the
   file date (`get_fattime()`) use the AI clock plus this difference, so `getutc` shows the new time at
   once. The same applies to `setutc` from the app or the console.
3. **Write the RTC at sleep, rarely:** when it is more than a limit out (proposed 10 minutes: at 4.6 %
   about one write every 3.6 hours) or was never set (reads before 2025). The 1.4 s then comes after
   the photos. Photos do not depend on it; the AI clock is still used for the wake message, `rtc_cal.c`
   and the fallback below.
4. **Hold commands that change settings** (`setop`, `camreg` and the like) until the settings are
   final: CONFIG.TXT loaded, or the fast-start record when there is no card. Otherwise CONFIG.TXT
   overwrites them. `selftest`, `setutc`, `getutc` and `ver` run at once. The BLE processor waits up
   to 10 s for a reply (`AI_RESPONSE_TIMEOUT_MS`); the settings are final by about 300 ms.
5. **One time reference per wake.** Each photo's time = reference + ticks since it, at the frame's
   tick. Only the first photo may wait for the reference; later frames of a burst never wait. No
   buffering of more than one JPEG. The reference, in order:
   - the `setutc` time, if it arrives before the first photo is ready to save, or within a short wait
     (proposed 300 ms). When the BLE processor has no time it sends no `setutc`, so the first photo
     waits the full limit: only after the BLE processor's own restart, before the app connects;
   - else the AI processor's clock, once it has ticked after the wake (the stale-read guard);
   - else, if the clock was never set (2024), stamp it as now (decided 1 Nov; ask Victor later).

### BLE processor

1. **Send `setutc` every wake** when `iso_time_isSet()`: `mustSendUtc` no longer depends on
   `PERMITTEDTIMEERROR` (300 s), which then only affects the `differ by` log line. The order and the
   SETUTC state stay as they are.
2. **Handle messages from the AI processor in WOKEN** as in SELFTEST, SETUTC and IDLE: `NN+`/`NN-` flash
   the LED and go to the app, other messages go to the app. Also in SLEEP, where they can only follow a
   lost wake message.

## Compatibility

The two processors are always updated together (Charles, 1 Nov), so mixed builds are not designed for.

## Cross-repo

- No new command. `setutc` to the AI processor changes behaviour (the RTC is written at the next sleep,
  the time is used at once) and its reply text gains `(was out by N s)`: update
  `_Documentation/ble_commands.md`.
- The app sees the same messages as today (`Wake (Timer)`, `Error bits = ...`, `NN-`, "Captured ...");
  it receives `NN-` and the rest again on timer and motion wakes. No EXIF change. The EXIF time when
  nobody knows it is the one question for Victor.

## Tests

1. Wake message first; a `setop` queued in the app before a wake survives CONFIG.TXT.
2. `setutc` on every wake about 100 ms after the wake message; no 1.4 s block during the capture;
   `NN-` reaches the app; photo stamps match the BLE processor's time to 1 s, including the first photo
   after a long sleep.
3. BLE processor with no time (after its own restart, before the app): no `setutc`, the first photo
   waits the limit, fallback stamps.
4. A burst (`setop 5 5`, `setop 6 200`): only the first frame waits, and only when no reference is ready.
5. `getutc` right after `setutc` shows the new time; the RTC is written at sleep when out by more than
   the limit, and not otherwise.

## AI processor side: coded 1 November 2026 (not yet built)

| Spec item | Where |
|---|---|
| 1. Wake message first (warm boot) | `if_task.c`: queued to the front as the task starts (`wakeMessageQueued`) |
| 2. `setutc` keeps the time, no RTC write | `CLI-commands.c` `prvSetUtc()` -> `exif_utc_set_reference()`; reply `RTC set to <time> (was out by N s)` (no "out by" while the RTC may still read the time at sleep) |
| 3. RTC written at sleep if over 10 min out or before 2025 | `exif_utc_before_sleep()`, from `image_sleepNow()` after `rtc_cal_beforeSleep()`; prints `RTC was out by N s: set to ...` or `RTC out by N s: not set` |
| 4. Settings commands held | `CLI-commands.c` `waitForSettingsIfNeeded()`: all but `selftest`, `setutc`, `getutc`, `ver` wait for `fatfs_isSdReady()`; prints `Holding '<cmd>' until the settings are loaded` |
| 5. One time reference per wake | `exif_utc.c`: `exif_utc_wait_for_reference()` (first photo: up to 300 ms for `setutc`, then the RTC once it has ticked), `exif_utc_get_time_at_tick()` with the frame-ready tick (`image_task.c`); `getutc`, `get_fattime()`, the time-of-day flash and the "Wrote ... at" line use `exif_utc_get_time()` |

Left on the bare RTC: the wake message's time, the sleep alarm, `rtc_cal.c`, the image file name
(uniqueness only) and the console's `Woke at`.

## Bench, AI processor side, 1 November 2026 (logs/ai5.txt, ble5.txt)

HM0360 image, board WILD-CMSS, BLE processor unchanged (clocks 135 to 149 s apart, so no `setutc`).

1. **Wake message first: passed.** On all 7 wakes it went before `selfTest`, `NN-`, the AE registers and
   the grid. The BLE processor handled `NN-` in IDLE every time; no `UNHANDLED ... in SLEEP`.
2. **Held command: passed.** `setop 6 500` sent while asleep: `Holding 'setop 6 500' until the settings
   are loaded`, then `Set OpParam 6 = 500`. On the next wake `getop 6` (also held) replied 500, so
   CONFIG.TXT did not overwrite it. `ver` ran at once.
3. **Fallback stamp: passed.** Every motion wake printed `No setutc after 300ms`. On one wake the RTC was
   stale (wake message and `Woke at` 05:30:15, 12 s behind); the photo was stamped 05:30:28, correct.
4. **Cost of the fallback.** Photo queued at about 550 ms after boot (was 230 to 275 ms in ai4), 868 ms
   on the stale wake (300 ms, then the wait for the RTC's tick). Expected until the BLE processor sends
   `setutc` every wake: from ble5 it would arrive about 150 ms after the wake message, before the photo
   is ready.
5. **Not yet run:** `setutc` from the console (no RTC write, `getutc`, the write at sleep). Nothing
   prints at sleep without a `setutc`, as designed.

The wake message's time is still the bare RTC, so it can be stale; the BLE processor's `differ by`
line is then wrong by up to the sleep length. Harmless once it sends `setutc` every wake.

## Bench, `setutc`, 1 November 2026 (logs/ai6.txt, ble6.txt)

Same board and builds. All passed.

1. **Console `setutc`, no RTC write:** `setutc 2026-11-01T06:46:05Z` replied at once `RTC set to ...
   (was out by 3492s)`; `getutc` 4 s later gave 06:46:09. At sleep: `RTC was out by 3492s: set to
   2026-11-01T06:46:20Z (took 1352ms)`; the next wake read 06:46:36, right.
2. **Back again:** the BLE processor then saw 3687 s and sent `setutc`; at sleep `RTC was out by
   -3688s: set to ...`. Same for a day ahead (86367 s, then -86368 s).
3. **Under the limit:** a `setutc` 189 s from the RTC printed `RTC out by -189s: not set (limit 600s)`.
4. **`setutc` from the BLE processor during a motion wake:** it arrived 160 ms after the wake message
   (BLE log), the photo waited for it (queued at 343 ms) and was stamped with its time. `NN-` arrived
   in SETUTC and was handled; the `RTC set ` reply moved the BLE processor to IDLE as before.
5. **Reply text:** `setutc` from the BLE processor always comes within 1.1 s of boot, so its reply has
   no `(was out by N s)` (the RTC might still read the time at sleep). Only the console shows it.
6. **RTC write time:** 1346 to 1907 ms. It runs after the `Sleep` message, so the BLE processor
   believes the AI processor is asleep for that time, as during an `rtc_cal` measurement.
7. **Stale wake message:** after a 13 min sleep the wake message carried the time at sleep, so the BLE
   processor saw 644 s instead of about 190 s.

`rtc_cal.c` is not upset by the write: a move back makes the next measurement due
(`now < rtcMeasuredAt`), a move forward only makes it due sooner.

## Decided, 1 November 2026

The RTC write limit stays at 10 minutes (`RTC_WRITE_LIMIT_S` 600, Charles). The write only happens at
the end of a wake that had a `setutc`, so at most about 7 a day at 4.6 % drift, fewer if the camera
wakes less often.

The first photo's 300 ms wait for `setutc` stays counted from when the photo is ready (Charles): it only
matters when the BLE processor has no time or does not respond, both rare.
