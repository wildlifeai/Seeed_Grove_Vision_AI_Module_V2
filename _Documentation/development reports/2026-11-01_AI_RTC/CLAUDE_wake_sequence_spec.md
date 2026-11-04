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

## Bench, both processors, 2 November 2026 (logs/ai7.txt, ble7.txt)

HM0360 image, board WILD-CMSS, BLE processor coded in ww-hardware (`261101_AI_RTC`). Power-on, then
2 motion wakes, 2 BLE wakes (`setop 7 20`, `setop 7 0`) and 3 timer wakes.

1. **BLE processor with no time (power-on):** both clocks 2024, `Can't send UTC`, IDLE. LoRaWAN then
   joined and the BLE processor had the time.
2. **`setutc` on every wake: passed, 7 of 7.** Sent 110 to 130 ms after the wake message (BLE log).
   No `No setutc after 300ms`.
3. **Photo times: passed, 5 of 5** stamped with the `setutc` time, within 1 s of the PC clock.
   Photo queued at 342 to 344 ms after boot on motion wakes (the photo waited for `setutc`), 275 to
   278 ms on timer wakes (no wait).
4. **#152 in practice:** the first wake after the BLE processor had the time wrote the RTC from 2024
   (`RTC was out by 89508573s: set to 2026-11-01T23:30:50Z (took 1821ms)`). Afterwards `RTC out by
   -2s` to `-6s: not set` (4.5 % fast).
5. **Messages: passed.** `NN-` arrived in SETUTC on every photo wake and went to the app. No
   `UNHANDLED` or `Ignore this in`.
6. **Fault, AI processor (`sleep_mode.c`): an unwanted timer wake.** The timer sleep at 23:33:14 set an
   alarm for 23:33:35. A BLE wake at 23:33:31 set `setop 7 0`; the camera went to sleep at 23:33:34
   with `No RTC wake: RTC kept running, alarm off`, and woke at 23:33:35 by `RTC Timer` and took a
   photo. `hx_drv_rtc_set_alarm_int(RTC_ID_0, RTC_AIE_OFF)` does not stop the PMU wake: with RTC0
   unmasked, the old alarm still wakes it. Happens once each time the timer is turned off before its
   alarm. Test 1 (1 Nov) missed it because its old alarms were all in the past.

Not run: the brief's burst (6), and the BLE processor's tests 8 (command during processing) and 9
(command just after `Sleep`).

### Fix for item 6, coded 2 November 2026 (not yet built)

`sleep_mode_enter_dpd()`, no-RTC-wake branch: the alarm is also set to 1 January 2020 (disabled) and its
status cleared, as well as `RTC_AIE_OFF`. The alarm fires only when the RTC equals it, and the RTC never
reads before 2024. Bench: `AI setop 7 20`, wait for one timer wake, then `AI setop 7 0` within 20 s; no
`RTC Timer` wake may follow.

## Bench, alarm fix and burst, 2 November 2026 (logs/ai8.txt, ble8.txt)

AI processor built 12:48 2 Nov with the alarm fix; same BLE processor build.

1. **Alarm fix: passed.** The timer sleep at 23:52:14 set an alarm for 23:52:35. `setop 7 0` came on a
   BLE wake at 23:52:20 (held 100 ms until the settings were loaded); the camera slept at 23:52:24 with
   `No RTC wake`. No `RTC Timer` wake followed: the next wake was motion, 42 s later.
2. **Burst (`setop 5 5`, `setop 6 200`): passed.** One `setutc`, no `No setutc`, 5 photos written in
   order (photo queued at 344 ms, the first waited for `setutc`).
3. **`setutc` on every wake: 10 of 10**, about 110 ms after the wake message. No `UNHANDLED` or
   `Ignore this in`. After power-on the RTC was written from 2024 on the first wake with `setutc`
   (`took 1101ms`), then `RTC out by -1s` to `-14s: not set`.
4. **Not run:** the BLE processor with no time and a photo. LoRaWAN joined 8 s after power-on (op 32 was
   not 0), so the BLE processor had the time before the first photo. Also not run: the BLE processor's
   tests 8 and 9.

## Bench, BLE processor with no time, 2 November 2026 (logs/ai9.txt, ble9.txt)

Op 32 = 0 (no LoRaWAN), power cycle. Passed.

1. **Power-on and a motion wake with no time:** `Can't send UTC` on both wakes. The photo waited the
   300 ms, then took the RTC: `No setutc after 300ms`, photo queued at 662 ms, written `at 0:0:15
   1/1/2024`. Nothing printed about the RTC at sleep. The RTC kept counting through DPD with op 7 = 0.
2. **A BLE connection alone did not set the time:** connected at 13:03:34, then `AI setop 32 15` at
   13:04:02 still gave `Can't send UTC`. Nothing on the connection sent `setutc` to the BLE processor.
3. **LoRaWAN then joined** (13:04:12), and the next motion wake got `setutc`: photo `at 0:4:46
   2/11/2026`, queued at 346 ms, and at sleep `RTC was out by 89510540s: set to ... (took 1014ms)`.

Item 2 explained (Charles): the connection was from nRF Toolbox, a BLE terminal, which sends no
`setutc`; the Wildlife Watcher app does. LoRaWAN joined at once because the BLE processor reads op 32
from the AI processor's `Sleep` message. Not an issue.

## Bench, time from the app, 2 November 2026 (logs/ai10.txt, ble10.txt)

Op 32 = 0, power cycle, then the Wildlife Watcher app. Passed.

1. Before the app: `Can't send UTC`, motion photo `at 0:0:10 1/1/2024` (fallback, queued 552 ms).
2. The app connected and sent, in order: `selftest`, `setutc 2026-11-02T00:17:50Z` (to the BLE processor),
   `battery`, `AI info`, `ver`, `AI ver`. `AI info` woke the camera; the wake handshake sent `setutc`
   before `info` was passed on. So the time reaches the AI processor on the first `AI` command after
   connecting, not from `ver` itself.
3. At that sleep: `RTC was out by 89511445s: set to ... (took 1221ms)`. The next two motion wakes got
   `setutc` (`was out by 0s`) and photos stamped 2/11/2026.
4. Photos taken before the app connected keep their 2024 time.

The BLE processor's test 8 (`NN±` while an app command waits for its reply) is skipped (Charles, 2 Nov):
the window is 50 to 100 ms and `NN-` now usually arrives before IDLE. Checked in the code instead
(`aiStateMachine.c` `handleEventsForProcessing()`: `NN±` go to `handleAiReport()` and stay in PROCESSING).

## Bench, app command during the RTC write at sleep, 2 November 2026 (logs/ai11.txt, ble11.txt)

The BLE processor's test 9. Passed.

1. Console `setutc` 912 s ahead, then `dpd`. `Sleep` reached the BLE processor at 14:24:02.24; the AI
   processor then wrote its RTC (`took 1273ms`).
2. `AI ver` arrived at 14:24:02.88, during the write. The WAKE pulse was lost, as expected (the
   AI processor wakes on the pin's level in DPD, and the 10 ms pulse ended before DPD).
3. `No Wake after WAKE pulse: trying once more` at 14:24:05.91; the second pulse woke it, `setutc` put
   the time right and `ver` was answered. Reply after about 3.4 s instead of about 0.2 s; no
   `AI processor not responding`.
4. At that sleep `RTC was out by -1103s: set to ... (took 1838ms)`.

## Image file name, coded 2 November 2026 (not yet built)

Found by Charles: on the first wake after a board with no time got `setutc`, a photo stamped 2/11/2026
was named `59200DF0.JPG`, which decodes to 1 January 2024. The name came from a bare RTC read, left
there on 1 Nov in the belief it only had to be unique; it is meant to encode the photo time.

Fix: `dir_mgr_generateImageFilename()` takes the photo's time in seconds instead of reading the RTC.
`prepareJpegFile()` passes the EXIF time (`exif_utc_time_to_seconds(&photoTime)`, new), so the name and the
EXIF time always agree. The BMP path and the `TESTFILENAMES` test use `exif_utc_get_time()`. The name's
format is unchanged. Not done (Charles): opening with `FA_CREATE_NEW` to avoid overwriting a file of the
same name, which is possible after the RTC moves back.

## Bench, file name fix, 2 November 2026 (logs/ai12.txt, ble12.txt, ai13.txt, ble13.txt)

AI processor built 14:37 2 Nov with the file name fix.

1. **File name: passed.** ai13, 14:47:43: LoRaWAN had given the BLE processor the time while the camera
   slept; the motion wake read 2024 from the RTC and got `setutc`. The photo was `AE7EBBE0.JPG`
   (2 Nov 2026 01:47:42, its EXIF time), not `5920....JPG`. Earlier photos with no time anywhere were
   `592008C0` and `59200900` (1 Jan 2024), as designed.
2. **Fault, BLE processor: a missed interrupt.** On that wake the AI processor replied `RTC set to ...`
   and signalled it, but the BLE processor never read it. At that moment it was handling a LoRaWAN
   downlink (`DeviceTimeAns`, two flash writes to NVM). No phone was connected, so the BLE processor
   used its low-power PORT/SENSE interrupt for `/IP_INT`, which its own code (`aiProcessor.c`,
   `setAiIntHighAccuracy()`, 16 Sep 2026) says can miss the AI processor's short pulse when it is busy;
   the latched GPIOTE channel is only used while a phone is connected. Result:
   - BLE processor: SETUTC timed out after 3 s, `AI processor not responding: woke but did not reach
     IDLE (SETUTC)`, SLEEP; the later messages were handled in SLEEP, and `responding again` at `Sleep`.
   - AI processor: `I2C master did not read our I2C message` after 3.75 s. Its later messages waited
     (`I2C busy`), and the photo was saved about 4 s late. Nothing was lost but the `RTC set` reply.
   Not new, but this work makes it likelier: one more message (`RTC set`) in every wake, and with
   LoRaWAN on, an uplink and downlink often fall in the same second as a motion wake.

Correction, 2 November 2026: ai12 and ai13 ran the RP3 image (`Camera: RP`, IMX708), not HM0360. All
earlier logs from ai4 ran the HM0360 image. So the final code (with the file name fix) has run on
the RP3 image; the HM0360 image still needs building with it.

## The missed interrupt: logs 14 to 16 and the held-low handshake, 3 November 2026

1. **The latched interrupt while awake failed** (ww-hardware build 16:23:45 2 Nov; logs/ai14.txt,
   ble14.txt). On every wake with a phone connected the BLE processor started a read about 48 ms after
   its WAKE pulse, while the AI processor was still in its bootloader. The AI processor logged
   `I2C Error: data unready`, and the wake message arrived with its header zeroed (`CRC Failed`), so the
   wake was lost. The previous BLE build (12:22:38) with the same AI image was clean (ai15, ble15), and
   so was the reverted build 09:15:45 3 Nov (ai16, ble16). Report: ww-hardware thread,
   `CLAUDE_IP_INT_early_read_report.md`.
2. **Replaced by a held-low handshake**, agreed with Charles: ww-hardware thread,
   `IP_INT_handshake_spec.md`, which holds the AI processor session's answers in its section 7. Low
   means a complete message is armed; the AI processor releases the pin when the read completes, and
   keeps it high at least 20 ms before the next. Found while answering: PB11 resets with a weak
   pull-down (HX6538 datasheet), so until our code pulls it up the line may not read high.
3. **AI processor side coded, 3 November (not built):**
   - `if_task.c`: `sendI2CMessage()` waits for the 20 ms high (`IP_INT_HIGH_MIN_MS`), arms, then drives
     the pin low; `i2csTxDoneEvent()` releases it; the 4 s no-read timeout releases it and drops the
     message. The first message's 300 ms `BLE_PROBE_TIME` now only sets the "BLE processor unresponsive"
     flag (a late read clears it), and the pin stays low for the full 4 s. An I2C error during a send no
     longer returns to IDLE with the pin still low. `TEST_INT_PULSE` is off.
   - `ifTask_ipIntInit()`, called at the start of `app_main()` (`ww500_md.c`): PB11 pulled up first.
   - `CLI-commands.c`: the `int` test command removed (it pulled PB11 low with no message).
   - Docs: `architecture_of_inter_processor_communications.md`, `inter_processor_comms_details.md`,
     `txfile.md`, `ble_commands.md`, `hardware-traps.md`, `cross-repo-contracts.md`.

## First run of the held-low handshake: log 17, 3 November 2026

Both processors on the handshake (AI processor HM0360 image built 12:11:01; BLE processor built
12:01:33). Phone connected, `AI ver` repeated, LoRaWAN active (logs/ai17.txt, bl17.txt). 40 messages
were read and released, with no `data unready` and no `CRC Failed`. One reply (`RTC set`, 12:15:44) was
never read. Its low fell on a LoRaWAN receive, and the AI processor dropped it after 4 s.

The cause is in the BLE processor. The radio's DIO1 uses a rising-only PORT interrupt, so while DIO1 is
high it holds the nRF52's shared DETECT signal, and the /IP_INT low raises no event. ai13 fits the
same pattern. The report, with a proposed fix (toggle sense for all PORT pins, plus a level check
that the held-low design makes safe), is in the ww-hardware thread:
`CLAUDE_IP_INT_missed_low_report.md`. No AI processor change is needed.

## The missed-low fix: logs 18 and 19, 3 November 2026

The fix is in the BLE processor only (ww-hardware spec section 9, build 17:43:10): every PORT interrupt
pin uses both edges, so no pin holds DETECT, plus a level check at the 3 s timeout. AI processor image
unchanged (HM0360, 12:11:01). Note: logs/ai17.txt was overwritten; the 12:15 run above survives only
as logs/bl17.txt, and the new pair was renamed ai18/ble18.

- **18** (phone connected, `AI ver` x 11, LoRaWAN joined, 76 s): 56 of 56 messages read `CRC OK`. One
  WAKE retry: `AI ver` came 2 s after `Sleep` was read, before the AI processor reached DPD (it was
  saving the RTC calibration), and the retry woke it. No message fell near a radio event.
- **19** (no phone, `AI setop 32 1`, waving, 18:06 to 18:14): 29 motion wakes, 232 reads `CRC OK`, no
  `not responding`, no AI timeout, no `did not read`, no `interrupt missed`. Twice an /IP_INT low came
  right on a radio event and was read at once: `!107` at 18:08:51.152, just after `MAC rxDone` at 51.090
  (then FDS writes), and `!211` at 18:12:01.621, just after `MAC txDone` at 01.578. That is the case
  missed at 12:15:44. The BLE log only times these to a few tens of ms, so whether DIO1 was still high
  is likely, not proven.
- Not exercised: the accelerometer interrupt, and the burst test.

## The blocking test (log 20) and deferred events, 3 November 2026

Run 20 (ww-hardware spec section 10) held DETECT high on a spare pin with a temporary `hack`. Under the
fixed setting every message was read at once; under the old setting none was, which confirms the cause.
The BLE processor's 3 s rescue read every message, but the wake did not complete, because of the AI
processor (ai20):

1. The late read of `Wake` released an `Inactivity` deferred while waiting, so `Sleep` went at once.
2. The BLE processor's `selftest`, deferred while `Sleep` was waiting, was then overwritten by the next
   `Inactivity`: there was one slot for deferred events.

Coded in `if_task.c` (not built):

- Deferred events are kept in a queue of 4 (`deferEvent()`), oldest first, replayed one at a time in
  IDLE. `Inactivity` is kept at most once. If the queue is full the event is dropped and logged
  (`discardEvent()`), and a sender waiting on `xI2CTxSemaphore` is released.
- When the BLE processor reads one of our messages, a deferred `Inactivity` is dropped
  (`dropDeferredInactivity()`). The detector fires again after a further idle period. Logs 16 to 19
  never deferred one, so normal wakes are unchanged.
- When `Sleep` is read, or not read before the timeout, anything still deferred is dropped and logged
  (`discardDeferredEvents()`). The BLE processor treats us as asleep, so a late reply would confuse it.

Not done: section 10's suggestion to process a command that arrives after `Sleep`, or abandon the sleep.
The BLE processor has read `Sleep`, so its next command would start with a WAKE pulse and wait for a
`Wake` that never comes.

## Log 21: the deferred-event fix works, 4 November 2026

Both images built 10:19 4 Nov (AI processor HM0360 image, deferred-event queue and the define tidy).
Section 10 `hack 12` rerun, then a reset and three normal `AI ver`.

- **`hack 12` (old setting), `AI ver` at 10:31:52: the wake completes.** No `!N` line; the BLE
  processor's 3 s rescue read `Wake` (55.86), the `selftest` reply (58.96) and `RTC set` (02.03), each
  `CRC OK`, then sent `ver`. AI log: each `Inactivity` deferred while a message waited was dropped at
  the read (`Deferred 'Inactivity' dropped ...`), and `selftest` and `setutc` were both processed.
- **As expected, the `ver` reply was lost** (sent from IDLE, no rescue there): `did not read` at
  06.135, the deferred `Inactivity` issued, `Sleep` not read either, `Dropping event 0x070e: going to
  sleep`, DPD at 10:32:12. The BLE processor timed out to IDLE at 12.15 and still thought the AI
  processor awake until the reset.
- **Normal wakes after the reset:** 10:34:00, 10:34:16 and 10:34:25 all read at once (`!3` to `!11`
  and on), `ver` reply within 0.5 s, no `interrupt missed`, `did not read`, `Deferring` or `Dropping`.
- Not new: `Wake 2024-...` after each cold boot (no time yet), the stale wake-message time after
  DPD (`differ by 13s` at 10:34:16, `was out by 0s` at setutc), and `unhandled event 'Awake' in
  'I2C TX State'`.

The `hack` code and `GPIOTE_CONFIG_NUM_OF_LOW_POWER_EVENTS` 7 can now come out of the BLE processor.

## Log 22: `NN-` arrives as `HM0`, 4 November 2026

The handshake was clean (259 `CRC OK`, no missed lows, no `did not read`). But on all 17 motion wakes the
BLE processor got a 4-byte `HM0` and no `NN-`; the AE registers message was lost too, and the console-only
MD grid went to the app. `NN` messages arrived in ble15 and ble16, none from ble19 on.

Cause: `sendMsgToMaster()` (`image_task.c`) passed the IF task a pointer to the shared `msgToMaster` and its
length then. The image task has the higher priority, so it wrote the next message into the buffer before
the IF task copied it: each message went with the next one's text and the earlier one's length. Likely
exposed by the extra wake traffic (`selftest`, `setutc`) holding `xI2CTxSemaphore` just as `NN-` was sent.

Fix (not built): `sendMsgToMaster()` copies the string into its own buffer (`msgToMasterTx`) once it has the
semaphore, which is only free when the previous message has gone or been dropped. Covers `lightSensor.c`
too. Bench: motion wakes, phone connected; expect `NN-` (4 bytes), the AE registers, `HM0360 woken by
motion ...`, and no grid text in the BLE log.

Benched 4 November (Charles): the fix works, `NN-` arrives again on motion wakes.
