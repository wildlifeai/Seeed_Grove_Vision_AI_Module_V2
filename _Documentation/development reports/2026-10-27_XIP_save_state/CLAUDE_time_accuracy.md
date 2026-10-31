# Time on the AI processor: what is wrong, and ideas

#### File: `CLAUDE_time_accuracy.md`
#### Path: `_Documentation\development reports\2026-10-27_XIP_save_state\CLAUDE_time_accuracy.md`
#### Author: Claude (Opus 5.5), for review by Charles Palmer
#### Date: 30 October 2026

## In short

The open time issues (#245, #56, #152) are really **two separate problems**:

1. **The timelapse interval is wrong.** The AI processor's sleep timer runs about 4 to 5 % fast,
   so a 90 s interval is really about 85 s.
2. **Photos are stamped with the wrong time.** The AI processor's clock is fast, stale after a
   motion wake, and reset to 2024 by every restart.

They need different fixes. Problem 2 need not depend on the AI processor's clock at all: the
camera could take the frame at once, then stamp and save the photo only when the BLE processor has
sent the right time, the same way the photo already waits in RAM for the SD card.

## What we know

**The clocks**
- The AI processor's clock (RTC) and its sleep timer run from an internal 32 kHz RC oscillator.
  There is no 32 kHz crystal. It runs about 4 to 5 % fast (#245; `ww500_minimal`, 21 Sep).
- While awake, the FreeRTOS tick comes from the 24 MHz crystal and is accurate (about 10 ppm). So
  times measured within one wake are good; only the time asleep is wrong.
- The RC oscillator's trim register changes its rate. On one board, awake: trim 31 (the default)
  +4.4 %, 33 about 0 %, 34 -2.2 %, 63 -37 %. Steps of about 2 %, so the best trim leaves up to
  about 1 %. The register reads back at its default after every wake. **Never tested: whether the
  trim works during DPD.** Every timed sleep so far used the default trim.
- RC oscillators also drift with temperature. A value calibrated on the bench may not hold in a
  cold or hot field. Not measured.
- The BLE processor has a 32 kHz crystal (about 20 ppm). It gets the time from the app, or from
  LoRaWAN where there is a network, and keeps it well. It loses it when it restarts, until the
  app next connects.

**How the AI processor's time is kept now**
- **Cold boot** (power-up, `reset`, `switchslot`, a firmware update, the day/night camera
  switch): the clock is set to 2024-01-01 on purpose (#152).
- **Warm boot:** the clock is read with a fast first read that returns the time the processor went
  to sleep (#56). The correct, synced read takes about 900 ms (`ww500_md.c:689`).
- **Correction:** each wake the AI processor sends `Wake`, `Timer` or `Motion` with its time. The
  BLE processor sends `setutc` only if the two differ by more than 300 s, and only after the
  wake's photos, and only if it has the time itself.
- **Setting the clock** (`hx_drv_rtc_set_time()`) blocks interrupts for about 1.4 s.

**Where the time is used**
- The EXIF time stamp, read when the photo is prepared (`image_task.c:2666`).
- The file date on the SD card (`get_fattime()`).
- The time in the wake message to the BLE processor.
- The timer alarm that ends the sleep (`sleep_mode.c`): this is problem 1.

## Problem 2: stamp photos with the BLE processor's time

**The idea.** Treat the time like the settings: do not let it hold up the frame.
1. Wake, start the camera, take the frame (as now).
2. At once, ask the BLE processor for the time. The wake message already goes to it: it would
   reply with its time every time, straight away.
3. Note the tick count when the frame arrives. When the time comes back, the frame's time is the
   BLE processor's time minus the ticks since the frame. The tick is accurate while awake.
4. Stamp the EXIF and the file with that time and save the photo. The photo already waits in RAM
   for the card, so in most wakes this adds nothing.
5. Keep the difference between the AI clock and the BLE time in RAM, and use it for any more photos
   in the wake. Do not set the AI clock while photos are being taken (it blocks interrupts for
   1.4 s). It could be set at sleep, for the console and the next wake message.

**What it fixes.** #56 and #152 for photos: a stale or reset AI clock no longer matters, as long as
the BLE processor has the time. The AI clock's rate (#245) stops mattering for stamps.

**What it needs**
- **A BLE processor change** (ww-hardware): reply to every wake message with its time, at once,
  and say when it does not have a valid time. This is a cross-repo contract change, to agree with
  Victor.
- **A fallback** when the BLE processor has no time (after its own restart, before the app
  connects) or does not answer: use the AI clock as now, and mark the photo's time as unreliable
  (`iso_time_isSet()` exists for this), or leave the EXIF time out.
- **A measurement:** how long the BLE processor takes to answer. If it is under about 250 ms (the
  time the card takes now), photos are not delayed.

**Questions**
- What should a photo carry when nobody knows the time: no time, the 2024 time, or the AI clock's
  time marked as unreliable?
- Should the file date on the card follow the same time? (FatFS asks `get_fattime()` when the file
  is written, so it would need the same offset.)

## Problem 1: make the timelapse interval right

Possible fixes, smallest first:

| | Fix | Per-board work | Notes |
|---|---|---|---|
| a | **Trim the oscillator** at every boot, from a per-board value in the board record (flash 0x00F01000) | Calibration at production | Only works if the trim acts during DPD (to test). Leaves up to about 1 %. Temperature drift not covered |
| b | **Correct in software:** shorten each alarm by a per-board error in ppm, kept in the board record | Calibration at production | Works whatever the trim does. As accurate as the calibration |
| c | **Self-calibrate:** with problem 2's fix the AI processor learns the real time at every wake, so it can compare how long it really slept with what its clock said, and keep its own error figure up to date | None | Follows temperature and ageing. Needs the BLE processor to have the time. Revisits the earlier decision of calibration at production |
| d | **The BLE processor times the timelapse** and wakes the AI processor (its crystal is accurate) | None | Largest change, mostly on the BLE processor side |

**A question first:** how accurate does the interval need to be? 5 % may already be acceptable for
a timelapse. 1 % (fix a) may be enough. If photos are stamped correctly (problem 2), an interval
that is slightly short may matter little.

## Tests

1. **Does the trim work during DPD?** Timer sleeps at the default trim and at a trimmed value, on
   several boards, at several intervals: a few seconds, about a minute, several minutes, then
   hours. Measure the real sleep on the PC (from `Entering DPD` to the next boot banner). Needs a
   `rc32ktrim` command and the trim applied at every boot in `ww500_md`.
2. **How much does the error vary** between boards, and with temperature?
3. **How fast can the BLE processor answer** a wake message with its time?

## Suggested order

1. Decide the questions above, and whether problem 2's fix is worth a BLE processor change.
2. Test 1 (the trim in DPD), since it decides between fixes a and b.
3. Problem 2, if agreed: it fixes #56 and #152 for photos and makes fix c possible.

---

## Experiment 1: does the trim work during DPD? (coded 30 October 2026, not built)

**Firmware (`ww500_md`):**
- `rc32ktrim`: shows the trim register now and the value saved in the board record.
  `rc32ktrim <0-255>` sets the register at once and saves the value in the board record.
  `rc32ktrim default` removes the saved value. Refused while the camera is busy.
- The board record (`xip_state.c`, sector 0x00F01000) has its own format version
  (`XIP_STATE_BOARD_VERSION`), so a later change to the fast-start record never loses a board's
  value. Shown by `xipstate`.
- `xip_state_applyBoard()` sets the saved trim just before every sleep, from the image task when
  the NN is not running (reading the flash switches XIP off). The awake time runs at the default
  rate, since the register resets at every wake. That is fine: the AI processor is normally awake
  for only a few seconds, so its awake-time error needs no separate correction (Charles, 30 Oct).
- `RC32K trim N` is printed just before `>>> Entering DPD`, so every sleep in a log carries its trim.

**Measuring: `_Tools/sleep_timing.py`.** Reads saved Tera Term logs (with timestamps), no board
needed. For each timer-ended sleep: the trim, the RTC seconds asked for (`Entering DPD` to
`Will wake at`), the real seconds (PC time from `Entering DPD` to the next bootloader line), and
the error. The summary per trim and interval adds a "rate %" that allows for the half second lost
on average to the whole-second alarm. Today's logs (default trim, 20 s): rate -5.6 %, matching #245.

### Result 1 (`logs/t1.log`, board WILD-CMSS, 30 October 2026)

**The trim has no effect during DPD.** At 60 s, trim 31 and trim 33 give the same sleep (56.66 s,
17 to 20 sleeps each). The log shows trim 33 in the register just before each of those sleeps. On the
same board awake, 33 brought the error to about 0 %, so the trim changes the clock while awake but
not the sleep timer.

| Trim | Asked | Sleeps | Real (mean) | Spread |
|---|---|---|---|---|
| 31 | 60 s | 20 | 56.66 s | 56.47 to 57.02 |
| 33 | 10 s | 6 | 8.92 s | 8.79 to 8.99 |
| 33 | 60 s | 17 | 56.68 s | 56.46 to 57.23 |
| 33 | 300 s | 7 | 285.57 s | 285.48 to 286.08 |

All three intervals fit **real = 0.9535 x asked - 0.55 s**: the timer runs **4.65 % fast**, plus a
fixed 0.55 s. That is why 10 s looks worse (-10.8 %). Sleeps repeat to about 0.02 s, so the 300 s
sleeps already give the rate to about 0.01 %; hour-long sleeps are not needed to measure it.

Notes:
- One trim 31 sleep is listed by the script as 0.09 s. A stray character at DPD entry started the
  bootloader line early, so Tera Term stamped it 56.5 s too early. That sleep was really about
  56.65 s. The script should take the time of the following line in this case.
- Four sleeps were ended by a WAKE signal (the commands) and are skipped.

**What this means:** fix (a) is out. Fix (b), a software correction, works: shorten each alarm by
the board's rate. Still to learn: the rate on other boards and its drift with temperature, which
decide between one fixed figure, a per-board value, and fix (c).

### Result 2 (`logs/xaes.txt`, board WILD-XAES, 31 October 2026)

| Trim | Asked | Sleeps | Real (mean) | Spread |
|---|---|---|---|---|
| 28 (its default) | 60 s | 7 | 58.40 s | 58.39 to 58.43 |
| 28 | 300 s | 1 | 292.97 s | |

Fit: **real = 0.9774 x asked - 0.24 s**, so this board's timer runs **2.26 % fast**, half WILD-CMSS's
4.65 %. Its trim register starts at 28, not 31: the default differs per chip, so it is
probably a factory value, and it still leaves a different error on each board. **A per-board
correction is needed.**

**The RTC itself runs at the same rate.** Between one `Entering DPD` and the next, the RTC's own time
moves on 62 s while the PC sees 59.1 s on WILD-CMSS (0.9535) and 60.7 s on WILD-XAES (about
0.979). So the alarm is not at fault: the 32 kHz clock is simply fast, and correcting its rate fixes
both the interval and the time of day kept by the AI processor.

### Result 3 (`logs/depop.txt`, the board with no BLE processor, 31 October 2026)

| Trim | Asked | Sleeps | Real (mean) | Spread |
|---|---|---|---|---|
| 31 (its default) | 60 s | 7 | 57.52 s | 57.52 to 57.53 (one more, the first, 57.15) |
| 31 | 300 s | 1 | 287.67 s | |

Fit: **real = 0.9590 x asked - 0.01 s**, so the timer runs **4.10 % fast**.

**Against `ww500_minimal` in September** (at least some of those tests were on this board):
- DPD 30 s alarm then: 28.81 s. Today's fit predicts 28.76 s. Within 0.2 %, five weeks apart.
- RTC awake then: 3.96 to 4.05 % fast, so 1 RTC second = 0.961 real seconds, an error of about
  -3.85 %. Today's sleep: -4.10 %. **The awake and sleep rates agree to about 0.25 %**, against
  errors of 2 to 5 %. A measurement made while awake would remove most of the error. To confirm on
  the same day and board with a finer awake measurement (the `rtcrate` idea).

The fixed part differs per board (0.55, 0.24, 0.01 s). It comes from where in the RTC second each
board goes to sleep, so it follows the awake time, not the board.

## Experiment 2: the RC32K rate measured while awake (coded 31 October 2026, not built)

Charles's idea: time RTC alarm interrupts against the crystal, with no other RTC channel needed.
Temporary console command `rtcrate [1-3]` (`CLI-commands.c`, marked TEMPORARY):
- Reads the RTC, sets its alarm 2 s ahead, then 1 to 3 s after that. Notes the core's cycle counter
  (400 MHz from the 24 MHz crystal) at each alarm, from the interrupt, or by polling the alarm's raw
  status if the interrupt does not come. Prints the error in ppm and %, same sign as
  `sleep_timing.py`, which alarm source was used, and how long `hx_drv_rtc_set_alarm()` took.
- Spins for up to 5 s (the cycle counter stops if the core idles), so refused while the camera is
  busy. The alarm is left disabled; the sleep code sets it again before DPD.

To compare with each board's sleep rate: WILD-CMSS -4.65 %, WILD-XAES -2.26 %, depopulated -4.10 %.

### Result 4 (`logs/depop_rate.txt`, the depopulated board, 31 October 2026)

**Awake (`rtcrate`):** -3.91 %, five readings within -39,104 to -39,125 ppm. 1 s as good as 3 s. The
alarm interrupt works while awake; `hx_drv_rtc_set_alarm()` takes 316 us.

**Asleep, same session:** two 300 s sleeps, 287.60 and 287.97 s. The fixed part of a sleep is between
0 and -1 s (where in the RTC second the board goes to sleep), which at 300 s is up to 0.33 %. So these
sleeps give -3.9 to -4.1 %, and the morning's 60 s + 300 s fit gave -4.10 %.

**So awake and asleep agree to within 0 to 0.2 %.** 300 s sleeps cannot say which. A one-hour sleep
can: the fixed part is then under 0.03 %.

## The fix: RTC self-calibration (31 October 2026)

Charles agreed: measure while awake, keep the value in the board record, correct the alarm. Fractions
of a second are not carried between sleeps. The trim code and `rc32ktrim`/`rtcrate` are removed.
- **`rtc_cal.c/.h` (new):** the measurement (`rtcrate`'s method, 1 RTC second); before sleep, measure
  if due (cold boot, no saved value, a day of RTC time since the last, or the RTC set back), save it
  with the RTC time; `rtc_cal_correctAlarm()` gives the RTC seconds for a real interval.
  `RTC_CAL_SLEEP_OFFSET_PPM` (0) is there for the awake/asleep gap if one is confirmed.
- **Board record** (`xip_state`, version 2): `rtcCalValid`, `rtcCalOff`, `rtcPpm`, `rtcMeasuredAt`.
- **`sleep_mode.c`:** prints `RTC correction N ppm` (or `none`) and `Sleep Ns = M RTC s`.
- **`rtccal [measure | on | off]`** on the console. `off` is for timing the uncorrected sleep.
- **`sleep_timing.py`** reads the new lines, groups per log, and handles the stray character.

### Result 5 (`logs/depop_cal.txt`, the depopulated board, 31 October 2026)

The correction works. Cold boot, `rtccal` said "not measured yet", then `setop 7 300`.
- First sleep: "RTC calibration: -38768 ppm (-3.88 %), saved", "Sleep 300s = 312 RTC s". The
  measurement took 2.6 s (wait for the first alarm, then one RTC second), so the core is busy 2 to
  3 s, not 1 to 2 s as first written. Later sleeps used the saved value without measuring.
- 10 sleeps: 299.53 to 299.72 s, mean 299.56 s (-0.15 %). Allowing for the half second lost to the
  whole-second alarm, the rate is +0.02 %. Before the fix this board slept about 288 s.
- -38768 ppm here against -39104 to -39125 ppm with `rtcrate` earlier the same day: 0.035 % apart,
  perhaps temperature. Small against the 4 % corrected.
- So the awake measurement predicts the sleep rate on this board: `RTC_CAL_SLEEP_OFFSET_PPM` stays 0.


### Result 6 (`logs/xaes_cal.txt`, board WILD-XAES, 31 October 2026)

The correction works here too. Cold boot, measured -19424 ppm (-1.94 %), so 300 s became 306 RTC s.
- 3 sleeps of 300 s: 299.16, 299.46, 299.49 s, mean 299.37 s (-0.21 %, rate -0.04 %). Before the
  fix this board slept about 293 s.
- A first 20 s sleep was not corrected: 20 x 1.0194 = 20.39 rounds to 20 RTC s. Sleeps under about
  25 s (at 2 %) or 12 s (at 4 %) cannot be corrected in whole seconds. Expected, and small in seconds.
- Result 2's fit put this board at 2.26 % fast, against 1.94 % measured: that fit was from 60 to
  300 s sleeps, where the fixed part and the half second weigh more. The corrected sleeps agree with
  the measurement, so the offset stays 0.


### Result 7 (`logs/cmss_cal.txt`, board WILD-CMSS, 31 October 2026)

Measured -45874 ppm (-4.59 %) at the cold boot, so 300 s became 314 RTC s.
- 6 sleeps of 300 s: 298.76 to 299.45 s, mean 298.98 s (-0.34 %, rate -0.17 % allowing the half
  second). Result 1 found this board has a fixed part of 0.55 s per sleep; allowing that too, the
  rate is 0.0 %. So the correction is right here as well, and the offset stays 0.
- Without a card the second sleep set no timer (op 7 back to 0 from the old record): the build
  predates the fix for that (step log, 31 October). With the card in, op 7 came from CONFIG.TXT.

**Summary, three boards (HM0360 image):** 2 to 4.6 % fast before, within about 1 s in 300 s after
(-0.15, -0.21, -0.34 %), all of it the whole-second alarm and each board's fixed part.


### Result 8 (`logs/cmss_rp3.txt`, WILD-CMSS, RP3 image, 31 October 2026)

The RP3 build works the same. Cold boot measured -45466 ppm (-4.55 %), 300 s became 314 RTC s.
4 sleeps: 299.54 (the first, just after the measurement), then 298.65 to 298.75 s; mean 298.91 s
(-0.36 %). About 0.2 s shorter than the HM0360 image on this board, which is within the
whole-second alarm's spread. The three cold-boot measurements on WILD-CMSS today: -45874,
-45546, -45466 ppm (0.04 % spread).
