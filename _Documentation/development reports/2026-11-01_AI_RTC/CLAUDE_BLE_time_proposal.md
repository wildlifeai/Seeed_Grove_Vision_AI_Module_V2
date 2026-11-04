# Proposal: get the time from the BLE processor early in each wake

#### File: `CLAUDE_BLE_time_proposal.md`
#### Path: `_Documentation\development reports\2026-11-01_AI_RTC\CLAUDE_BLE_time_proposal.md`
#### Author: Claude (Opus 5.5), for review by Charles Palmer
#### Date: 1 November 2026

## The problem

Photos are stamped from the AI processor's clock, read when the photo is prepared
(`image_task.c`, `exif_utc_get_rtc_as_exif_string()`). That clock is wrong in three ways:

1. **Stale after a wake** (#56). The warm boot reads it with the fast read, which returns the time
   the processor went to sleep. The synced read takes about 900 ms (`ww500_md.c`, commented out).
   On 2 Oct, after timer wakes the clock caught up within about 2 s, but after motion wakes it never
   did, and the error grew with every wake (60 s to 120 s behind in 1.5 minutes).
2. **Reset to 2024** by every restart that is not a DPD wake (#152): power-up, `reset`,
   `switchslot`, the day/night switch, a firmware update.
3. **2 to 4.6 % fast** between updates (from #245): about 3 minutes an hour.

The BLE processor has an accurate clock (32 kHz crystal, about 20 ppm), once the app or LoRaWAN
has given it the time. Today it corrects the AI processor only when the two differ by more than
300 s, only after the wake's photos, and with `setutc`, which blocks the AI processor's
interrupts for about 1.4 s (`hx_drv_rtc_set_time()`).

## What we know about the timing

From the 2 Oct bench log (`2026-10-02_todo_rebench/56_retest_motion_vs_timer.txt`, before the
XIP save state work):
- The BLE processor compared the two clocks about 0.7 s after each wake, so it reads the AI
  processor's first message ("Wake", "Timer" or "Motion" with its time) quickly.
- The photo was saved about 0.85 s after the wake.

Since PR #283 the first frame is ready about 100 ms after `app_main()`, and the photo waits in RAM
for the SD card (about 250 ms). **Not measured yet:** how soon the BLE processor could answer the
first message now.

## The idea

Treat the time like the settings in PR #283: it must not hold up the frame, only the save.

1. Wake and take the frame, as now. Note the tick count when the frame is ready.
2. The first message to the BLE processor ("Wake/Timer/Motion <AI time>") already goes out early.
   **The BLE processor replies at once, every wake, with its own time**, or says it has none.
3. The AI processor keeps the difference (BLE time minus its clock) in RAM. It does **not** set its
   RTC here, so no 1.4 s interrupt block during the capture.
4. The photo's time = the BLE time at the reply, minus the ticks since the frame. The tick runs from
   the 24 MHz crystal, so it is accurate while awake.
5. If the reply has not come when the photo is otherwise ready to save, wait up to a limit (say
   500 ms), then fall back (below).
6. The same difference serves every photo in the wake, the file date (`get_fattime()`) and the
   time-of-day flash decision once known.

**What it fixes:** #56 and the drift for photos, whenever the BLE processor has the time. #152 for
photos too, since a 2024 clock no longer matters.

## Options for the BLE processor's reply

| | Reply | AI processor | Notes |
|---|---|---|---|
| A | A new message, e.g. `bletime 2026-11-01T01:02:03Z` or `bletime none`, after every wake message | New command: keep the difference, do not set the RTC | **Recommended.** Clear, small change on both sides. A new cross-repo command |
| B | `setutc` every wake, at once (drop the 300 s rule) | Unchanged | No AI change, but 1.4 s of blocked interrupts during the capture and every wake. Not recommended |
| C | The AI processor asks (`getutc` to the BLE processor) | Sends a request, waits | An extra message each way; the BLE processor already has the AI time in the wake message, so A is simpler |

## Keeping the AI processor's own clock roughly right

Still needed for the wake message, the console, `rtc_cal.c` (when it last measured) and wakes when
the BLE processor has no time. Choices:
- Set the RTC at sleep when it is more than some limit out (say 10 s). Costs about 1.4 s awake when
  it does, which at 4 % drift is most wakes of a long timelapse.
- Or rarely (once an hour or a day), and rely on the difference for photos.

## When nobody knows the time

The BLE processor has no time after its own restart, until the app connects. Then:
- use the AI processor's clock if it reads 2025 or later, else
- leave the EXIF time out, or mark it unreliable (`iso_time_isSet()` exists for this).

#152 separately: on a cold boot, keep the RTC if it already reads a real time (2025 or later)
instead of writing 2024. To test first: does the RTC keep running through a `reset` and a
watchdog reset?

## Also to check early

- **Is the RTC really stopped through motion wakes, or is only the fast read stale?** 2 Oct
  suggests the error grows with each motion wake. If the RTC is wrong at sleep, so is the alarm
  `sleep_mode.c` sets from it, and `rtc_cal.c`'s measurement, which sets alarms 2 s ahead of a fast
  read, would fail (it keeps the saved value, so this is safe, but a motion-only camera might never
  re-measure). A bench run with motion wakes and `rtccal measure` would show it.
- **The synced read's 900 ms:** could it run in the background after the frame, instead of at boot?

## Questions for Charles

1. Option A (a new `bletime` reply)? It needs a ww-hardware (BLE processor) change.
2. With no time anywhere: leave the EXIF time out, or stamp 2024 as now?
3. Set the AI clock at sleep when it is out by more than a limit, or rarely?
4. Should the file date on the card follow the photo's time?

## Proposed steps

1. **Measure:** a three-way log (BLE processor, AI processor, PC) of timer and motion wakes on the
   current build: when the first message goes, when the BLE processor reads it, when the photo is
   saved, and what the AI clock reads at wake and at sleep.
2. **Define the message** for both processors (Charles manages the BLE processor code; a cross-repo
   contract, so the app and backend are told if they see it).
3. **AI processor side:** the `bletime` command, the difference in RAM, photo stamps from it, the
   wait and the fallback. Testable before the BLE processor changes, by typing `bletime ...` on the
   console.
4. **BLE processor side** (ww-hardware): reply at once to every wake message.
5. **#152:** keep a real time through a restart.

## Decisions

Charles, 1 November 2026:
1. **Option A**, a new `bletime` reply. Still to define (step 2).
2. **With no time anywhere, keep the 2024 stamp for now.** Ask Victor later whether to leave the
   EXIF time out instead.
3. **Set the AI processor's clock rarely** (once a day, or when more than a few minutes out), since
   photos no longer depend on it.
4. **The file date follows the photo's time** (`get_fattime()` uses the same difference).

## Step 1 results: the three-way log (1 November 2026)

Logs `logs/ai_1.txt` and `logs/ble_1.txt`, board WILD-CMSS, HM0360 image, build from `dev` after
PR #283. Both consoles in Tera Term on one PC, stamped in NZ daylight time (UTC+13). The BLE
processor already had the time: its `getutc` reply matched the PC to the second. Run: 7 timer wakes
(`setop 7 20`), 2 BLE wakes, 4 motion wakes, `rtccal measure`, `getutc`.

**1. The RTC stops in DPD when no alarm is set.** Each sleep with op 7 = 0 (motion or BLE wake only)
woke with the clock where it went to sleep, or 1 s on:

| Asleep (PC) | "Entering DPD at" | Next wake reads |
|---|---|---|
| 5.3 s | 01:32:07 | 01:32:07 |
| 6.0 s | 01:32:11 | 01:32:11 |
| 3.6 s | 01:32:14 | 01:32:14 |
| 11.1 s | 01:32:17 | 01:32:17 |
| 2.9 s | 01:32:19 | 01:32:20 |
| 19.3 s | 01:32:22 | 01:32:23 |
| 15.4 s | 01:32:28 | 01:32:28 |

It is not only a stale first read: the clock counts while awake (01:32:28 at the wake, 01:32:30 at
sleep 2.3 s later) and carries on from the stopped value. Sleeps with an alarm (op 7 = 20) did not
lose time. So a camera with timelapse off loses every second it spends asleep: after these 7 sleeps
it was 45 s behind. This is probably the main cause of #56. With no alarm, `sleep_mode.c` leaves the
PMU's RTC interrupt masked (`PM_RTC_INT_MASK_ALLMASK`) and sets no alarm; whether that is what stops
the RTC is not known yet.

**2. The clock gains during timer sleeps,** as expected from #245: the BLE processor's "differ by"
rose 8, 9, 10, 11, 12, 13 s over 6 wakes of 22 s (about 4.6 %). Never corrected, as it stayed under 300 s.

**3. The wake message goes out after the photo is stamped,** just as its write starts. It waits for CONFIG.TXT and the end of
every task's start-up (`APP_MSG_IFTASK_FREERTOS_INIT` in `if_task.c`):

| ms after the boot loader's first line | Timer wakes | Motion wakes |
|---|---|---|
| First message to the BLE processor (`NN-`) | 220 to 360 | 195 to 285 |
| Wake message sent ("Timer ..." / "Motion ...") | 605 to 660 | 595 to 705 |
| BLE processor logs it and compares the clocks | 640 to 680 | 640 to 700 |
| Photo written (end of the write) | 745 to 840 | 735 to 850 |

Before the wake message the AI processor sends `NN-` and the HM0360 AE registers (and on motion wakes
the motion grid), which the BLE processor logs as `UNHANDLED event ... in SLEEP`. For the proposal,
the BLE processor's time could come back within about 50 ms of the wake message, but that message
must be sent earlier, near the `NN-`, or the save waits about 0.4 s for it.

**4. `rtccal measure` works after motion wakes:** -45908 ppm against the saved -46002 ppm (0.01 %).
The risk flagged above is limited to the alarm-less sleeps in finding 1.

## Step 1b: motion wakes with an alarm set (1 November 2026)

Logs `logs/ai2.txt` and `logs/ble2.txt`, same board and build. `setop 7 3600`, so every sleep sets an
alarm an hour ahead; then 6 motion wakes, a BLE wake and `getutc` on both consoles.

**1. With an alarm set, the RTC keeps running through motion-wake sleeps.** The BLE processor's
"differ by" stayed at 167 to 170 s for all 6 motion wakes (an offset left from earlier, never
corrected as it is under 300 s). In step 1, with no alarm, it grew with every sleep. So finding 1 of
step 1 is confirmed: the RTC stops only when DPD is entered with no alarm.

**2. The first read after a wake can still be stale.** Twice the time read early in the wake was the
time at sleep, and a read a moment later was right:

| Asleep (PC) | "Entering DPD at" | Early read | Read a moment later |
|---|---|---|---|
| 2.1 s | 01:42:45 | 01:42:45 (wake message, 650 ms after boot) | 01:42:48 (photo stamp, 190 ms later) |
| 13.4 s | 01:42:57 | 01:42:57 (wake message, 330 ms after boot) | 01:43:12 (`getutc`, 150 ms later) |

So the RTC counted through the sleep, but the fast read returned the old value for part of the first
second. Probably the value it reads is only refreshed on the RTC's next one-second tick. A photo
stamped in that window gets the time of the last sleep. The BLE processor's 181 s on the last wake
comes from this stale read.

**What this means:**
- **#56 has a simple main fix:** always set an RTC alarm at DPD, even when op 7 = 0 (for example the
  longest the alarm allows, waking only to sleep again). To check first: whether unmasking the RTC
  interrupt alone is enough, and what a far alarm costs.
- **The stale first read** needs either the synced read, a wait for the first tick, or the BLE
  processor's time (this proposal) before the photo is stamped.

## Test 1: RTC0 unmasked with no alarm (1 November 2026)

Temporary change in `sleep_mode_enter_dpd()`: when no RTC wake is wanted, turn the alarm interrupt off
(`hx_drv_rtc_set_alarm_int(RTC_ID_0, RTC_AIE_OFF)`) and leave RTC0 unmasked in the PMU
(`pmu_rtc_mask = RTC1 | RTC2`). Logs `logs/ai3.txt` and `logs/ble3.txt`, same board, HM0360 image.
After flashing (a cold boot, so the clock read 2024) the BLE processor sent `setutc`; then op 7 = 0
and 6 sleeps ended by motion or the BLE processor.

**It works: the RTC keeps counting through DPD, and nothing wakes the camera but motion or the BLE
processor** (every wake was `WAKE signal`, no `RTC Timer`).

| Asleep (PC) | "Entering DPD at" | Wake message | BLE "differ by" |
|---|---|---|---|
| 28.2 s | 02:11:16 | 02:11:45 | 0 s |
| 5.1 s | 02:11:47 | 02:11:52 | 0 s |
| 38.2 s | 02:11:54 | 02:11:54 (stale, see below) | 38 s |
| 1.4 s | 02:12:37 | 02:12:38 | 2 s |
| 12.7 s | 02:12:40 | 02:12:53 | 3 s |
| 27.2 s | 02:12:55 | 02:13:24 | 4 s |

At the end the AI processor's `getutc` read 02:13:24 against the BLE processor's 02:13:21: 3 s fast
after 2 minutes, the 4.6 % drift from #245.

**The stale first read again.** After the 38 s sleep, both the wake message (580 ms after boot) and
the photo stamp (650 ms) read 02:11:54, the time at sleep. The clock had counted: the sleep 2 s later
read 02:12:37. So photo `AE69FEA0.JPG` is stamped 39 s early. With test 1 this is the remaining cause
of wrong photo times after a sleep (1 wake in 6 here, 2 in 7 in step 1b).

**Made permanent, 1 November 2026 (not yet built):** the same change, with its comment, in
`sleep_mode_enter_dpd()`; the console prints `No RTC wake: RTC kept running, alarm off` before such a
sleep. Entry added to `.agents/skills/references/hardware-traps.md`.

**Permanent version benched, 1 November 2026** (`logs/ai4.txt`, HM0360 image, op 7 = 0, 5 motion wakes
after sleeps of 3 to 18 s). The clock ran through every sleep: each wake's later reads were on from
the time at sleep by the time asleep (plus the 4.6 % drift). All 5 photo stamps were right. The
earliest read (`Woke at`, about 40 ms after boot) was stale once in 6 wakes (02:42:47 after a 6 s sleep,
corrected to 02:42:54 by the wake message 0.4 s later); the other times it was already right.

## Decision 1 revised (1 November 2026)

No `bletime` command. The existing `setutc` carries the time, sent by the BLE processor every wake in
its existing order (after `selftest`), and the AI processor no longer writes the RTC when it arrives:
it keeps the difference and writes the RTC at sleep only when far out. Details:
[CLAUDE_wake_sequence_spec.md](CLAUDE_wake_sequence_spec.md).

