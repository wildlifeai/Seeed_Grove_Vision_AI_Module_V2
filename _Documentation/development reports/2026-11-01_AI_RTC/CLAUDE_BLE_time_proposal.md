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

1. Option A (a new `bletime` reply)? It needs a ww-hardware change and Victor's agreement.
2. With no time anywhere: leave the EXIF time out, or stamp 2024 as now?
3. Set the AI clock at sleep when it is out by more than a limit, or rarely?
4. Should the file date on the card follow the photo's time?

## Proposed steps

1. **Measure:** a three-way log (BLE processor, AI processor, PC) of timer and motion wakes on the
   current build: when the first message goes, when the BLE processor reads it, when the photo is
   saved, and what the AI clock reads at wake and at sleep.
2. **Agree the message** with Victor (cross-repo contract).
3. **AI processor side:** the `bletime` command, the difference in RAM, photo stamps from it, the
   wait and the fallback. Testable before the BLE processor changes, by typing `bletime ...` on the
   console.
4. **BLE processor side** (ww-hardware): reply at once to every wake message.
5. **#152:** keep a real time through a restart.
